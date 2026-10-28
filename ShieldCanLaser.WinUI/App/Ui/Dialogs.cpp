#include "pch.h"
#include "Ui/Dialogs.h"

#include "Base/Text.h"
#include "Ui/UiUtil.h"
#include "Utility/Log/LogWorker.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;
using namespace winrt::Microsoft::UI::Xaml::Media;
namespace WF = winrt::Windows::Foundation;

namespace ui
{
    namespace
    {
        XamlRoot g_root{ nullptr };
        int g_openModals = 0;

        base::Event<bool>& ModalEvent()
        {
            static base::Event<bool> e;
            return e;
        }

        struct Request
        {
            DialogKind Kind{};
            std::string Title;
            std::string Message;
            std::function<void(bool)> Completed;
        };

        // Hộp đang hiện + các hộp bị nó đè lên. Đóng hộp trên thì hiện lại hộp dưới — không giữ lại thì hộp
        // dưới mất nút bấm và nơi gọi nó chờ mãi.
        std::shared_ptr<Request> g_current;
        std::vector<std::shared_ptr<Request>> g_covered;
        ModalOverlay g_dialogOverlay;

        void Record(Request const& r, std::optional<bool> answer)
        {
            auto lines = base::Split(base::Replace(r.Message, "\r", ""), '\n', true);
            std::string text = "Dialog \"" + r.Title + "\": " + base::Join(lines, " | ");
            if (answer.has_value()) text += *answer ? " → Yes" : " → No";

            auto level = r.Kind == DialogKind::Error ? scs::LogLevel::Error
                       : r.Kind == DialogKind::Warn ? scs::LogLevel::Warning
                       : scs::LogLevel::Info;
            scs::LogWorker::Instance().RecordLog(text, level, "UI");
        }

        wchar_t const* GlyphOf(DialogKind kind)
        {
            switch (kind)
            {
            case DialogKind::Warn:    return L"\xE7BA";   // Warning
            case DialogKind::Error:   return L"\xEA39";   // ErrorBadge
            case DialogKind::Confirm: return L"\xE9CE";   // Unknown (dấu hỏi)
            default:                  return L"\xE946";   // Info
            }
        }

        uint32_t ColorOfKind(DialogKind kind)
        {
            switch (kind)
            {
            case DialogKind::Warn:    return 0xFF9D5D00;
            case DialogKind::Error:   return 0xFFC42B1C;
            case DialogKind::Confirm: return 0xFF0078D4;
            default:                  return 0xFF0F7B0F;
            }
        }

        void Present();

        void Complete(bool result)
        {
            auto closing = g_current;
            if (g_covered.empty()) g_current = nullptr;
            else
            {
                g_current = g_covered.back();
                g_covered.pop_back();
            }
            Present();
            if (closing && closing->Completed) closing->Completed(result);
        }

        Button MakeButton(wchar_t const* text, bool primary, bool result)
        {
            Button b;
            b.Content(box_value(text));
            b.Width(100);
            if (primary) b.Style(ResStyle(L"PrimaryButton"));
            b.Click([result](auto&&, auto&&) { Complete(result); });
            return b;
        }

        // Thẻ hộp thoại kiểu ContentDialog Windows 11: phần nội dung nền trắng; dải nút dưới cùng nền lớp 0,
        // ngăn bằng đường kẻ mảnh.
        FrameworkElement BuildCard(Request const& r)
        {
            Border card;
            card.Width(460);
            card.Background(Res(L"SurfaceBrush"));
            card.BorderBrush(Res(L"CardStrokeBrush"));
            card.BorderThickness(ThicknessHelper::FromUniformLength(1));
            card.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));

            Grid layout;
            RowDefinition contentRow; contentRow.Height(GridLengthHelper::Auto());
            RowDefinition buttonRow; buttonRow.Height(GridLengthHelper::Auto());
            layout.RowDefinitions().Append(contentRow);
            layout.RowDefinitions().Append(buttonRow);

