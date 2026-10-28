#pragma once
#include "Infor/Enums.h"
#include "Utility/DateTimeText.h"

#include <format>
#include <optional>
#include <string>

namespace scs
{
    /// <summary>Một alarm đang active hoặc một dòng lịch sử — PLAN.md §7.3. AlarmManager trả về
    /// BẢN CHỤP (copy) của kiểu này; sửa bản chụp không ảnh hưởng alarm trong lõi.</summary>
    struct AlarmData
    {
        int Code{ 0 };
        EAlarmLevel Level{ EAlarmLevel::Info };

        /// <summary>Nguồn phát: "Z", "Vision", "Safety", "System"...</summary>
        std::string Source;

        std::string MessageVi;
        std::string MessageEn;
        std::string Cause;
        std::string Remedy;
        std::string CauseEn;
        std::string RemedyEn;

        // ── Lời văn hiển thị: ưu tiên tiếng Anh, trống thì rơi về tiếng Việt ──
        const std::string& Message() const { return MessageEn.empty() ? MessageVi : MessageEn; }
        const std::string& CauseText() const { return CauseEn.empty() ? Cause : CauseEn; }
        const std::string& RemedyText() const { return RemedyEn.empty() ? Remedy : RemedyEn; }

        /// <summary>Dữ liệu động kèm theo: ErrorCode của driver, vị trí, score vision...</summary>
        std::string Detail;

        TimePoint RaisedAt{};
        std::optional<TimePoint> ClearedAt;

        /// <summary>Nguyên nhân vật lý còn tồn tại hay không. Do bên phát alarm cập nhật liên tục
        /// (SafetyMonitor quét mỗi 20 ms). RESET chỉ xoá được alarm có cờ này = false.</summary>
        bool ConditionActive{ true };

        /// <summary>Alarm đầu tiên của một cụm phát sinh trong vòng 200 ms — cho UI đưa alarm gốc
        /// lên đầu thay vì để operator đi sửa cái cuối cùng của chùm alarm phụ.</summary>
        bool IsRootCause{ false };

        std::string Display() const
        {
            return std::format("[{}] {}", Code, Message()) + (Detail.empty() ? "" : " — " + Detail);
        }

        std::string ToCsvLine() const;

        static constexpr const char* CsvHeader = "RaisedAt,ClearedAt,Code,Level,Source,RootCause,Message,Detail";
    };
}
