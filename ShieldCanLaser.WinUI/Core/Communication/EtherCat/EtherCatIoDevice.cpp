#include "Communication/EtherCat/EtherCatIoDevice.h"

#include <algorithm>
#include <format>

#include "Infor/AlarmManager.h"
#include "Utility/Log/LogWorker.h"

namespace scs
{
    namespace
    {
        using Lock = std::lock_guard<std::recursive_mutex>;

        std::vector<std::shared_ptr<ecm::IoSlotState>> SlotsInOrder(const ecm::IoModuleState& st)
        {
            auto slots = st.Slots;
            std::stable_sort(slots.begin(), slots.end(),
                [](const auto& a, const auto& b) { return a->SlotIndex < b->SlotIndex; });
            return slots;
        }

        std::shared_ptr<ecm::IoSlotState> FindSlot(const ecm::IoModuleState& st, int slot)
        {
            for (const auto& s : st.Slots)
                if (s && s->SlotIndex == slot) return s;
            return nullptr;
        }
    }

    EtherCatIoDevice::EtherCatIoDevice(std::function<std::shared_ptr<IoModuleConfig>()> config, std::shared_ptr<EtherCatBusManager> bus)
        : m_bus(std::move(bus)), m_config(std::move(config))
    {
    }

    int EtherCatIoDevice::SlavePosition() const
    {
        auto cfg = m_config ? m_config() : nullptr;
        return cfg ? cfg->SlavePosition : -1;
    }

    std::shared_ptr<ecm::IoModuleState> EtherCatIoDevice::State() const
    {
        auto service = m_bus->Service();
        return service ? service->GetIoModuleState(SlavePosition()) : nullptr;
    }

    int EtherCatIoDevice::MappedDiCount() const
    {
        Lock l(m_mapLock);
        return m_mapsBuilt ? static_cast<int>(m_diMap.size()) : 0;
    }

    int EtherCatIoDevice::MappedDoCount() const
    {
        Lock l(m_mapLock);
        return m_mapsBuilt ? static_cast<int>(m_doMap.size()) : 0;
    }

    std::vector<std::string> EtherCatIoDevice::ModuleNames() const
    {
        std::vector<std::string> names;
        auto st = State();
        if (st == nullptr) return names;
        for (const auto& s : SlotsInOrder(*st)) names.push_back(s->ModuleName);
        return names;
    }

    bool EtherCatIoDevice::Connect()
    {
        bool busOk = m_bus->EnsureConnected();
        int slave = SlavePosition();
        bool resultOk = false;
        for (const auto& r : m_bus->LastIoResults())
        {
            if (r.SlavePosition == slave) { resultOk = r.IsOk(); break; }
        }
        m_connected = busOk && resultOk && State() != nullptr;
        // Không tự raise alarm — SafetyMonitor raise IoDisconnected khi IsConnected = false.
        return m_connected.load();
    }

    void EtherCatIoDevice::Disconnect()
    {
        m_connected = false;
        m_bus->Disconnect();
    }

    bool EtherCatIoDevice::GetDi(int bit)
    {
        Lock l(m_lock);
        return bit >= 0 && bit < IoMap::DiCount && m_di[bit];
    }

    bool EtherCatIoDevice::TryMapDo(const ecm::IoModuleState& st, int bit, std::pair<int, int>& slotChannel)
    {
        Lock l(m_mapLock);
        EnsureBitMaps(st);
        if (!m_mapsBuilt || bit >= static_cast<int>(m_doMap.size())) return false;
        slotChannel = m_doMap[bit];
        return true;
    }

    bool EtherCatIoDevice::GetDo(int bit)
    {
        auto st = State();
        if (st == nullptr || bit < 0 || bit >= IoMap::DoCount) return false;
        std::pair<int, int> map;
        if (!TryMapDo(*st, bit, map)) return false;
        auto slotState = FindSlot(*st, map.first);
        return slotState != nullptr && ((slotState->OutputRegister.load() >> map.second) & 1u) != 0;
    }

