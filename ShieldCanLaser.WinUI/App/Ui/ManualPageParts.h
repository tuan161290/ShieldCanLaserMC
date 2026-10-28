#pragma once
// Trang Manual — các phần dựng trong code: đèn tròn (bảng I/O, POT/NOT) và MỘT HÀNG TRỤC (thay AxisRowVm của bản
// WPF). Bảng trục có 9 hàng giống hệt nhau nên dựng bằng code thay vì DataTemplate; mỗi hàng giữ control của mình
// và giá trị đã hiện lần trước, OnTick chỉ đụng tới control khi giá trị ĐỔI.
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace scs { class IAxis; }

namespace ui
{
    /// <summary>Brush dùng chung của các đèn (LampOffBrush / LampStrokeBrush / CriticalBrush của bản WPF). Tạo một
    /// lần trên luồng UI rồi chia cho mọi đèn.</summary>
    struct ManualLampBrushes
    {
        winrt::Microsoft::UI::Xaml::Media::Brush Off{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Brush Stroke{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Brush On{ nullptr };
    };

    /// <summary>
    /// Đèn bit thô (DI/DO, POT/NOT): OFF xám nhạt trung tính, ON đỏ + quầng sáng đỏ — bắt mắt ngay bit nào đang lên
    /// (nhiều bit ON là BÌNH THƯỜNG, vd. NC như EmgOk; không phải kiểu "tốt = xanh"). Quầng sáng là bóng đổ
    /// Composition lấy đúng hình tròn của chấm làm mặt nạ (như DropShadowEffect cỡ chấm của bản WPF), tạo một lần
    /// khi đèn sáng lần đầu, sau đó chỉ bật/tắt.
    /// </summary>
    class ManualLamp
    {
    public:
        winrt::Microsoft::UI::Xaml::FrameworkElement Build(double size, ManualLampBrushes const& brushes);
        /// <summary>Gọi mỗi nhịp được: chỉ đụng tới control khi trạng thái đổi.</summary>
        void Set(bool on);

    private:
        ManualLampBrushes m_brushes;
        winrt::Microsoft::UI::Xaml::Controls::Grid m_root{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Border m_glowHost{ nullptr };
        winrt::Microsoft::UI::Xaml::Shapes::Ellipse m_dot{ nullptr };
        winrt::Microsoft::UI::Composition::SpriteVisual m_glow{ nullptr };
        float m_size{ 10 };
        int m_state{ -1 };   // -1 = chưa vẽ lần nào
    };

    /// <summary>
    /// TextBox hẹp của bảng trục (46–70 px như bản WPF): nút "xoá chữ" của TextBox WinUI chiếm ~30 px bên phải lúc
    /// đang gõ, ô hẹp chỉ còn thấy 2–3 ký tự. Gỡ nút đó khỏi bố cục (bề rộng 0, không nhận chuột) khi template đã dựng.
    /// </summary>
    void HideTextBoxDeleteButton(winrt::Microsoft::UI::Xaml::Controls::TextBox const& box);

    /// <summary>Nút hẹp của trang Manual (CompactButtonStyle của bản WPF): Padding 4,3 — Padding mặc định 11,5 ăn hết
    /// chỗ của nút rộng 40–80 px. Bề rộng đặt theo đúng cột của nút.</summary>
    winrt::Microsoft::UI::Xaml::Controls::Button ManualCompactButton(wchar_t const* text, double width, wchar_t const* tip = nullptr);

    /// <summary>
    /// Một hàng của bảng trục trang Manual — thay <c>AxisRowVm</c>: Trục | Vị trí | Cữ (POT/NOT) | Servo (SVO toggle,
    /// RST) | Gốc (Home) | Jog −/+ | Inching (bước, −, +) | Move Abs (đích, Move) | Dừng. Bề rộng mỗi ô cố định và PHẢI
    /// khớp dòng tiêu đề cột trong ManualPage.xaml.
    ///
    /// <para>Lệnh dài (SVO, RST, Home, Move, Inching) chạy trên luồng nền; trong lúc lệnh của HÀNG NÀY còn chạy các nút
    /// lệnh dài của hàng bị khoá để không phát chồng lệnh thứ hai lên cùng trục. Stop và Jog không bao giờ bị khoá.</para>
    /// </summary>
    class ManualAxisRow : public std::enable_shared_from_this<ManualAxisRow>
    {
    public:
        static std::shared_ptr<ManualAxisRow> Create(std::shared_ptr<scs::IAxis> axis, ManualLampBrushes const& brushes);

        explicit ManualAxisRow(std::shared_ptr<scs::IAxis> axis);
        ManualAxisRow(const ManualAxisRow&) = delete;
        ManualAxisRow& operator=(const ManualAxisRow&) = delete;

        winrt::Microsoft::UI::Xaml::FrameworkElement Root() const { return m_root; }
        const std::shared_ptr<scs::IAxis>& Axis() const { return m_axis; }

        /// <summary>Một nhịp OnTick (luồng UI): chỉ đọc getter rẻ của IAxis, chỉ ghi control khi giá trị đổi.</summary>
        void Update();

    private:
        void Build(ManualLampBrushes const& brushes);

        void OnServoToggle();
        void OnResetAlarm();
        void OnHome();
        void OnMove();
        void OnInch(int dir);
        void OnStop();

        /// <summary>Chạy một lệnh dài của hàng trên luồng nền; bỏ qua nếu hàng đang có lệnh chạy.</summary>
        void RunBlocking(std::function<void()> work);
        void ApplyBusy(bool busy);

        std::shared_ptr<scs::IAxis> m_axis;
        std::string m_name;
        /// <summary>Cờ bận của hàng — chia sẻ với việc nền (không bắt <c>this</c> vào luồng nền).</summary>
        std::shared_ptr<std::atomic<bool>> m_busy = std::make_shared<std::atomic<bool>>(false);

        winrt::Microsoft::UI::Xaml::Controls::Border m_root{ nullptr };
        winrt::Microsoft::UI::Xaml::Documents::Run m_posRun{ nullptr };
        winrt::Microsoft::UI::Xaml::Documents::Run m_unitRun{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel m_limitPanel{ nullptr };
        ManualLamp m_pot;
        ManualLamp m_not;
        winrt::Microsoft::UI::Xaml::Controls::Button m_svoButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_rstButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_homeButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_incMinusButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_incPlusButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_moveButton{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBox m_stepBox{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBox m_targetBox{ nullptr };

        // Giá trị đã hiện lần trước (-1 = chưa hiện).
        int m_shownServo{ -1 };
        int m_shownAlarm{ -1 };
        int m_shownOrg{ -1 };
        int m_shownBusy{ -1 };
        int m_shownHasLimit{ -1 };
        winrt::hstring m_shownPos;
        std::string m_shownUnit{ "\x01" };
    };
}