            StackPanel content;
            content.Margin(ThicknessHelper::FromLengths(24, 20, 24, 24));

            StackPanel header;
            header.Orientation(Orientation::Horizontal);
            header.Margin(ThicknessHelper::FromLengths(0, 0, 0, 12));
            FontIcon icon;
            icon.Glyph(GlyphOf(r.Kind));
            icon.FontSize(22);
            icon.Foreground(Solid(ColorOfKind(r.Kind)));
            icon.Margin(ThicknessHelper::FromLengths(0, 0, 10, 0));
            icon.VerticalAlignment(VerticalAlignment::Center);
            TextBlock title;
            title.Text(H(r.Title));
            title.FontSize(20);
            title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            title.VerticalAlignment(VerticalAlignment::Center);
            title.TextWrapping(TextWrapping::Wrap);
            title.MaxWidth(370);
            header.Children().Append(icon);
            header.Children().Append(title);

            TextBlock message;
            message.Text(H(r.Message));
            message.TextWrapping(TextWrapping::Wrap);
            message.FontSize(14);
            message.IsTextSelectionEnabled(true);

            ScrollViewer scroll;
            scroll.MaxHeight(420);
            scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroll.Content(message);

            content.Children().Append(header);
            content.Children().Append(scroll);

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
            if (r.Kind == DialogKind::Confirm)
            {
                buttons.Children().Append(MakeButton(L"YES", true, true));
                buttons.Children().Append(MakeButton(L"NO", false, false));
            }
            else
            {
                buttons.Children().Append(MakeButton(L"OK", true, true));
            }
            bar.Child(buttons);

