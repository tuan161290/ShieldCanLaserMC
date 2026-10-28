#pragma once
// Định dạng / đọc thời gian theo kiểu .NET cho log, CSV và JSON (Newtonsoft ISO 8601).
#include <chrono>
#include <string>
#include <string_view>

namespace scs
{
    using TimePoint = std::chrono::system_clock::time_point;

    /// <summary>Giờ ĐỊA PHƯƠNG tách thành từng phần.</summary>
    struct LocalParts
    {
        int Year{ 1 }, Month{ 1 }, Day{ 1 }, Hour{ 0 }, Minute{ 0 }, Second{ 0 };
        /// <summary>Phần lẻ của giây, đơn vị 100 ns (0..9 999 999) — cùng độ phân giải DateTime.Ticks.</summary>
        int Ticks{ 0 };
        /// <summary>Lệch so với UTC, phút (+420 cho UTC+7).</summary>
        int OffsetMinutes{ 0 };
    };

    LocalParts ToLocalParts(TimePoint t);

    /// <summary>DateTime.ToString(pattern) cho giờ địa phương; hỗ trợ yyyy MM dd HH mm ss fff.</summary>
    std::string FormatLocal(TimePoint t, std::string_view pattern);

    /// <summary>Số nguyên yyyyMMdd của ngày địa phương — so "cùng ngày" (DateTime.Date).</summary>
    int LocalDateKey(TimePoint t);

    /// <summary>
    /// Chuỗi ISO như Newtonsoft ghi DateTime.Now: "2026-10-06T14:23:45.1234567+07:00" (bỏ số 0 thừa
    /// ở phần lẻ). TimePoint{} đứng vai DateTime.MinValue và ghi ra "0001-01-01T00:00:00".
    /// </summary>
    std::string ToIsoString(TimePoint t);

    /// <summary>
    /// Đọc "yyyy-MM-dd", "yyyy-MM-dd HH:mm[:ss[.f…]]" hoặc ISO "yyyy-MM-ddTHH:mm:ss[.f…][Z|±HH:mm]".
    /// Không có múi giờ thì hiểu là giờ địa phương. Năm trước 1970 (DateTime.MinValue) → TimePoint{}.
    /// </summary>
    bool TryParseDateTime(std::string_view text, TimePoint& t);
}
