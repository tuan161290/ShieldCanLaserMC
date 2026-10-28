#include "EtherCat/Transport/SimulatedEtherCatBus.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>

namespace ecm
{
    namespace
    {
        using Sw = CiA402Registers::Statusword;
        using Cw = CiA402Registers::Controlword;
        using Lock = std::lock_guard<std::mutex>;

        /// (int)Math.Round(x): làm tròn nửa về chẵn; kẹp vào miền int để tránh UB khi tràn.
        int RoundToInt(double v)
        {
            double r = std::nearbyint(v);
            if (!(r > -2147483648.0)) return INT32_MIN;
            if (r >= 2147483647.0) return INT32_MAX;
            return static_cast<int>(r);
        }

        int Sign(double v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }
    }

    SimulatedEtherCatBus::~SimulatedEtherCatBus() { Close(); }

    void SimulatedEtherCatBus::SetSimulateInputs(int slavePosition, uint64_t bits) { Lock l(m_mutex); m_simulateInputs[slavePosition] = bits; }

    void SimulatedEtherCatBus::SetSimulatedIoLayout(std::vector<SimIoSlot> slots)
    {
        Lock l(m_mutex);
        m_ioLayout = std::move(slots);
    }
    void SimulatedEtherCatBus::RemoveSimulateInputs(int slavePosition) { Lock l(m_mutex); m_simulateInputs.erase(slavePosition); }
    std::map<int, uint64_t> SimulatedEtherCatBus::SimulateInputs() const { Lock l(m_mutex); return m_simulateInputs; }

    void SimulatedEtherCatBus::SetSimulateMissingAt(int slavePosition, bool missing)
    {
        Lock l(m_mutex);
        if (missing) m_simulateMissingAt.insert(slavePosition);
        else m_simulateMissingAt.erase(slavePosition);
    }
    std::set<int> SimulatedEtherCatBus::SimulateMissingAt() const { Lock l(m_mutex); return m_simulateMissingAt; }

    void SimulatedEtherCatBus::SetSimulatedPresence(int slavePosition, const DiscoveredSlaveInfo& info) { Lock l(m_mutex); m_simulatedPresence[slavePosition] = info; }
    void SimulatedEtherCatBus::RemoveSimulatedPresence(int slavePosition) { Lock l(m_mutex); m_simulatedPresence.erase(slavePosition); }
    std::map<int, DiscoveredSlaveInfo> SimulatedEtherCatBus::SimulatedPresence() const { Lock l(m_mutex); return m_simulatedPresence; }

    void SimulatedEtherCatBus::RestartClockLocked()
    {
        m_clockRunning = true;
        m_clockStart = std::chrono::steady_clock::now();
        m_lastCycleSeconds = 0;
    }

