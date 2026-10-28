#pragma once
#include "MainWindow.g.h"

#include "Infor/AlarmData.h"
#include "Ui/AlarmTable.h"
#include "Ui/Dialogs.h"
#include "Ui/Page.h"

namespace winrt::ShieldCanLaser::implementation
{
    /// <summary>
    /// Shell: thanh tiêu đề (tên máy, trạng thái, toạ độ trục), điều hướng 7 trang, thanh trạng thái, banner
    /// alarm và bảng chi tiết alarm. Một DispatcherQueueTimer 100 ms đọc trạng thái từ Core và gọi OnTick() của
    /// trang đang hiện — thay cho binding tới McStatus (Bindable) của bản WPF.
    /// </summary>
    struct MainWindow : MainWindowT<MainWindow>
    {
        /// <summary>Thứ tự khớp thứ tự trang trong MainWindow.xaml.</summary>
        enum class Section { Main = 0, Manual, Teaching, Setting, EtherCat, VisionEdit, Alarm, Count };

        MainWindow();

        void OnRootLoaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTabClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnResetClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnUserClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        // ── Phân quyền: Vision Edit, EtherCAT cần đăng nhập Engineer (scs::UserAccess) ──
        /// <summary>Trang cần Engineer mà đang là Operator (chế độ chụp ảnh --shot: không khoá).</summary>
        bool IsLocked(Section section) const;
        /// <summary>Nút LOGIN/ENGINEER và biểu tượng khoá trên tab theo cấp hiện tại.</summary>
        void UpdateUserUi();
        /// <summary>Cấp đổi: về Operator mà đang ở trang khoá thì quay về Main.</summary>
        void OnUserLevelChanged();

        void ShowSection(Section section);
        void UpdateShell();
        void UpdateRealtimeStatus();
        void BuildAxisCells();
        ui::IPage* PageOf(Section section);
        Microsoft::UI::Xaml::UIElement ElementOf(Section section);
        Microsoft::UI::Xaml::Controls::Primitives::ToggleButton TabOf(Section section);

        // ── Banner alarm (nổi giữa cửa sổ, không chặn thao tác) và bảng chi tiết alarm (modal) ──
        void BuildBanner();
        void OnAlarmRaised(scs::AlarmData const& alarm);
        void UpdateBanner();
        void PositionBanner();
        void ToggleAlarmPanel();
        void UpdateAlarmPanel();

        void OnTick(Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&);
        void OnAppWindowChanged(Microsoft::UI::Windowing::AppWindow const&, Microsoft::UI::Windowing::AppWindowChangedEventArgs const&);
        void OnClosed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::WindowEventArgs const&);
        void Shutdown();
        winrt::fire_and_forget RunScreenshots();

        HWND m_hwnd{};
        Section m_section{ Section::Main };
        bool m_shutdown{ false };
        // DispatcherQueueTimer, KHÔNG phải DispatcherTimer của XAML: Tick của DispatcherTimer chạy bên trong vùng XAML chặn
        // gọi lồng, mà OnTick các trang có thể gọi control VisionMaster (bơm thông điệp COM) → XAML fail-fast 0xC000027B
        // (dump 06/10). Tick của DispatcherQueueTimer chạy ở vòng thông điệp, ngoài XAML.
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{ nullptr };
        bool m_inTick{ false };
        std::vector<Microsoft::UI::Xaml::Controls::TextBlock> m_axisValues;
        int m_lastTowerLamp{ -1 };
        int m_alarmTabState{ -1 };

        int m_modalToken{ 0 };
        int m_alarmRaisedToken{ 0 };
        int m_autoRefusedToken{ 0 };
        int m_userLevelToken{ 0 };

        // Banner: chỉ Error/Fatal mới bật; tự ẩn sau 10 giây nếu không có alarm mới; "Ẩn" thì ẩn ngay.
        Microsoft::UI::Xaml::Controls::Primitives::Popup m_bannerPopup{ nullptr };
        Microsoft::UI::Xaml::Controls::Border m_bannerCard{ nullptr };
        Microsoft::UI::Xaml::Controls::TextBlock m_bannerCount{ nullptr };
        Microsoft::UI::Xaml::Controls::TextBlock m_bannerText{ nullptr };
        Microsoft::UI::Xaml::DispatcherTimer m_bannerTimer{ nullptr };
        bool m_bannerVisible{ false };
        std::string m_bannerDisplay;

        ui::ModalOverlay m_alarmPanel;
        ui::AlarmTable m_alarmPanelTable;
        Microsoft::UI::Xaml::Controls::TextBlock m_alarmPanelMessage{ nullptr };
        uint64_t m_alarmPanelVersion{ ~0ull };
    };
}

namespace winrt::ShieldCanLaser::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
