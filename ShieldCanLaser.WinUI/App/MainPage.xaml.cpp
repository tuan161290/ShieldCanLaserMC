#include "pch.h"
#include "MainPage.xaml.h"
#if __has_include("MainPage.g.cpp")
#include "MainPage.g.cpp"
#endif

#include <algorithm>
#include <cstring>
#include <format>

#include "Base/Text.h"
#include "Communication/Sim/FakeIo.h"
#include "Controller/McControl.h"
#include "Controller/Routine_Auto.h"
#include "Data/McDataManager.h"
#include "Infor/AlarmManager.h"
#include "Infor/IoMap.h"
#include "Ui/AppHost.h"
#include "Ui/Dialogs.h"
#include "Ui/UiUtil.h"
#include "Utility/DateTimeText.h"
#include "Utility/Log/LogWorker.h"
#include "Vision/VisionMasterDevice.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Animation;
using winrt::Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap;
// Không "using …::Ellipse": trùng tên hàm GDI ::Ellipse của wingdi.h.
namespace WF = winrt::Windows::Foundation;

namespace
{
    /// <summary>Ảnh của Vision (Gray8 / BGR24 / BGRA32, có Stride) → BGRA8 liền dòng cho WriteableBitmap. Ảnh camera là
    /// ảnh đục: kênh alpha luôn đặt 255 (ảnh 4 kênh từ nguồn kiểu BGRX có alpha = 0 sẽ thành trong suốt).
    /// False = ảnh hỏng (kích thước/stride/số kênh không hợp lệ).</summary>
    bool ToBgra(const scs::ImageData& img, std::vector<uint8_t>& out)
    {
        const int w = img.Width, h = img.Height, c = img.Channels;
        if (w <= 0 || h <= 0 || (c != 1 && c != 3 && c != 4)) return false;
        const size_t row = (size_t)w * (size_t)c;
        const size_t stride = img.Stride > 0 ? (size_t)img.Stride : row;
        if (stride < row || img.Pixels.size() < stride * (size_t)(h - 1) + row) return false;

        out.resize((size_t)w * (size_t)h * 4);
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* s = img.Pixels.data() + stride * (size_t)y;
            uint8_t* d = out.data() + (size_t)y * (size_t)w * 4;
            switch (c)
            {
            case 1:
                for (int x = 0; x < w; ++x, ++s, d += 4) { d[0] = d[1] = d[2] = *s; d[3] = 255; }
                break;
            case 3:
                for (int x = 0; x < w; ++x, s += 3, d += 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
                break;
            default:
                for (int x = 0; x < w; ++x, s += 4, d += 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
                break;
            }
        }
        return true;
    }

    bool SameLog(const scs::LogData& a, const scs::LogData& b)
    {
        return a.UpdateTime == b.UpdateTime && a.Level == b.Level && a.Source == b.Source && a.Message == b.Message;
    }

    /// <summary>Một thuộc tính của hiệu ứng ẩn / hiện card Log. EnableDependentAnimation cho Width (đổi bố cục); Opacity, Angle
    /// không cần cờ này và vẫn chạy độc lập như thường.</summary>
    DoubleAnimation LogSlide(DependencyObject const& target, wchar_t const* property, double from, double to)
    {
        CubicEase ease;
        ease.EasingMode(EasingMode::EaseOut);
        DoubleAnimation a;
        a.From(WF::IReference<double>(from));
        a.To(WF::IReference<double>(to));
        a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(240)));
        a.EasingFunction(ease);
        a.EnableDependentAnimation(true);
        Storyboard::SetTarget(a, target);
        Storyboard::SetTargetProperty(a, property);
        return a;
    }

    /// <summary>Một dòng nhật ký — DataTemplate LogData của bản WPF: vạch trái 3 px + giờ (Consolas) + nguồn, dưới là
    /// nội dung. Màu theo mức (Tuấn 02/10): Info vạch accent; Warning vạch #E8A317, chữ #9D5D00; Error vạch #E94949,
    /// chữ #C42B1C đậm; Debug vạch #A0A0A0, nội dung #707070.</summary>
    FrameworkElement MakeLogRow(const scs::LogData& d)
    {
        Brush bar = ui::Res(L"AccentBrush");
        Brush source = ui::Res(L"TextSecondaryBrush");
        Brush message{ nullptr };
        bool bold = false;
        switch (d.Level)
        {
        case scs::LogLevel::Warning:
            bar = ui::Solid(0xFFE8A317);
            source = ui::Solid(0xFF9D5D00);
            message = ui::Solid(0xFF9D5D00);
            break;
        case scs::LogLevel::Error:
            bar = ui::Solid(0xFFE94949);
            source = ui::Solid(0xFFC42B1C);
            message = ui::Solid(0xFFC42B1C);
            bold = true;
            break;
        case scs::LogLevel::Debug:
            bar = ui::Solid(0xFFA0A0A0);
            message = ui::Solid(0xFF707070);
            break;
        default:
            break;
        }

        TextBlock time;
        time.Text(ui::H(scs::FormatLocal(d.UpdateTime, "HH:mm:ss")));
        time.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Consolas"));
        time.FontSize(11);
        time.Foreground(ui::Res(L"TextTertiaryBrush"));
        time.VerticalAlignment(VerticalAlignment::Center);

        TextBlock src;
        src.Text(ui::H(d.Source));
        src.FontSize(11);
        src.Margin(ThicknessHelper::FromLengths(6, 0, 0, 0));
        src.Foreground(source);
        src.VerticalAlignment(VerticalAlignment::Center);

        StackPanel head;
        head.Orientation(Orientation::Horizontal);
        head.Children().Append(time);
        head.Children().Append(src);

        TextBlock msg;
        msg.Text(ui::H(d.Message));
        msg.TextWrapping(TextWrapping::Wrap);
        msg.FontSize(12);
        if (message) msg.Foreground(message);
        if (bold) msg.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());

        StackPanel stack;
        stack.Children().Append(head);
        stack.Children().Append(msg);

        Border barBorder;
        barBorder.BorderThickness(ThicknessHelper::FromLengths(3, 0, 0, 0));
        barBorder.BorderBrush(bar);
        barBorder.Padding(ThicknessHelper::FromLengths(7, 0, 0, 0));
        barBorder.CornerRadius(CornerRadiusHelper::FromUniformRadius(1));
        barBorder.Child(stack);

        // Lề của ListBoxItem bản WPF (Padding 6,3).
        Border item;
        item.Padding(ThicknessHelper::FromLengths(6, 3, 6, 3));
        item.Child(barBorder);
        return item;
    }

    /// <summary>Đường dẫn Flow/module của khung CAM. Ô ở Setting (RenderCam) ghi đường dẫn đầy đủ: "Flow1" = cả Flow,
    /// "Flow1.Module" = module trong Flow. Trống: cả Flow kiểm tra (phần Flow của đường dẫn Flow.Group).</summary>
    std::string CurrentRenderPath()
    {
        auto sys = scs::McDataManager::SysData();
        const scs::VisionOutputNames& n = sys->VisionOutputs;
        return scs::VisionOutputNames::RenderPath(n.InspectFlow(), n.RenderCam);
    }

    std::string BindKey(int loadCount, const std::string& path) { return std::to_string(loadCount) + "|" + path; }

    wchar_t const* CellBrushKey(int state)
    {
        switch ((scs::ECellState)state)
        {
        case scs::ECellState::Loaded:     return L"TrayCellLoadedBrush";
        case scs::ECellState::Processing: return L"TrayCellProcessingBrush";
        case scs::ECellState::Ok:         return L"TrayCellOkBrush";
        case scs::ECellState::Ng:         return L"TrayCellNgBrush";
        case scs::ECellState::Skipped:    return L"TrayCellSkippedBrush";
        default:                          return L"TrayCellLoadedBrush";
        }
    }

    int CountOf(std::vector<int> const& cells, scs::ECellState state)
    {
        return (int)std::count(cells.begin(), cells.end(), (int)state);
    }
}

