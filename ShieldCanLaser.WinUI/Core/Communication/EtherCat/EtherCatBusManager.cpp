#include "Communication/EtherCat/EtherCatBusManager.h"

#include <algorithm>
#include <format>
#include <stdexcept>

#include "Base/Text.h"
#include "Communication/EtherCat/NativeBusFactory.h"
#include "EtherCat/Esi/EsiCatalog.h"
#include "EtherCat/Soem/SoemEtherCatBus.h"
#include "EtherCat/Transport/SimulatedEtherCatBus.h"
#include "Infor/AlarmManager.h"
#include "Infor/IoMap.h"
#include "SysConfig/AppPaths.h"
#include "Utility/Log/LogWorker.h"

namespace scs
{
    namespace
    {
        using Gate = std::lock_guard<std::recursive_mutex>;

        // ── Catalog ESI của bộ ghép modular (R1EC) ──
        // Coupler không tự khai được PDO mapping của từng module cắm vào rack (PdoUpload="false"),
        // nên native phải được cấp trước bề rộng bit + SyncManager của từng loại module đọc từ ESI —
        // thiếu bước này ConnectAllAxis vẫn báo OK giả trong khi coupler không lộ ra I/O nào (Partron
        // đã gặp thật 2026-08-20). Nạp MỘT LẦN cho tiến trình.
        struct EsiCache
        {
            std::vector<ecm::EsiModuleInfo> Modules;
            std::vector<ecm::EsiSyncManagerInfo> SyncManagers;
        };

        const EsiCache& EnsureEsiCatalogLoaded()
        {
            static const EsiCache cache = [] {
                EsiCache c;
                std::filesystem::path dir = EtherCatBusManager::DriverInfoDir();
                std::error_code ec;
                if (std::filesystem::is_directory(dir, ec))
                {
                    std::string folder = base::PathToU8(dir);
                    c.Modules = ecm::EsiCatalog::LoadModulesFromFolder(folder);
                    c.SyncManagers = ecm::EsiCatalog::LoadSyncManagersFromFolder(folder);
                }

                if (c.Modules.empty())
                    LogWorker::Instance().RecordLog(
                        "DriverInfo\\*.xml not found next to exe — Remote IO R1EC will expose no I/O on Connect All Axis "
                        "(ESI required for module bit widths).", LogLevel::Warning, "EtherCAT");
                return c;
            }();
            return cache;
        }
    }

    EtherCatBusManager::EtherCatBusManager(std::function<std::shared_ptr<SystemData>()> sys)
        : m_sys(std::move(sys))
    {
        auto data = m_sys();
        std::string adapter = data->NetworkAdapter;
        int ioSlave = data->IoModule ? data->IoModule->SlavePosition : -1;

        if (TryResolveBus(adapter, ioSlave, m_bus, m_busResolutionError))
        {
            AttachBus();
            m_busDescription = adapter == SimulatedAdapterSentinel
                ? "Simulated bus (SimulatedEtherCatBus) — no hardware access"
                : "SOEM — " + adapter;
        }
        else
        {
            m_bus = nullptr;
            m_busDescription = "BUS OPEN FAILED: " + m_busResolutionError;
        }
    }

    EtherCatBusManager::EtherCatBusManager(std::function<std::shared_ptr<SystemData>()> sys,
                                           std::shared_ptr<ecm::IEtherCatBus> bus, std::string busDescription)
        : m_sys(std::move(sys)), m_bus(std::move(bus)), m_busDescription(std::move(busDescription))
    {
        if (m_bus)
        {
            AttachBus();
        }
        else
        {
            m_busResolutionError = "no bus";
            m_busDescription = "BUS OPEN FAILED: " + m_busResolutionError;
        }
    }