            Grid::SetRow(content, 0);
            Grid::SetRow(bar, 1);
            layout.Children().Append(content);
            layout.Children().Append(bar);
            card.Child(layout);
            return card;
        }

        void Present()
        {
            if (!g_current)
            {
                g_dialogOverlay.Close();
                return;
            }
            g_dialogOverlay.Close();
            g_dialogOverlay.Open(BuildCard(*g_current));
        }

        void Enqueue(std::shared_ptr<Request> req)
        {
            if (!g_root)
            {
                // Chưa có cửa sổ (lỗi lúc khởi động): không có chỗ hiện, trả lời "Không" để nơi gọi đi tiếp.
                if (req->Completed) req->Completed(false);
                return;
            }
            if (g_current) g_covered.push_back(g_current);
            g_current = std::move(req);
            Present();
        }

        void ShowDetached(DialogKind kind, std::string message, std::string title)
        {
            auto req = std::make_shared<Request>();
            req->Kind = kind;
            req->Message = std::move(message);
            req->Title = std::move(title);
            Record(*req, std::nullopt);
            if (OnUiThread()) Enqueue(std::move(req));
            else PostUi([req]() mutable { Enqueue(std::move(req)); });
        }
    }

    // ── ModalOverlay ──

    void ModalOverlay::Attach(XamlRoot const& root) { g_root = root; }

    XamlRoot ModalOverlay::Root() { return g_root; }

    base::Event<bool>& ModalOverlay::ModalChanged() { return ModalEvent(); }

    bool ModalOverlay::AnyOpen() { return g_openModals > 0; }

    void ModalOverlay::Open(FrameworkElement const& card)
    {
        if (m_popup || !g_root) return;

        auto size = g_root.Size();
        m_smoke = Grid();
        m_smoke.Width(size.Width);
        m_smoke.Height(size.Height);
        m_smoke.Background(Res(L"SmokeBrush"));

        // Popup là một cây hiển thị riêng, không thừa hưởng font của cửa sổ chính.
        ContentControl host;
        host.FontFamily(FontFamily(L"Segoe UI"));
        host.HorizontalAlignment(HorizontalAlignment::Center);
        host.VerticalAlignment(VerticalAlignment::Center);
        host.Content(card);
        host.Shadow(ThemeShadow());
        host.Translation({ 0, 0, 32 });
        m_smoke.Children().Append(host);

        m_popup = Popup();
        m_popup.XamlRoot(g_root);
        m_popup.ShouldConstrainToRootBounds(false);
        m_popup.IsLightDismissEnabled(false);
        m_popup.Child(m_smoke);
        m_popup.IsOpen(true);

        if (++g_openModals == 1) ModalEvent().Raise(true);
    }

    void ModalOverlay::Close()
    {
        if (!m_popup) return;
        m_popup.IsOpen(false);
        m_popup.Child(nullptr);
        m_popup = nullptr;
        m_smoke = nullptr;
        if (--g_openModals == 0) ModalEvent().Raise(false);
    }

    void ModalOverlay::Resize()
    {
        if (!m_smoke || !g_root) return;
        auto size = g_root.Size();
        m_smoke.Width(size.Width);
        m_smoke.Height(size.Height);
    }

    // ── Dialogs ──

    void Dialogs::Info(std::string message, std::string title) { ShowDetached(DialogKind::Info, std::move(message), std::move(title)); }
    void Dialogs::Warn(std::string message, std::string title) { ShowDetached(DialogKind::Warn, std::move(message), std::move(title)); }
    void Dialogs::Error(std::string message, std::string title) { ShowDetached(DialogKind::Error, std::move(message), std::move(title)); }

    WF::IAsyncOperation<bool> Dialogs::ShowAsync(DialogKind kind, std::string message, std::string title)
    {
        winrt::apartment_context uiContext;   // luồng UI đang gọi — quay lại đúng luồng này sau khi chờ

        auto req = std::make_shared<Request>();
        req->Kind = kind;
        req->Message = std::move(message);
        req->Title = std::move(title);
        if (kind != DialogKind::Confirm) Record(*req, std::nullopt);

        // Kết quả và tín hiệu nằm trên heap dùng chung: callback có thể chạy sau khi coroutine này đã bị huỷ.
        struct State { winrt::handle done{ CreateEventW(nullptr, TRUE, FALSE, nullptr) }; std::atomic<bool> result{ false }; };
        auto state = std::make_shared<State>();
        req->Completed = [state](bool r) {
            state->result = r;
            SetEvent(state->done.get());
        };

        Enqueue(req);
        co_await winrt::resume_on_signal(state->done.get());
        co_await uiContext;

        bool result = state->result;
        if (kind == DialogKind::Confirm) Record(*req, result);
        co_return result;
    }

    WF::IAsyncOperation<bool> Dialogs::ConfirmAsync(std::string message, std::string title)
    {
        return ShowAsync(DialogKind::Confirm, std::move(message), std::move(title));
    }

    bool Dialogs::ConfirmBlocking(std::string message, std::string title)
    {
        auto req = std::make_shared<Request>();
        req->Kind = DialogKind::Confirm;
        req->Message = std::move(message);
        req->Title = std::move(title);

        if (OnUiThread())
        {
            scs::LogWorker::Instance().RecordLog(
                "ConfirmBlocking called on UI thread (\"" + req->Title + "\") — answered No", scs::LogLevel::Error, "UI");
            return false;
        }

        struct State { winrt::handle done{ CreateEventW(nullptr, TRUE, FALSE, nullptr) }; std::atomic<bool> result{ false }; };
        auto state = std::make_shared<State>();
        req->Completed = [state](bool r) {
            state->result = r;
            SetEvent(state->done.get());
        };

        PostUi([req]() mutable { Enqueue(std::move(req)); });
        WaitForSingleObject(state->done.get(), INFINITE);

        bool result = state->result;
        Record(*req, result);
        return result;
    }

    void Dialogs::CloseAll()
    {
        // Complete() tự lấy hộp kế trong chồng lên; lặp tới khi hết.
        while (g_current) Complete(false);
    }
}
