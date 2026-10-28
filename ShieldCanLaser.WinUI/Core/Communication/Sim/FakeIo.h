#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <set>

#include "Communication/IIoDevice.h"
#include "Infor/IoMap.h"

namespace scs
{
    /// <summary>
    /// Remote IO mô phỏng. Mặc định mọi tín hiệu an toàn ở trạng thái bình thường (NC = 1), nút bấm nhả, hai tray có mặt.
    /// Trang Manual có bảng công tắc để lật từng bit và bấm nút giả lập.
    /// </summary>
    class FakeIo : public IIoDevice
    {
    public:
        FakeIo();
        FakeIo(const FakeIo&) = delete;
        FakeIo& operator=(const FakeIo&) = delete;

        bool IsConnected() const override { return m_connected.load(); }

        bool Connect() override { m_connected = true; return true; }
        void Disconnect() override { m_connected = false; }

        bool GetDi(int bit) override;
        bool GetDo(int bit) override;
        void SetDo(int bit, bool value) override;

        /// <summary>Gọi ngay sau khi một ngõ ra ĐỔI trạng thái — chỗ móc mô phỏng cảm biến bám theo van. Gán TRƯỚC khi
        /// chia sẻ đối tượng cho luồng khác.</summary>
        std::function<void(int, bool)> AfterSetDo;

        void Refresh() override;

        bool RisingEdge(int bit) override;
        bool FallingEdge(int bit) override;

        // ── API riêng cho bảng mô phỏng trên trang Manual ──

        /// <summary>Nhấn-nhả một nút: bit lên 1 trong đúng một vòng quét.</summary>
        void SimPress(int diBit);

        /// <summary>Lật một DI giữ mức (EMG, khí, rèm, tray…).</summary>
        void SimToggle(int diBit);

        void SimSet(int diBit, bool value);

        /// <summary>Nozzle <paramref name="nozzle"/> (1…7) hút trượt: bật van mà cảm biến chân không không lên. Mặc định
        /// cảm biến bám theo van của nó — mô phỏng lần gắp nào cũng dính can.</summary>
        void SimSetPickFail(int nozzle, bool fail);

        /// <summary>Số lần DO kích laser đã lên 1 — bài tự kiểm đếm số lần khắc.</summary>
        int LaserPulseCount() const { return m_laserPulses.load(); }

    private:
        std::array<bool, IoMap::DiCount> m_di{};
        std::array<bool, IoMap::DiCount> m_diPrev{};
        std::array<bool, IoMap::DoCount> m_do{};
        std::set<int> m_pendingPress;
        std::array<bool, IoMap::NozzleCount + 1> m_pickFail{};
        std::atomic<int> m_laserPulses{ 0 };
        mutable std::recursive_mutex m_lock;
        std::atomic<bool> m_connected{ false };
    };
}