    double SimulatedEtherCatBus::ElapsedSecondsLocked() const
    {
        if (!m_clockRunning) return 0;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_clockStart).count();
    }

    void SimulatedEtherCatBus::CloseLocked()
    {
        m_slaves.clear();
        m_ioSlaves.clear();
        m_slaveCount = 0;
        m_clockRunning = false;
    }

    bool SimulatedEtherCatBus::Open(const std::string& /*networkAdapter*/, int expectedSlaveCount)
    {
        Lock l(m_mutex);
        m_slaves.clear();
        for (int i = 0; i < expectedSlaveCount; i++)
            m_slaves[i] = SlaveSim();
        m_slaveCount = expectedSlaveCount;
        RestartClockLocked();
        return true;
    }

    void SimulatedEtherCatBus::Close()
    {
        Lock l(m_mutex);
        CloseLocked();
    }

    int SimulatedEtherCatBus::SlaveCount() const { Lock l(m_mutex); return m_slaveCount; }

    std::vector<DiscoveredSlaveInfo> SimulatedEtherCatBus::ScanSlaves(const std::string& /*networkAdapter*/)
    {
        Lock l(m_mutex);
        std::vector<DiscoveredSlaveInfo> list;   // std::map đã sắp theo vị trí (OrderBy key)
        for (const auto& kv : m_simulatedPresence)
            list.push_back(kv.second);
        return list;
    }

    std::vector<AxisConnectResult> SimulatedEtherCatBus::ConnectAllAxis(const std::string& /*networkAdapter*/,
                                                                        const std::vector<AxisConfig>& axes,
                                                                        const std::vector<IoModuleConfig>& ioModules,
                                                                        std::vector<AxisConnectResult>& ioResults)
    {
        Lock l(m_mutex);
        std::vector<AxisConnectResult> results;
        ioResults.clear();
        bool allOk = true;

        for (const auto& axis : axes)
        {
            AxisConnectResult result;
            result.SlavePosition = axis.SlavePosition;
            result.AxisChannel = axis.AxisChannel;
            result.AxisName = axis.Name;

            if (m_simulateMissingAt.count(axis.SlavePosition) != 0)
            {
                result.Status = AxisConnectStatus::NoResponse;
                result.Message = std::format("[Simulated] No slave found at position {}", axis.SlavePosition);
                allOk = false;
            }
            else
            {
                // The simulator has no real slave identity to disagree with — it always matches
                // whatever driver the slot was configured with (SimulateMissingAt is the only way
                // to exercise a non-Ok result without real hardware).
                result.ActualVendorId = axis.ExpectedVendorId;
                result.ActualProductCode = axis.ExpectedProductCode;
                result.ActualRevision = axis.ExpectedRevision;
                result.Status = AxisConnectStatus::Ok;
                result.Message = "OK (simulated)";
            }
            results.push_back(std::move(result));
        }

        for (const auto& io : ioModules)
        {
            AxisConnectResult result;
            result.SlavePosition = io.SlavePosition;
            result.AxisName = io.Name;

            if (m_simulateMissingAt.count(io.SlavePosition) != 0)
            {
                result.Status = AxisConnectStatus::NoResponse;
                result.Message = std::format("[Simulated] No slave found at position {}", io.SlavePosition);
                allOk = false;
            }
            else
            {
                result.ActualVendorId = io.ExpectedVendorId;
                result.ActualProductCode = io.ExpectedProductCode;
                result.ActualRevision = io.ExpectedRevision;
                result.Status = AxisConnectStatus::Ok;
                result.Message = "OK (simulated)";
                // Simulated rack shape (no real PDO to discover) — SetSimulatedIoLayout, 8 DI / 8 DO by default.
                for (const auto& slot : m_ioLayout)
                {
                    result.InputBitCount += slot.InputBits;
                    result.OutputBitCount += slot.OutputBits;
                }
            }
            ioResults.push_back(std::move(result));
        }

        // Build slaves at their actual SlavePosition (not necessarily contiguous from 0 — some
        // slots can be left Unconfigured and are absent from `axes`), unlike Open(). Mỗi TRỤC một bộ mô
        // phỏng theo địa chỉ trục (slave + kênh); hai trục của một driver hai trục vẫn chỉ tính MỘT slave.
        CloseLocked();
        if (allOk)
        {
            std::set<int> slaves;
            for (const auto& axis : axes)
            {
                m_slaves[axis.Address()] = SlaveSim();
                slaves.insert(axis.SlavePosition);
            }
            for (const auto& io : ioModules)
                m_ioSlaves[io.SlavePosition] = IoSim();
            m_slaveCount = static_cast<int>(slaves.size() + ioModules.size());
            RestartClockLocked();
        }
        return results;
    }

    void SimulatedEtherCatBus::CyclicExchange()
    {
        Lock l(m_mutex);
        double now = ElapsedSecondsLocked();
        double dt = std::max(0.0, now - m_lastCycleSeconds);
        m_lastCycleSeconds = now;

        for (auto& kv : m_slaves)
            StepSlave(kv.second, dt);
    }

    /// <summary>Advance one simulated slave by dt seconds: CiA402 state machine + pp-mode motion.</summary>
    void SimulatedEtherCatBus::StepSlave(SlaveSim& s, double dt)
    {
        bool faultReset = (s.Controlword & Cw::FaultReset) != 0;
        bool enableVoltage = (s.Controlword & Cw::EnableVoltage) != 0;
        bool quickStop = (s.Controlword & Cw::QuickStop) != 0;
        bool switchOn = (s.Controlword & Cw::SwitchOn) != 0;
        bool enableOp = (s.Controlword & Cw::EnableOperation) != 0;
        bool wasFault = (s.Statusword & Sw::Fault) != 0;

        if (wasFault)
        {
            if (faultReset) s.Statusword = Sw::SwitchOnDisabled;
        }
        else if (!enableVoltage)
            s.Statusword = Sw::SwitchOnDisabled;
        else if (!quickStop)
            s.Statusword = static_cast<uint16_t>(Sw::ReadyToSwitchOn | Sw::VoltageEnabled);
        else if (!switchOn)
            s.Statusword = static_cast<uint16_t>(Sw::ReadyToSwitchOn | Sw::VoltageEnabled | Sw::QuickStop);
        else if (!enableOp)
            s.Statusword = static_cast<uint16_t>(Sw::ReadyToSwitchOn | Sw::SwitchedOn | Sw::VoltageEnabled | Sw::QuickStop);
        else
            s.Statusword = static_cast<uint16_t>(Sw::ReadyToSwitchOn | Sw::SwitchedOn | Sw::OperationEnabled |
                                                 Sw::VoltageEnabled | Sw::QuickStop);

        bool operationEnabled = (s.Statusword & Sw::OperationEnabled) != 0;
        bool halt = (s.Controlword & Cw::Halt) != 0;

        if (operationEnabled && s.Mode == static_cast<int8_t>(OperationMode::ProfilePosition))
        {
            if (halt)
            {
                // EtherCatMasterService.Stop() sets this bit to decelerate smoothly on the real
                // drive's own ramp. The simulator has no ramp to model, so it just freezes motion
                // immediately — good enough to verify a caller's Stop() actually halts a move.
                s.ActualVelocity = 0;
            }
            else
            {
                double error = s.TargetPosition - s.ActualPosition;
                double maxStep = s.ProfileVelocity * dt;

                if (std::fabs(error) <= maxStep || maxStep <= 0)
                {
                    s.ActualPosition = s.TargetPosition;
                    s.ActualVelocity = 0;
                    s.Statusword |= static_cast<uint16_t>(Sw::TargetReached | Sw::SetpointAckOrHomingAttained);
                }
                else
                {
                    s.ActualPosition += Sign(error) * maxStep;
                    s.ActualVelocity = Sign(error) * static_cast<double>(s.ProfileVelocity);
                    s.Statusword &= static_cast<uint16_t>(~Sw::TargetReached);
                }
            }
        }
        else if (operationEnabled && s.Mode == static_cast<int8_t>(OperationMode::Homing) && !halt)
        {
            // Không có công tắc home/index thật để mô phỏng tìm gốc — coi homing "đạt" ngay tại vị
            // trí hiện tại. Đủ để bên gọi kiểm được luồng StartHome → chờ SetpointAckOrHomingAttained
            // → OrgOk và huỷ giữa chừng (Stop() bật lại `halt`, chặn nhánh này).
            //
            // CHỈ set bit12 (SetpointAckOrHomingAttained) — KHÔNG set bit10 (TargetReached): đó là
            // bit RIÊNG của Profile Position. Set nhầm ở đây thì nó rò sang lượt MoveAbsolute NGAY
            // SAU — WaitForMotionComplete() đọc TargetReached dư từ Homing TRƯỚC KHI CyclicExchange
            // kịp chạy một vòng ProfilePosition để xoá nó, nên trả "đã xong" tức thời dù trục còn
            // cách đích rất xa. Bug thật, bắt được qua --selftest.
            s.ActualVelocity = 0;
            s.Statusword |= static_cast<uint16_t>(Sw::SetpointAckOrHomingAttained);
        }
    }

    void SimulatedEtherCatBus::WriteSdo(int, uint16_t, uint8_t, int, uint8_t) { /* no-op: SDO config not modeled by the simulator */ }
    int SimulatedEtherCatBus::ReadSdo(int, uint16_t, uint8_t, uint8_t) { return 0; }

    bool SimulatedEtherCatBus::TryReadSdo(int slavePosition, uint16_t index, uint8_t subIndex, uint8_t sizeBytes, int& value)
    {
        value = ReadSdo(slavePosition, index, subIndex, sizeBytes);
        return true;
    }

    void SimulatedEtherCatBus::SetControlword(int slavePosition, uint16_t controlword) { Lock l(m_mutex); Get(slavePosition).Controlword = controlword; }
    void SimulatedEtherCatBus::SetModeOfOperation(int slavePosition, int8_t mode) { Lock l(m_mutex); Get(slavePosition).Mode = mode; }
    void SimulatedEtherCatBus::SetTargetPosition(int slavePosition, int counts) { Lock l(m_mutex); Get(slavePosition).TargetPosition = counts; }
    void SimulatedEtherCatBus::SetProfileVelocity(int slavePosition, uint32_t countsPerSec) { Lock l(m_mutex); Get(slavePosition).ProfileVelocity = std::max<uint32_t>(1, countsPerSec); }
    void SimulatedEtherCatBus::SetProfileAcceleration(int slavePosition, uint32_t countsPerSec2) { Lock l(m_mutex); Get(slavePosition).ProfileAcceleration = countsPerSec2; }
    void SimulatedEtherCatBus::SetProfileDeceleration(int slavePosition, uint32_t countsPerSec2) { Lock l(m_mutex); Get(slavePosition).ProfileDeceleration = countsPerSec2; }

    uint16_t SimulatedEtherCatBus::GetStatusword(int slavePosition) { Lock l(m_mutex); return Get(slavePosition).Statusword; }
    int8_t SimulatedEtherCatBus::GetModeOfOperationDisplay(int slavePosition) { Lock l(m_mutex); return Get(slavePosition).Mode; }
    int SimulatedEtherCatBus::GetActualPosition(int slavePosition) { Lock l(m_mutex); return RoundToInt(Get(slavePosition).ActualPosition); }
    int SimulatedEtherCatBus::GetActualVelocity(int slavePosition) { Lock l(m_mutex); return RoundToInt(Get(slavePosition).ActualVelocity); }
    int16_t SimulatedEtherCatBus::GetActualTorque(int) { return 0; }
    uint16_t SimulatedEtherCatBus::GetErrorCode(int) { return 0; }

    uint64_t SimulatedEtherCatBus::GetDigitalInputs(int slavePosition)
    {
        Lock l(m_mutex);
        auto it = m_simulateInputs.find(slavePosition);
        return it != m_simulateInputs.end() ? it->second : 0;
    }

    void SimulatedEtherCatBus::SetDigitalOutputs(int slavePosition, uint64_t mask) { Lock l(m_mutex); GetIo(slavePosition).Outputs = mask; }

    /// <summary>No real coupler to diagnose in the simulator — always 0 (OK).</summary>
    uint32_t SimulatedEtherCatBus::GetIoModuleDeviceStatus(int) { return 0; }

    /// <summary>The simulator models I/O modules as flat DI/DO bit counts, with no modular-coupler
    /// rack behind them, so there are no module idents to report — always empty.</summary>
    std::vector<uint32_t> SimulatedEtherCatBus::GetDetectedModuleIdents(int) { return {}; }

    // ── Per-module IORegister — a connected I/O module simulates the rack set by SetSimulatedIoLayout. Each slot
    // is a window onto the same flat DI/DO bits GetDigitalInputs/SetDigitalOutputs fake: slot k's channels start
    // after all channels of slots 0…k-1.
    namespace
    {
        uint64_t LowBits(int count) { return count >= 64 ? ~0ULL : ((1ULL << count) - 1); }
    }

    int SimulatedEtherCatBus::GetIoSlotCount(int slavePosition)
    {
        Lock l(m_mutex);
        return m_ioSlaves.count(slavePosition) != 0 ? static_cast<int>(m_ioLayout.size()) : 0;
    }

    uint32_t SimulatedEtherCatBus::GetIoSlotIdent(int, int) { return 0; }

    int SimulatedEtherCatBus::GetIoSlotInputBitCount(int slavePosition, int slot)
    {
        Lock l(m_mutex);
        bool ok = m_ioSlaves.count(slavePosition) != 0 && slot >= 0 && slot < static_cast<int>(m_ioLayout.size());
        return ok ? m_ioLayout[slot].InputBits : 0;
    }

    int SimulatedEtherCatBus::GetIoSlotOutputBitCount(int slavePosition, int slot)
    {
        Lock l(m_mutex);
        bool ok = m_ioSlaves.count(slavePosition) != 0 && slot >= 0 && slot < static_cast<int>(m_ioLayout.size());
        return ok ? m_ioLayout[slot].OutputBits : 0;
    }

    uint32_t SimulatedEtherCatBus::GetIoSlotInputRegister(int slavePosition, int slot)
    {
        Lock l(m_mutex);
        if (slot < 0 || slot >= static_cast<int>(m_ioLayout.size())) return 0;
        int offset = 0;
        for (int i = 0; i < slot; ++i) offset += m_ioLayout[i].InputBits;
        auto it = m_simulateInputs.find(slavePosition);
        uint64_t inputs = it != m_simulateInputs.end() ? it->second : 0;
        return offset >= 64 ? 0 : static_cast<uint32_t>((inputs >> offset) & LowBits(m_ioLayout[slot].InputBits));
    }

    void SimulatedEtherCatBus::SetIoSlotOutputRegister(int slavePosition, int slot, uint32_t value)
    {
        Lock l(m_mutex);
        if (slot < 0 || slot >= static_cast<int>(m_ioLayout.size())) return;
        int offset = 0;
        for (int i = 0; i < slot; ++i) offset += m_ioLayout[i].OutputBits;
        if (offset >= 64) return;
        uint64_t mask = LowBits(m_ioLayout[slot].OutputBits) << offset;
        IoSim& io = GetIo(slavePosition);
        io.Outputs = (io.Outputs & ~mask) | ((static_cast<uint64_t>(value) << offset) & mask);
    }

    /// <summary>No real native timer or EtherCAT wire in the simulator — always zeroed
    /// (SampleCount 0), which the UI treats as "n/a".</summary>
    CyclicStats SimulatedEtherCatBus::GetCyclicStats() { return CyclicStats(); }

    /// <summary>No-op: the simulator takes its rack shape from SetSimulatedIoLayout rather than deriving
    /// widths from an ESI.</summary>
    void SimulatedEtherCatBus::SetModuleCatalog(const std::vector<EsiModuleInfo>&) {}
    void SimulatedEtherCatBus::SetSyncManagerCatalog(const std::vector<EsiSyncManagerInfo>&) {}

    uint64_t SimulatedEtherCatBus::GetSimulatedOutputs(int slavePosition) const
    {
        Lock l(m_mutex);
        auto it = m_ioSlaves.find(slavePosition);
        return it != m_ioSlaves.end() ? it->second.Outputs : 0;
    }

    SimulatedEtherCatBus::SlaveSim& SimulatedEtherCatBus::Get(int slavePosition)
    {
        auto it = m_slaves.find(slavePosition);
        if (it == m_slaves.end())
            throw std::out_of_range(std::format("No simulated slave at position {}", AxisAddress::Text(slavePosition)));
        return it->second;
    }

    SimulatedEtherCatBus::IoSim& SimulatedEtherCatBus::GetIo(int slavePosition)
    {
        auto it = m_ioSlaves.find(slavePosition);
        if (it == m_ioSlaves.end())
            throw std::out_of_range(std::format("No simulated I/O module at position {}", slavePosition));
        return it->second;
    }
}
