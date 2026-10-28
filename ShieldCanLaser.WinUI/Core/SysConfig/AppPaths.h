#pragma once
#include <filesystem>
#include <string_view>

namespace scs
{
    /// <summary>
    /// Đường dẫn dữ liệu chạy máy — khớp cây thư mục ở PLAN.md §8.
    ///
    /// Gốc mặc định là <c>D:\ShieldCanLaser</c> (tách khỏi ổ hệ thống để ghost/cài lại Windows
    /// không mất dữ liệu teach và log sản xuất). Máy phát triển thường không có ổ D: nên
    /// <see cref="Init"/> tự rơi về thư mục <c>_RunData</c> cạnh exe — nhờ vậy chạy thử ở đâu
    /// cũng được mà không phải sửa hằng số rồi quên sửa lại lúc bàn giao.
    /// </summary>
    class AppPaths
    {
    public:
        static constexpr const char* DefaultRoot = "D:\\ShieldCanLaser";

        static std::filesystem::path Root();

        static std::filesystem::path ConfigDir()  { return Root() / "Config"; }
        static std::filesystem::path ModelDir()   { return Root() / "Model"; }
        static std::filesystem::path VisionDir()  { return Root() / "Vision"; }
        static std::filesystem::path RuntimeDir() { return Root() / "Runtime"; }
        static std::filesystem::path DataDir()    { return Root() / "Data"; }
        static std::filesystem::path ImageNgDir() { return Root() / "Image" / "NG"; }
        static std::filesystem::path LogDir()     { return Root() / "Log"; }

        static std::filesystem::path SystemFile()  { return ConfigDir() / "system.json"; }
        static std::filesystem::path AlarmsFile()  { return ConfigDir() / "alarms.json"; }
        static std::filesystem::path UsersFile()   { return ConfigDir() / "users.json"; }
        static std::filesystem::path RunningFile() { return RuntimeDir() / "running.json"; }

        /// <summary>Thư mục chứa exe — nơi tìm bản mẫu alarms.json khi Config chưa có.</summary>
        static std::filesystem::path ExeDir();

        /// <summary>true nếu đang chạy ở thư mục dự phòng cạnh exe (hiện lên status bar).</summary>
        static bool UsingFallbackRoot();

        static void Init(std::string_view overrideRoot = {});

        /// <summary>CHỈ CHO TEST: thay thư mục "cạnh exe" (nơi tìm Config\alarms.json mẫu). Truyền
        /// path rỗng để trả về thư mục thật của module đang chạy.</summary>
        static void SetExeDirForTests(const std::filesystem::path& dir);
    };
}
