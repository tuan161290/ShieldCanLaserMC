#include "Communication/EtherCat/NativeBusFactory.h"

#include <exception>

#include "EtherCat/Soem/SoemEtherCatBus.h"

namespace scs
{
    bool NativeBusFactory::IsAvailable()
    {
        return ecm::SoemEtherCatBus::IsNpcapAvailable();
    }

    bool NativeBusFactory::TryCreateSoemBus(std::shared_ptr<ecm::IEtherCatBus>& bus, std::string& error)
    {
        bus = nullptr;
        error.clear();

        // Hay gặp nhất: Npcap chưa cài (wpcap.dll/Packet.dll).
        std::string npcapError;
        if (!ecm::SoemEtherCatBus::IsNpcapAvailable(&npcapError))
        {
            error = npcapError.empty()
                ? "Npcap not installed (cannot load wpcap.dll/Packet.dll) — install Npcap, then restart app."
                : npcapError;
            return false;
        }

        try
        {
            bus = std::make_shared<ecm::SoemEtherCatBus>();
            return true;
        }
        catch (const std::exception& ex)
        {
            // Chọn phần cứng thật — KHÔNG âm thầm rơi về mô phỏng: báo lỗi, bus để trống.
            bus = nullptr;
            error = std::string("SOEM bus creation failed: ") + ex.what();
            return false;
        }
    }
}
