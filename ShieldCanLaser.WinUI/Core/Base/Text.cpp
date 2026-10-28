#include "Base/Text.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace base
{
    std::wstring Widen(std::string_view utf8)
    {
        if (utf8.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
        return w;
    }

    std::string Narrow(std::wstring_view wide)
    {
        if (wide.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
        std::string s((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), s.data(), n, nullptr, nullptr);
        return s;
    }

    static bool IsSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    }

    std::string Trim(std::string_view s)
    {
        size_t b = 0, e = s.size();
        while (b < e && IsSpace(s[b])) ++b;
        while (e > b && IsSpace(s[e - 1])) --e;
        return std::string(s.substr(b, e - b));
    }

    // Chỉ đổi hoa/thường phần ASCII: đủ cho so khớp tên file, khoá cấu hình, mã hex. Chữ có dấu
    // tiếng Việt giữ nguyên byte UTF-8.
    std::string ToLower(std::string_view s)
    {
        std::string r(s);
        for (char& c : r) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return r;
    }

    std::string ToUpper(std::string_view s)
    {
        std::string r(s);
        for (char& c : r) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        return r;
    }

    bool EqualsIgnoreCase(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() && ToLower(a) == ToLower(b);
    }

    bool StartsWith(std::string_view s, std::string_view prefix)
    {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    bool EndsWith(std::string_view s, std::string_view suffix)
    {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool Contains(std::string_view s, std::string_view part)
    {
        return s.find(part) != std::string_view::npos;
    }

    bool IsBlank(std::string_view s)
    {
        return std::all_of(s.begin(), s.end(), IsSpace);
    }

    std::vector<std::string> Split(std::string_view s, char sep, bool removeEmpty)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true)
        {
            size_t pos = s.find(sep, start);
            std::string_view piece = s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start);
            if (!removeEmpty || !piece.empty()) parts.emplace_back(piece);
            if (pos == std::string_view::npos) break;
            start = pos + 1;
        }
        return parts;
    }

    std::string Join(const std::vector<std::string>& parts, std::string_view sep)
    {
        std::string r;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0) r += sep;
            r += parts[i];
        }
        return r;
    }

    std::string Replace(std::string s, std::string_view from, std::string_view to)
    {
        if (from.empty()) return s;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos)
        {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
        return s;
    }

    std::string NowString(std::string_view pattern)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);

        std::string out;
        char buf[8];
        for (size_t i = 0; i < pattern.size();)
        {
            auto take = [&](std::string_view token) {
                if (pattern.compare(i, token.size(), token) != 0) return false;
                i += token.size();
                return true;
            };
            if (take("yyyy"))     { snprintf(buf, sizeof(buf), "%04d", t.wYear);         out += buf; }
            else if (take("fff")) { snprintf(buf, sizeof(buf), "%03d", t.wMilliseconds); out += buf; }
            else if (take("MM"))  { snprintf(buf, sizeof(buf), "%02d", t.wMonth);        out += buf; }
            else if (take("dd"))  { snprintf(buf, sizeof(buf), "%02d", t.wDay);          out += buf; }
            else if (take("HH"))  { snprintf(buf, sizeof(buf), "%02d", t.wHour);         out += buf; }
            else if (take("mm"))  { snprintf(buf, sizeof(buf), "%02d", t.wMinute);       out += buf; }
            else if (take("ss"))  { snprintf(buf, sizeof(buf), "%02d", t.wSecond);       out += buf; }
            else out += pattern[i++];
        }
        return out;
    }

    bool ReadAllText(const std::filesystem::path& file, std::string& text)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
            text.erase(0, 3);
        return true;
    }

    bool WriteAllText(const std::filesystem::path& file, std::string_view text)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), (std::streamsize)text.size());
        return out.good();
    }
}
