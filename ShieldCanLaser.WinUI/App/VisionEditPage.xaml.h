#pragma once
#include "VisionEditPage.g.h"

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include "Controller/Routine_Calib.h"
#include "Ui/Page.h"
#include "Ui/VmPane.h"

namespace winrt::ShieldCanLaser::implementation
{
    /// <summary>
    /// Trang Vision Edit — máy Laser Marking: thanh jog 3 trục, khung chỉnh Flow của VisionMaster (VmGlobalToolControl +
    /// VmMainViewConfigControl) và cột hiệu chuẩn vision (PLAN-LASER.md §4.2).
    ///
    /// <para><b>Cột hiệu chuẩn</b>: chọn nozzle (1…7) → chọn bước trên thanh bước ngang (vòng ✓ xanh = đã xong) → khung của
    /// bước → một dòng kết quả ở đáy. Ba bước: Lưới 9 điểm X × Z (một camera, dùng chung), SET MASTER từng nozzle, Thử laser
    /// từng nozzle (tới điểm khắc đã bù, TEST MARK, trim). Logic nằm ở <c>scs::Routine_Calib</c>; trang chỉ đọc ô nhập, gọi
    /// routine trên luồng nền và hiện kết quả.</para>
    ///
    /// <para>Nút làm trục chạy / kích laser: BẤM ĐÚP (DoubleTapped), như mọi trang. Nút chỉ kích Flow hay ghi số: bấm một lần.</para>
    /// </summary>
    struct VisionEditPage : VisionEditPageT<VisionEditPage>, ui::IPage
    {
        VisionEditPage() = default;

        void OnRootLoaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnZSafeDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void OnJogSlowClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnJogFastClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStopAllClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnReloadSolutionClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        // ── hiệu chuẩn: chọn nozzle / bước, ô nhập ──
        void OnStepClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnInputLostFocus(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        // ── 1 · Lưới X × Z ──
        void OnSaveCenterClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnGridPointDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        winrt::fire_and_forget OnRunGridDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void OnCaptureHereClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnVerifyGridDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);

        // ── 2 · Master ──
        void OnGoToCamDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void OnCaptureClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnSetMasterDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);

        // ── 3 · Thử laser ──
        void OnGoToLaserDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        winrt::fire_and_forget OnTestMarkDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void OnApplyTrimClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        void OnTick() override;
        void OnShown(bool visible) override;
        void OnWindowMoved() override;
        void OnModalChanged(bool modalOpen) override;
        void OnClosing() override;
        /// <summary>--shot chụp cả 3 bước của cột hiệu chuẩn (5-visionedit, 5-visionedit-2, -3).</summary>
        int ShotVariants() const override { return 3; }
        void ShowShotVariant(int index) override { SelectStep(index); }

    private:
        using Work = std::function<scs::CalibResult(scs::Routine_Calib&, std::stop_token)>;
        enum class ResultKind { Idle, Busy, Ok, Fail };
        static constexpr int NozzleCount = scs::Routine_Calib::NozzleCount;
        static constexpr int StepCount = 3;

        void BuildJogGrid();
        void BuildNozzleChips();
        void BuildNumpad();
        void BuildNozzleSummary();
        /// <summary>Xếp hàng EnsureVmPanes + gắn solution chạy ở lượt sau — không gọi control VisionMaster ngay trong
        /// Tick/bấm tab (xem Ui/VmPane.h: XAML fail-fast khi bị gọi lồng).</summary>
        void QueueVmWork();
        void EnsureVmPanes();
        void BindProcedures();

        // ── hiệu chuẩn ──
        /// <summary>Kiểm điều kiện trên luồng UI (đã về gốc, đang MANUAL, không có chu trình nào chạy), rồi chạy
        /// <paramref name="work"/> trên luồng nền; xong thì hiện kết quả ở dòng đáy. Bận thì bỏ qua.</summary>
        void Run(std::string title, Work work, bool needsOrigin = true);
        bool CanRun(bool needsOrigin, std::string& why) const;
        void ShowResult(ResultKind kind, std::string const& text);
        void SelectNozzle(int nozzle);
        void SelectStep(int step);
        /// <summary>Đọc ô nhập vào tham số hiệu chuẩn / bù (system.json); ô hỏng thì trả lại số cũ. Lưu nếu có thay đổi.</summary>
        void CommitInputs();
        void LoadInputs();
        void LoadTrim();
        void UpdateCalibView();
        Microsoft::UI::Xaml::Controls::Button StepButton(int i);
        Microsoft::UI::Xaml::Controls::Border StepBadge(int i);
        Microsoft::UI::Xaml::Controls::TextBlock StepBadgeText(int i);
        Microsoft::UI::Xaml::Controls::TextBlock StepStatus(int i);
        Microsoft::UI::Xaml::Controls::TextBlock StepLabel(int i);
        Microsoft::UI::Xaml::FrameworkElement StepPanel(int i);

        bool m_loaded{ false };
        bool m_visible{ false };
        bool m_closing{ false };
        std::atomic<bool> m_busy{ false };
        std::vector<Microsoft::UI::Xaml::Controls::Button> m_jogButtons;
        std::vector<Microsoft::UI::Xaml::Controls::Button> m_numpad;
        /// <summary>Chỉ số 0…6 = nozzle 1…7. Dựng trong code (winrt không có nút "rỗng" để đặt trong std::array).</summary>
        std::vector<Microsoft::UI::Xaml::Controls::Button> m_nozzleChips;
        std::vector<Microsoft::UI::Xaml::Shapes::Ellipse> m_nozzleDots;
        std::vector<Microsoft::UI::Xaml::Controls::TextBlock> m_summaryRows;

        int m_nozzle{ 1 };
        int m_step{ 0 };
        /// <summary>Chữ tiến trình đặt từ luồng nền (chuỗi 9 điểm) — Tick đưa lên dòng kết quả.</summary>
        std::shared_ptr<std::string> m_progress{ std::make_shared<std::string>() };
        std::shared_ptr<std::mutex> m_progressMutex{ std::make_shared<std::mutex>() };
        std::string m_runTitle;
        /// <summary>Tooltip đã đặt lần gần nhất — chỉ đặt lại khi chữ đổi.</summary>
        std::string m_gateTip, m_solutionTip;
        std::array<std::string, NozzleCount + 1> m_nozzleTips{};

        // Control VisionMaster: tạo lần đầu trang được mở, không tạo trong chế độ chụp ảnh.
        bool m_vmAttempted{ false };
        ui::VmPane m_globalTool;
        ui::VmPane m_config;
        /// <summary>VisionLoadCount lúc gắn trình soạn Flow vào solution lần gần nhất; -1 = chưa gắn.</summary>
        int m_boundLoadCount{ -1 };
        bool m_vmQueued{ false };
        bool m_vmBusy{ false };
    };
}

namespace winrt::ShieldCanLaser::factory_implementation
{
    struct VisionEditPage : VisionEditPageT<VisionEditPage, implementation::VisionEditPage>
    {
    };
}
