#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Communication/EtherCat/EtherCatBusManager.h"
#include "Communication/IIoDevice.h"
#include "Communication/IoModuleConfig.h"
#include "EtherCat/Models/IoModuleState.h"
#include "Infor/IoMap.h"

namespace scs
{
    /// <summary>
    /// Remote IO EtherCAT thật — Leadshine R1EC + module 32 DI (X000…X031) + module 32 DO (Y000…Y031), bọc
    /// <c>EtherCatBusManager::Service()</c>. Port từ Partron CAM TEST.
    ///
    /// <para><see cref="Refresh"/> đọc thanh ghi 32-bit RIÊNG từng module qua <c>IoModuleState::Slots</c>
    /// (không phải <c>Inputs</c>/<c>Outputs</c> gộp cả rack) rồi nối các slot theo thứ tự SlotIndex
    /// thành dải bit liên tục của <c>IoMap</c> — module đầu chiếm bit thấp. Thêm/bớt module
    /// trong rack thì dải bit tự dịch theo; sơ đồ IoMap phải khớp đúng thứ tự module cắm thật.</para>
    /// </summary>
    class EtherCatIoDevice : public IIoDevice
    {
    public:
        EtherCatIoDevice(std::function<std::shared_ptr<IoModuleConfig>()> config, std::shared_ptr<EtherCatBusManager> bus);
        EtherCatIoDevice(const EtherCatIoDevice&) = delete;
        EtherCatIoDevice& operator=(const EtherCatIoDevice&) = delete;

        bool IsConnected() const override { return m_connected.load(); }

        /// <summary>Số DI/DO thật sự lộ ra từ rack (tổng kênh các module) — trang Setting hiển thị để đối
        /// chiếu với cấu hình rack.</summary>
        int MappedDiCount() const;
        int MappedDoCount() const;

        /// <summary>Tên các module trong rack theo thứ tự slot, vd. "SC-3200-1(32 channels MIL DI Module)".</summary>
        std::vector<std::string> ModuleNames() const;

        bool Connect() override;
        void Disconnect() override;

        bool GetDi(int bit) override;
        bool GetDo(int bit) override;
        void SetDo(int bit, bool value) override;
        void Refresh() override;
        bool RisingEdge(int bit) override;
        bool FallingEdge(int bit) override;

    private:
        using BitMap = std::vector<std::pair<int, int>>;   // (slot, kênh trong thanh ghi riêng của slot)

        int SlavePosition() const;
        std::shared_ptr<ecm::IoModuleState> State() const;

        /// <summary>Dựng bảng tra bit-toàn-cục → (slot, kênh) một lần cho mỗi hình dạng rack — nối kênh của
        /// từng slot theo thứ tự SlotIndex tăng dần.</summary>
        void EnsureBitMaps(const ecm::IoModuleState& st);
        /// <summary>Tra (slot, kênh) của một DO; false nếu bit nằm ngoài số kênh rack lộ ra.</summary>
        bool TryMapDo(const ecm::IoModuleState& st, int bit, std::pair<int, int>& slotChannel);

        std::shared_ptr<EtherCatBusManager> m_bus;
        std::function<std::shared_ptr<IoModuleConfig>()> m_config;

        std::array<bool, IoMap::DiCount> m_di{};
        std::array<bool, IoMap::DiCount> m_diPrev{};
        mutable std::recursive_mutex m_lock;      // m_di, m_diPrev

        // Bit DI/DO toàn cục (chỉ số IoMap) → (slot, kênh). Dựng lại khi số slot đổi — chỉ xảy ra lúc mới
        // kết nối/kết nối lại. Có khoá riêng vì GetDo/SetDo (UI, vòng điều khiển) và Refresh (vòng quét)
        // cùng có thể dựng bảng.
        mutable std::recursive_mutex m_mapLock;
        BitMap m_diMap;
        BitMap m_doMap;
        bool m_mapsBuilt{ false };
        int m_mappedSlotCount{ -1 };

        std::atomic<bool> m_connected{ false };
    };
}
