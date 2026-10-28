#include "EtherCat/Services/EtherCatMasterService.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

#include "Base/Async.h"
#include "Base/Text.h"
#include "EtherCat/Models/LinearMoveProfile.h"
#include "EtherCat/Transport/SimulatedEtherCatBus.h"

namespace ecm
{
    namespace
    {
        using Sw = CiA402Registers::Statusword;
        using Cw = CiA402Registers::Controlword;
        using Cmd = CiA402Registers::ControlwordCmd;

        const AxisConfig* FindAxisConfig(const EtherCatConfig* config, int slavePosition)
        {
            if (!config) return nullptr;
            for (const auto& a : config->Axes)
                if (a.Address() == slavePosition) return &a;
            return nullptr;
        }

        /// (int)Math.Round(x): nửa về chẵn; kẹp miền int để tránh UB khi tràn.
        int RoundToInt(double v)
        {
            double r = std::nearbyint(v);
            if (!(r > -2147483648.0)) return INT32_MIN;
            if (r >= 2147483647.0) return INT32_MAX;
            return static_cast<int>(r);
        }

        /// (uint)x với x >= 1: cắt phần thập phân, kẹp miền uint.
        uint32_t ToUInt(double v)
        {
            if (!(v > 0)) return 0;
            if (v >= 4294967295.0) return UINT32_MAX;
            return static_cast<uint32_t>(v);
        }

        const char* B(bool v) { return v ? "True" : "False"; }

        /// <summary>Số slave vật lý khác nhau mà các trục chiếm — hai trục của một driver WE2x là MỘT slave.</summary>
        int DistinctSlaveCount(const std::vector<AxisConfig>& axes)
        {
            std::vector<int> slaves;
            for (const auto& a : axes)
                if (std::find(slaves.begin(), slaves.end(), a.SlavePosition) == slaves.end()) slaves.push_back(a.SlavePosition);
            return static_cast<int>(slaves.size());
        }

        /// double.ToString() mặc định của .NET Framework (15 chữ số có nghĩa).
        std::string D(double v) { return std::format("{:.15G}", v); }

        /// <summary>Converts a velocity value as typed by the user (in config.SpeedUnit) into axis
        /// units/s. Rpm needs TravelPerRotationUnits to convert — without it, the number is passed
        /// through unchanged rather than silently guessing a scale factor.</summary>
        double ToUnitsPerSecond(double velocity, const AxisConfig& config)
        {
            switch (config.SpeedUnit)
            {
                case SpeedUnit::UnitPerMin:
                    return velocity / 60.0;
                case SpeedUnit::Rpm:
                    return config.TravelPerRotationUnits.has_value()
                        ? velocity * config.TravelPerRotationUnits.value() / 60.0
                        : velocity;
                default: // UnitPerTime, UnitPerSec
                    return velocity;
            }
        }
    }

    EtherCatMasterService::EtherCatMasterService(std::shared_ptr<IEtherCatBus> bus)
        : m_bus(bus ? std::move(bus) : std::make_shared<SimulatedEtherCatBus>())
    {
    }

    EtherCatMasterService::~EtherCatMasterService() { Dispose(); }

