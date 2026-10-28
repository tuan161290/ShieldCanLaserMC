#pragma once
// Hộp thoại và lớp phủ modal của app — thay DialogHost/overlay của bản WPF.
//
// Mọi lớp phủ nằm trong một Popup KHÔNG ràng buộc trong cửa sổ (ShouldConstrainToRootBounds = false):
// WinUI cấp cho nó một HWND riêng nằm TRÊN các popup chứa control của VisionMaster. ContentDialog chuẩn vẽ
// trong cửa sổ chính nên bị VisionMaster che (BAN-GIAO-VM-WinUI3.md mục 6.4).
#include <functional>
#include <string>

#include "Base/Event.h"

namespace ui
{
    enum class DialogKind { Info, Warn, Error, Confirm };

    /// <summary>
    /// Một lớp phủ modal: nền mờ phủ cả cửa sổ + một thẻ nội dung ở giữa. Trong lúc mở, các popup của
    /// VisionMaster bị vô hiệu (vẫn hiện, không nhận chuột/phím) qua sự kiện <see cref="ModalChanged"/>.
    /// </summary>
    class ModalOverlay
    {
    public:
        /// <summary>Gắn với cửa sổ chính. Gọi một lần khi nội dung cửa sổ đã Loaded.</summary>
        static void Attach(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
        static winrt::Microsoft::UI::Xaml::XamlRoot Root();

        /// <summary>Số lớp phủ modal đang mở đổi giữa 0 và khác 0: true = vừa có lớp phủ đầu tiên,
        /// false = lớp phủ cuối vừa đóng. Phát trên luồng UI.</summary>
        static base::Event<bool>& ModalChanged();
        static bool AnyOpen();

        void Open(winrt::Microsoft::UI::Xaml::FrameworkElement const& card);
        void Close();
        bool IsOpen() const { return m_popup != nullptr; }
        /// <summary>Cửa sổ đổi kích thước: phủ lại cho vừa.</summary>
        void Resize();

    private:
        winrt::Microsoft::UI::Xaml::Controls::Primitives::Popup m_popup{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_smoke{ nullptr };
    };

    /// <summary>
    /// Thông báo và hỏi xác nhận. Mọi hộp thoại người vận hành thấy đều vào log (bấm OK là bảng biến mất,
    /// log thì còn để tra lại); hộp xác nhận ghi sau khi bấm, kèm câu trả lời.
    /// </summary>
    class Dialogs
    {
    public:
        /// <summary>Hiện rồi đi tiếp ngay, không chờ người bấm. Gọi được từ luồng bất kỳ.</summary>
        static void Info(std::string message, std::string title = "Info");
        static void Warn(std::string message, std::string title = "Warning");
        static void Error(std::string message, std::string title = "Error");

        /// <summary>Hiện và CHỜ người bấm (co_await). Gọi trên luồng UI; chạy tiếp cũng trên luồng UI.
        /// Trả true khi bấm OK / Có.</summary>
        static winrt::Windows::Foundation::IAsyncOperation<bool> ShowAsync(DialogKind kind, std::string message, std::string title);
        static winrt::Windows::Foundation::IAsyncOperation<bool> ConfirmAsync(std::string message, std::string title = "Confirm");

        /// <summary>Hỏi xác nhận từ LUỒNG NỀN: chặn luồng gọi tới khi người vận hành bấm. Không gọi trên
        /// luồng UI (sẽ trả false ngay và ghi log, vì chặn luồng UI thì không ai bấm được).</summary>
        static bool ConfirmBlocking(std::string message, std::string title = "Confirm");

        /// <summary>Đóng mọi hộp đang mở với câu trả lời "Không" — lúc tắt app.</summary>
        static void CloseAll();
    };
}
