#include "Utility/Log/LogData.h"

#include "Base/Text.h"

namespace scs
{
    namespace
    {
        /// <summary>Tab và xuống dòng trong nội dung sẽ phá cấu trúc TSV — thay bằng khoảng trắng.</summary>
        std::string Clean(std::string_view s)
        {
            std::string r(s);
            for (char& c : r)
                if (c == '\t' || c == '\r' || c == '\n') c = ' ';
            return r;
        }
    }

    std::string LogData::ToLine() const
    {
        return FormatLocal(UpdateTime, "yyyy-MM-dd HH:mm:ss.fff") + "\t" +
               base::ToUpper(ToString(Level)) + "\t" +
               Clean(Source) + "\t" +
               Clean(Message);
    }

    bool LogData::TryParseLine(std::string_view line, LogData& data)
    {
        if (base::IsBlank(line)) return false;

        auto parts = base::Split(line, '\t');
        if (parts.size() < 4) return false;

        TimePoint t;
        if (!TryParseDateTime(parts[0], t)) return false;

        // Enum.TryParse(ignoreCase): nhận tên không phân biệt hoa thường và cả dạng số.
        LogLevel level = LogLevel::Info;
        std::string name = base::ToLower(base::Trim(parts[1]));
        if (name == "debug" || name == "0") level = LogLevel::Debug;
        else if (name == "info" || name == "1") level = LogLevel::Info;
        else if (name == "warning" || name == "2") level = LogLevel::Warning;
        else if (name == "error" || name == "3") level = LogLevel::Error;

        data.UpdateTime = t;
        data.Level = level;
        data.Source = parts[2];
        data.Message = parts[3];
        return true;
    }
}