namespace winrt::ShieldCanLaser::implementation
{
    void MainPage::OnRootLoaded(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loaded) return;
        m_loaded = true;

        m_cam.Captured = CamCapturedText();
        m_cam.Placeholder = CamPlaceholder();
        m_cam.VmText = CamVmText();
        m_cam.Picture = CamImage();
        m_cam.Host = CamHost();
        m_cam.ToolbarButton = CamToolbarButton();

        // RUN lúc Armed: xanh NHẤP NHÁY 1 Hz cùng nhịp đèn nút START ngoài máy — nháy bằng Opacity rời rạc
        // (1 trong 0,5 s đầu, 0,3 trong 0,5 s sau), như Storyboard ArmedBlink của bản WPF.
        DoubleAnimationUsingKeyFrames blink;
        blink.Duration(DurationHelper::FromTimeSpan(std::chrono::seconds(1)));
        DiscreteDoubleKeyFrame on;
        on.KeyTime(KeyTimeHelper::FromTimeSpan(std::chrono::milliseconds(0)));
        on.Value(1.0);
        DiscreteDoubleKeyFrame dim;
        dim.KeyTime(KeyTimeHelper::FromTimeSpan(std::chrono::milliseconds(500)));
        dim.Value(0.3);
        blink.KeyFrames().Append(on);
        blink.KeyFrames().Append(dim);
        Storyboard::SetTarget(blink, RunButton());
        Storyboard::SetTargetProperty(blink, L"Opacity");
        m_armedBlink = Storyboard();
        m_armedBlink.Children().Append(blink);
        m_armedBlink.RepeatBehavior(RepeatBehaviorHelper::Forever());

        m_trayMaps[0].Host = TrayMap1();
        m_trayMaps[1].Host = TrayMap2();
        m_trayMaps[1].Tray = 1;
        HookTrayMap(0);
        HookTrayMap(1);
        BuildTrayLegend();
        BuildNozzleStrip();
        QueueVmWork(true);   // trang Main là trang mặc định: OnShown(true) có thể đã tới trước Loaded
        OnTick();
    }

    // ══════════ Cập nhật theo nhịp (100 ms, chỉ khi trang đang hiện) ══════════

    void MainPage::OnTick()
    {
        if (!m_loaded || m_closing) return;

        scs::McStatus& status = scs::McControl::Status();
        const scs::EMcState state = status.State();
        const scs::EMcMode mode = status.Mode();

        // Công tắc AUTO/MANUAL: bên đang chọn có khối màu đèn tháp (AUTO xanh lá, MANUAL cam) + chữ trắng, bên kia chữ xám.
        const int isAuto = mode == scs::EMcMode::Auto ? 1 : 0;
        if (isAuto != m_shownAuto)
        {
            m_shownAuto = isAuto;
            ui::Show(ModeThumbAuto(), isAuto);
            ui::Show(ModeThumbManual(), !isAuto);
            ModeTextAuto().Foreground(ui::Res(isAuto ? L"TextOnAccentBrush" : L"StatusOffBrush"));
            ModeTextManual().Foreground(ui::Res(isAuto ? L"StatusOffBrush" : L"TextOnAccentBrush"));
        }

        UpdateRunButton(state);

        // Đèn nút trang Main: STOP đỏ khi máy không chạy, RESET đỏ khi còn alarm — cùng ranh giới với đèn nút cứng.
        const bool isStopped = state != scs::EMcState::Running && state != scs::EMcState::Pausing &&
                               state != scs::EMcState::WaitOperator;
        ui::SetTint(StopButton(), isStopped ? ui::Tint::Red : ui::Tint::None);
        ui::SetTint(OriginButton(), status.OrgOk() ? ui::Tint::Blue : ui::Tint::None);
        // Lần dừng trước là dừng giữa hành trình — phải Init (hoặc Về gốc) trước khi chạy tiếp.
        auto run = scs::McDataManager::RunData();
        ui::SetTint(InitButton(), run && run->UnsafePause ? ui::Tint::Caution : ui::Tint::None);
        ui::SetTint(ResetButton(), scs::AlarmManager::ActiveCount() > 0 ? ui::Tint::Red : ui::Tint::None);

        ui::Show(WaitOperatorBar(), state == scs::EMcState::WaitOperator);
        std::string message = status.Message();
        ui::Show(MessageBar(), !::base::IsBlank(message));
        ui::SetText(MessageText(), message);

        UpdateCounters();
        ui::SetText(AutoStepText(), "Step: " + status.AutoStepText());
        ui::SetText(ProgressText(), scs::Routine_Auto::ProgressText());

        auto bus = scs::McControl::Motion().Bus();
        ui::SetText(BusText(), "EtherCAT: " + (bus ? bus->BusDescription() : std::string("Simulation (FakeAxis / FakeIo)")));

        UpdateTrays();
        UpdateNozzles();

#define MAINPAGE_SHOT_SELFTEST 1
#ifdef MAINPAGE_SHOT_SELFTEST
        if (!ui::AppHost::Options().ScreenshotDir.empty())
        {
            static bool injected = false;
            if (!injected)
            {
                injected = true;
                auto make = [](int w, int h, int c, int pad, auto pixel) {
                    auto img = std::make_shared<scs::ImageData>();
                    img->Width = w; img->Height = h; img->Channels = c; img->Stride = w * c + pad;
                    img->Pixels.assign((size_t)img->Stride * h, 0xEE);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x) pixel(img->Pixels.data() + (size_t)y * img->Stride + (size_t)x * c, x, y);
                    return std::shared_ptr<const scs::ImageData>(img);
                };
                SyncImage(make(400, 300, 1, 16, [](uint8_t* p, int x, int y) { p[0] = (uint8_t)(x * 255 / 399); if (std::abs(x - y) < 3) p[0] = 255; }));
                scs::LogWorker::Instance().RecordLog("Self-test: error line", scs::LogLevel::Error, "Test");
                scs::McControl::Status().SetMessage("Self-test: reason message shown below the buttons");
                // Sơ đồ tray: tray 1 đã khắc 3 hàng, hàng vừa xong 6 OK + 1 NG, chọn vài ô hàng 5, 6 chạy lẻ; tray 2 vắng (chỉ dữ
                // liệu tạm của chế độ chụp ảnh).
                if (auto fake = std::dynamic_pointer_cast<scs::FakeIo>(scs::McControl::Io()))
                    fake->SimSet(scs::IoMap::TrayPresent(2), false);
                if (auto r = scs::McDataManager::RunData())
                {
                    const int ok = (int)scs::ECellState::Ok, ng = (int)scs::ECellState::Ng;
                    for (int cell = 0; cell < 3 * NozzleCount; ++cell) r->Trays[0].MarkDone(cell);
                    for (int cell : { 29, 30, 31, 35 }) r->Trays[0].Toggle(cell);
                    r->LastCycleSec = 95.4;
                    r->LastCycleProducts = 47;
                    r->CycleStartedAtMs = scs::RunningData::NowMs() - 41300;
                    r->CycleOk = 20;
                    r->LastRowResult = { ok, ok, ng, ok, ok, ok, ok };
                    r->LastRowTray = 1;
                    r->LastRowIndex = 3;
                }
            }
            ui::Show(WaitOperatorBar(), true);
            LeftScroll().ChangeView(nullptr, WF::IReference<double>(LeftScroll().ScrollableHeight()), nullptr, true);
        }
        else
