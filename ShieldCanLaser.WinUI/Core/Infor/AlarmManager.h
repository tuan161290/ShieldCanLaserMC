#pragma once
#include "Base/Event.h"
#include "Infor/AlarmCode.h"
#include "Infor/AlarmData.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace scs
{
    /// <summary>
    /// Quản lý alarm — PLAN.md §7.
    ///
    /// <para><b>Hai cờ tách biệt.</b> Một alarm active có <c>ConditionActive</c> (nguyên nhân vật
    /// lý còn hay hết) và bản thân việc nó còn nằm trong danh sách (đã chốt/latched). Bên phát
    /// alarm gọi <see cref="Raise"/> liên tục khi điều kiện còn đúng và <see cref="ClearCondition"/>
    /// khi hết. RESET chỉ gỡ được alarm đã hết nguyên nhân — đúng quy tắc "không tự động xoá" ở
    /// §7.4, và cũng là lý do bấm RESET lúc EMG còn nhấn thì không có gì xảy ra.</para>
    ///
    /// <para>Mọi hàm an toàn đa luồng và trả BẢN CHỤP. UI poll <see cref="Version"/> rồi mới lấy lại
    /// <see cref="ActiveAlarms"/>/<see cref="History"/>.</para>
    /// </summary>
    class AlarmManager
    {
    public:
        /// <summary>Alarm đang active, theo thứ tự phát.</summary>
        static std::vector<AlarmData> ActiveAlarms();

        /// <summary>Lịch sử phiên chạy hiện tại (cũ nhất đứng đầu, tối đa MaxHistory; đã ghi CSV song song).</summary>
        static std::vector<AlarmData> History();

        /// <summary>Tăng mỗi khi danh sách active/lịch sử hoặc một trường của alarm (ConditionActive,
        /// Detail, ClearedAt) đổi. Raise lặp lại không đổi gì thì không tăng.</summary>
        static uint64_t Version();

        static int  MaxHistory();
        static void SetMaxHistory(int n);

        /// <summary>Bắn khi có alarm MỚI (không bắn khi alarm cũ được raise lại). Chạy trên luồng phát.</summary>
        static base::Event<AlarmData>& AlarmRaised();

        static bool HasFatal();
        static bool HasError();     // Error trở lên
        static bool HasWarning();
        static int  ActiveCount();

        /// <summary>Alarm gốc của cụm đang active — cái UI cần hiện to nhất.</summary>
        static std::optional<AlarmData> RootCause();

        /// <summary>Phát alarm. Gọi lại nhiều lần với cùng mã là an toàn — chỉ làm mới ConditionActive
        /// (và Detail nếu có), không nhân bản dòng hay spam log. source rỗng → suy từ mã.</summary>
        static AlarmData Raise(int code, std::string_view source = {}, std::string_view detail = {});

        static AlarmData RaiseAxis(std::string_view axisName, AxisFault fault, std::string_view detail = {});

        /// <summary>
        /// Phát alarm cho một SỰ KIỆN đã xảy ra xong (một lần thử Home/Move/SVO ON thất bại) — khác
        /// <see cref="Raise"/> dùng cho ĐIỀU KIỆN đang tiếp diễn mà một vòng quét liên tục Raise lại
        /// và ClearCondition lúc hết. Sự kiện không có gì để theo dõi tiếp: nếu Raise trơn thì
        /// ConditionActive mãi là true, Reset không bao giờ gỡ được và HasError khoá SafeToMove CẢ
        /// MÁY vĩnh viễn (bug thật 2026-09-09: Z chạm hard limit lúc về gốc → HomeTimeout kẹt → Jog
        /// rời hard limit cũng bị chặn). Đánh dấu ConditionActive=false NGAY sau Raise là an toàn:
        /// trạng thái an toàn THẬT đã có cờ riêng theo dõi (OrgOk, ServoOn, axis.Alarm...).
        /// </summary>
        static AlarmData RaiseEvent(int code, std::string_view source = {}, std::string_view detail = {});

        static AlarmData RaiseAxisEvent(std::string_view axisName, AxisFault fault, std::string_view detail = {});

        /// <summary>Báo nguyên nhân đã hết. Alarm vẫn nằm lại chờ RESET, ở MỌI mức — kể cả Warning
        /// (chốt 2026-09-26). Warning mà máy tự chạy tiếp thì dùng <see cref="Dismiss"/>.</summary>
        static void ClearCondition(int code);

        /// <summary>Gỡ hẳn alarm khỏi danh sách, không chờ RESET — cho Warning mà máy tự chạy tiếp
        /// được (chốt 2026-09-29). Vẫn còn trong log, alarm.csv và lịch sử.</summary>
        static void Dismiss(int code);

        /// <summary>Alarm đã hết nguyên nhân mà operator chưa bấm RESET xác nhận (mức cao nhất, cũ
        /// nhất). McControl chặn vào chạy khi còn loại này. Alarm CÒN nguyên nhân không tính.</summary>
        static std::optional<AlarmData> PendingReset();

        static void ClearConditionAxis(std::string_view axisName, AxisFault fault);

        static bool IsActive(int code);

        /// <summary>Lấy alarm đang active theo mã (đọc thẳng từ lõi).</summary>
        static std::optional<AlarmData> Get(int code);

        /// <summary>Operator bấm RESET. Gỡ mọi alarm đã hết nguyên nhân; trả về danh sách alarm KHÔNG
        /// gỡ được để UI nói rõ vì sao thay vì im lặng không phản ứng (§7.4).</summary>
        static std::vector<AlarmData> Reset();

        /// <summary>Xoá sạch không điều kiện. Chỉ dùng khi khởi tạo lại hệ thống hoặc trong test.</summary>
        static void ForceClearAll();

        /// <summary>Ghi nốt phần CSV còn trong hàng đợi. Gọi lúc thoát app.</summary>
        static void FlushCsv();
    };
}
