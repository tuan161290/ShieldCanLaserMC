#include "pch.h"
#include "VisionEditPage.xaml.h"
#if __has_include("VisionEditPage.g.cpp")
#include "VisionEditPage.g.cpp"
#endif

#include <format>
#include <mutex>

#include "Base/Async.h"
#include "Base/Text.h"
#include "Controller/McControl.h"
#include "Controller/VmCycle.h"
#include "Data/McDataManager.h"
#include "Infor/AxisId.h"
#include "Infor/IoMap.h"
#include "Ui/AppHost.h"
#include "Ui/Dialogs.h"
#include "Ui/JogControl.h"
#include "Ui/UiUtil.h"
#include "Utility/DateTimeText.h"
#include "Utility/Log/LogWorker.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
namespace WF = winrt::Windows::Foundation;

namespace
{
    std::string When(const std::optional<scs::TimePoint>& t)
    {
        return t ? scs::FormatLocal(*t, "dd/MM HH:mm") : std::string();
    }

    std::string Num(double v) { return scs::VmCycle::Fmt(v); }

    /// Đặt tooltip chỉ khi chữ đổi — vòng cập nhật 100 ms không được dựng lại tooltip mỗi nhịp.
    void SetTip(FrameworkElement const& e, std::string& last, std::string const& text)
    {
        if (text == last) return;
        last = text;
        ToolTipService::SetToolTip(e, box_value(ui::H(text)));
    }

    int TagOf(WF::IInspectable const& sender)
    {
        auto fe = sender.try_as<FrameworkElement>();
        if (!fe) return -1;
        int v = -1;
        return ui::ParseInt(unbox_value_or<hstring>(fe.Tag(), L""), v) ? v : -1;
    }
}