    std::vector<std::shared_ptr<AxisState>> EtherCatMasterService::AxesSnapshot() const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_axes;
    }

    std::vector<std::shared_ptr<IoModuleState>> EtherCatMasterService::IoModulesSnapshot() const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_ioModules;
    }

    std::shared_ptr<const EtherCatConfig> EtherCatMasterService::Config() const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_config;
    }

    void EtherCatMasterService::SetConfig(const EtherCatConfig& config)
    {
        auto copy = std::make_shared<const EtherCatConfig>(config);
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_config = std::move(copy);
    }

    // ══════════════════════════════════════════════════════════════
    //  Connection
    // ══════════════════════════════════════════════════════════════

    bool EtherCatMasterService::Connect(const EtherCatConfig& config)
    {
        try
        {
            SetConfig(config);

            if (!m_bus->Open(config.NetworkAdapter, DistinctSlaveCount(config.Axes)))
                throw std::runtime_error("Failed to open EtherCAT bus");

            std::vector<std::shared_ptr<AxisState>> axes;
            for (const auto& axisConfig : config.Axes)
            {
                auto axis = std::make_shared<AxisState>();
                axis->Name = axisConfig.Name;
                axis->SlavePosition = axisConfig.SlavePosition;
                axis->Address = axisConfig.Address();
                axis->IsConnected = true;
                axes.push_back(std::move(axis));
            }
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_axes.swap(axes);
            }

            m_isConnected = true;
            StartCyclicExchange();
            OnConnectionChanged.Raise(true);
            Log(std::format("Connected on '{}', {} axis(es), cycle={}ms", config.NetworkAdapter, config.Axes.size(), config.CycleTimeMs));
            return true;
        }
        catch (const std::exception& ex)
        {
            Log(std::format("Connection failed: {}", ex.what()));
            m_isConnected = false;
            OnConnectionChanged.Raise(false);
            return false;
        }
    }

    std::vector<AxisConnectResult> EtherCatMasterService::ConnectAllAxis(const EtherCatConfig& config)
    {
        std::vector<AxisConnectResult> ioResults;
        return ConnectAllAxis(config, ioResults);
    }

    std::vector<AxisConnectResult> EtherCatMasterService::ConnectAllAxis(const EtherCatConfig& config, std::vector<AxisConnectResult>& ioResults)
    {
        ioResults.clear();
        try
        {
            SetConfig(config);
            std::vector<AxisConfig> configuredAxes;
            for (const auto& a : config.Axes)
                if (a.IsConfigured()) configuredAxes.push_back(a);
            std::vector<IoModuleConfig> configuredIo;
            for (const auto& m : config.IoModules)
                if (m.IsConfigured()) configuredIo.push_back(m);

            if (configuredAxes.empty() && configuredIo.empty())
            {
                // Found live on real hardware: with zero configured slots, All() over an empty
                // result list is vacuously true, which used to read as "fully validated" and start
                // cyclic exchange against a bus that was never actually opened — the cyclic thread
                // then spammed "bus is not open" every cycle. Nothing to validate here, so don't
                // touch the bus at all.
                m_isConnected = false;
                OnConnectionChanged.Raise(false);
                Log("Connect All Axis: no axes or I/O modules configured — nothing to connect.");
                return {};
            }

            // Must precede the bring-up: a modular coupler's SyncManager lengths are programmed
            // from these module widths during the PRE-OP configuration hook, and the device
            // itself cannot supply them (see IEtherCatBus::SetModuleCatalog).
            m_bus->SetModuleCatalog(config.ModuleCatalog);
            m_bus->SetSyncManagerCatalog(config.SyncManagerCatalog);

            auto results = m_bus->ConnectAllAxis(config.NetworkAdapter, configuredAxes, configuredIo, ioResults);
            auto isOk = [](const AxisConnectResult& r) { return r.IsOk(); };
            bool allOk = results.size() == configuredAxes.size() && std::all_of(results.begin(), results.end(), isOk)
                      && ioResults.size() == configuredIo.size() && std::all_of(ioResults.begin(), ioResults.end(), isOk);

            if (allOk)
            {
                std::vector<std::shared_ptr<AxisState>> axes;
                for (const auto& axisConfig : configuredAxes)
                {
                    auto axis = std::make_shared<AxisState>();
                    axis->Name = axisConfig.Name;
                    axis->SlavePosition = axisConfig.SlavePosition;
                axis->Address = axisConfig.Address();
                    axis->IsConnected = true;
                    axes.push_back(std::move(axis));
                    WriteDriveParameters(axisConfig);
                }

                std::vector<std::shared_ptr<IoModuleState>> ioModules;
                for (const auto& ioConfig : configuredIo)
                {
                    auto ioResult = std::find_if(ioResults.begin(), ioResults.end(),
                                                 [&](const AxisConnectResult& r) { return r.SlavePosition == ioConfig.SlavePosition; });
                    auto io = std::make_shared<IoModuleState>();
                    io->Name = ioConfig.Name;
                    io->SlavePosition = ioConfig.SlavePosition;
                    io->InputCount = ioResult != ioResults.end() ? ioResult->InputBitCount : 0;
                    io->OutputCount = ioResult != ioResults.end() ? ioResult->OutputBitCount : 0;
                    io->Slots = BuildSlotStates(ioConfig.SlavePosition, config.ModuleCatalog);
                    ioModules.push_back(std::move(io));
                }

                {
                    // Hoán đổi nguyên khối dưới khoá: người đọc không bao giờ thấy vector dựng dở.
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    m_axes.swap(axes);
                    m_ioModules.swap(ioModules);
                }

                m_isConnected = true;
                StartCyclicExchange();
                OnConnectionChanged.Raise(true);
                Log(std::format("Connect All Axis: {} axis(es) and {} I/O module(s) validated OK, connected on '{}'.",
                                configuredAxes.size(), configuredIo.size(), config.NetworkAdapter));
            }
            else
            {
                m_isConnected = false;
                OnConnectionChanged.Raise(false);
                auto failCount = std::count_if(results.begin(), results.end(), [](const AxisConnectResult& r) { return !r.IsOk(); })
                               + std::count_if(ioResults.begin(), ioResults.end(), [](const AxisConnectResult& r) { return !r.IsOk(); });
                Log(std::format("Connect All Axis: {} of {} configured slot(s) failed validation — not connected.",
                                failCount, configuredAxes.size() + configuredIo.size()));
            }

            return results;
        }
        catch (const std::exception& ex)
        {
            Log(std::format("Connect All Axis failed: {}", ex.what()));
            m_isConnected = false;
            OnConnectionChanged.Raise(false);
            return {};
        }
    }

    /// <summary>
    /// Writes the real, drive-side parameters ConnectAllAxis is allowed to set once a slot is
    /// validated Ok — the software position limits (0x607D) and the feed constant (0x6092:1), the
    /// only extra objects confirmed present+writable on real hardware (deliberately not a generic
    /// "any SDO" facility). nullopt fields are left untouched on the drive. The bus is already
    /// OPERATIONAL by the time this runs, so mailbox/SDO is safe here.
    /// </summary>
    void EtherCatMasterService::WriteDriveParameters(const AxisConfig& axisConfig)
    {
        auto writeOne = [&](uint16_t index, uint8_t sub, double units, const std::string& label, const std::string& unitText)
        {
            int counts = RoundToInt(units * axisConfig.CountsPerUnit);
            m_bus->WriteSdo(axisConfig.Address(), index, sub, counts, 4);
            int readBack = m_bus->ReadSdo(axisConfig.Address(), index, sub, 4);
            Log(std::format("[Axis {}] {} -> {} {} ({} counts); read back from drive: {} counts{}", AxisAddress::Text(axisConfig.Address()), label,
                            D(units), unitText, counts, readBack, readBack == counts ? " (confirmed)" : " (MISMATCH!)"));
        };
        if (axisConfig.SoftwareLimitNegativeUnits.has_value())
            writeOne(CiA402Registers::SOFTWARE_POSITION_LIMIT, 1, *axisConfig.SoftwareLimitNegativeUnits, "Software limit (negative)", "units");
        if (axisConfig.SoftwareLimitPositiveUnits.has_value())
            writeOne(CiA402Registers::SOFTWARE_POSITION_LIMIT, 2, *axisConfig.SoftwareLimitPositiveUnits, "Software limit (positive)", "units");
        if (axisConfig.TravelPerRotationUnits.has_value())
            writeOne(CiA402Registers::FEED_CONSTANT, 1, *axisConfig.TravelPerRotationUnits, "Travel/rotation", ToString(axisConfig.Unit));
    }

    std::vector<DiscoveredSlaveInfo> EtherCatMasterService::ScanSlaves(const std::string& networkAdapter)
    {
        return m_bus->ScanSlaves(networkAdapter);
    }

    void EtherCatMasterService::Disconnect()
    {
        StopCyclicExchange();

        try { if (m_bus) m_bus->Close(); } catch (...) {}

        m_isConnected = false;
        for (const auto& axis : AxesSnapshot())
            axis->IsConnected = false;

        OnConnectionChanged.Raise(false);
        Log("Disconnected");
    }

    // ══════════════════════════════════════════════════════════════
    //  CiA402 State Machine — Servo ON / OFF / Fault Reset
    // ══════════════════════════════════════════════════════════════

    void EtherCatMasterService::ServoOn(int slavePosition)
    {
        auto axis = GetAxisState(slavePosition);
        if (!axis) return;

        WriteControlword(slavePosition, Cmd::EnableOperation);

        // Confirmed on real hardware: the Switched-On -> Operation-Enabled transition can take
        // well over 500ms (observed ~1-2s), likely a brake-release delay on the drive side — a
        // short timeout here was producing a false "did not reach Operation Enabled" diagnosis.
        if (WaitForOperationEnabled(slavePosition, 3000))
        {
            Log(std::format("[Axis {}] Servo ON (Operation Enabled)", AxisAddress::Text(slavePosition)));
        }
        else
        {
            Log(std::format("[Axis {}] Servo ON did NOT reach Operation Enabled "
                            "(statusword=0x{:04X}: ReadyToSwitchOn={} SwitchedOn={} "
                            "Fault={} VoltageEnabled={} QuickStopActive={} "
                            "SwitchOnDisabled={}) — check E-stop/STO/24V enable input on the drive.",
                            AxisAddress::Text(slavePosition), axis->Statusword.load(), B(axis->ReadyToSwitchOn), B(axis->SwitchedOn),
                            B(axis->Fault), B(axis->VoltageEnabled), B(axis->QuickStopActive), B(axis->SwitchOnDisabled)));
        }
    }

    /// <summary>Poll (10ms) until the axis reports Operation Enabled or a Fault, up to timeoutMs.</summary>
    bool EtherCatMasterService::WaitForOperationEnabled(int slavePosition, int timeoutMs)
    {
        auto axis = GetAxisState(slavePosition);
        if (!axis) return false;

        base::Stopwatch sw;
        while (!axis->OperationEnabled)
        {
            if (axis->Fault) return false;
            if (sw.ElapsedMilliseconds() > timeoutMs) return false;
            base::Delay(10);
        }
        return true;
    }

    void EtherCatMasterService::ServoOff(int slavePosition)
    {
        WriteControlword(slavePosition, Cmd::Shutdown);
        Log(std::format("[Axis {}] Servo OFF", AxisAddress::Text(slavePosition)));
    }

    void EtherCatMasterService::FaultReset(int slavePosition)
    {
        WriteControlword(slavePosition, Cmd::FaultReset);
        base::Delay(20);
        WriteControlword(slavePosition, 0x0000);
        Log(std::format("[Axis {}] Fault reset", AxisAddress::Text(slavePosition)));
    }

    void EtherCatMasterService::SetMode(int slavePosition, OperationMode mode)
    {
        std::lock_guard<std::mutex> lock(m_busLock);
        m_bus->SetModeOfOperation(slavePosition, static_cast<int8_t>(mode));
        const char* name = ToString(mode);
        Log(std::format("[Axis {}] Mode -> {}", AxisAddress::Text(slavePosition), name[0] ? std::string(name) : std::to_string(static_cast<int>(mode))));
    }

    // ══════════════════════════════════════════════════════════════
    //  Motion — Profile Position (pp) mode
    // ══════════════════════════════════════════════════════════════

    void EtherCatMasterService::MoveAbsolute(int slavePosition, double positionUnits, double profileVelocity,
                                             double accelTimeS, double decelTimeS)
    {
        auto axis = GetAxisState(slavePosition);
        auto config = Config();
        if (!axis || !FindAxisConfig(config.get(), slavePosition)) return;

        if (!axis->CanMove())
        {
            Log(std::format("[Axis {}] Cannot move: OperationEnabled={}, Fault={}", AxisAddress::Text(slavePosition), B(axis->OperationEnabled), B(axis->Fault)));
            return;
        }

        std::lock_guard<std::mutex> lock(m_busLock);
        try
        {
            WriteMoveCommand(config.get(), slavePosition, positionUnits, profileVelocity, accelTimeS, decelTimeS);
            base::Delay(config ? config->NewSetpointPulseMs : 20);
            ClearNewSetpoint(slavePosition);

            Log(std::format("[Axis {}] MoveAbsolute target={:.3f} vel={:.3f} tAcc={:.2f}s tDec={:.2f}s",
                            AxisAddress::Text(slavePosition), positionUnits, profileVelocity, accelTimeS, decelTimeS));
        }
        catch (const std::exception& ex)
        {
            Log(std::format("[Axis {}] MoveAbsolute error: {}", AxisAddress::Text(slavePosition), ex.what()));
        }
    }

    void EtherCatMasterService::MoveRelative(int slavePosition, double deltaUnits, double profileVelocity,
                                             double accelTimeS, double decelTimeS)
    {
        auto axis = GetAxisState(slavePosition);
        if (!axis) return;

        MoveAbsolute(slavePosition, axis->ActualPositionUnits + deltaUnits, profileVelocity, accelTimeS, decelTimeS);
    }

    void EtherCatMasterService::Jog(int slavePosition, int direction, double speed, double accelTimeS, double decelTimeS)
    {
        if (direction == 0)
        {
            Log(std::format("[Axis {}] Jog: direction must be -1 or +1, got 0", AxisAddress::Text(slavePosition)));
            return;
        }

        auto axis = GetAxisState(slavePosition);
        auto cfg = Config();
        const AxisConfig* config = FindAxisConfig(cfg.get(), slavePosition);
        if (!axis || !config) return;

        double farTarget = direction > 0
            ? config->SoftwareLimitPositiveUnits.value_or(axis->ActualPositionUnits + JogReachUnits)
            : config->SoftwareLimitNegativeUnits.value_or(axis->ActualPositionUnits - JogReachUnits);

        MoveAbsolute(slavePosition, farTarget, speed, accelTimeS, decelTimeS);
    }

    /// <summary>
    /// Write target position/profile velocity/accel/decel and raise the pp-mode "new set-point"
    /// pulse for one axis, WITHOUT waiting/clearing it. Must be called within m_busLock. Used by
    /// MoveAbsolute and MoveLinear (multiple axes triggered in the same lock hold, so their moves
    /// start within the same cycle instead of tens of ms apart).
    /// </summary>
    void EtherCatMasterService::WriteMoveCommand(const EtherCatConfig* cfg, int slavePosition, double positionUnits,
                                                 double profileVelocity, double accelTimeS, double decelTimeS)
    {
        const AxisConfig* config = FindAxisConfig(cfg, slavePosition);
        if (!config)
            throw std::runtime_error("Object reference not set to an instance of an object.");
        int targetCounts = RoundToInt(positionUnits * config->CountsPerUnit);
        uint32_t velCounts = ToUInt(std::max(1.0, std::fabs(ToUnitsPerSecond(profileVelocity, *config)) * config->CountsPerUnit));
        uint32_t accelCounts = ToUInt(std::max(1.0, velCounts / std::max(0.001, accelTimeS)));
        uint32_t decelCounts = ToUInt(std::max(1.0, velCounts / std::max(0.001, decelTimeS)));

        m_bus->SetModeOfOperation(slavePosition, static_cast<int8_t>(OperationMode::ProfilePosition));
        m_bus->SetProfileVelocity(slavePosition, velCounts);
        m_bus->SetProfileAcceleration(slavePosition, accelCounts);
        m_bus->SetProfileDeceleration(slavePosition, decelCounts);
        m_bus->SetTargetPosition(slavePosition, targetCounts);

        // Pulse "new set-point" (bit4) + "change set immediately" (bit5) — ON/delay/OFF — so the
        // drive latches the move.
        uint16_t cwOn = static_cast<uint16_t>(Cmd::EnableOperation | Cw::NewSetPoint | Cw::ChangeSetImmediately);
        m_bus->SetControlword(slavePosition, cwOn);
    }

    /// <summary>Clear the pp-mode "new set-point" bit, leaving Operation Enabled. Must be called within m_busLock.</summary>
    void EtherCatMasterService::ClearNewSetpoint(int slavePosition)
    {
        m_bus->SetControlword(slavePosition, Cmd::EnableOperation);
    }

    bool EtherCatMasterService::MoveLinear(int slavePos1, double pos1, int slavePos2, double pos2,
                                           double velocity, double accelTimeS, double decelTimeS)
    {
        auto axis1 = GetAxisState(slavePos1);
        auto axis2 = GetAxisState(slavePos2);
        if (!axis1 || !axis2) return false;

        if (!axis1->CanMove() || !axis2->CanMove())
        {
            Log(std::format("[Linear] Cannot move: axis{}.CanMove={} axis{}.CanMove={}", slavePos1, B(axis1->CanMove()), slavePos2, B(axis2->CanMove())));
            return false;
        }

        LinearMoveProfile profile;
        profile.Init(axis1->ActualPositionUnits, axis2->ActualPositionUnits, pos1, pos2, velocity);

        if (profile.L < 1e-6)
            return true;

        // Both axes are written and pulsed within the SAME lock hold, so they start moving within
        // the same cycle instead of ~20ms apart (which would bow the path off a straight line).
        auto config = Config();
        {
            std::lock_guard<std::mutex> lock(m_busLock);
            WriteMoveCommand(config.get(), slavePos1, pos1, std::fabs(profile.Vx), accelTimeS, decelTimeS);
            WriteMoveCommand(config.get(), slavePos2, pos2, std::fabs(profile.Vy), accelTimeS, decelTimeS);
            base::Delay(config ? config->NewSetpointPulseMs : 20);
            ClearNewSetpoint(slavePos1);
            ClearNewSetpoint(slavePos2);
        }

        Log(std::format("[Linear] ({:.2f},{:.2f}) -> ({:.2f},{:.2f}) V={:.2f} Vx={:.2f} Vy={:.2f}",
                        axis1->ActualPositionUnits.load(), axis2->ActualPositionUnits.load(), pos1, pos2, velocity, profile.Vx, profile.Vy));
        return false;
    }

    // ══════════════════════════════════════════════════════════════
    //  Homing
    // ══════════════════════════════════════════════════════════════

    void EtherCatMasterService::StartHome(int slavePosition)
    {
        auto axis = GetAxisState(slavePosition);
        auto cfg = Config();
        const AxisConfig* config = FindAxisConfig(cfg.get(), slavePosition);
        if (!axis || !config) return;

        if (!axis->CanMove())
        {
            Log(std::format("[Axis {}] Cannot home: not ready", AxisAddress::Text(slavePosition)));
            return;
        }

        std::lock_guard<std::mutex> lock(m_busLock);
        double vel = std::max(1.0, std::fabs(ToUnitsPerSecond(config->HomingVelocity, *config)) * config->CountsPerUnit);
        int velCounts = vel >= 2147483647.0 ? INT32_MAX : static_cast<int>(vel);

        m_bus->SetModeOfOperation(slavePosition, static_cast<int8_t>(OperationMode::Homing));
        m_bus->WriteSdo(slavePosition, CiA402Registers::HOMING_METHOD, 0, config->HomingMethod, 1);
        m_bus->WriteSdo(slavePosition, CiA402Registers::HOMING_SPEED, 1, velCounts, 4);
        m_bus->WriteSdo(slavePosition, CiA402Registers::HOMING_SPEED, 2, velCounts, 4);
        m_bus->SetControlword(slavePosition, static_cast<uint16_t>(Cmd::EnableOperation | Cw::HomingStart));
        Log(std::format("[Axis {}] Homing started (method {}, vel={:.3f})", AxisAddress::Text(slavePosition), static_cast<int>(config->HomingMethod), config->HomingVelocity));
    }

    // ══════════════════════════════════════════════════════════════
    //  Digital I/O (Fastech Ezi-IO and similar remote modules)
    // ══════════════════════════════════════════════════════════════

    std::vector<uint32_t> EtherCatMasterService::GetDetectedModuleIdents(int slavePosition)
    {
        return m_bus->GetDetectedModuleIdents(slavePosition);
    }

    std::vector<EsiModuleInfo> EtherCatMasterService::GetDetectedModules(int slavePosition, const std::vector<EsiModuleInfo>& catalog)
    {
        std::vector<EsiModuleInfo> result;
        for (uint32_t ident : GetDetectedModuleIdents(slavePosition))
        {
            auto match = std::find_if(catalog.begin(), catalog.end(), [&](const EsiModuleInfo& m) { return m.ModuleIdent == ident; });
            if (match != catalog.end())
            {
                result.push_back(*match);
            }
            else
            {
                EsiModuleInfo unknown;
                unknown.ModuleIdent = ident;
                unknown.DisplayName = std::format("Unknown module 0x{:08X}", ident);
                result.push_back(std::move(unknown));
            }
        }
        return result;
    }

    void EtherCatMasterService::SetDigitalOutput(int slavePosition, int channel, bool value)
    {
        auto io = GetIoModuleState(slavePosition);
        if (!io) return;

        std::lock_guard<std::mutex> lock(m_busLock);
        uint64_t bit = 1ull << (channel & 63);   // C# che số bit dịch về 6 bit
        uint64_t newMask = value ? (io->Outputs | bit) : (io->Outputs & ~bit);
        m_bus->SetDigitalOutputs(slavePosition, newMask);
        io->Outputs = newMask;
        Log(std::format("[IO {}] DO{} -> {}", slavePosition, channel, value ? "1" : "0"));
    }

    void EtherCatMasterService::SetSlotOutput(int slavePosition, int slot, int channel, bool value)
    {
        auto io = GetIoModuleState(slavePosition);
        std::shared_ptr<IoSlotState> slotState;
        if (io)
            for (const auto& s : io->Slots)
                if (s->SlotIndex == slot) { slotState = s; break; }
        if (!slotState) return;

        std::lock_guard<std::mutex> lock(m_busLock);
        uint32_t bit = 1u << (channel & 31);
        uint32_t newMask = value ? (slotState->OutputRegister | bit) : (slotState->OutputRegister & ~bit);
        m_bus->SetIoSlotOutputRegister(slavePosition, slot, newMask);
        slotState->OutputRegister = newMask;
        Log(std::format("[IO {} slot {}] DO{} -> {}", slavePosition, slot, channel, value ? "1" : "0"));
    }

    /// <summary>Build one IoSlotState per module the coupler reported in its rack (0xF050),
    /// resolving each slot's ident against the ESI module catalog for a display name. Empty for a
    /// non-modular I/O slave.</summary>
    std::vector<std::shared_ptr<IoSlotState>> EtherCatMasterService::BuildSlotStates(int slavePosition, const std::vector<EsiModuleInfo>& catalog)
    {
        std::vector<std::shared_ptr<IoSlotState>> slots;
        int count = m_bus->GetIoSlotCount(slavePosition);
        for (int slot = 0; slot < count; slot++)
        {
            uint32_t ident = m_bus->GetIoSlotIdent(slavePosition, slot);
            auto match = std::find_if(catalog.begin(), catalog.end(), [&](const EsiModuleInfo& m) { return m.ModuleIdent == ident; });
            auto state = std::make_shared<IoSlotState>();
            state->SlotIndex = slot;
            state->ModuleIdent = ident;
            state->ModuleName = match != catalog.end() ? match->DisplayName : std::format("Unknown module 0x{:08X}", ident);
            state->InputCount = m_bus->GetIoSlotInputBitCount(slavePosition, slot);
            state->OutputCount = m_bus->GetIoSlotOutputBitCount(slavePosition, slot);
            slots.push_back(std::move(state));
        }
        return slots;
    }

    // ══════════════════════════════════════════════════════════════
    //  Stop
    // ══════════════════════════════════════════════════════════════

    void EtherCatMasterService::Stop(int slavePosition)
    {
        WriteControlword(slavePosition, static_cast<uint16_t>(Cmd::EnableOperation | Cw::Halt));
        Log(std::format("[Axis {}] Stop (halt)", AxisAddress::Text(slavePosition)));
    }

    void EtherCatMasterService::EmergencyStop(int slavePosition)
    {
        WriteControlword(slavePosition, Cmd::QuickStop);
        Log(std::format("[Axis {}] EMERGENCY STOP", AxisAddress::Text(slavePosition)));
    }

    void EtherCatMasterService::StopAllAxes()
    {
        for (const auto& axis : AxesSnapshot())
            Stop(axis->Address);
    }

    bool EtherCatMasterService::WaitForMotionComplete(int slavePosition, int timeoutMs)
    {
        auto axis = GetAxisState(slavePosition);
        if (!axis) return false;

        base::Stopwatch sw;
        while (!axis->TargetReached)
        {
            if (timeoutMs > 0 && sw.ElapsedMilliseconds() > timeoutMs)
            {
                Log(std::format("[Axis {}] WaitForMotionComplete timed out after {}ms", AxisAddress::Text(slavePosition), timeoutMs));
                return false;
            }
            if (axis->Fault || axis->IsEMG)
            {
                Log(std::format("[Axis {}] WaitForMotionComplete aborted: Fault={}, EMG={}", AxisAddress::Text(slavePosition), B(axis->Fault), B(axis->IsEMG)));
                return false;
            }
            base::Delay(10);
        }
        return true;
    }

    // ══════════════════════════════════════════════════════════════
    //  Cyclic Exchange (background thread — axis-state polling, NOT the EtherCAT heartbeat)
    // ══════════════════════════════════════════════════════════════

    /// <summary>
    /// Start the background axis-state polling thread. On the real bus the actual EtherCAT cyclic
    /// exchange (what drives' watchdogs depend on) does NOT happen here: it runs on a dedicated
    /// native real-time thread inside the bus, independent of this thread or any SDO call in
    /// progress — driving it from this thread under the same lock as every SDO call was the
    /// confirmed root cause of real "PDI Watchdog Timeout" faults. m_bus->CyclicExchange() is a
    /// deliberate no-op on the real bus (kept for SimulatedEtherCatBus, which needs explicit
    /// stepping); this thread's real job is refreshing state at a UI-relevant rate.
    /// </summary>
    void EtherCatMasterService::StartCyclicExchange()
    {
        std::lock_guard<std::mutex> threadLock(m_threadMutex);
        if (m_cyclicThread.joinable()) return;

        auto config = Config();
        int cycleMs = config ? config->CycleTimeMs : 4;

        m_cyclicThread = std::jthread([this, cycleMs](std::stop_token token)
        {
            Log("Cyclic exchange started");
            while (!token.stop_requested())
            {
                try
                {
                    std::lock_guard<std::mutex> lock(m_busLock);
                    m_bus->CyclicExchange();
                    bool pollVelocityTorque = PollVelocityTorque && (m_pollCounter++ % VelocityTorquePollDivider) == 0;

                    std::vector<std::shared_ptr<AxisState>> axes;
                    std::vector<std::shared_ptr<IoModuleState>> ioModules;
                    std::shared_ptr<const EtherCatConfig> cfg;
                    {
                        std::lock_guard<std::mutex> stateLock(m_stateMutex);
                        axes = m_axes;
                        ioModules = m_ioModules;
                        cfg = m_config;
                    }
                    for (const auto& axis : axes)
                        PollAxisState(*axis, cfg.get(), pollVelocityTorque);
                    for (const auto& io : ioModules)
                        PollIoModuleState(*io);
                }
                catch (const std::exception& ex)
                {
                    Log(std::format("Cyclic exchange error: {}", ex.what()));
                }

                base::Delay(cycleMs);
            }
            Log("Cyclic exchange stopped");
        });
    }

    /// <summary>Stop the polling thread and wait for it to exit (bản C# chỉ huỷ token; ở đây JOIN
    /// để bus không bị đóng khi luồng còn đang đọc). Gọi từ chính luồng polling (handler sự kiện)
    /// thì chỉ yêu cầu dừng, không tự join.</summary>
    void EtherCatMasterService::StopCyclicExchange()
    {
        std::jthread thread;
        {
            std::lock_guard<std::mutex> threadLock(m_threadMutex);
            thread = std::move(m_cyclicThread);
        }
        if (!thread.joinable()) return;
        thread.request_stop();
        if (thread.get_id() == std::this_thread::get_id()) thread.detach();
        else thread.join();
    }

    /// <summary>Refresh one axis's state from the bus's cyclic input PDOs. Must be called within m_busLock.</summary>
    void EtherCatMasterService::PollAxisState(AxisState& axis, const EtherCatConfig* cfg, bool pollVelocityTorque)
    {
        int pos = axis.Address;
        const AxisConfig* config = FindAxisConfig(cfg, pos);

        uint16_t sw = m_bus->GetStatusword(pos);
        axis.Statusword = sw;
        axis.ReadyToSwitchOn = (sw & Sw::ReadyToSwitchOn) != 0;
        axis.SwitchedOn = (sw & Sw::SwitchedOn) != 0;
        axis.OperationEnabled = (sw & Sw::OperationEnabled) != 0;
        axis.Fault = (sw & Sw::Fault) != 0;
        axis.VoltageEnabled = (sw & Sw::VoltageEnabled) != 0;
        axis.QuickStopActive = (sw & Sw::QuickStop) == 0; // active low
        axis.SwitchOnDisabled = (sw & Sw::SwitchOnDisabled) != 0;
        axis.Warning = (sw & Sw::Warning) != 0;
        axis.TargetReached = (sw & Sw::TargetReached) != 0;
        axis.InternalLimitActive = (sw & Sw::InternalLimitActive) != 0;
        axis.SetpointAckOrHomingAttained = (sw & Sw::SetpointAckOrHomingAttained) != 0;
        axis.FollowingErrorOrHomingError = (sw & Sw::FollowingErrorOrHomingError) != 0;

        axis.ModeOfOperationDisplay = m_bus->GetModeOfOperationDisplay(pos);
        int raw = m_bus->GetActualPosition(pos);
        axis.ActualPositionRaw = raw;
        axis.ActualPositionUnits = (config && config->CountsPerUnit != 0) ? raw / config->CountsPerUnit : static_cast<double>(raw);
        if (pollVelocityTorque)
        {
            axis.ActualVelocityRaw = m_bus->GetActualVelocity(pos);
            axis.ActualTorque = m_bus->GetActualTorque(pos);
        }

        uint16_t newErrorCode = m_bus->GetErrorCode(pos);
        if (newErrorCode != 0 && newErrorCode != axis.ErrorCode)
            OnAlarm.Raise(pos, newErrorCode);
        axis.ErrorCode = newErrorCode;

        axis.IsConnected = true;
    }

    /// <summary>Refresh one I/O module's live input bits from the bus's cyclic input PDO. Must be
    /// called within m_busLock. Plain PDO read (no SDO), safe to do every cycle.</summary>
    void EtherCatMasterService::PollIoModuleState(IoModuleState& io)
    {
        io.Inputs = m_bus->GetDigitalInputs(io.SlavePosition);

        uint32_t newDeviceStatus = m_bus->GetIoModuleDeviceStatus(io.SlavePosition);
        if (newDeviceStatus != 0 && newDeviceStatus != io.DeviceStatus)
            OnIoModuleFault.Raise(io.SlavePosition, newDeviceStatus);
        io.DeviceStatus = newDeviceStatus;

        // Same plain-PDO read as above, once per module physically in the rack.
        for (const auto& slot : io.Slots)
            slot->InputRegister = m_bus->GetIoSlotInputRegister(io.SlavePosition, slot->SlotIndex);
    }

    // ══════════════════════════════════════════════════════════════
    //  Internal Helpers
    // ══════════════════════════════════════════════════════════════

    void EtherCatMasterService::WriteControlword(int slavePosition, uint16_t controlword)
    {
        std::lock_guard<std::mutex> lock(m_busLock);
        try
        {
            m_bus->SetControlword(slavePosition, controlword);
        }
        catch (const std::exception& ex)
        {
            Log(std::format("[Axis {}] WriteControlword error: {}", AxisAddress::Text(slavePosition), ex.what()));
        }
    }

    std::shared_ptr<AxisState> EtherCatMasterService::GetAxisState(int slavePosition) const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        for (const auto& a : m_axes)
            if (a->Address == slavePosition) return a;
        return nullptr;
    }

    std::shared_ptr<IoModuleState> EtherCatMasterService::GetIoModuleState(int slavePosition) const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        for (const auto& m : m_ioModules)
            if (m->SlavePosition == slavePosition) return m;
        return nullptr;
    }

    CyclicStats EtherCatMasterService::GetCyclicStats() { return m_bus->GetCyclicStats(); }

    void EtherCatMasterService::Log(const std::string& message)
    {
        OnLog.Raise(std::format("[{}] {}", base::NowString("HH:mm:ss.fff"), message));
    }

    void EtherCatMasterService::Dispose()
    {
        if (m_disposed) return;
        m_disposed = true;

        StopCyclicExchange();
        try { if (m_bus) m_bus->Close(); } catch (...) {}
    }
}
