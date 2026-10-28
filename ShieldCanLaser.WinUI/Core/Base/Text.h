#pragma once
// Chuỗi trong Core là std::string UTF-8 (file nguồn biên dịch với /utf-8). Chỉ đổi sang UTF-16 ở
// ranh giới Win32/WinRT, và đường dẫn file luôn đi qua std::filesystem::path — KHÔNG đưa thẳng
// std::string cho fopen/ifstream vì CRT hiểu chuỗi hẹp theo code page ANSI, vỡ tên tiếng Việt.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace base
{
    std::wstring Widen(std::string_view utf8);
    std::string  Narrow(std::wstring_view wide);

    inline std::filesystem::path U8Path(std::string_view utf8)
    {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    }

    inline std::string PathToU8(const std::filesystem::path& p)
    {
        auto s = p.u8string();
        return std::string(s.begin(), s.end());
    }

    std::string Trim(std::string_view s);
    std::string ToLower(std::string_view s);
    std::string ToUpper(std::string_view s);
    bool EqualsIgnoreCase(std::string_view a, std::string_view b);
    bool StartsWith(std::string_view s, std::string_view prefix);
    bool EndsWith(std::string_view s, std::string_view suffix);
    bool Contains(std::string_view s, std::string_view part);
    /// <summary>string.IsNullOrWhiteSpace.</summary>
    bool IsBlank(std::string_view s);

    std::vector<std::string> Split(std::string_view s, char sep, bool removeEmpty = false);
    std::string Join(const std::vector<std::string>& parts, std::string_view sep);
    std::string Replace(std::string s, std::string_view from, std::string_view to);

    /// <summary>Giờ máy theo mẫu kiểu .NET, hỗ trợ yyyy MM dd HH mm ss fff — đủ cho log và tên file
    /// (vd. "HH:mm:ss.fff", "yyyy-MM-dd", "yyyyMMdd_HHmmss").</summary>
    std::string NowString(std::string_view pattern = "HH:mm:ss.fff");

    /// <summary>Đọc/ghi cả file dạng UTF-8 (bỏ BOM khi đọc). Trả false nếu không mở được.</summary>
    bool ReadAllText(const std::filesystem::path& file, std::string& text);
    bool WriteAllText(const std::filesystem::path& file, std::string_view text);
}