#endif
        if (auto vision = scs::McControl::Vision())
        {
            SyncImage(vision->LastImage());
            const int nozzle = vision->LastImageNozzle();
            if (nozzle != m_camNozzle)
            {
                m_camNozzle = nozzle;
                ui::SetText(CamTitleText(), nozzle > 0 ? std::format("CAM — N{}", nozzle) : std::string("CAM"));
            }
        }

        UpdateLog();
        QueueVmWork(false);
    }

    // RUN — chữ luôn RUN, MÀU báo trạng thái: xám khi đứng, xanh nhấp nháy khi sẵn sàng (Armed), xanh lá + quầng sáng
    // khi chạy (Running).
    void MainPage::UpdateRunButton(scs::EMcState state)
    {
        const int visual = state == scs::EMcState::Running ? 2 : state == scs::EMcState::Armed ? 1 : 0;
        if (visual == m_runVisual) return;
        m_runVisual = visual;

        auto button = RunButton();
        if (visual == 2)
        {
            ui::SetTint(button, ui::Tint::Green);
        }
        else
        {
            ui::SetTint(button, ui::Tint::None);   // gỡ quầng sáng nếu vừa ở trạng thái chạy
            Brush fill = ui::Res(visual == 1 ? L"GreenOnBrush" : L"StatusOffBrush");
            button.Background(fill);
            button.BorderBrush(fill);
            button.Foreground(ui::Res(L"TextOnAccentBrush"));
        }

        if (!m_armedBlink) return;
        if (visual == 1) m_armedBlink.Begin();
        else m_armedBlink.Stop();
    }

    // RunningData là POCO ghi từ thread điều khiển — đọc theo nhịp hiển thị như MainViewModel.Refresh.
    void MainPage::UpdateCounters()
    {
        auto run = scs::McDataManager::RunData();
        if (!run) return;

        ui::SetText(CountOkText(), std::to_string(run->CountOk));
        ui::SetText(CountNgText(), std::to_string(run->CountNg));
        ui::SetText(CountTotalText(), std::to_string(run->CountTotal));

        const int consecutive = run->ConsecutiveNg;
        ui::SetText(ConsecutiveNgText(), std::to_string(consecutive));
        const int streak = consecutive > 0 ? 1 : 0;
        if (streak != m_lastNgStreak)
        {
            m_lastNgStreak = streak;
            ConsecutiveNgText().Foreground(ui::Res(streak ? L"CriticalBrush" : L"TextPrimaryBrush"));
        }

        ui::SetText(NgRateText(), std::format("{:.2f} %", run->NgRatePercent()));
        // Bảng thời gian — như ShieldCanAssy.WinUI, xem chú thích ở MainPage.xaml.
        const std::string none = "—";
        auto perProduct = [](double s) { return std::format("{:.2f} s", s); };
        auto clock = [](double s) {
            const long long tenths = (long long)std::floor(std::max(0.0, s) * 10.0);
            const long long h = tenths / 36000, m = tenths / 600 % 60, sec = tenths / 10 % 60, t = tenths % 10;
            return h >= 1 ? std::format("{}:{:02}:{:02}.{}", h, m, sec, t) : std::format("{:02}:{:02}.{}", m, sec, t);
        };
        const bool hasLast = run->LastCycleSec > 0;
        const std::string lastCycle = hasLast ? clock(run->LastCycleSec) : none;
        const std::string lastTack = run->LastCycleProducts > 0 ? perProduct(run->LastTackSec()) : none;
        double currentTack = 0;
        std::string curCycle = none, curTack = none;
        if (run->CycleStartedAtMs != 0)
        {
            const double sec = std::max(0.0, (double)(scs::RunningData::NowMs() - run->CycleStartedAtMs) / 1000.0);
            const int done = run->CycleOk;
            if (done > 0) currentTack = sec / done;
            curCycle = clock(sec);
            curTack = done > 0 ? perProduct(currentTack) : none;
        }
        else if (run->CycleHoldLast && hasLast)
        {
            curCycle = lastCycle;
            curTack = lastTack;
        }
        ui::SetText(CycleText(), curCycle);
        ui::SetText(LastCycleText(), lastCycle);
        ui::SetText(TackText(), curTack);
        ui::SetText(LastTackText(), lastTack);

        const double tackForUph = run->LastCycleProducts > 0 ? run->LastTackSec() : currentTack;
        ui::SetText(UphText(), tackForUph > 0 ? std::format("Est. output: {:.0f} pcs/h", 3600.0 / tackForUph) : std::string("Est. output: —"));
    }

    // ══════════ Sơ đồ tray + dải nozzle ══════════

    // Chú thích màu ở đáy card Trays: ô mẫu + chữ.
    void MainPage::BuildTrayLegend()
    {
        auto legend = TrayLegend();
        legend.Children().Clear();
        auto add = [&](wchar_t const* fill, wchar_t const* stroke, double thickness, wchar_t const* label) {
            StackPanel item;
            item.Orientation(Orientation::Horizontal);
            item.Spacing(5);
            Border swatch;
            swatch.Width(14);
            swatch.Height(14);
            swatch.CornerRadius(CornerRadiusHelper::FromUniformRadius(3));
            swatch.BorderThickness(ThicknessHelper::FromUniformLength(thickness));
            swatch.BorderBrush(ui::Res(stroke));
            swatch.Background(ui::Res(fill));
            swatch.VerticalAlignment(VerticalAlignment::Center);
            TextBlock text;
            text.Text(label);
            text.FontSize(12);
            text.VerticalAlignment(VerticalAlignment::Center);
            text.Foreground(ui::Res(L"TextSecondaryBrush"));
            item.Children().Append(swatch);
            item.Children().Append(text);
            legend.Children().Append(item);
        };
        const std::pair<scs::ECellState, wchar_t const*> items[] = {
            { scs::ECellState::Loaded, L"Loaded" }, { scs::ECellState::Processing, L"Processing" },
            { scs::ECellState::Ok, L"OK" }, { scs::ECellState::Ng, L"NG" },
            { scs::ECellState::Skipped, L"Done earlier" },
        };
        for (const auto& [state, label] : items) add(CellBrushKey((int)state), L"ControlStrokeSecondaryBrush", 1, label);
        add(L"TrayCellLoadedBrush", L"AccentBrush", 2, L"Selected");
    }

    // Dải 7 nozzle: mỗi hàng = ô màu "N{k}" (36 px) + trạng thái + độ lệch (dX / dZ, mm) — hàng giãn kín chiều cao card.
    void MainPage::BuildNozzleStrip()
    {
        auto grid = NozzleStrip();
        grid.Children().Clear();
        grid.ColumnDefinitions().Clear();
        grid.RowDefinitions().Clear();
        auto star = GridLengthHelper::FromValueAndType(1, GridUnitType::Star);
        for (double w : { 44.0, 0.0, 0.0 })
        {
            ColumnDefinition def;
            def.Width(w > 0 ? GridLengthHelper::FromPixels(w) : star);
            grid.ColumnDefinitions().Append(def);
        }
        for (int k = 1; k <= NozzleCount; ++k)
        {
            RowDefinition def;
            def.Height(star);
            grid.RowDefinitions().Append(def);

            NozzleRow& row = m_nozzles[(size_t)k];
            row.Shown = -1;

            row.Cell = Border();
            row.Cell.MinHeight(22);
            row.Cell.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
            row.Cell.BorderThickness(ThicknessHelper::FromUniformLength(1));
            row.Cell.BorderBrush(ui::Res(L"ControlStrokeSecondaryBrush"));
            row.Cell.Background(ui::Res(L"TrayCellLoadedBrush"));
            TextBlock name;
            name.Text(ui::H(std::format("N{}", k)));
            name.FontSize(13);
            name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            name.HorizontalAlignment(HorizontalAlignment::Center);
            name.VerticalAlignment(VerticalAlignment::Center);
            row.Cell.Child(name);
            Grid::SetRow(row.Cell, k - 1);
            Grid::SetColumn(row.Cell, 0);
            grid.Children().Append(row.Cell);

            row.Text = TextBlock();
            row.Text.FontSize(12);
            row.Text.VerticalAlignment(VerticalAlignment::Center);
            row.Text.TextTrimming(TextTrimming::CharacterEllipsis);
            row.Text.Foreground(ui::Res(L"TextSecondaryBrush"));
            Grid::SetRow(row.Text, k - 1);
            Grid::SetColumn(row.Text, 1);
            grid.Children().Append(row.Text);

            row.ResultText = TextBlock();
            row.ResultText.FontSize(12);
            row.ResultText.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Consolas"));
            row.ResultText.VerticalAlignment(VerticalAlignment::Center);
            row.ResultText.HorizontalAlignment(HorizontalAlignment::Right);
            Grid::SetRow(row.ResultText, k - 1);
            Grid::SetColumn(row.ResultText, 2);
            grid.Children().Append(row.ResultText);
        }
    }

    // Ô kiểu nút Calculator Win11 (như ShieldCANAssy): không bóng, viền 1 px nhạt, bo góc 4, khe 3 px. Chỉ dựng lại khi
    // số ô hoặc số cột đổi (đổi số hàng tray ở trang Teaching).
    void MainPage::LayoutMap(TrayMap& map, int count, int cols, double cellHeight, bool labels)
    {
        if (!map.Host || cols < 1) return;
        if ((int)map.Cells.size() == count && map.Cols == cols) return;
        CancelCellDrag();   // Anchor / End trỏ vào ô sắp bị xoá
        auto grid = map.Host;
        grid.Children().Clear();
        grid.ColumnDefinitions().Clear();
        grid.RowDefinitions().Clear();
        grid.ColumnSpacing(3);
        grid.RowSpacing(3);
        map.Cells.clear();
        map.Cols = cols;

        const int rows = (count + cols - 1) / cols;
        const bool stretchRows = cellHeight <= 0;
        auto star = GridLengthHelper::FromValueAndType(1, GridUnitType::Star);
        for (int c = 0; c < cols; ++c)
        {
            ColumnDefinition def;
            def.Width(star);
            grid.ColumnDefinitions().Append(def);
        }
        for (int r = 0; r < rows; ++r)
        {
            RowDefinition def;
            def.Height(stretchRows ? star : GridLengthHelper::Auto());
            grid.RowDefinitions().Append(def);
        }
        for (int i = 0; i < count; ++i)
        {
            TrayCell cell;
            cell.Root = Grid();
            cell.Fill = Border();
            if (stretchRows) cell.Fill.MinHeight(18);
            else cell.Fill.Height(cellHeight);
            cell.Fill.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
            cell.Fill.BorderThickness(ThicknessHelper::FromUniformLength(1));
            cell.Fill.BorderBrush(ui::Res(L"ControlStrokeSecondaryBrush"));
            cell.Fill.Background(ui::Res(L"TrayCellLoadedBrush"));
            if (labels)
            {
                TextBlock text;
                text.Text(ui::H(std::format("{}-{}", i / cols + 1, i % cols + 1)));
                text.FontSize(11.5);
                text.Foreground(ui::Res(L"TextPrimaryBrush"));
                text.HorizontalAlignment(HorizontalAlignment::Center);
                text.VerticalAlignment(VerticalAlignment::Center);
                cell.Fill.Child(text);
            }
            cell.Selection = Border();
            cell.Selection.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
            cell.Selection.BorderThickness(ThicknessHelper::FromUniformLength(2));
            cell.Selection.BorderBrush(ui::Res(L"AccentBrush"));
            cell.Selection.IsHitTestVisible(false);
            cell.Selection.Visibility(Visibility::Collapsed);
            cell.Root.Children().Append(cell.Fill);
            cell.Root.Children().Append(cell.Selection);
            Grid::SetRow(cell.Root, i / cols);
            Grid::SetColumn(cell.Root, i % cols);
            grid.Children().Append(cell.Root);
            map.Cells.push_back(cell);
        }
    }

    void MainPage::PaintMap(TrayMap& map, std::vector<int> const& states, std::vector<int> const& selected, bool anySelected)
    {
        for (size_t i = 0; i < map.Cells.size(); ++i)
        {
            TrayCell& cell = map.Cells[i];
            int state = i < states.size() ? states[i] : (int)scs::ECellState::Loaded;
            if (cell.ShownState != state)
            {
                cell.ShownState = state;
                cell.Fill.Background(ui::Res(CellBrushKey(state)));
            }
            bool isSelected = std::binary_search(selected.begin(), selected.end(), (int)i);
            if (InDragRect(map, (int)i)) isSelected = m_drag.Add && (isSelected || cell.Selectable);
            ui::Show(cell.Selection, isSelected);
            const int dim = anySelected && !isSelected ? 1 : 0;
            if (cell.ShownDim != dim)
            {
                cell.ShownDim = dim;
                cell.Root.Opacity(dim ? 0.35 : 1.0);
            }
        }
    }

    // Chọn ô (AUTO), như TrayCard của ShieldCanSlit: chạm một ô = bật/tắt ô đó. Nhấn giữ một ô rồi kéo sang ô khác cùng tray = cả
    // vùng chữ nhật giữa hai ô: ô bắt đầu chưa chọn → chọn cả vùng (chỉ ô chọn được), đã chọn → bỏ chọn cả vùng. Lúc kéo, viền
    // chọn hiện trước kết quả; nhả mới áp.
    void MainPage::HookTrayMap(int tray)
    {
        auto host = m_trayMaps[(size_t)tray].Host;
        // ManipulationMode khác System: kéo bằng ngón tay bắt đầu trên lưới không bị ScrollViewer cha giành để cuộn giữa chừng.
        host.ManipulationMode(Input::ManipulationModes::TranslateX | Input::ManipulationModes::TranslateY);
        host.PointerPressed([weak = get_weak(), tray](auto&&, Input::PointerRoutedEventArgs const& e) {
            if (auto self = weak.get()) self->OnMapPressed(tray, e);
        });
        host.PointerMoved([weak = get_weak(), tray](auto&&, Input::PointerRoutedEventArgs const& e) {
            if (auto self = weak.get()) self->OnMapMoved(tray, e);
        });
        host.PointerReleased([weak = get_weak(), tray](auto&&, Input::PointerRoutedEventArgs const& e) {
            if (auto self = weak.get()) self->OnMapReleased(tray, e);
        });
        auto cancel = [weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get())
            {
                self->CancelCellDrag();
                self->UpdateTrays();
            }
        };
        host.PointerCanceled(cancel);
        host.PointerCaptureLost(cancel);
    }

    int MainPage::CellAt(TrayMap const& map, WF::Point p)
    {
        for (size_t i = 0; i < map.Cells.size(); ++i)
        {
            const auto& b = map.Cells[i].Bounds;
            if (p.X >= b.X && p.X < b.X + b.Width && p.Y >= b.Y && p.Y < b.Y + b.Height) return (int)i;
        }
        return -1;
    }

    bool MainPage::InDragRect(TrayMap const& map, int cell) const
    {
        if (!m_drag.Active || m_drag.Tray != map.Tray) return false;
        const auto [r0, r1] = std::minmax({ m_drag.Anchor / map.Cols, m_drag.End / map.Cols });
        const auto [c0, c1] = std::minmax({ m_drag.Anchor % map.Cols, m_drag.End % map.Cols });
        const int row = cell / map.Cols, col = cell % map.Cols;
        return row >= r0 && row <= r1 && col >= c0 && col <= c1;
    }

    void MainPage::OnMapPressed(int tray, Input::PointerRoutedEventArgs const& e)
    {
        if (m_drag.Active || !scs::McControl::CanSelectCells()) return;   // ngón tay thứ hai lúc đang kéo: bỏ qua
        TrayMap& map = m_trayMaps[(size_t)tray];
        auto point = e.GetCurrentPoint(map.Host);
        if (e.Pointer().PointerDeviceType() == winrt::Microsoft::UI::Input::PointerDeviceType::Mouse &&
            !point.Properties().IsLeftButtonPressed())
            return;
        for (size_t i = 0; i < map.Cells.size(); ++i)
        {
            TrayCell& cell = map.Cells[i];
            cell.Bounds = cell.Root.TransformToVisual(map.Host).TransformBounds(
                { 0, 0, (float)cell.Root.ActualWidth(), (float)cell.Root.ActualHeight() });
            cell.Selectable = scs::McControl::CanSelectCell(tray, (int)i);
        }
        const int index = CellAt(map, point.Position());
        auto run = scs::McDataManager::RunData();
        if (index < 0 || !run) return;   // khe giữa ô
        m_drag.Active = true;
        m_drag.PointerId = e.Pointer().PointerId();
        m_drag.Tray = tray;
        m_drag.Anchor = m_drag.End = index;
        m_drag.Add = !run->Trays[(size_t)tray].IsSelected(index);
        map.Host.CapturePointer(e.Pointer());   // kéo ra ngoài lưới / nhả ở ngoài vẫn nhận được
        e.Handled(true);
        UpdateTrays();
    }

    void MainPage::OnMapMoved(int tray, Input::PointerRoutedEventArgs const& e)
    {
        if (!m_drag.Active || m_drag.Tray != tray || e.Pointer().PointerId() != m_drag.PointerId) return;
        e.Handled(true);
        TrayMap& map = m_trayMaps[(size_t)tray];
        const int index = CellAt(map, e.GetCurrentPoint(map.Host).Position());
        if (index < 0 || index == m_drag.End) return;   // khe / ngoài lưới: giữ ô cuối
        m_drag.End = index;
        UpdateTrays();
    }

    void MainPage::OnMapReleased(int tray, Input::PointerRoutedEventArgs const& e)
    {
        if (!m_drag.Active || m_drag.Tray != tray || e.Pointer().PointerId() != m_drag.PointerId) return;
        e.Handled(true);
        TrayMap& map = m_trayMaps[(size_t)tray];
        const bool tap = m_drag.Anchor == m_drag.End;
        const int anchor = m_drag.Anchor;
        const bool add = m_drag.Add;
        std::vector<int> cells;
        for (size_t i = 0; i < map.Cells.size(); ++i)
            if (InDragRect(map, (int)i)) cells.push_back((int)i);
        m_drag = {};   // trước khi nhả bắt chuột — PointerCaptureLost → CancelCellDrag thấy không còn kéo
        map.Host.ReleasePointerCapture(e.Pointer());
        if (tap) scs::McControl::ToggleCellSelection(tray, anchor);
        else scs::McControl::SetCellsSelected(tray, cells, add);
        UpdateTrays();
    }

    void MainPage::CancelCellDrag()
    {
        if (!m_drag.Active) return;
        const int tray = m_drag.Tray;
        m_drag = {};
        m_trayMaps[(size_t)tray].Host.ReleasePointerCaptures();
    }

    // 2 tray TrayRows × 7 ô (hàng = một lượt gắp, cột = nozzle). Chu trình ghi Routine_Auto::Trays lúc chạy; mới mở app
    // (mảng rỗng / sai kích thước) thì suy từ RunningData.Trays: ô đã xong = Skipped, còn lại Loaded. Tray vắng
    // (cảm biến tắt): mọi ô Loaded, dòng tóm tắt ghi No tray. Có ô chọn: viền 2 px ở ô chọn, mọi ô khác mờ 0.35 — như ô tray ShieldCanAssy.WinUI.
    void MainPage::UpdateTrays()
    {
        auto model = scs::McDataManager::Model();
        auto run = scs::McDataManager::RunData();
        if (!model || !run) return;
        if (m_drag.Active && !scs::McControl::CanSelectCells()) CancelCellDrag();
        auto view = scs::Routine_Auto::Trays();
        auto sys = scs::McDataManager::SysData();
        auto io = scs::McControl::Io();

        const int rows = std::max(1, model->Auto.TrayRows), cols = NozzleCount;
        const size_t size = (size_t)rows * (size_t)cols;
        std::array<TextBlock, TrayCount> summaries{ TraySummaryText1(), TraySummaryText2() };
        const bool anySelected = run->HasCellSelection();
        int selCount = 0;
        for (int t = 0; t < TrayCount; ++t)
        {
            const bool present = !sys || !sys->HasTraySensors || (io && io->GetDi(scs::IoMap::TrayPresent(t + 1)));
            const scs::TrayCells& book = run->Trays[(size_t)t];
            const int done = book.DoneCount((int)size);
            const std::vector<int> selected = book.Selected();
            selCount += (int)selected.size();
            std::vector<int> cells = view.Trays[(size_t)t];
            if (cells.size() != size)
            {
                cells.assign(size, (int)scs::ECellState::Loaded);
                if (present)
                    for (size_t i = 0; i < size; ++i)
                        if (book.IsDone((int)i)) cells[i] = (int)scs::ECellState::Skipped;
            }
            else if (!present && CountOf(cells, scs::ECellState::Processing) == 0)
            {
                cells.assign(size, (int)scs::ECellState::Loaded);
            }
            LayoutMap(m_trayMaps[(size_t)t], (int)size, cols, 0, true);
            PaintMap(m_trayMaps[(size_t)t], cells, selected, anySelected);
            const int ok = CountOf(cells, scs::ECellState::Ok), ng = CountOf(cells, scs::ECellState::Ng);
            std::string text = present ? std::format("{}/{} cells done", done, size) : std::string("No tray");
            if (ok + ng > 0) text += std::format(" · {} OK · {} NG", ok, ng);
            if (!selected.empty()) text += std::format(" · {} selected", selected.size());
            ui::SetText(summaries[(size_t)t], text);
        }

        auto clearCells = ClearCellsButton();
        ui::Show(clearCells, selCount > 0);
        clearCells.IsEnabled(!scs::McControl::CellSelectionLocked());
        if (selCount != m_shownSelCount)
        {
            m_shownSelCount = selCount;
            clearCells.Content(box_value(ui::H(std::format("Clear ({})", selCount))));
        }
        ui::SetText(TrayModeText(), selCount > 0 ? std::string("Runs selected cells only")
                                    : scs::McControl::CanSelectCells() ? std::string("Tap or drag cells to run only those")
                                                                        : std::string());
    }

    // Dải nozzle: ô màu theo ECellState (Empty = không giữ can, Loaded = đang giữ, Processing = đang khắc/kiểm tra, Ok =
    // kiểm tra đạt, Ng), chữ trạng thái và output Result của lần kiểm tra gần nhất (chỉ khi HasResult).
    void MainPage::UpdateNozzles()
    {
        auto noz = scs::Routine_Auto::Nozzles();
        int holding = 0, ok = 0, ng = 0;
        for (int k = 1; k <= NozzleCount; ++k)
        {
            const auto& n = noz[(size_t)k];
            NozzleRow& row = m_nozzles[(size_t)k];
            if (!row.Cell) continue;
            if (n.State != row.Shown)
            {
                row.Shown = n.State;
                row.Cell.Background(ui::Res(n.State == (int)scs::ECellState::Loaded ? L"NozzleHoldingBrush" : CellBrushKey(n.State)));
            }
            ui::SetText(row.Text, n.Text.empty() ? std::string("—") : n.Text);
            ui::SetText(row.ResultText, n.HasResult ? "Result " + scs::VmCycle::Fmt(n.Result) : std::string());
            if (n.State == (int)scs::ECellState::Loaded || n.State == (int)scs::ECellState::Processing) ++holding;
            else if (n.State == (int)scs::ECellState::Ok) ++ok;
            else if (n.State == (int)scs::ECellState::Ng) ++ng;
        }
        std::string summary;
        if (holding > 0) summary += std::format("{} holding", holding);
        if (ok + ng > 0) summary += (summary.empty() ? "" : " · ") + std::format("{} OK · {} NG", ok, ng);
        ui::SetText(NozzleSummaryText(), summary.empty() ? std::string("idle") : summary);
    }

    // ══════════ Nhật ký ══════════

    // LogWorker chèn dòng mới ở ĐẦU danh sách (mới nhất lên trên) và bỏ dòng cũ ở cuối. Chỉ dựng dòng mới: tìm dòng
    // đầu đang hiện trong bản chụp mới — những dòng đứng trước nó là dòng mới; không thấy thì dựng lại cả danh sách.
    void MainPage::UpdateLog()
    {
        auto& worker = scs::LogWorker::Instance();
        const uint64_t version = worker.Version();
        if (version == m_logVersion) return;
        m_logVersion = version;

        std::vector<scs::LogData> list = worker.LogList();
        auto children = LogPanel().Children();

        size_t added = list.size();
        bool incremental = false;
        if (!m_shownLogs.empty())
        {
            for (size_t k = 0; k < list.size(); ++k)
            {
                if (SameLog(list[k], m_shownLogs.front()))
                {
                    added = k;
                    incremental = true;
                    break;
                }
            }
        }

        if (incremental)
        {
            for (size_t i = 0; i < added; ++i) children.InsertAt((uint32_t)i, MakeLogRow(list[i]));
            while (children.Size() > list.size()) children.RemoveAtEnd();
            if (children.Size() != list.size()) incremental = false;   // không khớp như dự kiến: dựng lại
        }
        if (!incremental)
        {
            children.Clear();
            for (const auto& d : list) children.Append(MakeLogRow(d));
        }
        m_shownLogs = std::move(list);

        // Tự cuộn lên dòng mới nhất khi có dòng mới (bản WPF: ScrollIntoView(list[0]) lúc Add).
        if (added > 0)
            LogScroll().ChangeView(nullptr, WF::IReference<double>(0.0), nullptr, true);
    }

    // Cột Log giãn ra đẩy cột giữa, không trượt đè lên: control VisionMaster ở khung CAM là cửa sổ riêng, nằm trên mọi thứ
    // của XAML nên sẽ che card. Mỗi khung hình popup của nó đi theo qua VmPane::Sync (gộp và xếp hàng, chỉ di chuyển khi đổi).
    // Log vẫn cập nhật lúc ẩn: mở ra là thấy ngay dòng mới nhất.
    void MainPage::OnLogToggleClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_drag.Active)   // Bounds chụp lúc nhấn: bố cục sắp đổi, kéo tiếp sẽ chọn sai ô
        {
            CancelCellDrag();
            UpdateTrays();
        }
        m_logShown = !m_logShown;

        // Bấm lại giữa chừng thì quay đầu từ chỗ đang dở, không giật — đọc trước khi Stop.
        const double full = LogCard().Width() + LogCard().Margin().Right;
        const double from = std::clamp(LogHost().ActualWidth() / full, 0.0, 1.0);
        const double to = m_logShown ? 1.0 : 0.0;
        if (m_logAnim) m_logAnim.Stop();

        // Giá trị gốc = chỗ đang đứng, không phải đích: bố cục có chạy trước nhịp đầu của hiệu ứng thì card vẫn đứng yên, không
        // nháy tới đích. Hết hiệu ứng, Storyboard giữ giá trị cuối (HoldEnd).
        LogHost().Width(from * full);
        LogCard().Opacity(from);
        LogChevronRotate().Angle(from * 180.0);

        m_logAnim = Storyboard();
        m_logAnim.Children().Append(LogSlide(LogHost(), L"Width", from * full, to * full));
        m_logAnim.Children().Append(LogSlide(LogCard(), L"Opacity", from, to));
        m_logAnim.Children().Append(LogSlide(LogChevronRotate(), L"Angle", from * 180.0, to * 180.0));
        m_logAnim.Begin();
    }

    // ══════════ Ảnh CAM (ảnh Bitmap chụp gần nhất) ══════════

    // Vision vừa có ảnh MỚI (so con trỏ, như ReferenceEquals bản WPF) thì đổi ảnh và ghi giờ chụp. Đổi định dạng điểm ảnh
    // trên luồng nền — ảnh camera vài MB không được làm khựng luồng UI.
    void MainPage::SyncImage(std::shared_ptr<const scs::ImageData> src)
    {
        CamFrame& f = m_cam;
        if (src == f.LastSrc) return;
        f.LastSrc = src;
        const uint64_t generation = ++f.Generation;

        ui::SetText(f.Captured, src ? "Captured " + scs::FormatLocal(std::chrono::system_clock::now(), "HH:mm:ss")
                                    : std::string());
        if (!src)
        {
            f.Picture.Source(nullptr);
            f.Bitmap = nullptr;
            ui::Show(f.Placeholder, true);
            return;
        }

        auto bgra = std::make_shared<std::vector<uint8_t>>();
        ui::RunBackgroundThenUi(
            [src, bgra] {
                if (!ToBgra(*src, *bgra)) bgra->clear();
            },
            [weak = get_weak(), generation, src, bgra] {
                if (auto self = weak.get()) self->ShowConverted(generation, src->Width, src->Height, *bgra);
            });
    }

    void MainPage::ShowConverted(uint64_t generation, int width, int height, std::vector<uint8_t> const& bgra)
    {
        if (m_closing) return;
        CamFrame& f = m_cam;
        if (generation != f.Generation) return;   // đã có ảnh mới hơn trong lúc đổi định dạng

        auto clear = [&f] {
            f.Picture.Source(nullptr);
            f.Bitmap = nullptr;
            ui::Show(f.Placeholder, true);
        };
        if (bgra.empty() || width <= 0 || height <= 0)
        {
            clear();
            return;
        }

        try
        {
            bool fresh = false;
            if (!f.Bitmap || f.Bitmap.PixelWidth() != width || f.Bitmap.PixelHeight() != height)
            {
                f.Bitmap = WriteableBitmap(width, height);
                fresh = true;
            }
            auto buffer = f.Bitmap.PixelBuffer();
            std::memcpy(buffer.data(), bgra.data(), std::min<size_t>(bgra.size(), buffer.Length()));
            f.Bitmap.Invalidate();
            if (fresh) f.Picture.Source(f.Bitmap);
            ui::Show(f.Placeholder, false);
        }
        catch (winrt::hresult_error const& e)
        {
            scs::LogWorker::Instance().RecordLog("Main: cannot display CAM image — " + ui::U8(e.message()),
                                                 scs::LogLevel::Warning, "Vision");
            clear();
        }
    }

    // ══════════ Khung VmRenderControl của VisionMaster (khi vision là VisionMaster thật) ══════════

    // Control VM là .NET và gọi sang tiến trình vServerApp; trong lúc chờ, COM bơm thông điệp. Gọi nó ngay trong Loaded,
    // Tick của DispatcherTimer hay lúc bấm tab là XAML nhận lại thông điệp của chính nó khi đang xử lý dở và tự kết thúc
    // app (0xC000027B, CXcpDispatcher::OnReentrancyProtectedWindowMessage — dump 06/10). Nên chỉ xếp hàng; việc chạy ở
    // lượt sau, ngoài mọi sự kiện XAML. Gộp các lần gọi dồn dập thành một.
    void MainPage::QueueVmWork(bool force)
    {
        m_vmForce = m_vmForce || force;
        if (m_vmQueued || m_closing) return;
        m_vmQueued = true;
        ui::PostUi([weak = get_weak()] {
            auto self = weak.get();
            if (!self) return;
            self->m_vmQueued = false;
            // Đang ở giữa một lời gọi VM (nó bơm thông điệp nên việc này chạy lồng vào): bỏ, nhịp sau xếp lại.
            if (self->m_vmBusy || self->m_closing) return;
            const bool force = std::exchange(self->m_vmForce, false);
            self->m_vmBusy = true;
            try
            {
                self->EnsureVmPane();
                self->UpdateRenderBinding(force);
            }
            catch (...) { self->m_vmBusy = false; throw; }
            self->m_vmBusy = false;
        });
    }

    // Tạo control lần đầu trang hiện. Vision giả / chụp ảnh: không có VisionMaster — khung chỉ có ảnh dự phòng và chữ
    // "(no image)".
    void MainPage::EnsureVmPane()
    {
        if (m_vmAttempted || !m_loaded || !m_visible) return;
        m_vmAttempted = true;

        // Không chặn theo SimulationMode (bản WPF cũng không): mô phỏng vẫn dùng VisionMaster thật trừ khi Setting chọn
        // vision giả — khi đó không có solution nào trong SDK để vẽ, khung dùng ảnh của bản giả.
        if (!ui::AppHost::Options().ScreenshotDir.empty()) return;
        auto vision = scs::McControl::Vision();
        if (vision && vision->IsSimulation()) return;

        // Nền sáng đặc thay cho nền caro tối mặc định của VmRenderControl (Tuấn 06/10: "sáng màu lên cho đồng nhất với
        // App") — cùng màu khung ảnh dự phòng trong XAML. SetBackground nhận "#RRGGBB" hoặc đường dẫn một ảnh nền nhỏ
        // (<100×100) để lát. Bo góc: VmPane::Create.
        constexpr const wchar_t* renderBackground = L"#F9F9F9";

        std::string error;
        CamFrame& f = m_cam;
        if (f.Pane.Create(f.Host, ui::vmkind::Render, &error))
        {
            if (FAILED(f.Pane.Host().CallStr(ui::vmdisp::RenderSetBackground, renderBackground)))
                scs::LogWorker::Instance().RecordLog("Main: CAM image frame — SetBackground failed: " +
                                                         ::base::Narrow(f.Pane.Host().LastError()),
                                                     scs::LogLevel::Warning, "Vision");
            // Thanh công cụ của SDK chỉ có màu tối, không đổi được: ẩn mặc định, nút ở tiêu đề card bật lại khi cần.
            f.Pane.Host().CallBool(ui::vmdisp::RenderSetToolbarVisible, f.ToolbarShown);
            ui::Show(f.ToolbarButton, true);
        }
        else
        {
            // Thường là chưa đăng ký ActiveX của VisionMaster.
            ui::SetText(f.VmText, "Cannot create VisionMaster image frame: " + error);
            ui::Show(f.VmText, true);
        }
        m_vmActive = f.Pane.Created();

        ApplyPaneVisibility();   // hiện ngay, chưa gắn thì là nền đặc ở trên
        UpdateRenderBinding(true);
    }

    // Control hiện suốt lúc trang Main hiện, gắn hay chưa: chưa gắn Flow/module nào thì nó chỉ có thanh công cụ + nền
    // (EnsureVmPane đặt nền đặc thay cho caro mặc định), như VmRenderControl của bản WPF (Tuấn 06/10). Ảnh dự phòng bên
    // dưới chỉ lộ ra khi không có control (vision giả, máy chưa cài VisionMaster).
    void MainPage::ApplyPaneVisibility()
    {
        if (!m_vmActive) return;
        const int show = m_visible && !m_closing ? 1 : 0;
        CamFrame& f = m_cam;
        if (!f.Pane.Created() || show == f.PaneShown) return;
        f.PaneShown = show;
        f.Pane.SetVisible(show == 1);
    }

    // Gắn control vào Flow/module đặt ở trang Setting (mặc định cả Flow kiểm tra). Chỉ gắn SAU khi McControl nạp xong
    // solution: nạp xong, VisionMasterDevice::LoadSolution tắt callback kết quả của mọi module, gắn trước đó thì bị tắt
    // theo (Tuấn 30/09: khung chỉ có nền caro). Nạp lại (VisionLoadCount đổi) thì gắn lại; đổi tên ở Setting thì gắn lại.
    // Chưa gắn được thì thử lại mỗi giây (như timer 1 s của bản WPF) — mỗi lần thử, RenderSource có thể chờ khoá SDK tới
    // 30 ms trên luồng UI nên không thử ở mọi nhịp 100 ms.
    void MainPage::UpdateRenderBinding(bool force)
    {
        if (!m_vmActive || !m_visible || m_closing) return;

        const int load = scs::McControl::VisionLoadCount();
        const std::string path = CurrentRenderPath();
        if (load != m_lastLoadCount)
        {
            m_lastLoadCount = load;
            force = true;
        }

        if (!m_cam.Pane.Created() || m_cam.BoundKey == BindKey(load, path)) return;

        const auto now = std::chrono::steady_clock::now();
        if (!force && now - m_lastBindTry < std::chrono::seconds(1)) return;
        m_lastBindTry = now;

        auto vision = scs::McControl::Vision();
        std::shared_ptr<scs::VisionMasterDevice> device =
            vision ? std::dynamic_pointer_cast<scs::VisionMasterDevice>(vision->Device()) : nullptr;
        const bool ready = load > 0 && device != nullptr && vision->IsSolutionLoaded();

        std::string diag = ready ? "" : ", waiting for solution load";
        if (ready)
        {
            const bool ok = BindCam(*device, path, load);
            diag += std::format(", '{}'={}", path, ok ? "bound" : "NOT found");
        }

        // Log một lần cho mỗi bộ tên — đủ để biết kẹt ở đâu (chưa nạp xong solution, hay sai tên module), không log
        // lặp lại mỗi giây gây rác Nhật ký. Đổi tên ở Setting thì log lại.
        if (diag != m_loggedVisionDiagnostic)
        {
            m_loggedVisionDiagnostic = diag;
            scs::LogWorker::Instance().RecordLog("Main: CAM image frame" + diag, scs::LogLevel::Info, "Vision");
        }
    }

    // Gán Flow/module <path> cho khung; đã gán đúng đường dẫn đó trong lần nạp đó rồi thì thôi. False khi solution chưa
    // có Flow/module đó, hoặc SDK đang bận chạy Flow (RenderSource không chờ) — nhịp sau thử lại.
    bool MainPage::BindCam(scs::VisionMasterDevice& device, std::string const& path, int loadCount)
    {
        CamFrame& f = m_cam;
        const std::string key = BindKey(loadCount, path);
        if (f.BoundKey == key) return true;
        if (!f.Pane.Created() || path.empty()) return false;

        // Con trỏ thuộc về SDK: đưa cho control NGAY, không cất giữ.
        void* source = device.RenderSource(path);
        if (source == nullptr) return false;

        HRESULT hr = f.Pane.Host().CallPtr(ui::vmdisp::RenderSetParamsInfo, source);
        if (FAILED(hr))
        {
            if (f.FailLoggedKey != key)
            {
                f.FailLoggedKey = key;
                scs::LogWorker::Instance().RecordLog(
                    std::format("Main: CAM image frame '{}' — SetParamsInfo hr=0x{:08X} {}", path, (unsigned)hr,
                                ::base::Narrow(f.Pane.Host().LastError())),
                    scs::LogLevel::Warning, "Vision");
            }
            return false;
        }
        f.BoundKey = key;
        return true;
    }

    // Hiện/ẩn thanh công cụ của khung CAM (Tuấn 06/10). Nút tô màu nhấn khi thanh đang hiện. Lời gọi vào control đi qua
    // hàng đợi như mọi lời gọi VM khác (xem QueueVmWork) — không gọi ngay trong handler Click.
    void MainPage::OnCamToolbarClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        CamFrame& f = m_cam;
        f.ToolbarShown = !f.ToolbarShown;
        ui::SetTint(f.ToolbarButton, f.ToolbarShown ? ui::Tint::Accent : ui::Tint::None);
        ui::PostUi([weak = get_weak()] {
            auto self = weak.get();
            if (!self || self->m_closing) return;
            CamFrame& cam = self->m_cam;
            if (cam.Pane.Created())
                cam.Pane.Host().CallBool(ui::vmdisp::RenderSetToolbarVisible, cam.ToolbarShown);
        });
    }

    void MainPage::OnShown(bool visible)
    {
        m_visible = visible;
        ApplyPaneVisibility();
        // Mỗi lần mở lại trang Main thì xét lại — đổi tên module ở Setting là ăn ngay.
        if (visible) QueueVmWork(true);
    }

    void MainPage::OnWindowMoved()
    {
        m_cam.Pane.Sync();
    }

    void MainPage::OnModalChanged(bool modalOpen)
    {
        m_cam.Pane.SetEnabled(!modalOpen);
    }

    void MainPage::OnClosing()
    {
        m_closing = true;
        if (m_armedBlink) m_armedBlink.Stop();
        m_cam.Pane.Dispose();
    }

    // ══════════ Lệnh ══════════ Request* chỉ đặt cờ — vòng quét của McControl quyết định làm gì (interlock ở đó).

    // Công tắc AUTO/MANUAL như ShieldCanAssy.WinUI: chạm vào đâu trên thanh cũng đổi sang chế độ kia.
    void MainPage::OnModeSwitchTapped(WF::IInspectable const&, Input::TappedRoutedEventArgs const&)
    {
        const bool isAuto = scs::McControl::Status().Mode() == scs::EMcMode::Auto;
        scs::McControl::RequestSetMode(isAuto ? scs::EMcMode::Manual : scs::EMcMode::Auto);
    }

    void MainPage::OnModeSwitchPointerEntered(WF::IInspectable const&, Input::PointerRoutedEventArgs const&)
    {
        ModeSwitch().Background(ui::Solid(0x17000000));
    }

    void MainPage::OnModeSwitchPointerExited(WF::IInspectable const&, Input::PointerRoutedEventArgs const&)
    {
        ModeSwitch().Background(ui::Solid(0x0F000000));
    }

    void MainPage::OnModeSwitchPointerPressed(WF::IInspectable const&, Input::PointerRoutedEventArgs const&)
    {
        ModeSwitch().Background(ui::Solid(0x1F000000));
    }

    void MainPage::OnModeSwitchPointerReleased(WF::IInspectable const&, Input::PointerRoutedEventArgs const&)
    {
        ModeSwitch().Background(ui::Solid(0x17000000));
    }

    // Nút mềm CỐ Ý chỉ đưa máy vào sẵn sàng (Armed) — máy chạy thật chỉ nhận từ nút START vật lý
    // (McControl::RequestStart), không có đường nào từ màn hình cảm ứng chạy thẳng máy.
    void MainPage::OnRunClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::RequestArm();
    }

    void MainPage::OnStopClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::RequestStop();
    }

    void MainPage::OnOriginClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::RequestOrigin();
    }

    // Bấm đúp mới chạy (quy ước 2026-09-26: nút làm trục chạy phải bấm đúp) — không có Click.
    void MainPage::OnInitDoubleTapped(WF::IInspectable const&, Input::DoubleTappedRoutedEventArgs const&)
    {
        scs::McControl::RequestInit();
    }

    void MainPage::OnResetClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::RequestReset();
    }

    void MainPage::OnMuteClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::RequestMuteBuzzer();
    }

    void MainPage::OnClearCountersClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto run = scs::McDataManager::RunData()) run->ResetCounters();
        UpdateCounters();
    }

    void MainPage::OnClearCellsClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::ClearCellSelection();
        UpdateTrays();
    }

    // RESET TRAY: đặt lại tiến độ 2 tray (lưu running.json) — đang chờ thay tray thì bằng bấm START; đang chạy thì từ chối
    // kèm lý do. ResetTrays ghi file nên chạy ở luồng nền.
    void MainPage::OnResetTrayClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        auto why = std::make_shared<std::string>();
        ui::RunBackgroundThenUi(
            [why] { *why = scs::McControl::ResetTrays(); },
            [why] {
                if (!why->empty()) ui::Dialogs::Warn("Cannot reset tray progress: " + *why, "Reset tray");
            });
    }
}