    void EtherCatBusManager::AttachBus()
    {
        m_soem = std::dynamic_pointer_cast<ecm::SoemEtherCatBus>(m_bus);
        if (m_soem)
            m_soem->OnLog += [](const std::string& msg) { LogWorker::Instance().RecordLog(msg, LogLevel::Info, "EtherCAT"); };

        m_service = std::make_shared<ecm::EtherCatMasterService>(m_bus);
        m_service->OnLog += [](std::string msg) { LogWorker::Instance().RecordLog(msg, LogLevel::Info, "EtherCAT"); };
    }

    EtherCatBusManager::~EtherCatBusManager()
    {
        try { Dispose(); } catch (...) {}
    }

    std::string EtherCatBusManager::LastError() const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_lastError;
    }

    void EtherCatBusManager::SetLastError(const std::string& error)
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_lastError = error;
    }

    std::vector<ecm::AxisConnectResult> EtherCatBusManager::LastAxisResults() const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_lastAxisResults;
    }

    std::vector<ecm::AxisConnectResult> EtherCatBusManager::LastIoResults() const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_lastIoResults;
    }

    int EtherCatBusManager::ExpectedSlaveCount(const SystemData& sys)
    {
        // Đếm SLAVE, không đếm trục: hai trục chung một driver hai trục (cùng SlavePosition) là một slave.
        std::vector<int> slaves;
        for (const auto& a : sys.Axes)
            if (a && a->IsConfigured() && std::find(slaves.begin(), slaves.end(), a->SlavePosition) == slaves.end())
                slaves.push_back(a->SlavePosition);
        return (int)slaves.size() + (sys.IoModule != nullptr && sys.IoModule->IsConfigured() ? 1 : 0);
    }

    bool EtherCatBusManager::EnsureConnected()
    {
        Gate gate(m_gate);

        if (m_service == nullptr)
        {
            SetLastError(m_busResolutionError);
            AlarmManager::Raise(AlarmCode::EtherCatBusUnavailable, "EtherCAT", m_busResolutionError);
            return false;
        }
        AlarmManager::ClearCondition(AlarmCode::EtherCatBusUnavailable);
        if (m_service->IsConnected()) return true;
        if (m_attempted) return false;
        m_attempted = true;
        m_lastFoundSlaveCount = -1;

        auto sys = m_sys();
        ecm::EtherCatConfig config = BuildConfig(*sys);
        auto axisConfigured = [](const ecm::AxisConfig& a) { return a.IsConfigured(); };
        auto ioConfigured = [](const ecm::IoModuleConfig& m) { return m.IsConfigured(); };
        if (std::none_of(config.Axes.begin(), config.Axes.end(), axisConfigured) &&
            std::none_of(config.IoModules.begin(), config.IoModules.end(), ioConfigured))
        {
            std::string error = "No slot assigned a driver — EtherCAT page: Scan, then press \"APPLY SCAN\"";
            SetLastError(error);
            AlarmManager::RaiseEvent(AlarmCode::EtherCatSlotInvalid, "EtherCAT", error);
            return false;
        }

        ApplyRealtimeOptions(*sys);

        std::vector<ecm::AxisConnectResult> ioResults;
        std::vector<ecm::AxisConnectResult> axisResults = m_service->ConnectAllAxis(config, ioResults);
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_lastAxisResults = axisResults;
            m_lastIoResults = ioResults;
        }

        if (m_service->IsConnected())
        {
            // ConnectAllAxis chỉ xét các vị trí đã cấu hình: slave THỪA phía sau (gắn thêm driver, nối nhầm
            // chuỗi) vẫn lọt. Slave THIẾU thì thường đã rơi vào NoResponse/WrongDriver ở trên — đếm thêm
            // ở đây để báo rõ một con số.
            int found = m_bus->SlaveCount();
            int expected = ExpectedSlaveCount(*sys);
            m_lastFoundSlaveCount = found;
            if (found != expected)
            {
                m_service->Disconnect();
                std::string error = std::format(
                    "EtherCAT network has {} slaves, config requires {} (axes + Remote IO with driver) — "
                    "check driver cabling/power, or fix config on EtherCAT page", found, expected);
                SetLastError(error);
                LogWorker::Instance().RecordLog("Connect All Axis failed: " + error, LogLevel::Error, "EtherCAT");
                AlarmManager::RaiseEvent(AlarmCode::EtherCatSlotInvalid, "EtherCAT", error);
                return false;
            }

            ++m_epoch;
            SetLastError({});
            LogWorker::Instance().RecordLog(std::format("EtherCAT bus open: {} axes, {} Remote IO, {} slaves on network — all OK",
                axisResults.size(), ioResults.size(), found), LogLevel::Info, "EtherCAT");
            return true;
        }

        std::vector<std::string> bad;
        for (const auto& r : axisResults) if (!r.IsOk()) bad.push_back(r.ToString());
        for (const auto& r : ioResults) if (!r.IsOk()) bad.push_back(r.ToString());
        std::string error = !bad.empty()
            ? base::Join(bad, "; ")
            : "ConnectAllAxis could not open bus (see EtherCAT log)";
        SetLastError(error);
        LogWorker::Instance().RecordLog("Connect All Axis failed: " + error, LogLevel::Error, "EtherCAT");
        // Sự kiện: lần Connect All Axis sau ở trang EtherCAT mới là nơi xác nhận lại, RESET phải gỡ được.
        AlarmManager::RaiseEvent(AlarmCode::EtherCatSlotInvalid, "EtherCAT", error);
        return false;
    }

    bool EtherCatBusManager::Reconnect()
    {
        Gate gate(m_gate);
        if (m_service != nullptr && m_service->IsConnected()) m_service->Disconnect();
        m_attempted = false;
        return EnsureConnected();
    }

    void EtherCatBusManager::Disconnect()
    {
        Gate gate(m_gate);
        if (m_service != nullptr && m_service->IsConnected()) m_service->Disconnect();
    }

    std::vector<ecm::DiscoveredSlaveInfo> EtherCatBusManager::ScanSlaves()
    {
        if (m_service == nullptr) return {};
        if (m_service->IsConnected())
            throw std::runtime_error("Bus is open — disconnect before Scan");
        auto sys = m_sys();
        ApplyRealtimeOptions(*sys);
        return m_service->ScanSlaves(sys->NetworkAdapter);
    }

    ecm::CyclicStats EtherCatBusManager::CyclicStats() const
    {
        return m_service != nullptr ? m_service->GetCyclicStats() : ecm::CyclicStats{};
    }

    ecm::rt::RtStats EtherCatBusManager::RtStats() const
    {
        return m_soem != nullptr ? m_soem->GetRtStats() : ecm::rt::RtStats{};
    }

    std::string EtherCatBusManager::RealtimeInfo() const
    {
        return m_soem != nullptr ? m_soem->RealtimeInfo() : std::string{};
    }

    void EtherCatBusManager::ApplyRealtimeOptions(const SystemData& sys)
    {
        if (m_soem == nullptr) return;

        ecm::rt::RtOptions options;
        int cycleUs = std::clamp(sys.RtCycleUs, 500, 20000);
        options.PeriodUs = static_cast<uint32_t>(cycleUs);
        options.CpuIndex = sys.RtCpuIndex;
        options.ReserveCore = sys.RtReserveCore;
        // RtSpinUs < 0 = quay chờ trọn chu kỳ (không ngủ) — mặc định của luồng thời gian thực.
        options.SpinUs = static_cast<uint32_t>(sys.RtSpinUs < 0 ? cycleUs : sys.RtSpinUs);
        m_soem->SetRealtimeOptions(options);
    }

    bool EtherCatBusManager::TryResolveBus(const std::string& networkAdapter, int ioSlave,
                                           std::shared_ptr<ecm::IEtherCatBus>& bus, std::string& error)
    {
        bus = nullptr;
        error.clear();

        if (networkAdapter == SimulatedAdapterSentinel)
        {
            // Bus mô phỏng đọc mọi DI = 0 — với tín hiệu an toàn đấu NC thì đó là EMG đang nhấn, không thao
            // tác được gì. Đặt sẵn các ngõ vào NC ở trạng thái bình thường (= 1) cho Remote IO.
            auto sim = std::make_shared<ecm::SimulatedEtherCatBus>();
            // Rack như máy thật (danh sách IO 07/10): module 32 DI rồi module 32 DO trên R1EC.
            sim->SetSimulatedIoLayout({ { IoMap::DiCount, 0 }, { 0, IoMap::DoCount } });
            if (ioSlave >= 0)
                sim->SetSimulateInputs(ioSlave,
                    (1ULL << IoMap::EmgOk) | (1ULL << IoMap::LightCurtainOk));
            bus = sim;
            return true;
        }

        if (base::IsBlank(networkAdapter))
        {
            error = "No EtherCAT network adapter selected (EtherCAT page)";
            return false;
        }

        // Chọn phần cứng thật — KHÔNG âm thầm rơi về mô phỏng nếu bus thật không dựng được (thiếu Npcap).
        return NativeBusFactory::TryCreateSoemBus(bus, error);
    }

    std::filesystem::path EtherCatBusManager::DriverInfoDir()
    {
        return AppPaths::ExeDir() / "DriverInfo";
    }

    ecm::EtherCatConfig EtherCatBusManager::BuildConfig(const SystemData& sys)
    {
        const EsiCache& esi = EnsureEsiCatalogLoaded();

        ecm::EtherCatConfig cfg;
        cfg.NetworkAdapter = sys.NetworkAdapter;
        cfg.ModuleCatalog = esi.Modules;
        cfg.SyncManagerCatalog = esi.SyncManagers;

        for (const auto& a : sys.Axes)
        {
            if (!a) continue;
            bool writeLimit = a->WriteSoftLimitToDrive && !a->WrapDegrees;
            ecm::AxisConfig e;
            e.Name = a->Name;
            e.SlavePosition = a->SlavePosition;
            e.AxisChannel = a->AxisChannel;
            e.CountsPerUnit = a->CountsPerUnit > 1e-9 ? a->CountsPerUnit : 1.0;
            e.HomingMethod = a->HomingMethod;
            e.HomingVelocity = a->HomingVelocity;
            e.DefaultVelocity = a->MoveSpeed;
            e.DefaultAccelTimeS = a->AccelTimeS;
            e.DefaultDecelTimeS = a->DecelTimeS;
            e.SoftwareLimitNegativeUnits = writeLimit ? std::optional<double>(a->SoftLimitNeg) : std::nullopt;
            e.SoftwareLimitPositiveUnits = writeLimit ? std::optional<double>(a->SoftLimitPos) : std::nullopt;
            e.TravelPerRotationUnits = a->TravelPerRotation;
            e.Unit = a->Unit == "deg" ? ecm::AxisUnit::degree : ecm::AxisUnit::mm;
            e.ExpectedVendorId = a->ExpectedVendorId;
            e.ExpectedProductCode = a->ExpectedProductCode;
            e.ExpectedRevision = a->ExpectedRevision;
            e.ExpectedDriverName = a->ExpectedDriverName;
            cfg.Axes.push_back(std::move(e));
        }

        if (const auto& io = sys.IoModule)
        {
            ecm::IoModuleConfig m;
            m.Name = io->Name;
            m.SlavePosition = io->SlavePosition;
            m.ExpectedVendorId = io->ExpectedVendorId;
            m.ExpectedProductCode = io->ExpectedProductCode;
            m.ExpectedRevision = io->ExpectedRevision;
            m.ExpectedDriverName = io->ExpectedDriverName;
            cfg.IoModules.push_back(std::move(m));
        }
        return cfg;
    }

    void EtherCatBusManager::Dispose()
    {
        if (m_service != nullptr) m_service->Dispose();
    }
}
