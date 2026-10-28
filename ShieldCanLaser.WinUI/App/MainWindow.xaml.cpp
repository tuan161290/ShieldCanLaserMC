#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <fstream>

#include "AlarmPage.xaml.h"
#include "MainPage.xaml.h"
#include "ManualPage.xaml.h"
#include "SettingPage.xaml.h"
#include "EtherCatPage.xaml.h"
#include "TeachingPage.xaml.h"
#include "VisionEditPage.xaml.h"

#include "Base/Text.h"
#include "Communication/EtherCat/EtherCatBusManager.h"
#include "Controller/McControl.h"
#include "Data/McDataManager.h"
#include "Data/UserAccess.h"
#include "Infor/AlarmManager.h"
#include "Infor/AxisId.h"
#include "SysConfig/AppPaths.h"
#include "Ui/AppHost.h"
#include "Ui/JogControl.h"
#include "Ui/LoginDialog.h"
#include "Ui/StatusColors.h"
#include "Ui/UiUtil.h"
#include "Ui/VmPane.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;
using namespace winrt::Microsoft::UI::Xaml::Media;
namespace WF = winrt::Windows::Foundation;

namespace winrt::ShieldCanLaser::implementation
{
    MainWindow::MainWindow()
    {
        // InitializeComponent do mã sinh tự gọi sau khi dựng xong.
        ExtendsContentIntoTitleBar(true);
        AppWindow().TitleBar().PreferredHeightOption(Microsoft::UI::Windowing::TitleBarHeightOption::Tall);
        AppWindow().Changed({ this, &MainWindow::OnAppWindowChanged });
        Closed({ this, &MainWindow::OnClosed });
    }

    void MainWindow::OnRootLoaded(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_hwnd) return;
        auto native = this->m_inner.as<::IWindowNative>();
        native->get_WindowHandle(&m_hwnd);
        ui::SetMainHwnd(m_hwnd);

        SetTitleBar(TitleBarArea());
        ui::ModalOverlay::Attach(Root().XamlRoot());
        m_modalToken = ui::ModalOverlay::ModalChanged().Subscribe([this](bool open) {
            if (open) ui::StopActiveJogs();   // lớp phủ che nút jog trước khi kịp nhả
            if (auto page = PageOf(m_section)) page->OnModalChanged(open);
        });

        std::string dataRoot = "Data: " + ::base::PathToU8(scs::AppPaths::Root());
        ui::SetText(DataRootText(), dataRoot);
        BuildAxisCells();
        BuildBanner();

        // AlarmRaised chỉ bắn cho alarm THẬT SỰ mới (không bắn khi raise lại alarm cũ) — đúng ý "có việc mới
        // thì hiện lại". Bắn từ luồng nền (vòng quét an toàn) nên phải chuyển về luồng UI.
        auto weak = get_weak();
        m_alarmRaisedToken = scs::AlarmManager::AlarmRaised().Subscribe([weak](scs::AlarmData alarm) {
            ui::PostUi([weak, alarm] {
                if (auto self = weak.get()) self->OnAlarmRaised(alarm);
            });
        });

        // Bấm AUTO mà chưa đủ điều kiện: máy ở lại MANUAL, bật hộp thoại nêu lý do (Tuấn 2026-09-30).
        m_autoRefusedToken = scs::McControl::AutoModeRefused().Subscribe([](std::string why) {
            ui::Dialogs::Warn(std::move(why), "Cannot enter AUTO");
        });

        // Phân quyền: cấp đổi (đăng nhập / đăng xuất / tự đăng xuất) → cập nhật nút, khoá tab. Mọi lần chạm chuột / phím
        // (kể cả đã được control xử lý) đặt lại đồng hồ tự đăng xuất.
        m_userLevelToken = scs::UserAccess::LevelChanged().Subscribe([weak](scs::EUserLevel) {
            ui::PostUi([weak] {
                if (auto self = weak.get()) self->OnUserLevelChanged();
            });
        });
        Root().AddHandler(UIElement::PointerPressedEvent(),
                          box_value(Input::PointerEventHandler([](auto&&, auto&&) { scs::UserAccess::Touch(); })), true);
        Root().AddHandler(UIElement::KeyDownEvent(),
                          box_value(Input::KeyEventHandler([](auto&&, auto&&) { scs::UserAccess::Touch(); })), true);
        Root().AddHandler(UIElement::PointerWheelChangedEvent(),
                          box_value(Input::PointerEventHandler([](auto&&, auto&&) { scs::UserAccess::Touch(); })), true);

