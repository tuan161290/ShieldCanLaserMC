#pragma once

namespace scs
{
    enum class LogLevel
    {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3
    };

    inline const char* ToString(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Debug:   return "Debug";
        case LogLevel::Info:    return "Info";
        case LogLevel::Warning: return "Warning";
        case LogLevel::Error:   return "Error";
        }
        return "?";
    }
}