    void EtherCatIoDevice::SetDo(int bit, bool value)
    {
        auto st = State();
        auto service = m_bus->Service();
        if (st == nullptr || bit < 0 || bit >= IoMap::DoCount || service == nullptr || !m_bus->IsConnected()) return;
        std::pair<int, int> map;
        if (!TryMapDo(*st, bit, map)) return;

        // Vòng quét gọi SetDo cho đèn tháp mỗi chu kỳ — chỉ ghi xuống bus khi bit thật sự đổi.
        if (GetDo(bit) == value) return;

        try
        {
            service->SetSlotOutput(SlavePosition(), map.first, map.second, value);
        }
        catch (const std::exception& ex)
        {
            LogWorker::Instance().RecordLog(std::format("[Remote IO] SetDo({}) error: {}", bit, ex.what()),
                LogLevel::Error, "EtherCAT");
            AlarmManager::RaiseEvent(AlarmCode::IoWriteFailed, "IO", IoMap::DoLabel(bit));
        }
    }

    void EtherCatIoDevice::Refresh()
    {
        auto st = State();
        m_connected = m_bus->IsConnected() && st != nullptr;
        if (st == nullptr) return;

        BitMap diMap;
        bool mapsBuilt;
        {
            Lock l(m_mapLock);
            EnsureBitMaps(*st);
            mapsBuilt = m_mapsBuilt;
            diMap = m_diMap;
        }

        {
            Lock l(m_lock);
            m_diPrev = m_di;
            if (mapsBuilt)
            {
                for (int i = 0; i < IoMap::DiCount; i++)
                {
                    if (i >= static_cast<int>(diMap.size())) { m_di[i] = false; continue; }
                    auto slotState = FindSlot(*st, diMap[i].first);
                    m_di[i] = slotState != nullptr && ((slotState->InputRegister.load() >> diMap[i].second) & 1u) != 0;
                }
            }
        }

        // 0xF100:01 Device status của bộ ghép — khác 0 là có module trong rack mất/lỗi.
        uint32_t deviceStatus = st->DeviceStatus.load();
        if (deviceStatus != 0)
            AlarmManager::Raise(AlarmCode::IoModuleFault, "IO", std::format("device status 0x{:08X}", deviceStatus));
        else
            AlarmManager::ClearCondition(AlarmCode::IoModuleFault);
    }

    void EtherCatIoDevice::EnsureBitMaps(const ecm::IoModuleState& st)
    {
        Lock l(m_mapLock);
        int slotCount = static_cast<int>(st.Slots.size());
        if (m_mappedSlotCount == slotCount) return;

        BitMap di, doMap;
        for (const auto& slot : SlotsInOrder(st))
        {
            for (int c = 0; c < slot->InputCount; c++) di.emplace_back(slot->SlotIndex, c);
            for (int c = 0; c < slot->OutputCount; c++) doMap.emplace_back(slot->SlotIndex, c);
        }
        m_diMap = std::move(di);
        m_doMap = std::move(doMap);
        m_mapsBuilt = true;
        m_mappedSlotCount = slotCount;

        if (static_cast<int>(m_diMap.size()) < IoMap::DiCount || static_cast<int>(m_doMap.size()) < IoMap::DoCount)
            LogWorker::Instance().RecordLog(
                std::format("Remote IO exposes {} DI / {} DO, IoMap requires {} DI / {} DO — check R1EC rack modules and ESI in DriverInfo",
                            m_diMap.size(), m_doMap.size(), IoMap::DiCount, IoMap::DoCount),
                LogLevel::Warning, "EtherCAT");
    }

    bool EtherCatIoDevice::RisingEdge(int bit)
    {
        Lock l(m_lock);
        return bit >= 0 && bit < IoMap::DiCount && m_di[bit] && !m_diPrev[bit];
    }

    bool EtherCatIoDevice::FallingEdge(int bit)
    {
        Lock l(m_lock);
        return bit >= 0 && bit < IoMap::DiCount && !m_di[bit] && m_diPrev[bit];
    }
}
