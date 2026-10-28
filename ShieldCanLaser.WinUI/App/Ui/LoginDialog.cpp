#include "pch.h"
#include "Ui/LoginDialog.h"

#include <format>
#include <vector>

#include "Data/UserAccess.h"
#include "Ui/Dialogs.h"
#include "Ui/UiUtil.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace ui
{
    namespace
    {
        ModalOverlay g_overlay;
        std::function<void(bool)> g_done;

        void Finish(bool ok)
        {
            g_overlay.Close();
            auto done = std::move(g_done);
            g_done = nullptr;
            if (done) done(ok);
        }

        PasswordBox MakeBox(wchar_t const* header)
        {
            PasswordBox box;
            box.Header(box_value(header));
            box.MaxLength(64);
            return box;
        }

        /// Thẻ kiểu Dialogs: tiêu đề + biểu tượng khoá, nội dung, dòng lỗi đỏ, dải nút dưới. Enter ở ô cuối = xác nhận.
        FrameworkElement BuildCard(wchar_t const* titleText, std::vector<PasswordBox> const& boxes, TextBlock const& error,
                                   TextBlock const& hint, wchar_t const* okText, std::function<void()> onOk)
        {
            Border card;
            card.Width(400);
            card.Background(Res(L"SurfaceBrush"));
            card.BorderBrush(Res(L"CardStrokeBrush"));
            card.BorderThickness(ThicknessHelper::FromUniformLength(1));
            card.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));

            Grid layout;
            RowDefinition r0; r0.Height(GridLengthHelper::Auto());
            RowDefinition r1; r1.Height(GridLengthHelper::Auto());
            layout.RowDefinitions().Append(r0);
            layout.RowDefinitions().Append(r1);

            StackPanel content;
            content.Spacing(12);
            content.Margin(ThicknessHelper::FromLengths(24, 20, 24, 20));

            StackPanel header;
            header.Orientation(Orientation::Horizontal);
            FontIcon icon;
            icon.Glyph(L"\xE72E");   // Lock
            icon.FontSize(22);
            icon.Foreground(Res(L"AccentBrush"));
            icon.Margin(ThicknessHelper::FromLengths(0, 0, 10, 0));
            icon.VerticalAlignment(VerticalAlignment::Center);
            TextBlock title;
            title.Text(titleText);
            title.FontSize(20);
            title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            title.VerticalAlignment(VerticalAlignment::Center);
            header.Children().Append(icon);
            header.Children().Append(title);
            content.Children().Append(header);
            if (!hint.Text().empty()) content.Children().Append(hint);

            for (size_t i = 0; i < boxes.size(); ++i)
            {
                PasswordBox box = boxes[i];
                bool last = i + 1 == boxes.size();
                PasswordBox next = last ? nullptr : boxes[i + 1];
                box.KeyDown([onOk, last, next](auto&&, Input::KeyRoutedEventArgs const& e) {
                    if (e.Key() == Windows::System::VirtualKey::Enter)
                    {
                        e.Handled(true);
                        if (last) onOk();
                        else next.Focus(FocusState::Keyboard);
                    }
                    else if (e.Key() == Windows::System::VirtualKey::Escape)
                    {
                        e.Handled(true);
                        Finish(false);
                    }
                });
                content.Children().Append(box);
            }
            content.Children().Append(error);

            Border bar;
            bar.Background(Res(L"BackgroundBrush"));
            bar.BorderBrush(Res(L"DividerBrush"));
            bar.BorderThickness(ThicknessHelper::FromLengths(0, 1, 0, 0));
            bar.CornerRadius(CornerRadius{ 0, 0, 8, 8 });
            bar.Padding(ThicknessHelper::FromLengths(24, 16, 24, 16));
            StackPanel buttons;
            buttons.Orientation(Orientation::Horizontal);
            buttons.HorizontalAlignment(HorizontalAlignment::Right);
            buttons.Spacing(8);
            Button ok;
            ok.Content(box_value(okText));
            ok.Width(100);
            ok.Style(ResStyle(L"PrimaryButton"));
            ok.Click([onOk](auto&&, auto&&) { onOk(); });
            Button cancel;
            cancel.Content(box_value(L"CANCEL"));
            cancel.Width(100);
            cancel.Click([](auto&&, auto&&) { Finish(false); });
            buttons.Children().Append(ok);
            buttons.Children().Append(cancel);
            bar.Child(buttons);

            Grid::SetRow(content, 0);
            Grid::SetRow(bar, 1);
            layout.Children().Append(content);
            layout.Children().Append(bar);
            card.Child(layout);

            // Đặt con trỏ vào ô đầu khi thẻ đã hiện.
            PasswordBox first = boxes.front();
            card.Loaded([first](auto&&, auto&&) { first.Focus(FocusState::Programmatic); });
            return card;
        }

        TextBlock MakeError()
        {
            TextBlock t;
            t.FontSize(13);
            t.Foreground(Res(L"CriticalBrush"));
            t.TextWrapping(TextWrapping::Wrap);
            t.Visibility(Visibility::Collapsed);
            return t;
        }

        TextBlock MakeHint(std::string const& text)
        {
            TextBlock t;
            t.FontSize(13);
            t.TextWrapping(TextWrapping::Wrap);
            t.Foreground(Res(L"TextSecondaryBrush"));
            t.Text(H(text));
            return t;
        }

        void ShowError(TextBlock const& error, std::string const& text)
        {
            error.Text(H(text));
            error.Visibility(Visibility::Visible);
        }
    }

    void LoginDialog::ShowLogin(std::function<void(bool)> done)
    {
        if (g_overlay.IsOpen()) return;
        g_done = std::move(done);
        PasswordBox pwd = MakeBox(L"Engineer password");
        TextBlock error = MakeError();
        TextBlock hint = MakeHint(std::format("Unlocks Vision Edit and EtherCAT pages. Auto logout after {} min without activity.",
                                              scs::UserAccess::IdleLogoutMinutes()));
        auto onOk = [pwd, error]() {
            std::string why;
            if (scs::UserAccess::Login(U8(pwd.Password()), why))
            {
                Finish(true);
                if (scs::UserAccess::UsingDefaultPassword())
                    Dialogs::Warn(std::string("Engineer password is still the default — change it: ENGINEER button → CHANGE PASSWORD."),
                                  "Default Password");
                return;
            }
            pwd.Password(L"");
            pwd.Focus(FocusState::Programmatic);
            ShowError(error, why);
        };
        g_overlay.Open(BuildCard(L"Engineer Login", { pwd }, error, hint, L"LOGIN", onOk));
    }

    void LoginDialog::ShowChangePassword()
    {
        if (g_overlay.IsOpen()) return;
        g_done = nullptr;
        PasswordBox current = MakeBox(L"Current password");
        PasswordBox next = MakeBox(L"New password");
        PasswordBox confirm = MakeBox(L"Confirm new password");
        TextBlock error = MakeError();
        TextBlock hint = MakeHint(std::format("At least {} characters.", scs::UserAccess::MinPasswordLength));
        auto onOk = [current, next, confirm, error]() {
            if (next.Password() != confirm.Password())
            {
                ShowError(error, "New passwords do not match");
                confirm.Password(L"");
                confirm.Focus(FocusState::Programmatic);
                return;
            }
            std::string why;
            if (!scs::UserAccess::ChangePassword(U8(current.Password()), U8(next.Password()), why))
            {
                ShowError(error, why);
                return;
            }
            Finish(true);
            Dialogs::Info("Engineer password changed.", "Password");
        };
        g_overlay.Open(BuildCard(L"Change Password", { current, next, confirm }, error, hint, L"SAVE", onOk));
    }
}