        ShowSection(Section::Main);
        UpdateShell();
        UpdateUserUi();

        m_timer = DispatcherQueue().CreateTimer();
        m_timer.Interval(std::chrono::milliseconds(100));
        m_timer.IsRepeating(true);
        m_timer.Tick([this](auto&&, auto&&) { OnTick(nullptr, nullptr); });
        m_timer.Start();

        if (ui::AppHost::Options().ScreenshotDir.empty())
        {
            if (auto presenter = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>())
                presenter.Maximize();
        }
        else
        {
            // Mica không thuộc cây XAML nên không vào ảnh chụp — tô nền đặc cho ảnh giống thật.
            Root().Background(ui::Res(L"BackgroundBrush"));
            RunScreenshots();
        }
    }

    // Mỗi trục một ô rộng cố định: tên trên, toạ độ dưới.
    void MainWindow::BuildAxisCells()
    {
        AxisPanel().Children().Clear();
        m_axisValues.clear();
        for (const char* axisName : scs::AxisId::All)
        {
            Border cell;
            cell.Width(56);
            cell.BorderBrush(ui::Res(L"DividerBrush"));
            cell.BorderThickness(ThicknessHelper::FromLengths(1, 0, 0, 0));
            cell.Padding(ThicknessHelper::FromLengths(0, 1, 0, 1));

            StackPanel stack;
            TextBlock name;
            name.Text(ui::H(axisName));
            name.FontSize(11);
            name.HorizontalAlignment(HorizontalAlignment::Center);
            name.Foreground(ui::Res(L"TextSecondaryBrush"));
            TextBlock value;
            value.Text(L"0.000");
            value.FontSize(13);
            value.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            value.TextAlignment(TextAlignment::Center);
            value.HorizontalAlignment(HorizontalAlignment::Stretch);
            stack.Children().Append(name);
            stack.Children().Append(value);
            cell.Child(stack);

            AxisPanel().Children().Append(cell);
            m_axisValues.push_back(value);
        }
    }

    ui::IPage* MainWindow::PageOf(Section section)
    {
        switch (section)
        {
        case Section::Main:       return get_self<MainPage>(PageMain());
        case Section::Manual:     return get_self<ManualPage>(PageManual());
        case Section::Teaching:   return get_self<TeachingPage>(PageTeaching());
        case Section::Setting:    return get_self<SettingPage>(PageSetting());
        case Section::EtherCat:   return get_self<EtherCatPage>(PageEtherCat());
        case Section::VisionEdit: return get_self<VisionEditPage>(PageVisionEdit());
        case Section::Alarm:      return get_self<AlarmPage>(PageAlarm());
        default:                  return nullptr;
        }
    }

    UIElement MainWindow::ElementOf(Section section)
    {
        switch (section)
        {
        case Section::Main:       return PageMain();
        case Section::Manual:     return PageManual();
        case Section::Teaching:   return PageTeaching();
        case Section::Setting:    return PageSetting();
        case Section::EtherCat:   return PageEtherCat();
        case Section::VisionEdit: return PageVisionEdit();
        case Section::Alarm:      return PageAlarm();
        default:                  return nullptr;
        }
    }

    ToggleButton MainWindow::TabOf(Section section)
    {
        switch (section)
        {
        case Section::Main:       return TabMain();
        case Section::Manual:     return TabManual();
        case Section::Teaching:   return TabTeaching();
        case Section::Setting:    return TabSetting();
        case Section::EtherCat:   return TabEtherCat();
        case Section::VisionEdit: return TabVisionEdit();
        case Section::Alarm:      return TabAlarm();
        default:                  return nullptr;
        }
    }

    void MainWindow::ShowSection(Section section)
    {
        Section previous = m_section;
        m_section = section;

        for (int i = 0; i < (int)Section::Count; ++i)
        {
            Section s = (Section)i;
            ui::Show(ElementOf(s), s == section);
            // Bấm vào tab đang chọn làm ToggleButton tự bỏ chọn — luôn đặt lại theo trang thật.
            TabOf(s).IsChecked(s == section);
        }

        if (previous != section)
        {
            ui::StopActiveJogs();   // nút jog của trang cũ có thể đang được giữ
            if (auto page = PageOf(previous)) page->OnShown(false);
        }
        if (auto page = PageOf(section))
        {
            page->OnShown(true);
            page->OnTick();
        }
    }

    void MainWindow::OnTabClick(WF::IInspectable const& sender, RoutedEventArgs const&)
    {
        auto tab = sender.as<ToggleButton>();
        for (int i = 0; i < (int)Section::Count; ++i)
        {
            if (TabOf((Section)i) != tab) continue;
            Section target = (Section)i;
            if (IsLocked(target))
            {
                ShowSection(m_section);   // ToggleButton đã tự đổi trạng thái chọn — trả lại theo trang đang hiện
                auto weak = get_weak();
                ui::LoginDialog::ShowLogin([weak, target](bool ok) {
                    if (auto self = weak.get(); self && ok) self->ShowSection(target);
                });
                return;
            }
            ShowSection(target);
            return;
        }
    }

    bool MainWindow::IsLocked(Section section) const
    {
        if (!ui::AppHost::Options().ScreenshotDir.empty()) return false;
        bool needsEngineer = section == Section::VisionEdit || section == Section::EtherCat;
        return needsEngineer && !scs::UserAccess::IsEngineer();
    }

    void MainWindow::UpdateUserUi()
    {
        bool engineer = scs::UserAccess::IsEngineer();
        ui::SetText(UserText(), engineer ? std::string("ENGINEER") : std::string("LOGIN"));
        UserIcon().Glyph(engineer ? L"\xE77B" : L"\xE72E");   // Contact / Lock
        ui::SetTint(UserButton(), engineer ? ui::Tint::Blue : ui::Tint::None);
        ToolTipService::SetToolTip(UserButton(), box_value(ui::H(engineer
            ? std::format("Logged in as Engineer — auto logout after {} min without activity. Click: change password / logout.",
                          scs::UserAccess::IdleLogoutMinutes())
            : std::string("Login as Engineer to unlock Vision Edit and EtherCAT"))));
        ui::Show(EtherCatLock(), IsLocked(Section::EtherCat));
        ui::Show(VisionEditLock(), IsLocked(Section::VisionEdit));
    }

    void MainWindow::OnUserLevelChanged()
    {
        if (m_shutdown) return;
        UpdateUserUi();
        if (IsLocked(m_section))
        {
            ShowSection(Section::Main);
            scs::McControl::Status().SetMessage("Engineer logged out — locked page closed");
        }
    }

    void MainWindow::OnUserClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (!scs::UserAccess::IsEngineer())
        {
            ui::LoginDialog::ShowLogin();
            return;
        }
        MenuFlyout menu;
        MenuFlyoutItem change;
        change.Text(L"CHANGE PASSWORD");
        change.Icon([] { FontIcon i; i.Glyph(L"\xE8D7"); return i; }());   // Permissions
        change.Click([](auto&&, auto&&) { ui::LoginDialog::ShowChangePassword(); });
        MenuFlyoutItem logout;
        logout.Text(L"LOGOUT");
        logout.Icon([] { FontIcon i; i.Glyph(L"\xF3B1"); return i; }());   // Leave
        logout.Click([](auto&&, auto&&) { scs::UserAccess::Logout("LOGOUT button"); });
        menu.Items().Append(change);
        menu.Items().Append(logout);
        menu.ShowAt(UserButton());
    }

    // RESET — chung một đường với nút RESET trong bảng alarm và nút RESET vật lý (McControl::RequestReset).
    void MainWindow::OnResetClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::RequestReset();
    }

    void MainWindow::UpdateShell()
    {
        scs::McStatus& status = scs::McControl::Status();

        ui::SetText(MachineNameText(), scs::McDataManager::SysData()->MachineName);
        ui::SetText(ModelText(), "Model: " + status.ModelName());

        // Nhãn trạng thái máy — màu đèn tháp (nguồn sự thật, cùng màu đèn tháp vật lý).
        scs::ETowerLamp lamp = status.TowerLamp();
        if (m_lastTowerLamp != (int)lamp)
        {
            m_lastTowerLamp = (int)lamp;
            auto solid = ui::Solid(ui::TowerLampColor(lamp));
            StatePill().Background(ui::Solid(ui::TowerLampTintColor(lamp)));
            StatePill().BorderBrush(solid);
            StateDot().Fill(solid);
            StateText().Foreground(solid);
        }
        ui::SetText(StateText(), status.StateText());

        // Toạ độ 14 trục.
        const auto& axes = scs::McControl::Motion().AxisList();
        for (size_t i = 0; i < m_axisValues.size() && i < axes.size(); ++i)
            ui::SetText(m_axisValues[i], ui::Num(axes[i]->CurrentPos(), 3));

        // Thanh trạng thái.
        ui::Show(SimBadge(), status.Simulation());
        MotionLamp().Fill(ui::StatusBrush(status.MotionConnected()));
        IoLamp().Fill(ui::StatusBrush(status.IoConnected()));
        VisionLamp().Fill(ui::StatusBrush(status.VisionConnected()));
        UpdateRealtimeStatus();

        bool hasAlarms = scs::AlarmManager::ActiveCount() > 0;

        // RESET — cam đặc khi không có alarm, đỏ khi có.
        ui::SetTint(ResetButton(), hasAlarms ? ui::Tint::Red : ui::Tint::Caution);

        // Tab Alarm — có alarm thì nền đỏ + quầng sáng, chữ trắng.
        if (m_alarmTabState != (hasAlarms ? 1 : 0))
        {
            m_alarmTabState = hasAlarms ? 1 : 0;
            AlarmTabFill().Background(hasAlarms ? ui::Res(L"RedOnBrush") : nullptr);
            ui::SetGlow(AlarmTabGlow(), hasAlarms ? 0xFFFF4D5E : 0);
            TabAlarm().Foreground(hasAlarms ? ui::Res(L"TextOnAccentBrush") : ui::Solid(0xFF000000));
        }

        UpdateBanner();
        UpdateAlarmPanel();
    }

    // Luồng chu kỳ EtherCAT trên core dành riêng: CPU đang chạy + jitter thực đo. Xanh = đang ghim đúng core,
    // không lỡ nhịp nào; vàng = có lỡ nhịp/đổi CPU/mất khung (chi tiết ở trang EtherCAT → Luồng chu kỳ).
    void MainWindow::UpdateRealtimeStatus()
    {
        auto bus = scs::McControl::Motion().Bus();
        ecm::rt::RtStats rt{};
        if (bus && bus->IsConnected()) rt = bus->RtStats();
        bool running = rt.Cycles > 0;
        ui::Show(RtPanel(), running);
        if (!running) return;

        bool healthy = rt.Overruns == 0 && rt.CpuMigrations == 0 && rt.WkcErrors == 0 && (rt.Pinned || !rt.CoreReserved);
        RtLamp().Fill(healthy ? ui::Res(L"SuccessBrush") : ui::Res(L"CautionBrush"));

        std::string text = rt.Pinned ? std::format("EtherCAT RT · dedicated CPU {}", rt.CpuIndex)
                                     : std::string("EtherCAT RT · core not pinned");
        text += std::format(" · cycle {:.1f} ms · jitter {:+.0f}…{:+.0f} µs", rt.PeriodUs / 1000.0,
                            rt.PeriodJitterMinUs, rt.PeriodJitterMaxUs);
        if (rt.Overruns > 0) text += std::format(" · overruns {}", rt.Overruns);
        if (rt.WkcErrors > 0) text += std::format(" · frame errors {}", rt.WkcErrors);
        ui::SetText(RtText(), text);
    }

    // ══════════ Banner alarm — nổi giữa cửa sổ, không chiếm chỗ trong bố cục ══════════
    // Chỉ to bằng nội dung, không có lớp nền phủ, nên phần còn lại vẫn bấm được — khác bảng chi tiết, CỐ Ý chặn
    // hết để bắt xử lý ngay. Nằm trong Popup không ràng buộc để nổi trên cả control VisionMaster.
    void MainWindow::BuildBanner()
    {
        m_bannerCard = Border();
        m_bannerCard.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
        m_bannerCard.Padding(ThicknessHelper::FromLengths(16, 12, 16, 12));
        m_bannerCard.MinWidth(460);
        m_bannerCard.MaxWidth(900);
        m_bannerCard.Background(ui::Res(L"CriticalBrush"));

        Grid layout;
        ColumnDefinition iconColumn; iconColumn.Width(GridLengthHelper::Auto());
        ColumnDefinition textColumn; textColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        ColumnDefinition buttonColumn; buttonColumn.Width(GridLengthHelper::Auto());
        layout.ColumnDefinitions().Append(iconColumn);
        layout.ColumnDefinitions().Append(textColumn);
        layout.ColumnDefinitions().Append(buttonColumn);

        FontIcon icon;
        icon.Glyph(L"\xE7BA");
        icon.FontSize(22);
        icon.Foreground(ui::Solid(0xFFFFFFFF));
        icon.Margin(ThicknessHelper::FromLengths(0, 0, 12, 0));
        icon.VerticalAlignment(VerticalAlignment::Center);

        StackPanel texts;
        texts.VerticalAlignment(VerticalAlignment::Center);
        m_bannerCount = TextBlock();
        m_bannerCount.Foreground(ui::Solid(0xDDFFFFFF));
        m_bannerCount.FontSize(12);
        m_bannerText = TextBlock();
        m_bannerText.Foreground(ui::Solid(0xFFFFFFFF));
        m_bannerText.FontSize(15);
        m_bannerText.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        m_bannerText.TextTrimming(TextTrimming::CharacterEllipsis);
        texts.Children().Append(m_bannerCount);
        texts.Children().Append(m_bannerText);

        StackPanel buttons;
        buttons.Orientation(Orientation::Horizontal);
        buttons.Margin(ThicknessHelper::FromLengths(16, 0, 0, 0));
        buttons.Spacing(8);
        Button detail;
        detail.Content(box_value(L"DETAILS"));
        detail.Width(96);
        detail.Click([this](auto&&, auto&&) { ToggleAlarmPanel(); });
        Button hide;
        hide.Content(box_value(L"HIDE"));
        hide.Width(64);
        ToolTipService::SetToolTip(hide, box_value(
            L"Hide this banner — alarms stay active, only hidden from the screen. Reappears on a new alarm."));
        hide.Click([this](auto&&, auto&&) {
            if (m_bannerTimer) m_bannerTimer.Stop();
            m_bannerVisible = false;
            UpdateBanner();
        });
        buttons.Children().Append(detail);
        buttons.Children().Append(hide);

        Grid::SetColumn(icon, 0);
        Grid::SetColumn(texts, 1);
        Grid::SetColumn(buttons, 2);
        layout.Children().Append(icon);
        layout.Children().Append(texts);
        layout.Children().Append(buttons);
        m_bannerCard.Child(layout);
        m_bannerCard.SizeChanged([this](auto&&, auto&&) { PositionBanner(); });

        // Popup là cây hiển thị riêng, không thừa hưởng font của cửa sổ chính.
        ContentControl host;
        host.FontFamily(FontFamily(L"Segoe UI"));
        host.Content(m_bannerCard);
        host.Shadow(ThemeShadow());
        host.Translation({ 0, 0, 28 });

        m_bannerPopup = Popup();
        m_bannerPopup.XamlRoot(Root().XamlRoot());
        m_bannerPopup.ShouldConstrainToRootBounds(false);
        m_bannerPopup.IsLightDismissEnabled(false);
        m_bannerPopup.Child(host);

        // Banner tự ẩn sau chừng này nếu không có alarm mới — đủ để đọc, không đứng chắn thao tác lâu.
        m_bannerTimer = DispatcherTimer();
        m_bannerTimer.Interval(std::chrono::seconds(10));
        m_bannerTimer.Tick([this](auto&&, auto&&) {
            m_bannerTimer.Stop();
            m_bannerVisible = false;
            UpdateBanner();
        });
    }

    void MainWindow::OnAlarmRaised(scs::AlarmData const& alarm)
    {
        if (m_shutdown) return;
        // Chỉ Error/Fatal mới bật banner (Tuấn 2026-09-29): Warning thì máy vẫn chạy tiếp, không cần nổi lên
        // che màn hình — vẫn nằm trong danh sách alarm và nút RESET vẫn sáng.
        if (alarm.Level < scs::EAlarmLevel::Error) return;
        m_bannerDisplay = alarm.Display();   // banner hiện đúng alarm vừa bật nó, không phải dòng đầu danh sách
        m_bannerVisible = true;
        m_bannerTimer.Stop();
        m_bannerTimer.Start();
        UpdateBanner();
    }

    void MainWindow::UpdateBanner()
    {
        if (!m_bannerPopup) return;
        int count = scs::AlarmManager::ActiveCount();
        // Điều kiện hiện thật sự: còn alarm VÀ chưa bị ẩn/hết giờ. Chế độ chụp ảnh không mở cửa sổ nổi nào.
        bool show = count > 0 && m_bannerVisible && ui::AppHost::Options().ScreenshotDir.empty();
        if (show)
        {
            ui::SetText(m_bannerCount, std::format("{} active alarm(s)", count));
            ui::SetText(m_bannerText, m_bannerDisplay);
        }
        if (m_bannerPopup.IsOpen() != show)
        {
            m_bannerPopup.IsOpen(show);
            if (show) PositionBanner();
        }
    }

    void MainWindow::PositionBanner()
    {
        if (!m_bannerPopup || !m_bannerPopup.IsOpen()) return;
        auto size = Root().XamlRoot().Size();
        double width = m_bannerCard.ActualWidth() > 0 ? m_bannerCard.ActualWidth() : m_bannerCard.MinWidth();
        double height = m_bannerCard.ActualHeight() > 0 ? m_bannerCard.ActualHeight() : 64.0;
        m_bannerPopup.HorizontalOffset(std::max(0.0, (size.Width - width) / 2));
        m_bannerPopup.VerticalOffset(std::max(0.0, (size.Height - height) / 2));
    }

    // ══════════ Bảng chi tiết alarm — kiểu ContentDialog Windows 11 ══════════
    // Phần nội dung nền trắng; dải nút dưới cùng nền lớp 0, ngăn bằng đường kẻ mảnh.
    void MainWindow::ToggleAlarmPanel()
    {
        if (m_alarmPanel.IsOpen())
        {
            m_alarmPanel.Close();
            return;
        }

        Border card;
        card.Width(920);
        card.Height(580);
        card.Background(ui::Res(L"SurfaceBrush"));
        card.BorderBrush(ui::Res(L"CardStrokeBrush"));
        card.BorderThickness(ThicknessHelper::FromUniformLength(1));
        card.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));

        Grid layout;
        RowDefinition titleRow; titleRow.Height(GridLengthHelper::Auto());
        RowDefinition tableRow; tableRow.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        RowDefinition barRow; barRow.Height(GridLengthHelper::Auto());
        layout.RowDefinitions().Append(titleRow);
        layout.RowDefinitions().Append(tableRow);
        layout.RowDefinitions().Append(barRow);

        TextBlock title;
        title.Text(L"Active Alarms");
        title.FontSize(20);
        title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        title.Margin(ThicknessHelper::FromLengths(24, 20, 24, 12));

        m_alarmPanelTable = ui::AlarmTable();
        FrameworkElement table = m_alarmPanelTable.Build(ui::AlarmTableKind::Active);
        table.Margin(ThicknessHelper::FromLengths(16, 0, 16, 16));

        Border bar;
        bar.Background(ui::Res(L"BackgroundBrush"));
        bar.BorderBrush(ui::Res(L"DividerBrush"));
        bar.BorderThickness(ThicknessHelper::FromLengths(0, 1, 0, 0));
        bar.CornerRadius(CornerRadius{ 0, 0, 8, 8 });
        bar.Padding(ThicknessHelper::FromLengths(24, 16, 24, 16));
        Grid barLayout;
        ColumnDefinition messageColumn; messageColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        ColumnDefinition actionColumn; actionColumn.Width(GridLengthHelper::Auto());
        barLayout.ColumnDefinitions().Append(messageColumn);
        barLayout.ColumnDefinitions().Append(actionColumn);
        m_alarmPanelMessage = TextBlock();
        m_alarmPanelMessage.VerticalAlignment(VerticalAlignment::Center);
        m_alarmPanelMessage.Foreground(ui::Res(L"CriticalBrush"));
        m_alarmPanelMessage.Margin(ThicknessHelper::FromLengths(0, 0, 16, 0));
        m_alarmPanelMessage.TextWrapping(TextWrapping::Wrap);
        StackPanel actions;
        actions.Orientation(Orientation::Horizontal);
        actions.Spacing(8);
        Button reset;
        reset.Content(box_value(L"RESET"));
        reset.Width(120);
        reset.Style(ui::ResStyle(L"PrimaryButton"));
        reset.Click([](auto&&, auto&&) { scs::McControl::RequestReset(); });
        Button close;
        close.Content(box_value(L"CLOSE"));
        close.Width(100);
        close.Click([this](auto&&, auto&&) { m_alarmPanel.Close(); });
        actions.Children().Append(reset);
        actions.Children().Append(close);
        Grid::SetColumn(m_alarmPanelMessage, 0);
        Grid::SetColumn(actions, 1);
        barLayout.Children().Append(m_alarmPanelMessage);
        barLayout.Children().Append(actions);
        bar.Child(barLayout);

        Grid::SetRow(title, 0);
        Grid::SetRow(table, 1);
        Grid::SetRow(bar, 2);
        layout.Children().Append(title);
        layout.Children().Append(table);
        layout.Children().Append(bar);
        card.Child(layout);

        m_alarmPanelVersion = ~0ull;
        m_alarmPanel.Open(card);
        UpdateAlarmPanel();
    }

    void MainWindow::UpdateAlarmPanel()
    {
        if (!m_alarmPanel.IsOpen()) return;
        uint64_t version = scs::AlarmManager::Version();
        if (version != m_alarmPanelVersion)
        {
            m_alarmPanelVersion = version;
            m_alarmPanelTable.Update(scs::AlarmManager::ActiveAlarms());
        }
        if (m_alarmPanelMessage) ui::SetText(m_alarmPanelMessage, scs::McControl::Status().Message());
    }

    void MainWindow::OnTick(WF::IInspectable const&, WF::IInspectable const&)
    {
        // Một lời gọi trong nhịp (vd. control VisionMaster) bơm thông điệp thì nhịp kế có thể tới lồng vào: bỏ nhịp đó.
        if (m_shutdown || m_inTick) return;
        m_inTick = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{ m_inTick };
        UpdateShell();
        scs::UserAccess::CheckIdle();   // quá hạn → LevelChanged → OnUserLevelChanged (đóng trang khoá)
        if (auto page = PageOf(m_section)) page->OnTick();
    }

    void MainWindow::OnAppWindowChanged(Microsoft::UI::Windowing::AppWindow const&,
                                        Microsoft::UI::Windowing::AppWindowChangedEventArgs const& e)
    {
        if (!m_hwnd || m_shutdown) return;
        if (e.DidPositionChange() || e.DidSizeChange())
        {
            // Popup sở hữu (control VisionMaster, lớp phủ) không tự đi theo cửa sổ chủ.
            if (auto page = PageOf(m_section)) page->OnWindowMoved();
            m_alarmPanel.Resize();
            PositionBanner();
        }
    }

    void MainWindow::Shutdown()
    {
        if (m_shutdown) return;
        m_shutdown = true;

        if (m_timer)
        {
            m_timer.Stop();
            m_timer = nullptr;
        }
        if (m_bannerTimer) m_bannerTimer.Stop();
        scs::AlarmManager::AlarmRaised().Unsubscribe(m_alarmRaisedToken);
        scs::McControl::AutoModeRefused().Unsubscribe(m_autoRefusedToken);
        scs::UserAccess::LevelChanged().Unsubscribe(m_userLevelToken);
        ui::ModalOverlay::ModalChanged().Unsubscribe(m_modalToken);

        // Trình tự tắt đã kiểm chứng ở VmWinUiProbe: popup XAML → control VisionMaster → SDK → thoát.
        if (m_bannerPopup) m_bannerPopup.IsOpen(false);
        m_alarmPanel.Close();
        ui::Dialogs::CloseAll();

        for (int i = 0; i < (int)Section::Count; ++i)
            if (auto page = PageOf((Section)i)) page->OnClosing();

        ui::AppHost::Shutdown();
    }

    void MainWindow::OnClosed(WF::IInspectable const&, WindowEventArgs const&)
    {
        Shutdown();
    }

    // --shot <thư mục>: chụp từng trang bằng RenderTargetBitmap (không cần cửa sổ nằm trong màn hình,
    // không đụng chuột/bàn phím) rồi tự thoát. Trang có nhiều trạng thái hiển thị (IPage::ShotVariants) được chụp
    // thêm <tên>-2, <tên>-3, …
    winrt::fire_and_forget MainWindow::RunScreenshots()
    {
        using namespace winrt::Windows::Graphics::Imaging;
        using namespace winrt::Windows::Storage::Streams;
        using winrt::Microsoft::UI::Xaml::Media::Imaging::RenderTargetBitmap;

        auto lifetime = get_strong();
        winrt::apartment_context uiContext;
        auto dir = ::base::U8Path(ui::AppHost::Options().ScreenshotDir);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        static const wchar_t* names[] = { L"1-main", L"2-manual", L"3-teaching", L"4-setting", L"5-ethercat", L"6-visionedit", L"7-alarm" };
        for (int i = 0; i < (int)Section::Count; ++i)
        {
            auto page = PageOf((Section)i);
            int variants = page ? page->ShotVariants() : 1;
            for (int variant = 0; variant < variants; ++variant)
            {
                if (variant == 0) ShowSection((Section)i);
                else page->ShowShotVariant(variant);
                co_await winrt::resume_after(std::chrono::milliseconds(variant == 0 ? 1200 : 400));
                co_await uiContext;

                RenderTargetBitmap bitmap;
                co_await bitmap.RenderAsync(Root());
                IBuffer pixels = co_await bitmap.GetPixelsAsync();

                InMemoryRandomAccessStream stream;
                BitmapEncoder encoder = co_await BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId(), stream);
                encoder.SetPixelData(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied,
                                     (uint32_t)bitmap.PixelWidth(), (uint32_t)bitmap.PixelHeight(), 96.0, 96.0,
                                     winrt::array_view<const uint8_t>(pixels.data(), pixels.data() + pixels.Length()));
                co_await encoder.FlushAsync();

                DataReader reader(stream.GetInputStreamAt(0));
                uint32_t size = (uint32_t)stream.Size();
                co_await reader.LoadAsync(size);
                std::vector<uint8_t> bytes(size);
                reader.ReadBytes(bytes);

                std::wstring file = std::wstring(names[i]) + (variant == 0 ? L"" : L"-" + std::to_wstring(variant + 1)) + L".png";
                std::ofstream out(dir / file, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
            }
        }

        co_await uiContext;
        Close();
    }
}
