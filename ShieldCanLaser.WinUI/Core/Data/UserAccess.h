#pragma once
#include <chrono>
#include <string>

#include "Base/Event.h"

namespace scs
{
    /// <summary>Cấp người dùng (Tuấn 2026-10-21: 2 cấp). Operator không cần đăng nhập; Engineer mở trang Vision Edit,
    /// EtherCAT.</summary>
    enum class EUserLevel { Operator = 0, Engineer = 1 };

    const char* ToString(EUserLevel level);

    /// <summary>
    /// Phân quyền người dùng: đăng nhập Engineer bằng mật khẩu, đăng xuất tay hoặc tự động sau
    /// <see cref="IdleLogoutMinutes"/> phút không chạm chuột / phím. Mật khẩu chỉ lưu dạng SHA-256(salt + mật khẩu) trong
    /// Config\users.json; chưa có file thì dùng mật khẩu mặc định <see cref="DefaultPassword"/> (đổi ngay sau lần đăng
    /// nhập đầu). Mật khẩu master (chỉ giữ băm trong UserAccess.cpp) luôn đăng nhập được và dùng được làm mật khẩu hiện
    /// tại khi đổi — cách lấy lại quyền khi quên mật khẩu. Mọi lần đăng nhập / sai mật khẩu / đăng xuất đều ghi log nguồn "User".
    /// Gọi từ luồng UI (trạng thái có khoá nên đọc được từ luồng bất kỳ).
    /// </summary>
    class UserAccess
    {
    public:
        using Clock = std::chrono::steady_clock;

        static constexpr const char* DefaultPassword = "1234";
        static constexpr int MinPasswordLength = 4;

        /// <summary>Nạp Config\users.json (thiếu / hỏng → mật khẩu mặc định). Đăng xuất về Operator.</summary>
        static void Load();

        static EUserLevel Level();
        static bool IsEngineer() { return Level() >= EUserLevel::Engineer; }
        /// <summary>Đang dùng mật khẩu mặc định (chưa đổi lần nào).</summary>
        static bool UsingDefaultPassword();

        /// <summary>Đăng nhập Engineer. Sai: trả false, <paramref name="why"/> nêu lý do.</summary>
        static bool Login(const std::string& password, std::string& why);
        /// <summary>Về Operator. <paramref name="reason"/> ghi vào log ("LOGOUT", "idle 10 min" …).</summary>
        static void Logout(const std::string& reason);
        /// <summary>Đổi mật khẩu Engineer (phải đang đăng nhập Engineer và nhập đúng mật khẩu cũ). Ghi users.json.</summary>
        static bool ChangePassword(const std::string& oldPassword, const std::string& newPassword, std::string& why);

        /// <summary>Người dùng vừa thao tác (chuột / phím) — đặt lại đồng hồ tự đăng xuất.</summary>
        static void Touch(Clock::time_point now = Clock::now());
        /// <summary>Gọi định kỳ: quá <see cref="IdleLogoutMinutes"/> phút không thao tác thì đăng xuất. Trả true khi vừa
        /// đăng xuất vì lý do này.</summary>
        static bool CheckIdle(Clock::time_point now = Clock::now());
        /// <summary>Số giây còn lại trước khi tự đăng xuất (0 khi đang là Operator).</summary>
        static int SecondsUntilIdleLogout(Clock::time_point now = Clock::now());

        static int IdleLogoutMinutes();

        /// <summary>Cấp đổi (đăng nhập / đăng xuất). Phát trên luồng gọi Login / Logout / CheckIdle.</summary>
        static base::Event<EUserLevel>& LevelChanged();
    };
}
