#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "Communication/EtherCat/EtherCatBusManager.h"
#include "Communication/IAxis.h"
#include "Data/SystemData.h"

namespace scs
{
    /// <summary>
    /// Quản 3 trục servo EtherCAT: tạo, kết nối, quét trạng thái, và cung cấp các điều kiện tổng hợp
    /// (đã về gốc chưa, Z có an toàn không) cho tầng trên. Sở hữu luôn <see cref="Bus"/> — bus
    /// EtherCAT dùng chung cho 3 trục VÀ Remote IO.
    /// </summary>
    class MotionConnect
    {
    public:
        MotionConnect() = default;
        ~MotionConnect();
        MotionConnect(const MotionConnect&) = delete;
        MotionConnect& operator=(const MotionConnect&) = delete;

        /// <summary>Chỉ thay đổi trong <see cref="Init"/> (lúc khởi động) — không gọi Init khi luồng khác
        /// còn đang duyệt hai danh sách này.</summary>
        const std::map<std::string, std::shared_ptr<IAxis>>& Axes() const { return m_axes; }

        /// <summary>Danh sách theo thứ tự cố định của <c>AxisId::All</c>.</summary>
        const std::vector<std::shared_ptr<IAxis>>& AxisList() const { return m_axisList; }

        bool Simulation() const { return m_simulation.load(); }

        /// <summary>Bus EtherCAT — nullptr khi chạy mô phỏng (FakeAxis/FakeIo).</summary>
        std::shared_ptr<EtherCatBusManager> Bus() const;

        /// <summary>Trục theo tên; nullptr nếu không có.</summary>
        std::shared_ptr<IAxis> Axis(std::string_view name) const;

        /// <summary>
        /// Dựng 3 trục. <c>SimulationMode</c> → <c>FakeAxis</c>; không thì <c>EtherCatAxis</c>
        /// trên một <c>EtherCatBusManager</c> chung.
        ///
        /// <para>Tạo được lớp thật KHÔNG có nghĩa là kết nối được — Connect vẫn có thể thất bại vì chưa
        /// cắm cáp, sai card mạng, thiếu Npcap. Đó là đường xử lý bình thường: trục báo mất kết nối, alarm
        /// nổi lên, máy không cho chạy. Không có đường nào lặng lẽ rơi về trục mô phỏng khi
        /// <c>SimulationMode = false</c>: trên màn hình hai thứ đó trông giống hệt nhau.</para>
        /// </summary>
        void Init(SystemData& sys);

        bool ConnectAll();
        void DisconnectAll();

        /// <summary>Một vòng quét: cập nhật trạng thái mọi trục và soi các điều kiện phát alarm.</summary>
        void Refresh();

        /// <summary>Về gốc một trục qua đây (không gọi thẳng <c>axis->Home</c>) để vòng quét biết trục
        /// đó đang về gốc.</summary>
        bool Home(std::shared_ptr<IAxis> axis, std::stop_token ct);

        bool IsHoming(std::string_view name) const;

        bool AllConnected() const;
        bool AllServoOn() const;
        bool AllHomed() const;
        bool AnyAlarm() const;
        bool AnyBusy() const;

        /// <summary>
        /// Chốt chặn chống va: X, Y chỉ được chạy khi Z ở trên cao độ an toàn (nếu bật <see cref="ZSafeInterlock"/>).
        /// Quy ước như ShieldCanAssy: gốc Z ở phía TRÊN, chiều dương của Z là ĐI XUỐNG — nên "an toàn" (đã rút lên) là
        /// vị trí NHỎ, ≤ ngưỡng. Chưa xác nhận trên máy Laser: đổi chiều Z thì phải đổi phép so sánh này.
        /// </summary>
        bool ZSafeOk() const;

        /// <summary>Các trục Z đang dưới ngưỡng an toàn, dạng "Z=12.00 (cần ≤ 11.00), Z3=4.20 (cần ≤ 0.70)" —
        /// rỗng khi đạt. Cho thông báo lý do bị chặn.</summary>
        std::string ZUnsafeText() const;

        /// <summary>Bật/tắt chốt chặn Z_SAFE — <c>SystemData::ZSafeInterlock</c>.</summary>
        bool ZSafeInterlock() const { return m_zSafeInterlock.load(); }
        void SetZSafeInterlock(bool v) { m_zSafeInterlock = v; }

        /// <summary>Dung sai của chốt chặn trên Z_SAFE (mm): Z dừng đúng Z_SAFE, dừng lệch vài micromet quanh
        /// đó không được làm chốt chặn báo sai.</summary>
        static constexpr double ZSafeMargin = 1.0;

        /// <summary>Ngưỡng thật của chốt chặn: X/Y chỉ chạy khi Z ≤ giá trị này.</summary>
        double ZSafeLimit() const { return m_zSafe.load() + ZSafeMargin; }

        double ZSafe() const { return m_zSafe.load(); }
        void SetZSafe(double v) { m_zSafe = v; }

        /// <summary>Cao độ an toàn (đích của EnsureZSafe) và ngưỡng chốt chặn của một trục Z cụ thể — máy này chỉ có một Z.</summary>
        double ZSafeFor(std::string_view) const { return ZSafe(); }
        double ZSafeLimitFor(std::string_view) const { return ZSafeLimit(); }

        void StopAll();

        /// <summary>Đóng bus EtherCAT khi tắt app.</summary>
        void DisposeBus();

    private:
        std::map<std::string, std::shared_ptr<IAxis>> m_axes;
        std::vector<std::shared_ptr<IAxis>> m_axisList;
        std::atomic<double> m_zSafe{ 0.0 };
        std::atomic<bool> m_zSafeInterlock{ true };
        std::atomic<bool> m_simulation{ false };

        mutable std::mutex m_busMutex;       // m_bus
        std::shared_ptr<EtherCatBusManager> m_bus;

        /// <summary>Trục đang về gốc — xem <see cref="Home"/>.</summary>
        mutable std::mutex m_homingMutex;
        std::set<std::string, std::less<>> m_homing;
    };
}
