#pragma once
// Vòng đời của app phía lõi máy: khởi động (đường dẫn, log, bảng alarm, dữ liệu, McControl) và tắt.
// Tách khỏi App.xaml.cpp để phần XAML chỉ còn việc dựng cửa sổ.
#include <string>

namespace ui
{
    struct AppOptions
    {
        /// <summary>--root &lt;thư mục&gt;: gốc dữ liệu thay cho D:\ShieldCanLaser (chạy thử không đụng dữ liệu máy).</summary>
        std::string DataRoot;
        /// <summary>--shot &lt;thư mục&gt;: chụp từng trang ra PNG rồi tự thoát; cửa sổ không giành focus.</summary>
        std::string ScreenshotDir;
    };

    class AppHost
    {
    public:
        static const AppOptions& Options();

        /// <summary>False = app phải thoát ngay (đã có một bản đang chạy).</summary>
        static bool Startup();

        /// <summary>Gọi được nhiều lần; chỉ lần đầu có tác dụng.</summary>
        static void Shutdown();

        /// <summary>Ngoại lệ lọt tới vòng lặp UI: ghi log + phát alarm dạng sự kiện, app chạy tiếp.</summary>
        static void OnUnhandledUiException(const std::string& message);
    };
}
