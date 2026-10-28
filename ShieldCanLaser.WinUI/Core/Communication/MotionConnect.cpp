#include "Communication/MotionConnect.h"

#include <algorithm>
#include <format>

#include "Base/Text.h"
#include "Communication/EtherCat/EtherCatAxis.h"
#include "Communication/Sim/FakeAxis.h"
#include "Data/McDataManager.h"
#include "Infor/AlarmManager.h"
#include "Infor/AxisId.h"
#include "Utility/Log/LogWorker.h"

namespace scs
{
    MotionConnect::~MotionConnect()
    {
        DisposeBus();
    }

    std::shared_ptr<EtherCatBusManager> MotionConnect::Bus() const
    {
        std::lock_guard<std::mutex> lock(m_busMutex);
        return m_bus;
    }

    std::shared_ptr<IAxis> MotionConnect::Axis(std::string_view name) const
    {
        auto it = m_axes.find(std::string(name));
        return it != m_axes.end() ? it->second : nullptr;
    }

    void MotionConnect::Init(SystemData& sys)
    {
        m_axes.clear();
        m_simulation = sys.SimulationMode;
        m_zSafe = sys.ZSafe;
        bool simulation = sys.SimulationMode;

        std::shared_ptr<EtherCatBusManager> bus;
        {
            std::lock_guard<std::mutex> lock(m_busMutex);
            if (m_bus) m_bus->Dispose();
            m_bus = simulation ? nullptr : std::make_shared<EtherCatBusManager>([] { return McDataManager::SysData(); });
            bus = m_bus;
        }

        std::vector<std::shared_ptr<IAxis>> list;
        for (const char* name : AxisId::All)
        {
            auto cfg = sys.GetAxis(name);
            std::shared_ptr<IAxis> axis;
            if (simulation) axis = std::make_shared<FakeAxis>(cfg);
            else axis = std::make_shared<EtherCatAxis>(cfg, bus);
            m_axes[name] = axis;
            list.push_back(axis);
        }

        m_axisList = std::move(list);
    }

    bool MotionConnect::ConnectAll()
    {
        bool all = true;
        for (const auto& axis : m_axisList)
        {
            bool ok = false;
            try { ok = axis->Connect(); }
            catch (const std::exception& ex)
            {
                LogWorker::Instance().RecordLog(std::format("Axis {} connect error: {}", axis->Name(), ex.what()),
                    LogLevel::Error, axis->Name());
            }

            if (!ok)
            {
                auto bus = Bus();
                AlarmManager::RaiseAxis(axis->Name(), AxisFault::Disconnected, bus ? bus->LastError() : std::string{});
                all = false;
            }
        }
        return all;
    }

    void MotionConnect::DisconnectAll()
    {
        for (const auto& axis : m_axisList)
        {
            try { axis->Disconnect(); } catch (...) {}
        }
    }

    void MotionConnect::Refresh()
    {
        for (const auto& axis : m_axisList)
        {
            try
            {
                axis->Refresh();

                if (!axis->IsConnected()) AlarmManager::RaiseAxis(axis->Name(), AxisFault::Disconnected);
                else AlarmManager::ClearConditionAxis(axis->Name(), AxisFault::Disconnected);

                if (axis->Alarm()) AlarmManager::RaiseAxis(axis->Name(), AxisFault::DriverAlarm, std::format("0x{:04X}", axis->ErrorCode()));
                else AlarmManager::ClearConditionAxis(axis->Name(), AxisFault::DriverAlarm);

                bool fls = axis->FLS();
                if (fls || axis->RLS())
                    AlarmManager::RaiseAxis(axis->Name(), AxisFault::HardLimit, fls ? "POT" : "NOT");
                else
                    AlarmManager::ClearConditionAxis(axis->Name(), AxisFault::HardLimit);
            }
            catch (const std::exception& ex)
            {
                LogWorker::Instance().RecordLog(std::format("Axis {} scan error: {}", axis->Name(), ex.what()),
                    LogLevel::Error, axis->Name());
            }
        }
    }

    bool MotionConnect::Home(std::shared_ptr<IAxis> axis, std::stop_token ct)
    {
        {
            std::lock_guard<std::mutex> lock(m_homingMutex);
            m_homing.insert(axis->Name());
        }
        // finally { _homing.TryRemove(...) }
        struct Remove
        {
            MotionConnect& self;
            const std::string& name;
            ~Remove()
            {
                std::lock_guard<std::mutex> lock(self.m_homingMutex);
                self.m_homing.erase(name);
            }
        } remove{ *this, axis->Name() };

        return axis->Home(ct);
    }

    bool MotionConnect::IsHoming(std::string_view name) const
    {
        std::lock_guard<std::mutex> lock(m_homingMutex);
        return m_homing.find(name) != m_homing.end();
    }

    bool MotionConnect::AllConnected() const
    {
        return !m_axisList.empty() && std::all_of(m_axisList.begin(), m_axisList.end(), [](const auto& a) { return a->IsConnected(); });
    }

    bool MotionConnect::AllServoOn() const
    {
        return !m_axisList.empty() && std::all_of(m_axisList.begin(), m_axisList.end(), [](const auto& a) { return a->ServoOn(); });
    }

    bool MotionConnect::AllHomed() const
    {
        return !m_axisList.empty() && std::all_of(m_axisList.begin(), m_axisList.end(), [](const auto& a) { return a->OrgOk(); });
    }

    bool MotionConnect::AnyAlarm() const
    {
        return std::any_of(m_axisList.begin(), m_axisList.end(), [](const auto& a) { return a->Alarm(); });
    }

    bool MotionConnect::AnyBusy() const
    {
        return std::any_of(m_axisList.begin(), m_axisList.end(), [](const auto& a) { return a->Busy(); });
    }

    bool MotionConnect::ZSafeOk() const
    {
        if (!ZSafeInterlock()) return true;
        for (const char* name : AxisId::ZAxes)
        {
            auto z = Axis(name);
            if (z == nullptr || z->CurrentPos() > ZSafeLimitFor(name)) return false;
        }
        return true;
    }

    std::string MotionConnect::ZUnsafeText() const
    {
        std::vector<std::string> parts;
        for (const char* name : AxisId::ZAxes)
        {
            auto z = Axis(name);
            if (z == nullptr) parts.push_back(std::string(name) + " missing");
            else if (z->CurrentPos() > ZSafeLimitFor(name))
                parts.push_back(std::format("{}={:.2f} (need ≤ {:.2f})", name, z->CurrentPos(), ZSafeLimitFor(name)));
        }
        return base::Join(parts, ", ");
    }

    void MotionConnect::StopAll()
    {
        for (const auto& axis : m_axisList)
        {
            try { axis->Stop(); } catch (...) {}
        }
    }

    void MotionConnect::DisposeBus()
    {
        std::shared_ptr<EtherCatBusManager> bus;
        {
            std::lock_guard<std::mutex> lock(m_busMutex);
            bus = std::move(m_bus);
            m_bus = nullptr;
        }
        try { if (bus) bus->Dispose(); } catch (...) {}
    }
}
