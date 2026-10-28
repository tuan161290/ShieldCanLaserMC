#pragma once
#include <functional>

namespace ui
{
    /// <summary>
    /// Hộp đăng nhập Engineer và hộp đổi mật khẩu — thẻ modal cùng kiểu Dialogs (ModalOverlay), ô mật khẩu là
    /// PasswordBox, Enter = xác nhận, Esc = huỷ. Kiểm tra / lưu mật khẩu ở scs::UserAccess.
    /// </summary>
    class LoginDialog
    {
    public:
        /// <summary>Hỏi mật khẩu Engineer. <paramref name="done"/>(true) khi đăng nhập được; false khi huỷ.</summary>
        static void ShowLogin(std::function<void(bool)> done = {});
        static void ShowChangePassword();
    };
}
