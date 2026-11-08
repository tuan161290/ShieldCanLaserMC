#pragma once
#include "Data/TrayCells.h"
#include "Infor/Enums.h"
#include "Utility/DateTimeText.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scs
{
    /// <summary>
    /// Tiến độ chu trình và bộ đếm sản xuất — lưu ở Runtime\running.json. Mỗi lần mở app tiến độ chu
    /// trình luôn bắt đầu lại từ đầu (<see cref="ResetCycleProgress"/>); bộ đếm, model gần nhất và các ô tray đã
    /// chạy được giữ (tray vẫn nằm trên máy). Ô đang chọn chạy lẻ không lưu: mở lại app là chưa chọn gì.
    /// </summary>
    struct RunningData
    {
        std::string RecentModelName;
        TimePoint SavedAt{};

        EAutoState AutoState = EAutoState::S00_Ready;

        /// <summary>Dừng giữa hành trình chứ không phải tại checkpoint — phải Init trước khi chạy tiếp.</summary>
        bool UnsafePause = false;

        // ── Tray (chu trình Auto) ──
        /// <summary>Ô đã chạy xong và ô chọn chạy lẻ của tray 1, tray 2. Ô đã xong GIỮ qua lần mở app; xoá khi người vận hành
        /// xác nhận đã thay tray (START lúc chờ thay tray, hoặc nút RESET TRAY trang Main).</summary>
        std::array<TrayCells, 2> Trays;
        /// <summary>Có ô chọn ở bất kỳ tray nào = chạy lẻ: chu trình CHỈ chạy các ô chọn, tray không chọn ô nào cũng không chạy
        /// (khắc laser không làm lại được, không đoán thay người vận hành).</summary>
        bool HasCellSelection() const { return Trays[0].HasSelection() || Trays[1].HasSelection(); }
        /// <summary>Kết quả 7 vị trí của hàng vừa xong (ECellState: Ok / Ng, Empty = ô không chạy lượt đó); tray và hàng của nó (1-based).</summary>
        std::vector<int> LastRowResult;
        int LastRowTray = 0;
        int LastRowIndex = 0;

        // ── Bộ đếm sản xuất ──
        int CountOk = 0;
        int CountNg = 0;
        int CountTotal = 0;
        int ConsecutiveNg = 0;

        // ── Cycle Time / Tack Time — như ShieldCanAssy.WinUI ──
        // Một chu kỳ = một tray, hoặc một lượt chạy lẻ. Đồng hồ thực, tính cả lúc chờ và STOP.
        int64_t CycleStartedAtMs = 0;     // 0 = không có chu kỳ đang chạy; không lưu
        int CycleOk = 0;                  // số can OK của chu kỳ đang chạy; không lưu
        double LastCycleSec = 0;          // chu kỳ xong gần nhất (s)
        int LastCycleProducts = 0;        // số can OK của chu kỳ đó
        bool CycleHoldLast = false;       // vừa xong chu kỳ: Cycle Time / Tack Time giữ số cuối tới khi chu kỳ mới bắt đầu

        static int64_t NowMs();
        /// <summary>Bắt đầu chu kỳ, đang chạy thì giữ nguyên giờ bắt đầu.</summary>
        void StartCycleClock(int64_t nowMs);
        /// <summary>Xong chu kỳ đang chạy, không có thì thôi.</summary>
        void FinishCycle(int64_t nowMs);
        double LastTackSec() const { return LastCycleProducts > 0 ? LastCycleSec / LastCycleProducts : 0; }

        double NgRatePercent() const { return CountTotal > 0 ? CountNg * 100.0 / CountTotal : 0; }

        void ResetCounters();

        /// <summary>Hai tray vừa thay: mọi ô chưa chạy. Ô chọn GIỮ — chọn trước lúc chờ thay tray là áp cho tray mới.</summary>
        void ResetTrays();

        /// <summary>Xoá tiến độ chu trình dở dang — gọi một lần ngay sau khi nạp running.json.</summary>
        void ResetCycleProgress();

        std::string ToJsonText() const;
        static std::shared_ptr<RunningData> FromJsonText(const std::string& text);
    };
}
