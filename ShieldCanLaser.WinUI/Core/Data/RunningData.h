#pragma once
#include "Infor/Enums.h"
#include "Utility/DateTimeText.h"

#include <memory>
#include <string>
#include <vector>

namespace scs
{
    /// <summary>
    /// Tiến độ chu trình và bộ đếm sản xuất — lưu ở Runtime\running.json. Mỗi lần mở app tiến độ chu
    /// trình luôn bắt đầu lại từ đầu (<see cref="ResetCycleProgress"/>); bộ đếm, model gần nhất và số hàng tray đã
    /// chạy được giữ (tray vẫn nằm trên máy).
    /// </summary>
    struct RunningData
    {
        std::string RecentModelName;
        TimePoint SavedAt{};

        EAutoState AutoState = EAutoState::S00_Ready;

        /// <summary>Dừng giữa hành trình chứ không phải tại checkpoint — phải Init trước khi chạy tiếp.</summary>
        bool UnsafePause = false;

        // ── Tray (chu trình Auto) ──
        /// <summary>Số hàng đã chạy xong của tray 1, tray 2 (hàng kế tiếp = số này, 0-based) — GIỮ qua lần mở app; về 0 khi
        /// người vận hành xác nhận đã thay tray (START lúc chờ thay tray, hoặc nút RESET TRAY trang Main).</summary>
        int TrayRowsDone[2] = { 0, 0 };
        /// <summary>Kết quả 7 vị trí của hàng vừa xong (ECellState: Ok / Ng / Empty); tray và hàng của nó (1-based).</summary>
        std::vector<int> LastRowResult;
        int LastRowTray = 0;
        int LastRowIndex = 0;

        // ── Bộ đếm sản xuất ──
        int CountOk = 0;
        int CountNg = 0;
        int CountTotal = 0;
        int ConsecutiveNg = 0;

        /// <summary>Số sản phẩm gần nhất lấy trung bình cycle time.</summary>
        static constexpr int CycleAvgWindow = 25;

        double LastProductTimeSec = 0;             // cycle time của sản phẩm vừa xong (s/sp)
        std::vector<double> RecentProductTimes;    // tối đa CycleAvgWindow cái gần nhất
        double AvgProductTimeSec = 0;              // trung bình của RecentProductTimes
        double LastCycleSec = 0;                   // thời gian cả lượt (một hàng) vừa xong (s)

        /// <summary>Ghi một lượt chạy xong sec giây ra products sản phẩm.</summary>
        void RecordCycle(double sec, int products);

        double NgRatePercent() const { return CountTotal > 0 ? CountNg * 100.0 / CountTotal : 0; }

        void ResetCounters();

        /// <summary>Hai tray vừa thay: mọi hàng chưa chạy.</summary>
        void ResetTrays();

        /// <summary>Xoá tiến độ chu trình dở dang — gọi một lần ngay sau khi nạp running.json.</summary>
        void ResetCycleProgress();

        std::string ToJsonText() const;
        static std::shared_ptr<RunningData> FromJsonText(const std::string& text);
    };
}
