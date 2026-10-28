#pragma once
#include "Utility/DateTimeText.h"
#include "Utility/Log/LogLevel.h"

#include <string>
#include <string_view>

namespace scs
{
    struct LogData
    {
        TimePoint UpdateTime{};
        LogLevel Level{ LogLevel::Debug };

        /// <summary>Nguồn phát: "X", "Z", "Auto", "Safety", "Vision", "System"...</summary>
        std::string Source;

        std::string Message;

        /// <summary>Một dòng trong file log — TSV, thời gian đứng trước để sort/grep được.</summary>
        std::string ToLine() const;

        static bool TryParseLine(std::string_view line, LogData& data);
    };
}
