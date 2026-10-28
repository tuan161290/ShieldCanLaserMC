#pragma once
// Tiện ích dùng chung cho mã giao diện (namespace ui). Lõi máy (Core) dùng std::string UTF-8 và hàm
// chặn; các hàm ở đây là cầu nối sang WinUI: đổi chuỗi, lấy brush/style, tô màu nút trạng thái,
// chuyển việc giữa luồng UI và luồng nền.
#include <functional>
#include <string>
#include <string_view>

namespace ui
{
    // ── Chuỗi và số ──
    winrt::hstring H(std::string_view utf8);
    std::string U8(winrt::hstring const& text);
    /// <summary>Số thực với số chữ số thập phân cố định, dấu chấm — vd. Num(1.5) = "1.500".</summary>
    winrt::hstring Num(double value, int decimals = 3);
    /// <summary>Đọc số người dùng gõ; nhận cả dấu phẩy lẫn dấu chấm thập phân. False nếu không phải số.</summary>
    bool ParseDouble(winrt::hstring const& text, double& value);
    bool ParseInt(winrt::hstring const& text, int& value);

    // ── Tài nguyên trong Styles.xaml ──
    winrt::Microsoft::UI::Xaml::Media::Brush Res(wchar_t const* key);
    winrt::Microsoft::UI::Xaml::Style ResStyle(wchar_t const* key);
    winrt::Windows::UI::Color ColorOf(uint32_t argb);
    winrt::Microsoft::UI::Xaml::Media::SolidColorBrush Solid(uint32_t argb);

    /// <summary>true = xanh lá (bình thường), false = xám đậm ("tắt" trung tính, không phải lỗi).</summary>
    winrt::Microsoft::UI::Xaml::Media::Brush StatusBrush(bool on);
    /// <summary>Đèn ALM/POT/NOT: true = đỏ, false = xám đậm — trạng thái bình thường KHÔNG tô xanh.</summary>
    winrt::Microsoft::UI::Xaml::Media::Brush AlarmLampBrush(bool on);

    inline void Show(winrt::Microsoft::UI::Xaml::UIElement const& element, bool visible)
    {
        using winrt::Microsoft::UI::Xaml::Visibility;
        auto v = visible ? Visibility::Visible : Visibility::Collapsed;
        if (element.Visibility() != v) element.Visibility(v);
    }

    /// <summary>Đặt Text chỉ khi khác — vòng cập nhật 100 ms không được làm bẩn layout mỗi nhịp.</summary>
    void SetText(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& block, winrt::hstring const& text);
    void SetText(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& block, std::string_view utf8);

    // ── Nút trạng thái ──
    /// <summary>Màu của nút "đang BẬT": tô đặc + chữ trắng + quầng sáng phẳng (Tuấn chốt 2026-09-11).
    /// None = trả về kiểu gốc của nút.</summary>
    /// <summary>Accent = lựa chọn đang chọn trong một nhóm (vd. Chậm/Nhanh): nền accent, chữ trắng, KHÔNG quầng sáng.</summary>
    enum class Tint { None = 0, Green, Red, Caution, Blue, Lime, Accent };

    /// <summary>Tô nút theo trạng thái. Gọi lặp lại với cùng giá trị không tốn gì — dùng thẳng trong
    /// vòng cập nhật. Nút phải dùng FlatButtonStyle (mặc định của mọi Button trong app).</summary>
    void SetTint(winrt::Microsoft::UI::Xaml::Controls::Button const& button, Tint tint);

    /// <summary>Quầng sáng quanh một phần tử bất kỳ (vd. Border bọc tab Alarm). argb = 0: tắt.</summary>
    void SetGlow(winrt::Microsoft::UI::Xaml::FrameworkElement const& host, uint32_t argb);

    // ── Luồng ──
    void InitDispatcher(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue);
    bool OnUiThread();
    /// <summary>Chạy trên luồng UI (xếp hàng, không chờ). Gọi được từ luồng bất kỳ.</summary>
    void PostUi(std::function<void()> fn);
    /// <summary>Chạy một việc dài (lệnh trục, gọi vision, lưu file…) trên luồng nền — luồng UI KHÔNG BAO
    /// GIỜ gọi thẳng hàm chặn của Core.</summary>
    void RunBackground(std::function<void()> work);
    /// <summary>Như RunBackground, xong thì chạy <paramref name="thenOnUi"/> trên luồng UI.</summary>
    void RunBackgroundThenUi(std::function<void()> work, std::function<void()> thenOnUi);
}
