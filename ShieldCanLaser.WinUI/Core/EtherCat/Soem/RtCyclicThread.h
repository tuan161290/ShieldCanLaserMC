#pragma once
// Luồng chu kỳ EtherCAT: một luồng native, mức ưu tiên TIME_CRITICAL, ghim vào core dành riêng
// (RtCpu.h), tự giữ nhịp bằng QueryPerformanceCounter.
//
// Thay cho timer multimedia (timeSetEvent) của bản C++/CLI. Timer multimedia chạy callback trên một
// luồng của winmm mà ta không điều khiển được: không ghim được core, chung CPU với UI/.NET/VisionMaster,
// và trên Windows 11 độ phân giải timer bị bỏ qua khi cửa sổ app bị che. Mỗi lần callback trễ vài ms
// là một lần driver thiếu khung PDO — nguồn gốc lỗi "PDI Watchdog Timeout".
//
// Giữ nhịp theo hai pha: ngủ bằng waitable timer độ phân giải cao tới sát hạn, rồi QUAY CHỜ
// (busy-wait) đoạn cuối `SpinUs` micro giây. SpinUs ≥ PeriodUs = không ngủ bao giờ: core dành riêng
// chạy 100%, và đó là MẶC ĐỊNH — đo trên i7-12700H, chu kỳ 4 ms, 14 luồng khác của tiến trình chạy
// hết tải (Tests\CoreTests\RtBench.cpp):
//
//   timer multimedia (bản cũ)          lệch ±1000…1500 µs, ~30% số chu kỳ lệch quá 500 µs
//   luồng RT ghim core, chỉ ngủ        trễ tới 7000 µs (timer của Windows đánh thức muộn)
//   luồng RT ghim core, quay chờ 1 ms  trễ tối đa 9…570 µs (core rảnh rơi vào trạng thái nghỉ sâu)
//   luồng RT ghim core, không ngủ      trễ tối đa 1…7 µs
//
// Giảm SpinUs chỉ để đổi lấy điện/nhiệt của một core, và chấp nhận đuôi trễ dài hơn.
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "EtherCat/Soem/RtCpu.h"

namespace ecm::rt
{
    struct RtOptions
    {
        /// <summary>Chu kỳ PDO, micro giây. 4000 = giá trị bản C# đã chạy trên máy thật.</summary>
        uint32_t PeriodUs{ 4000 };
        /// <summary>CPU logic dành cho luồng chu kỳ; -1 = tự chọn (xem ReserveCore).</summary>
        int CpuIndex{ -1 };
        /// <summary>false = vẫn TIME_CRITICAL nhưng không ghim, không dành riêng core.</summary>
        bool ReserveCore{ true };
        /// <summary>Độ dài đoạn quay chờ trước mỗi hạn, micro giây. Mặc định = cả chu kỳ (không ngủ).</summary>
        uint32_t SpinUs{ 4000 };
    };

    /// <summary>Số khoảng của biểu đồ độ trễ phát khung: &lt;50, &lt;100, &lt;250, &lt;500, &lt;1000, &lt;2000, ≥2000 µs.</summary>
    constexpr int LatenessBuckets = 7;
    extern const uint32_t LatenessBucketLimitUs[LatenessBuckets - 1];

    struct RtStats
    {
        uint64_t Cycles{};

        /// <summary>(khoảng cách hai lần bắt đầu chu kỳ liên tiếp) − chu kỳ đặt. Cùng định nghĩa với
        /// PeriodJitter của bản C++/CLI để so sánh được hai bản.</summary>
        double PeriodJitterMinUs{}, PeriodJitterMaxUs{}, PeriodJitterAvgUs{};

        /// <summary>Trễ so với hạn tuyệt đối (lúc bắt đầu chu kỳ − hạn). Không âm.</summary>
        double LatenessMaxUs{}, LatenessAvgUs{};
        uint64_t LatenessHistogram[LatenessBuckets]{};

        /// <summary>Thời gian một lần phát + nhận khung PDO.</summary>
        double ScanTimeMinUs{}, ScanTimeMaxUs{}, ScanTimeAvgUs{};

        /// <summary>Số lần trễ quá một chu kỳ (đã bỏ nhịp và đồng bộ lại, không phát dồn).</summary>
        uint64_t Overruns{};
        /// <summary>Số chu kỳ có working counter khác kỳ vọng (mất khung hoặc slave không trả lời).</summary>
        uint64_t WkcErrors{};
        int LastWkc{};
        int ExpectedWkc{};
        /// <summary>Số chu kỳ luồng bị bắt gặp chạy trên CPU khác CPU đã ghim. Phải bằng 0.</summary>
        uint64_t CpuMigrations{};

        int CpuIndex{ -1 };
        bool CoreReserved{};
        bool Pinned{};
        bool HighResTimer{};
        int ThreadPriority{};
        uint32_t PeriodUs{};
        uint32_t SpinUs{};
    };

    /// <summary>Việc của một chu kỳ, chạy trên luồng thời gian thực. Trả working counter (≤ 0 = lỗi).
    /// KHÔNG được cấp phát bộ nhớ, khoá mutex có thể bị luồng thường giữ, ghi log hay gọi SDO.</summary>
    using CycleFn = int (*)(void* user);

    class RtCyclicThread
    {
    public:
        RtCyclicThread() = default;
        ~RtCyclicThread();
        RtCyclicThread(const RtCyclicThread&) = delete;
        RtCyclicThread& operator=(const RtCyclicThread&) = delete;

        /// <summary>Khởi động. Chỉ trả về sau khi luồng đã ghim core và đặt xong mức ưu tiên;
        /// <paramref name="info"/> nhận một dòng mô tả cấu hình thực tế để ghi log.</summary>
        bool Start(const RtOptions& options, CycleFn cycle, void* user, int expectedWkc, std::string* info = nullptr);

        /// <summary>Dừng và CHỜ luồng thoát — sau khi hàm này trả về, callback chắc chắn không chạy nữa.</summary>
        void Stop();

        bool Running() const { return m_running.load(std::memory_order_acquire); }

        RtStats Stats() const;
        void ResetStats();

        /// <summary>Thời gian từ lần phát khung gần nhất, micro giây — để tầng trên phát hiện luồng chu kỳ bị treo.</summary>
        double LastCycleAgeUs() const;

    private:
        void ThreadMain();

        RtOptions m_options;
        CycleFn m_cycle{};
        void* m_user{};
        int m_expectedWkc{};

        std::thread m_thread;
        std::atomic<bool> m_stop{ false };
        std::atomic<bool> m_running{ false };
        std::atomic<bool> m_resetStats{ false };
        std::atomic<long long> m_lastCycleQpc{ 0 };

        // Bắt tay lúc khởi động
        void* m_readyEvent{};
        CoreReservation m_reservation;
        std::string m_info;

        // Thống kê: luồng chu kỳ là bên ghi DUY NHẤT và không bao giờ chờ (seqlock) — bên đọc đọc lại
        // nếu trúng lúc đang ghi.
        mutable std::atomic<uint32_t> m_statsSeq{ 0 };
        RtStats m_stats;
        double m_jitterSumUs{}, m_latenessSumUs{}, m_scanSumUs{};
        uint64_t m_jitterCount{};
    };
}
