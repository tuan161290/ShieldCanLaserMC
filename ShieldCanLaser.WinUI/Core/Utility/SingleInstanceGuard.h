#pragma once
#include <string_view>

namespace scs
{
    /// <summary>
    /// Chặn chạy hai bản app cùng lúc. Với máy sản xuất đây không phải tiện ích mà là yêu cầu an
    /// toàn: hai process cùng mở bus / cùng phát lệnh chuyển động là tình huống không kiểm soát được.
    /// Dùng named mutex Win32 — cùng tên với bản WPF thì hai bản loại trừ lẫn nhau.
    /// </summary>
    class SingleInstanceGuard
    {
    public:
        static bool TryLock(std::string_view mutexName);
        static void Release();
        /// <summary>Đưa cửa sổ chính của bản đang chạy (cùng tên exe) ra trước.</summary>
        static void BringExistingToFront();
    };
}
