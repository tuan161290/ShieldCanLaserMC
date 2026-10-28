#include "Utility/DateTimeText.h"

#include "Base/Text.h"

#include <cstdio>
#include <ctime>

namespace scs
{
    using namespace std::chrono;

    LocalParts ToLocalParts(TimePoint t)
    {
        LocalParts p;
        auto secs = floor<seconds>(t);
        std::time_t tt = system_clock::to_time_t(secs);
        std::tm tm{};
        if (localtime_s(&tm, &tt) != 0) return p;

        p.Year = tm.tm_year + 1900;
        p.Month = tm.tm_mon + 1;
        p.Day = tm.tm_mday;
        p.Hour = tm.tm_hour;
        p.Minute = tm.tm_min;
        p.Second = tm.tm_sec;
        p.Ticks = (int)(duration_cast<nanoseconds>(t - secs).count() / 100);

        std::tm asUtc = tm;
        std::time_t local = _mkgmtime(&asUtc);
        p.OffsetMinutes = (int)((local - tt) / 60);
        return p;
    }

    std::string FormatLocal(TimePoint t, std::string_view pattern)
    {
        LocalParts p = ToLocalParts(t);
        std::string out;
        char buf[16];
        for (size_t i = 0; i < pattern.size();)
        {
            auto take = [&](std::string_view token) {
                if (pattern.compare(i, token.size(), token) != 0) return false;
                i += token.size();
                return true;
            };
            if (take("yyyy"))     { snprintf(buf, sizeof(buf), "%04d", p.Year);          out += buf; }
            else if (take("fff")) { snprintf(buf, sizeof(buf), "%03d", p.Ticks / 10000); out += buf; }
            else if (take("MM"))  { snprintf(buf, sizeof(buf), "%02d", p.Month);         out += buf; }
            else if (take("dd"))  { snprintf(buf, sizeof(buf), "%02d", p.Day);           out += buf; }
            else if (take("HH"))  { snprintf(buf, sizeof(buf), "%02d", p.Hour);          out += buf; }
            else if (take("mm"))  { snprintf(buf, sizeof(buf), "%02d", p.Minute);        out += buf; }
            else if (take("ss"))  { snprintf(buf, sizeof(buf), "%02d", p.Second);        out += buf; }
            else out += pattern[i++];
        }
        return out;
    }

    int LocalDateKey(TimePoint t)
    {
        LocalParts p = ToLocalParts(t);
        return p.Year * 10000 + p.Month * 100 + p.Day;
    }

    std::string ToIsoString(TimePoint t)
    {
        if (t == TimePoint{}) return "0001-01-01T00:00:00";

        LocalParts p = ToLocalParts(t);
        char buf[48];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", p.Year, p.Month, p.Day, p.Hour, p.Minute, p.Second);
        std::string s = buf;
        if (p.Ticks != 0)
        {
            snprintf(buf, sizeof(buf), ".%07d", p.Ticks);
            s += buf;
            while (s.back() == '0') s.pop_back();
        }
        int off = p.OffsetMinutes;
        snprintf(buf, sizeof(buf), "%c%02d:%02d", off < 0 ? '-' : '+', (off < 0 ? -off : off) / 60, (off < 0 ? -off : off) % 60);
        s += buf;
        return s;
    }

    namespace
    {
        bool Digits(std::string_view s, size_t& i, int count, int& value)
        {
            if (i + count > s.size()) return false;
            int v = 0;
            for (int k = 0; k < count; ++k)
            {
                char c = s[i + k];
                if (c < '0' || c > '9') return false;
                v = v * 10 + (c - '0');
            }
            i += count;
            value = v;
            return true;
        }
    }

    bool TryParseDateTime(std::string_view raw, TimePoint& t)
    {
        std::string text = base::Trim(raw);
        std::string_view s = text;
        size_t i = 0;
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, ticks = 0;

        if (!Digits(s, i, 4, y) || i >= s.size() || s[i++] != '-') return false;
        if (!Digits(s, i, 2, mo) || i >= s.size() || s[i++] != '-') return false;
        if (!Digits(s, i, 2, d)) return false;

        bool hasZone = false;
        int offsetMin = 0;
        if (i < s.size())
        {
            if (s[i] != 'T' && s[i] != ' ') return false;
            ++i;
            if (!Digits(s, i, 2, h) || i >= s.size() || s[i++] != ':') return false;
            if (!Digits(s, i, 2, mi)) return false;
            if (i < s.size() && s[i] == ':')
            {
                ++i;
                if (!Digits(s, i, 2, sec)) return false;
                if (i < s.size() && s[i] == '.')
                {
                    ++i;
                    int n = 0;
                    while (i < s.size() && s[i] >= '0' && s[i] <= '9')
                    {
                        if (n < 7) { ticks = ticks * 10 + (s[i] - '0'); ++n; }
                        ++i;
                    }
                    if (n == 0) return false;
                    for (; n < 7; ++n) ticks *= 10;
                }
            }
            if (i < s.size())
            {
                if (s[i] == 'Z' || s[i] == 'z') { hasZone = true; ++i; }
                else if (s[i] == '+' || s[i] == '-')
                {
                    int sign = s[i] == '-' ? -1 : 1, oh = 0, om = 0;
                    ++i;
                    if (!Digits(s, i, 2, oh)) return false;
                    if (i < s.size() && s[i] == ':') ++i;
                    if (i < s.size() && !Digits(s, i, 2, om)) return false;
                    hasZone = true;
                    offsetMin = sign * (oh * 60 + om);
                }
                if (i != s.size()) return false;
            }
        }

        if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 59) return false;
        if (y < 1970) { t = TimePoint{}; return true; }

        std::tm tm{};
        tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
        tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = sec; tm.tm_isdst = -1;

        std::time_t tt;
        if (hasZone)
        {
            tt = _mkgmtime(&tm);
            if (tt == (std::time_t)-1) return false;
            tt -= (std::time_t)offsetMin * 60;
        }
        else
        {
            tt = mktime(&tm);
            if (tt == (std::time_t)-1) return false;
        }

        t = system_clock::from_time_t(tt) + duration_cast<system_clock::duration>(nanoseconds((long long)ticks * 100));
        return true;
    }
}
