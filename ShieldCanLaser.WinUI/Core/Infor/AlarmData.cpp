#include "Infor/AlarmData.h"

#include "Base/Text.h"

namespace scs
{
    namespace
    {
        std::string Csv(const std::string& s)
        {
            if (s.empty()) return "";
            if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
            return "\"" + base::Replace(s, "\"", "\"\"") + "\"";
        }
    }

    std::string AlarmData::ToCsvLine() const
    {
        const char* fmt = "yyyy-MM-dd HH:mm:ss.fff";
        return base::Join({
            Csv(FormatLocal(RaisedAt, fmt)),
            Csv(ClearedAt ? FormatLocal(*ClearedAt, fmt) : std::string()),
            std::to_string(Code),
            Csv(ToString(Level)),
            Csv(Source),
            Csv(IsRootCause ? "ROOT" : ""),
            Csv(Message()),
            Csv(Detail) }, ",");
    }
}
