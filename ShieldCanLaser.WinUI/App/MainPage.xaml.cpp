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

    /// <summary>Đường dẫn Flow/module của khung CAM. Ô ở Setting (RenderCam) ghi đường dẫn đầy đủ: "LocateCan" = cả Flow,
    /// "LocateCan.Module" = module trong Flow. Trống: cả Flow LocateCan.</summary>
    std::string CurrentRenderPath()
    {
        auto sys = scs::McDataManager::SysData();
        const scs::VisionOutputNames& n = sys->VisionOutputs;
        return scs::VisionOutputNames::RenderPath(n.FlowLocateCan, n.RenderCam);
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
        default:                          return L"TrayCellEmptyBrush";
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

        // Nút đang chọn tô theo màu đèn tháp tương ứng (AUTO xanh lá, MANUAL cam).
        ui::SetTint(AutoButton(), mode == scs::EMcMode::Auto ? ui::Tint::Green : ui::Tint::None);
        ui::SetTint(ManualButton(), mode == scs::EMcMode::Manual ? ui::Tint::Caution : ui::Tint::None);

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
                // Sơ đồ tray: tray 1 đã khắc 3 hàng, hàng vừa xong 6 OK + 1 NG; tray 2 vắng (chỉ dữ liệu tạm của chế độ chụp ảnh).
                if (auto fake = std::dynamic_pointer_cast<scs::FakeIo>(scs::McControl::Io()))
                    fake->SimSet(scs::IoMap::TrayPresent(2), false);
                if (auto r = scs::McDataManager::RunData())
                {
                    const int ok = (int)scs::ECellState::Ok, ng = (int)scs::ECellState::Ng;
                    r->TrayRowsDone[0] = 3;
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
        // Bảng thời gian — xem chú thích ở MainPage.xaml. "—" = chưa có số.
        auto perProduct = [](double s) { return s > 0 ? std::format("{:.2f} s", s) : std::string("—"); };
        auto clock = [](double s) {
            const long long tenths = std::llround(s * 10);
            return std::format("{:02}:{:02}.{}", tenths / 600, tenths / 10 % 60, tenths % 10);
        };
        const double nowSec = scs::McControl::CurrentCycleSec();
        const int nowDone = scs::McControl::CurrentCycleProducts();
        ui::SetText(PreTackText(), perProduct(run->LastProductTimeSec));
        ui::SetText(TackText(), nowDone > 0 ? perProduct(nowSec / nowDone) : std::string("—"));
        ui::SetText(CycleNowText(), nowSec > 0 ? clock(nowSec) : std::string("—"));
        ui::SetText(CyclePrevText(), run->LastCycleSec > 0 ? clock(run->LastCycleSec) : std::string("—"));

        const double avg = run->AvgProductTimeSec;
        const double uph = avg > 0 ? 3600.0 / avg : 0.0;
        ui::SetText(UphText(), std::format("Est. throughput: {} pcs/h", std::llround(uph)));
    }

    // ══════════ Sơ đồ tray + dải nozzle ══════════

    // Chú thích màu ở đáy card Trays: ô mẫu + chữ (chung cho tray và nozzle).
    void MainPage::BuildTrayLegend()
    {
        auto legend = TrayLegend();
        legend.Children().Clear();
        const std::pair<scs::ECellState, wchar_t const*> items[] = {
            { scs::ECellState::Loaded, L"Loaded" }, { scs::ECellState::Processing, L"Processing" },
            { scs::ECellState::Ok, L"OK" }, { scs::ECellState::Ng, L"NG" },
            { scs::ECellState::Skipped, L"Done earlier" }, { scs::ECellState::Empty, L"Empty" },
        };
        for (const auto& [state, label] : items)
        {
            StackPanel item;
            item.Orientation(Orientation::Horizontal);
            item.Spacing(5);
            Border swatch;
            swatch.Width(14);
            swatch.Height(14);
            swatch.CornerRadius(CornerRadiusHelper::FromUniformRadius(3));
            swatch.BorderThickness(ThicknessHelper::FromUniformLength(1));
            swatch.BorderBrush(ui::Res(L"ControlStrokeSecondaryBrush"));
            swatch.Background(ui::Res(CellBrushKey((int)state)));
            swatch.VerticalAlignment(VerticalAlignment::Center);
            TextBlock text;
            text.Text(label);
            text.FontSize(12);
            text.VerticalAlignment(VerticalAlignment::Center);
            text.Foreground(ui::Res(L"TextSecondaryBrush"));
            item.Children().Append(swatch);
            item.Children().Append(text);
            legend.Children().Append(item);
        }
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
            row.Cell.Background(ui::Res(L"TrayCellEmptyBrush"));
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

            row.Dev = TextBlock();
            row.Dev.FontSize(12);
            row.Dev.FontFamily(winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Consolas"));
            row.Dev.VerticalAlignment(VerticalAlignment::Center);
            row.Dev.HorizontalAlignment(HorizontalAlignment::Right);
            Grid::SetRow(row.Dev, k - 1);
            Grid::SetColumn(row.Dev, 2);
            grid.Children().Append(row.Dev);
        }
    }

    // Ô kiểu nút Calculator Win11 (như ShieldCANAssy): không bóng, viền 1 px nhạt, bo góc 4, khe 3 px. Chỉ dựng lại khi
    // số ô hoặc số cột đổi (đổi số hàng tray ở trang Teaching).
    void MainPage::LayoutMap(TrayMap& map, int count, int cols, double cellHeight, bool labels)
    {
        if (!map.Host || cols < 1) return;
        if ((int)map.Cells.size() == count && map.Cols == cols) return;
        auto grid = map.Host;
        grid.Children().Clear();
        grid.ColumnDefinitions().Clear();
        grid.RowDefinitions().Clear();
        grid.ColumnSpacing(3);
        grid.RowSpacing(3);
        map.Cells.clear();
        map.Shown.assign((size_t)std::max(0, count), -1);
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
            Border cell;
            if (stretchRows) cell.MinHeight(18);
            else cell.Height(cellHeight);
            cell.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
            cell.BorderThickness(ThicknessHelper::FromUniformLength(1));
            cell.BorderBrush(ui::Res(L"ControlStrokeSecondaryBrush"));
            cell.Background(ui::Res(L"TrayCellEmptyBrush"));
            if (labels)
            {
                TextBlock text;
                text.Text(winrt::to_hstring(i + 1));
                text.FontSize(12);
                text.HorizontalAlignment(HorizontalAlignment::Center);
                text.VerticalAlignment(VerticalAlignment::Center);
                cell.Child(text);
            }
            Grid::SetRow(cell, i / cols);
            Grid::SetColumn(cell, i % cols);
            grid.Children().Append(cell);
            map.Cells.push_back(cell);
        }
    }

    void MainPage::PaintMap(TrayMap& map, std::vector<int> const& states)
    {
        for (size_t i = 0; i < map.Cells.size(); ++i)
        {
            int state = i < states.size() ? states[i] : (int)scs::ECellState::Empty;
            if (map.Shown[i] == state) continue;
            map.Shown[i] = state;
            map.Cells[i].Background(ui::Res(CellBrushKey(state)));
        }
    }

    // 2 tray TrayRows × 7 ô (hàng = một lượt gắp, cột = nozzle). Chu trình ghi Routine_Auto::Trays lúc chạy; mới mở app
    // (mảng rỗng / sai kích thước) thì suy từ RunningData.TrayRowsDone: hàng đã xong = Skipped, còn lại Loaded. Tray vắng
    // (cảm biến tắt) tô Empty.
    void MainPage::UpdateTrays()
    {
        auto model = scs::McDataManager::Model();
        auto run = scs::McDataManager::RunData();
        if (!model || !run) return;
        auto view = scs::Routine_Auto::Trays();
        auto sys = scs::McDataManager::SysData();
        auto io = scs::McControl::Io();

        const int rows = std::max(1, model->Auto.TrayRows), cols = NozzleCount;
        const size_t size = (size_t)rows * (size_t)cols;
        std::array<TextBlock, TrayCount> summaries{ TraySummaryText1(), TraySummaryText2() };
        for (int t = 0; t < TrayCount; ++t)
        {
            const bool present = !sys || !sys->HasTraySensors || (io && io->GetDi(scs::IoMap::TrayPresent(t + 1)));
            const int done = std::clamp(run->TrayRowsDone[t], 0, rows);
            std::vector<int> cells = view.Trays[(size_t)t];
            if (cells.size() != size)
            {
                cells.assign(size, present ? (int)scs::ECellState::Loaded : (int)scs::ECellState::Empty);
                if (present) std::fill(cells.begin(), cells.begin() + (size_t)done * cols, (int)scs::ECellState::Skipped);
            }
            else if (!present && CountOf(cells, scs::ECellState::Processing) == 0)
            {
                cells.assign(size, (int)scs::ECellState::Empty);
            }
            LayoutMap(m_trayMaps[(size_t)t], (int)size, cols, 0, false);
            PaintMap(m_trayMaps[(size_t)t], cells);
            const int ok = CountOf(cells, scs::ECellState::Ok), ng = CountOf(cells, scs::ECellState::Ng);
            std::string text = present ? std::format("{}/{} rows done", done, rows) : std::string("No tray");
            if (ok + ng > 0) text += std::format(" · {} OK · {} NG", ok, ng);
            ui::SetText(summaries[(size_t)t], text);
        }
    }

    // Dải nozzle: ô màu theo ECellState (Empty = không giữ can, Loaded = đang giữ, Processing = đang soi/khắc, Ok = đã
    // khắc, Ng), chữ trạng thái và độ lệch dX / dZ đo ở CAM (chỉ khi HasDeviation).
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
                row.Cell.Background(ui::Res(CellBrushKey(n.State)));
            }
            ui::SetText(row.Text, n.Text.empty() ? std::string("—") : n.Text);
            ui::SetText(row.Dev, n.HasDeviation ? std::format("dX {:+.3f}  dZ {:+.3f}", n.Dx, n.Dz) : std::string());
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

    // Gắn control vào Flow/module đặt ở trang Setting (mặc định cả Flow LocateCan). Chỉ gắn SAU khi McControl nạp xong
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

    void MainPage::OnAutoClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::RequestSetMode(scs::EMcMode::Auto);
    }

    void MainPage::OnManualClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::RequestSetMode(scs::EMcMode::Manual);
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