namespace winrt::ShieldCanLaser::implementation
{
    void VisionEditPage::OnRootLoaded(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loaded) return;
        m_loaded = true;
        BuildJogGrid();
        BuildNozzleChips();
        BuildNumpad();
        BuildNozzleSummary();
        LoadInputs();
        // Nozzle đang chọn = nozzle ngậm can mẫu của bước lưới (system.json).
        int k = scs::McDataManager::SysData()->VisionCalib.CalibNozzle;
        SelectNozzle(k >= 1 && k <= NozzleCount ? k : 1);
        SelectStep(0);
        OnTick();
        if (m_visible) QueueVmWork();   // OnShown(true) tới trước Loaded thì EnsureVmPanes đã bỏ qua
    }

    // Một hàng X, Y, Z (AxisId::GridSlot): tên trục, nút −, nút +. Giữ để chạy, nhả là dừng.
    void VisionEditPage::BuildJogGrid()
    {
        for (const char* axisName : scs::AxisId::All)
        {
            StackPanel cell;
            cell.Orientation(Orientation::Horizontal);
            cell.Margin(ThicknessHelper::FromLengths(0, 2, 12, 2));

            TextBlock name;
            name.Text(ui::H(axisName));
            name.Width(26);
            name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            name.FontSize(14);
            name.VerticalAlignment(VerticalAlignment::Center);
            cell.Children().Append(name);

            for (int dir : { -1, 1 })
            {
                Button b;
                b.Content(box_value(dir < 0 ? L"−" : L"+"));
                b.Width(40);
                b.FontSize(18);
                b.Padding(ThicknessHelper::FromUniformLength(0));
                if (dir < 0) b.Margin(ThicknessHelper::FromLengths(0, 0, 4, 0));
                ToolTipService::SetToolTip(b, box_value(dir < 0 ? L"Hold to jog backward, release to stop" : L"Hold to jog forward, release to stop"));
                ui::AttachJog(b, axisName, dir);
                cell.Children().Append(b);
                m_jogButtons.push_back(b);
            }

            int row = 0, column = 0;
            scs::AxisId::GridSlot(axisName, row, column);
            Grid::SetRow(cell, row);
            Grid::SetColumn(cell, column);
            JogGrid().Children().Append(cell);
        }
    }

    // 7 nút chọn nozzle: số to + chấm trạng thái (xanh = master đã đặt).
    void VisionEditPage::BuildNozzleChips()
    {
        for (int k = 1; k <= NozzleCount; ++k)
        {
            Button b;
            b.Style(ui::ResStyle(L"FlatButtonStyle"));
            b.Width(38);
            b.Height(40);
            b.Padding(ThicknessHelper::FromUniformLength(0));
            b.FontSize(15);
            b.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            b.CornerRadius(CornerRadiusHelper::FromUniformRadius(6));
            b.Tag(box_value(winrt::to_hstring(k)));

            StackPanel content;
            content.Spacing(3);
            content.HorizontalAlignment(HorizontalAlignment::Center);
            content.VerticalAlignment(VerticalAlignment::Center);
            TextBlock num;
            num.Text(winrt::to_hstring(k));
            num.HorizontalAlignment(HorizontalAlignment::Center);
            content.Children().Append(num);
            Shapes::Ellipse dot;
            dot.Width(6);
            dot.Height(6);
            dot.HorizontalAlignment(HorizontalAlignment::Center);
            content.Children().Append(dot);
            b.Content(content);

            b.Click([weak = get_weak()](WF::IInspectable const& s, RoutedEventArgs const&) {
                if (auto self = weak.get()) { self->CommitInputs(); self->SelectNozzle(TagOf(s)); }
            });
            NozzleRow().Children().Append(b);
            m_nozzleChips.push_back(b);
            m_nozzleDots.push_back(dot);
        }
    }

    // Numpad 3×3 của lưới 9 điểm: 7 8 9 hàng trên, 1 2 3 hàng dưới, 5 = tâm. Bấm đúp = chạy tới rồi nạp một điểm.
    void VisionEditPage::BuildNumpad()
    {
        for (int n = 1; n <= 9; ++n)
        {
            Button b;
            b.Content(box_value(winrt::to_hstring(n)));
            b.Tag(box_value(winrt::to_hstring(n)));
            b.Width(44);
            b.Padding(ThicknessHelper::FromUniformLength(0));
            ToolTipService::SetToolTip(b, box_value(ui::H(std::format("Double-click: move to point {} and load one point into N-Point", n))));
            b.DoubleTapped([weak = get_weak()](WF::IInspectable const& s, Input::DoubleTappedRoutedEventArgs const& e) {
                if (auto self = weak.get()) self->OnGridPointDoubleTapped(s, e);
            });
            int idx = n - 1;
            Grid::SetRow(b, 2 - idx / 3);
            Grid::SetColumn(b, idx % 3);
            Numpad().Children().Append(b);
            m_numpad.push_back(b);
        }
    }

    // 7 dòng tóm tắt (bước Master): "N1  ref +0.123/−0.045 · 12/10 14:02 · trim +0.05/0".
    void VisionEditPage::BuildNozzleSummary()
    {
        for (int k = 1; k <= NozzleCount; ++k)
        {
            TextBlock row;
            // ValueTextStyle nằm trong Resources của trang (không phải Styles.xaml của app).
            // (Trong lớp, "Style" là thuộc tính của UserControl — ghi rõ kiểu.)
            if (auto value = Resources().TryLookup(box_value(L"ValueTextStyle")))
                if (auto style = value.try_as<winrt::Microsoft::UI::Xaml::Style>()) row.Style(style);
            row.FontSize(11);
            NozzleSummary().Children().Append(row);
            m_summaryRows.push_back(row);
        }
    }

    // ═════════════════════════ vòng cập nhật ═════════════════════════

    void VisionEditPage::OnTick()
    {
        if (!m_loaded) return;

        bool busy = m_busy.load();
        for (auto& b : m_jogButtons)
            if (b.IsEnabled() == busy) b.IsEnabled(!busy);
        if (ZSafeButton().IsEnabled() == busy) ZSafeButton().IsEnabled(!busy);
        // Đang chạy một bước: khoá chọn nozzle/bước và mọi nút của khung — một việc một lúc. STOP ở thanh trên vẫn bấm được.
        for (FrameworkElement e : { NavPanel().as<FrameworkElement>(), StepHost().as<FrameworkElement>() })
            if (e.IsHitTestVisible() == busy) { e.IsHitTestVisible(!busy); e.Opacity(busy ? 0.55 : 1.0); }

        // Mức Chậm/Nhanh dùng chung với trang Manual/Teaching — có thể vừa đổi ở đó.
        auto func = scs::McControl::Func();
        bool fast = func && func->JogFast();
        ui::SetTint(JogSlowButton(), fast ? ui::Tint::None : ui::Tint::Accent);
        ui::SetTint(JogFastButton(), fast ? ui::Tint::Accent : ui::Tint::None);

        auto vision = scs::McControl::Vision();
        bool loaded = vision && vision->IsSolutionLoaded();
        SolutionLamp().Fill(ui::StatusBrush(loaded));
        std::string path = scs::McDataManager::SysData()->VisionSolutionPath;
        size_t slash = path.find_last_of("\\/");
        std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
        ui::SetText(SolutionText(), ::base::IsBlank(path) ? std::string("No solution configured (Setting page)")
                                                          : (loaded ? "Solution: " : "Not loaded: ") + file);
        SetTip(SolutionText(), m_solutionTip, path);

        std::string message = scs::McControl::Status().Message();
        ui::Show(MessageBar(), !::base::IsBlank(message));
        ui::SetText(MessageText(), message);

        if (busy)
        {
            std::string p;
            {
                std::lock_guard<std::mutex> l(*m_progressMutex);
                p = *m_progress;
            }
            ui::SetText(ResultText(), m_runTitle + (p.empty() ? "…" : " — " + p));
        }

        UpdateCalibView();

        // Solution vừa được nạp (lần đầu hoặc nạp lại): gắn trình soạn Flow vào nó.
        if (m_visible && m_config.Created() && loaded && scs::McControl::VisionLoadCount() != m_boundLoadCount)
            QueueVmWork();
    }

    // Trạng thái của cột hiệu chuẩn: chip cổng Auto, chấm trạng thái từng nozzle, thanh bước, chữ trong khung của bước.
    void VisionEditPage::UpdateCalibView()
    {
        auto model = scs::McDataManager::Model();
        auto sys = scs::McDataManager::SysData();
        if (!model || !sys) return;
        const auto& c = model->Calib;
        const auto& n = c.Nozzle(m_nozzle);
        auto cal = scs::McControl::Calib();

        // ── chip cổng Auto ──
        auto gate = scs::VmCycle::EvaluateCalib(model.get(), sys.get(), 0);
        bool ready = gate.Ready();
        GateChip().Background(ui::Res(ready ? L"SuccessBackgroundBrush" : L"CautionBackgroundBrush"));
        GateDot().Fill(ui::Res(ready ? L"SuccessBrush" : L"CautionBrush"));
        GateText().Foreground(ui::Res(ready ? L"SuccessBrush" : L"CautionBrush"));
        ui::SetText(GateText(), ready ? std::string("Ready for Auto") : std::string("Not ready"));
        SetTip(GateChip(), m_gateTip, gate.Describe());

        // ── chip nozzle: xanh = master đã đặt; xám = chưa ──
        for (int k = 1; k <= NozzleCount && k <= (int)m_nozzleChips.size(); ++k)
        {
            const auto& nk = c.Nozzle(k);
            m_nozzleDots[k - 1].Fill(ui::Res(nk.RefMeasured() ? L"SuccessBrush" : L"ControlStrokeSecondaryBrush"));
            ui::SetTint(m_nozzleChips[k - 1], k == m_nozzle ? ui::Tint::Accent : ui::Tint::None);
            SetTip(m_nozzleChips[k - 1], m_nozzleTips[k], std::format("Nozzle {} — master: {}{}", k,
                nk.RefMeasured() ? "set " + When(nk.RefAt) : std::string("not set"),
                (nk.TrimX != 0 || nk.TrimZ != 0) ? std::format(" · trim {:+.3f}/{:+.3f}", nk.TrimX, nk.TrimZ) : std::string()));
        }

        // ── thanh bước ──
        std::optional<scs::Routine_Calib::LastRead> last = cal ? cal->Last(m_nozzle) : std::nullopt;
        std::array<bool, StepCount> done{ c.GridVerified(), n.RefMeasured(), false };
        std::array<std::string, StepCount> status{
            c.GridVerified() ? When(c.GridVerifiedAt) : std::string("not verified"),
            n.RefMeasured() ? When(n.RefAt) : std::string("not set"),
            (n.TrimX != 0 || n.TrimZ != 0) ? std::string("trim set") : std::string()
        };
        for (int i = 0; i < StepCount; ++i)
        {
            bool sel = i == m_step;
            StepButton(i).Background(sel ? ui::Res(L"SubtleSelectedBrush") : Media::Brush{ nullptr });
            StepLabel(i).Foreground(ui::Res(sel ? L"AccentTextBrush" : L"TextSecondaryBrush"));
            StepLabel(i).FontWeight(sel ? Windows::UI::Text::FontWeights::SemiBold() : Windows::UI::Text::FontWeights::Normal());
            StepBadge(i).Background(ui::Res(done[i] ? L"SuccessBrush" : sel ? L"AccentFillBrush" : L"ControlFillBrush"));
            StepBadge(i).BorderBrush(ui::Res(done[i] ? L"SuccessBrush" : sel ? L"AccentFillBrush" : L"ControlStrokeSecondaryBrush"));
            StepBadgeText(i).Foreground(ui::Res(done[i] || sel ? L"TextOnAccentBrush" : L"TextSecondaryBrush"));
            ui::SetText(StepBadgeText(i), done[i] ? std::string("✓") : std::to_string(i + 1));
            ui::SetText(StepStatus(i), status[i]);
        }

        // ── khung của bước ──
        const auto& vc = sys->VisionCalib;
        ui::SetText(GridTitle(), std::format("Grid X × Z · nozzle {}", m_nozzle));
        ui::SetText(CenterText(), vc.CenterSet ? std::format("X {:.3f} · Z {:.3f}", vc.CenterX, vc.CenterZ) : std::string("not saved"));
        ui::SetText(MasterTitle(), std::format("Master · nozzle {}", m_nozzle));
        ui::SetText(LaserTitle(), std::format("Laser trial · nozzle {}", m_nozzle));

        // Master của nozzle đang chọn + lần soi gần nhất (Rel và độ lệch so với master).
        ui::SetText(MasterStatusText(), n.RefMeasured()
            ? std::format("Ref X {:+.3f} · Z {:+.3f} mm · set {}", n.RefX, n.RefZ, When(n.RefAt))
            : std::string("Master not set — SET MASTER with a correctly seated can"));
        ui::SetText(LastReadText(), last
            ? std::format("Last capture: Rel X {:+.3f} · Z {:+.3f} mm → dX {:+.3f} · dZ {:+.3f}", last->RelX, last->RelZ, last->Dx, last->Dz)
            : std::string("No capture yet"));

        // 7 dòng tóm tắt.
        for (int k = 1; k <= NozzleCount && k <= (int)m_summaryRows.size(); ++k)
        {
            const auto& nk = c.Nozzle(k);
            std::string line = nk.RefMeasured()
                ? std::format("N{}  ref {:+.3f}/{:+.3f}  {}", k, nk.RefX, nk.RefZ, When(nk.RefAt))
                : std::format("N{}  master not set", k);
            if (nk.TrimX != 0 || nk.TrimZ != 0) line += std::format("  trim {:+.3f}/{:+.3f}", nk.TrimX, nk.TrimZ);
            ui::SetText(m_summaryRows[k - 1], line);
            m_summaryRows[k - 1].Foreground(ui::Res(k == m_nozzle ? L"AccentTextBrush" : nk.RefMeasured() ? L"TextPrimaryBrush" : L"TextTertiaryBrush"));
        }

        // Thử laser: đích sẽ tới = điểm teach + trim + bù theo lần soi gần nhất.
        std::string laser;
        {
            std::string why;
            auto p = model->LaserPoint(m_nozzle, why);
            if (!p || !p->Has(scs::AxisId::X) || !p->Has(scs::AxisId::Z)) laser = "Laser point not taught (Teaching page)";
            else
            {
                bool useVision = last.has_value() && n.RefMeasured();
                auto dev = scs::VmCycle::Deviation(n, last ? last->RelX : 0.0, last ? last->RelZ : 0.0, sys->LaserComp,
                                                   p->Get(scs::AxisId::X), p->Get(scs::AxisId::Z), useVision);
                laser = dev.Ok
                    ? std::format("Target X {:.3f} · Z {:.3f}  ({}dX {:+.3f} · dZ {:+.3f} · trim {:+.3f}/{:+.3f})",
                                  dev.TargetX, dev.TargetZ, useVision ? "" : "no capture — ", dev.Dx, dev.Dz, n.TrimX, n.TrimZ)
                    : "Cannot compensate: " + dev.Why;
            }
        }
        ui::SetText(LaserStatusText(), laser);
    }

    // ═════════════════════════ chọn nozzle / bước, ô nhập ═════════════════════════

    void VisionEditPage::SelectNozzle(int nozzle)
    {
        if (nozzle < 1 || nozzle > NozzleCount) return;
        m_nozzle = nozzle;
        // Nozzle ngậm can mẫu của bước lưới đi theo lựa chọn này (system.json).
        auto sys = scs::McDataManager::SysData();
        if (sys && sys->VisionCalib.CalibNozzle != nozzle)
        {
            sys->VisionCalib.CalibNozzle = nozzle;
            scs::McDataManager::SaveSystemData();
        }
        LoadTrim();
        if (m_loaded) UpdateCalibView();
    }

    void VisionEditPage::SelectStep(int step)
    {
        if (step < 0 || step >= StepCount) return;
        m_step = step;
        for (int i = 0; i < StepCount; ++i) ui::Show(StepPanel(i), i == step);
        if (m_loaded) UpdateCalibView();
    }

    void VisionEditPage::OnStepClick(WF::IInspectable const& sender, RoutedEventArgs const&)
    {
        CommitInputs();
        SelectStep(TagOf(sender));
    }

    void VisionEditPage::OnInputLostFocus(WF::IInspectable const&, RoutedEventArgs const&)
    {
        CommitInputs();
    }

    void VisionEditPage::LoadInputs()
    {
        auto sys = scs::McDataManager::SysData();
        const auto& c = sys->VisionCalib;
        GridStepXBox().Text(ui::H(Num(c.GridStepX)));
        GridStepZBox().Text(ui::H(Num(c.GridStepZ)));
        ShotsBox().Text(winrt::to_hstring(c.ShotsPerPoint));
        VerifyStepBox().Text(ui::H(Num(c.VerifyStepMm)));
        ResidualTolBox().Text(ui::H(Num(c.ResidualTolMm)));
        const auto& lc = sys->LaserComp;
        GainXBox().Text(ui::H(Num(lc.GainX)));
        GainZBox().Text(ui::H(Num(lc.GainZ)));
        MaxCompBox().Text(ui::H(Num(lc.MaxCompMm)));
    }

    void VisionEditPage::LoadTrim()
    {
        auto model = scs::McDataManager::Model();
        if (!model || !m_loaded) return;
        const auto& n = model->Calib.Nozzle(m_nozzle);
        TrimXBox().Text(ui::H(std::format("{:.3f}", n.TrimX)));
        TrimZBox().Text(ui::H(std::format("{:.3f}", n.TrimZ)));
    }

    // Ô hỏng thì trả lại số cũ — không lưu số rác, cũng không bật hộp thoại giữa lúc gõ.
    void VisionEditPage::CommitInputs()
    {
        if (!m_loaded) return;
        auto sys = scs::McDataManager::SysData();
        auto& c = sys->VisionCalib;
        auto& lc = sys->LaserComp;
        bool changed = false;
        auto num = [&](TextBox const& box, double& field, double lo, double hi) {
            double v = 0;
            if (ui::ParseDouble(box.Text(), v) && v >= lo && v <= hi)
            {
                if (v != field) { field = v; changed = true; }
            }
            box.Text(ui::H(Num(field)));
        };
        auto integer = [&](TextBox const& box, int& field, int lo, int hi) {
            int v = 0;
            if (ui::ParseInt(box.Text(), v) && v >= lo && v <= hi)
            {
                if (v != field) { field = v; changed = true; }
            }
            box.Text(winrt::to_hstring(field));
        };
        num(GridStepXBox(), c.GridStepX, 0.1, 100);
        num(GridStepZBox(), c.GridStepZ, 0.1, 100);
        integer(ShotsBox(), c.ShotsPerPoint, 1, 50);
        num(VerifyStepBox(), c.VerifyStepMm, -50, 50);
        num(ResidualTolBox(), c.ResidualTolMm, 0.001, 5);
        num(GainXBox(), lc.GainX, -10, 10);
        num(GainZBox(), lc.GainZ, -10, 10);
        num(MaxCompBox(), lc.MaxCompMm, 0.01, 50);
        if (changed) scs::McDataManager::SaveSystemData();
    }

    // ═════════════════════════ chạy một bước ═════════════════════════

    bool VisionEditPage::CanRun(bool needsOrigin, std::string& why) const
    {
        auto& st = scs::McControl::Status();
        if (!scs::McControl::IsInitialized() || !scs::McControl::Calib()) why = "machine not initialized";
        else if (st.Mode() != scs::EMcMode::Manual) why = "switch to MANUAL first";
        else if (scs::McControl::RoutineBusy()) why = "a routine is running — wait or STOP";
        else if (needsOrigin && !st.OrgOk()) why = "not homed — run Origin first";
        return why.empty();
    }

    void VisionEditPage::Run(std::string title, Work work, bool needsOrigin)
    {
        if (m_busy.load()) return;
        std::string why;
        if (!CanRun(needsOrigin, why)) { ShowResult(ResultKind::Fail, title + ": " + why); return; }
        if (m_busy.exchange(true)) return;
        CommitInputs();
        m_runTitle = title;
        {
            std::lock_guard<std::mutex> l(*m_progressMutex);
            m_progress->clear();
        }
        ShowResult(ResultKind::Busy, title + "…");
        OnTick();

        auto result = std::make_shared<scs::CalibResult>();
        ui::RunBackgroundThenUi(
            [work = std::move(work), result] {
                auto cal = scs::McControl::Calib();
                auto safety = scs::McControl::Safety();
                if (!cal || !safety) { *result = scs::CalibResult::Fail("machine not initialized"); return; }
                try { *result = work(*cal, safety->AbortToken()); }
                catch (const ::base::OperationCanceled&) { *result = scs::CalibResult::Fail("canceled"); }
                catch (const std::exception& ex) { *result = scs::CalibResult::Fail(ex.what()); }
            },
            [weak = get_weak(), title, result] {
                auto self = weak.get();
                if (!self || self->m_closing) return;
                self->m_busy = false;
                self->ShowResult(result->Ok ? ResultKind::Ok : ResultKind::Fail, title + (result->Ok ? " — " : ": ") + result->Text);
                self->LoadTrim();
                self->OnTick();
            });
    }

    void VisionEditPage::ShowResult(ResultKind kind, std::string const& text)
    {
        bool busy = kind == ResultKind::Busy;
        BusyRing().IsActive(busy);
        ui::Show(BusyRing(), busy);
        ui::Show(ResultIcon(), !busy);
        switch (kind)
        {
        case ResultKind::Ok:   ResultIcon().Glyph(L""); ResultIcon().Foreground(ui::Res(L"SuccessBrush")); break;
        case ResultKind::Fail: ResultIcon().Glyph(L""); ResultIcon().Foreground(ui::Res(L"CautionBrush")); break;
        default:               ResultIcon().Glyph(L""); ResultIcon().Foreground(ui::Res(L"TextTertiaryBrush")); break;
        }
        ResultText().Foreground(ui::Res(kind == ResultKind::Idle || busy ? L"TextSecondaryBrush" : L"TextPrimaryBrush"));
        ui::SetText(ResultText(), text);
    }

    // ── 1 · Lưới X × Z ──

    void VisionEditPage::OnSaveCenterClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        Run("Set center", [](scs::Routine_Calib& cal, std::stop_token) { return cal.SaveGridCenter(); });
    }

    void VisionEditPage::OnGridPointDoubleTapped(WF::IInspectable const& sender, Input::DoubleTappedRoutedEventArgs const&)
    {
        int n = TagOf(sender);
        if (n < 1 || n > 9) return;
        Run(std::format("Point {}", n), [n](scs::Routine_Calib& cal, std::stop_token ct) { return cal.RunGridPoint(n, ct); });
    }

    winrt::fire_and_forget VisionEditPage::OnRunGridDoubleTapped(WF::IInspectable const&, Input::DoubleTappedRoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (m_busy.load()) co_return;
        int nozzle = m_nozzle;
        bool ok = co_await ui::Dialogs::ConfirmAsync(
            std::format("The app will press Clear Point on the N-Point module in Flow LocateCan, then run 9 points with nozzle {} "
                        "(holding the master can), loading one point each (calibration branch, IsLocate = 0).\n\n"
                        "Is the N-Point module ENABLED? When done: Calibrate → Create Calibration File, then disable the module.", nozzle),
            "Run 9 points");
        if (!ok) co_return;
        auto progress = m_progress;
        auto mutex = m_progressMutex;
        Run("Run 9 points", [progress, mutex](scs::Routine_Calib& cal, std::stop_token ct) {
            return cal.RunGrid([progress, mutex](const std::string& s) { std::lock_guard<std::mutex> l(*mutex); *progress = s; }, ct);
        });
    }

    void VisionEditPage::OnCaptureHereClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        Run("Capture here", [](scs::Routine_Calib& cal, std::stop_token ct) { return cal.TriggerHere(ct); });
    }

    void VisionEditPage::OnVerifyGridDoubleTapped(WF::IInspectable const&, Input::DoubleTappedRoutedEventArgs const&)
    {
        Run("Verify grid", [](scs::Routine_Calib& cal, std::stop_token ct) { return cal.VerifyGrid(ct); });
    }

    // ── 2 · Master ──

    void VisionEditPage::OnGoToCamDoubleTapped(WF::IInspectable const&, Input::DoubleTappedRoutedEventArgs const&)
    {
        int k = m_nozzle;
        Run(std::format("Go to CAM · nozzle {}", k), [k](scs::Routine_Calib& cal, std::stop_token ct) { return cal.GoToCam(k, ct); });
    }

    void VisionEditPage::OnCaptureClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        int k = m_nozzle;
        Run(std::format("Capture · nozzle {}", k), [k](scs::Routine_Calib& cal, std::stop_token ct) { return cal.Capture(k, ct); });
    }

    winrt::fire_and_forget VisionEditPage::OnSetMasterDoubleTapped(WF::IInspectable const&, Input::DoubleTappedRoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (m_busy.load()) co_return;
        int k = m_nozzle;
        bool ok = co_await ui::Dialogs::ConfirmAsync(
            std::format("Nozzle {} will move to its CAM point and the average of N images becomes the master (Ref) of this nozzle. "
                        "Its laser trim is cleared.\n\nIs a correctly seated master can held on nozzle {}?", k, k),
            std::format("Set master · nozzle {}", k));
        if (!ok) co_return;
        Run(std::format("Set master · nozzle {}", k), [k](scs::Routine_Calib& cal, std::stop_token ct) { return cal.SetMaster(k, ct); });
    }

    // ── 3 · Thử laser ──

    void VisionEditPage::OnGoToLaserDoubleTapped(WF::IInspectable const&, Input::DoubleTappedRoutedEventArgs const&)
    {
        int k = m_nozzle;
        Run(std::format("Go to laser · nozzle {}", k), [k](scs::Routine_Calib& cal, std::stop_token ct) { return cal.GoToLaser(k, ct); });
    }

    winrt::fire_and_forget VisionEditPage::OnTestMarkDoubleTapped(WF::IInspectable const&, Input::DoubleTappedRoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (m_busy.load()) co_return;
        bool ok = co_await ui::Dialogs::ConfirmAsync(
            "The laser trigger output will be pulsed at the CURRENT position (MarkTimeMs of the model).\n\nIs the laser safe to fire here?",
            "Test mark");
        if (!ok) co_return;
        Run("Test mark", [](scs::Routine_Calib& cal, std::stop_token ct) { return cal.TestMark(ct); }, false);
    }

    void VisionEditPage::OnApplyTrimClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        double x = 0, z = 0;
        if (!ui::ParseDouble(TrimXBox().Text(), x) || !ui::ParseDouble(TrimZBox().Text(), z))
        {
            ShowResult(ResultKind::Fail, "Laser trim: invalid number");
            LoadTrim();
            return;
        }
        int k = m_nozzle;
        Run(std::format("Laser trim · nozzle {}", k), [k, x, z](scs::Routine_Calib& cal, std::stop_token) {
            std::string err = cal.SetTrim(k, x, z);
            return err.empty() ? scs::CalibResult::Done(std::format("X {:+.3f}, Z {:+.3f} mm — saved to model", x, z))
                               : scs::CalibResult::Fail(err);
        }, false);
    }

    // ═════════════════════════ VisionMaster ═════════════════════════

    void VisionEditPage::OnShown(bool visible)
    {
        m_visible = visible;
        if (visible) QueueVmWork();
        else CommitInputs();
        m_globalTool.SetVisible(visible);
        m_config.SetVisible(visible);
    }

    // Control VM là .NET và gọi sang tiến trình vServerApp; trong lúc chờ, COM bơm thông điệp. Gọi nó ngay trong Tick của
    // DispatcherTimer hay lúc bấm tab là XAML nhận lại thông điệp của chính nó khi đang xử lý dở và tự kết thúc app
    // (0xC000027B — dump 06/10). Nên chỉ xếp hàng; việc chạy ở lượt sau, ngoài mọi sự kiện XAML.
    void VisionEditPage::QueueVmWork()
    {
        if (m_vmQueued || m_closing) return;
        m_vmQueued = true;
        ui::PostUi([weak = get_weak()] {
            auto self = weak.get();
            if (!self) return;
            self->m_vmQueued = false;
            if (self->m_vmBusy || self->m_closing || !self->m_visible) return;   // chạy lồng trong một lời gọi VM: nhịp sau xếp lại
            self->m_vmBusy = true;
            try
            {
                if (!self->m_vmAttempted) self->EnsureVmPanes();   // gắn solution luôn ở cuối
                else if (self->m_config.Created() && scs::McControl::VisionLoadCount() != self->m_boundLoadCount)
                    self->BindProcedures();
            }
            catch (...) { self->m_vmBusy = false; throw; }
            self->m_vmBusy = false;
        });
    }

    void VisionEditPage::EnsureVmPanes()
    {
        if (m_vmAttempted || !m_loaded) return;
        m_vmAttempted = true;

        // Không chặn theo SimulationMode — bản WPF cũng không: VisionMaster chỉ là phần mềm.
        if (!ui::AppHost::Options().ScreenshotDir.empty())
        {
            ui::SetText(VmPlaceholderText(), "Screenshot mode: VisionMaster Flow editor not created.");
            return;
        }

        std::string error;
        if (!m_globalTool.Create(GlobalToolHost(), ui::vmkind::GlobalTool, &error))
            ui::SetText(VmPlaceholderText(), "Failed to create VisionMaster toolbar: " + error);
        if (!m_config.Create(ConfigHost(), ui::vmkind::MainView, &error))
        {
            ui::SetText(VmPlaceholderText(), "Failed to create VisionMaster Flow editor: " + error);
            return;
        }
        ui::SetText(VmPlaceholderText(), "");
        BindProcedures();
    }

    // Gắn trình soạn vào solution đang nạp. Có thể chưa có solution nào (VisionConnect còn đang nạp nền lúc app
    // khởi động) — khi đó chỉ ghi log, không làm sập trang.
    void VisionEditPage::BindProcedures()
    {
        auto vision = scs::McControl::Vision();
        if (!m_config.Created() || !vision || vision->IsSimulation() || !vision->IsSolutionLoaded()) return;

        m_boundLoadCount = scs::McControl::VisionLoadCount();
        HRESULT hr = m_config.Host().CallVoid(ui::vmdisp::ProcBindMultiProcedure);
        if (FAILED(hr))
            scs::LogWorker::Instance().RecordLog("Vision Edit: failed to bind solution — " + ::base::Narrow(m_config.Host().LastError()),
                                                 scs::LogLevel::Warning, "VisionEdit");
    }

    void VisionEditPage::OnWindowMoved()
    {
        m_globalTool.Sync();
        m_config.Sync();
    }

    void VisionEditPage::OnModalChanged(bool modalOpen)
    {
        m_globalTool.SetEnabled(!modalOpen);
        m_config.SetEnabled(!modalOpen);
    }

    void VisionEditPage::OnClosing()
    {
        m_closing = true;
        m_config.Dispose();
        m_globalTool.Dispose();
    }

    // ═════════════════════════ thanh jog ═════════════════════════

    void VisionEditPage::OnZSafeDoubleTapped(WF::IInspectable const&, Input::DoubleTappedRoutedEventArgs const&)
    {
        Run("Z → Z_SAFE", [](scs::Routine_Calib&, std::stop_token ct) {
            auto func = scs::McControl::Func();
            bool ok = func && func->EnsureZSafe(ct);
            return ok ? scs::CalibResult::Done("Z at safe height")
                      : scs::CalibResult::Fail("failed to raise Z — see machine status message");
        });
    }

    void VisionEditPage::OnJogSlowClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto func = scs::McControl::Func()) func->SetJogFast(false);
        OnTick();
    }

    void VisionEditPage::OnJogFastClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto func = scs::McControl::Func()) func->SetJogFast(true);
        OnTick();
    }

    // Đi cùng hàng đợi với lệnh jog: không có lệnh jog nào đang xếp hàng lại chạy SAU lệnh dừng.
    void VisionEditPage::OnStopAllClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        ui::PostMotionCommand([] { scs::McControl::Motion().StopAll(); });
    }

    void VisionEditPage::OnReloadSolutionClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        std::string refused = scs::McControl::ReloadVisionSolution();
        scs::McControl::Status().SetMessage(refused.empty() ? "Solution reload requested — see Vision log" : refused);
    }

    // ═════════════════════════ tra phần tử theo số bước ═════════════════════════

    Button VisionEditPage::StepButton(int i)
    {
        switch (i) { case 0: return StepButton0(); case 1: return StepButton1(); default: return StepButton2(); }
    }

    Border VisionEditPage::StepBadge(int i)
    {
        switch (i) { case 0: return StepBadge0(); case 1: return StepBadge1(); default: return StepBadge2(); }
    }

    TextBlock VisionEditPage::StepBadgeText(int i)
    {
        switch (i) { case 0: return StepBadgeText0(); case 1: return StepBadgeText1(); default: return StepBadgeText2(); }
    }

    TextBlock VisionEditPage::StepStatus(int i)
    {
        switch (i) { case 0: return StepStatus0(); case 1: return StepStatus1(); default: return StepStatus2(); }
    }

    TextBlock VisionEditPage::StepLabel(int i)
    {
        switch (i) { case 0: return StepLabel0(); case 1: return StepLabel1(); default: return StepLabel2(); }
    }

    FrameworkElement VisionEditPage::StepPanel(int i)
    {
        switch (i) { case 0: return StepPanel0(); case 1: return StepPanel1(); default: return StepPanel2(); }
    }
}
