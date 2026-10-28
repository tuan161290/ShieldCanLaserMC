#include "pch.h"
#include "ManualPage.xaml.h"
#if __has_include("ManualPage.g.cpp")
#include "ManualPage.g.cpp"
#endif

#include "Base/Async.h"
#include "Base/Text.h"
#include "Communication/EtherCat/EtherCatAxis.h"
#include "Communication/Sim/FakeAxis.h"
#include "Communication/Sim/FakeIo.h"
#include "Controller/McControl.h"
#include "Controller/Routine_Calib.h"
#include "Infor/AxisId.h"
#include "Ui/Dialogs.h"
#include "Ui/JogControl.h"
#include "Ui/UiUtil.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Documents;
namespace WF = winrt::Windows::Foundation;

namespace
{
    template <size_t N>
    bool InGroup(const std::array<const char*, N>& group, const std::string& name)
    {
        for (const char* member : group)
            if (name == member) return true;
        return false;
    }

    /// <summary>Io mô phỏng — nullptr trên máy thật (khi đó Bảng mô phỏng bị ẩn).</summary>
    std::shared_ptr<scs::FakeIo> SimIo()
    {
        return std::dynamic_pointer_cast<scs::FakeIo>(scs::McControl::Io());
    }

    /// <summary>Bit cao bên trái, bit 0 bên phải — cùng chiều với cách đọc sơ đồ điện.</summary>
    std::string Bits(uint64_t value, int count)
    {
        std::string s;
        s.reserve(count + count / 8);
        for (int i = count - 1; i >= 0; --i)
        {
            s.push_back(((value >> i) & 1) ? '1' : '0');
            if (i % 8 == 0 && i > 0) s.push_back(' ');
        }
        return s;
    }

    /// <summary>Đặt Text chỉ khi chuỗi UTF-8 đổi so với lần trước — khỏi đổi sang hstring và đọc lại Text() mỗi nhịp.</summary>
    void SetIfChanged(TextBlock const& block, std::string& shown, std::string value)
    {
        if (value == shown) return;
        shown = std::move(value);
        block.Text(ui::H(shown));
    }
}

namespace winrt::ShieldCanLaser::implementation
{
    void ManualPage::OnRootLoaded(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loaded) return;
        m_loaded = true;

        auto resources = Resources();
        m_lampBrushes.Off = resources.Lookup(box_value(L"LampOffBrush")).as<Media::Brush>();
        m_lampBrushes.Stroke = resources.Lookup(box_value(L"LampStrokeBrush")).as<Media::Brush>();
        m_lampBrushes.On = ui::Res(L"CriticalBrush");

        Run head;
        head.Text(L"Motion blocked: ");
        head.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        m_blockReasonRun = Run();
        BlockText().Inlines().Append(head);
        BlockText().Inlines().Append(m_blockReasonRun);

        BuildAxisRows();
        BuildNozzlePanel();
        BuildIoPanels();
        BuildSimAxisGrid();
        BuildSimPickFail();
        OnTick();
    }

    // ══════════ Dựng ══════════

    // Hai nhóm theo AxisId (Z | Gantry X, Y), giữ thứ tự của AxisList trong mỗi nhóm.
    void ManualPage::BuildAxisRows()
    {
        for (const auto& axis : scs::McControl::Motion().AxisList())
        {
            if (!axis) continue;
            if (auto ec = std::dynamic_pointer_cast<scs::EtherCatAxis>(axis)) m_etherCatAxes.push_back(ec);

            const std::string& name = axis->Name();
            StackPanel host{ nullptr };
            if (InGroup(scs::AxisId::ZAxes, name)) host = ZRows();
            else if (InGroup(scs::AxisId::PlanarAxes, name)) host = GantryRows();
            if (!host) continue;

            auto row = ui::ManualAxisRow::Create(axis, m_lampBrushes);
            host.Children().Append(row->Root());
            m_rows.push_back(std::move(row));
        }
    }

    // 7 nozzle: nút "N1"…"N7" (bấm đảo van Y003…Y009, xanh khi van đang mở) + chấm đèn cảm biến chân không (X007…X013) sát bên.
    void ManualPage::BuildNozzlePanel()
    {
        for (int k = 1; k <= scs::IoMap::NozzleCount; ++k)
        {
            StackPanel cell;
            cell.Orientation(Orientation::Horizontal);
            cell.Margin(ThicknessHelper::FromLengths(0, 0, 14, 0));

            std::wstring text = L"N" + std::to_wstring(k);
            std::wstring tip = L"Nozzle " + std::to_wstring(k) + L" vacuum valve (" + winrt::to_hstring(scs::IoMap::DoLabel(scs::IoMap::DoVacuum(k))).c_str() +
                               L") — click to toggle. Dot = vacuum sensor " + winrt::to_hstring(scs::IoMap::DiLabel(scs::IoMap::VacuumSensor(k))).c_str();
            Button button = ui::ManualCompactButton(text.c_str(), 56, tip.c_str());
            const int bit = scs::IoMap::DoVacuum(k);
            // Đọc-đảo-ghi trong hàng đợi lệnh tay — như bảng DO, bấm hai lần đảo đúng hai lần.
            button.Click([bit](auto&&, auto&&) {
                ui::PostMotionCommand([bit] {
                    if (auto io = scs::McControl::Io()) io->SetDo(bit, !io->GetDo(bit));
                });
            });
            cell.Children().Append(button);
            m_nozzleButtons[(size_t)k] = button;

            auto lamp = m_nozzleLamps[(size_t)k].Build(11, m_lampBrushes);
            lamp.Margin(ThicknessHelper::FromLengths(6, 0, 0, 0));
            lamp.VerticalAlignment(VerticalAlignment::Center);
            cell.Children().Append(lamp);

            NozzlePanel().Children().Append(cell);
        }
    }

    // Hút trượt giả lập từng nozzle (FakeIo::SimSetPickFail): bật thì van mở mà cảm biến không lên → chu trình đánh ô NG.
    void ManualPage::BuildSimPickFail()
    {
        for (int k = 1; k <= scs::IoMap::NozzleCount; ++k)
        {
            std::wstring text = L"N" + std::to_wstring(k);
            Button button = ui::ManualCompactButton(text.c_str(), 44, L"Simulate pick miss on this nozzle (sensor stays off while the valve is on) — click again to clear");
            button.Margin(ThicknessHelper::FromLengths(0, 0, 4, 8));
            button.Click([weak = get_weak(), k](auto&&, auto&&) {
                auto self = weak.get();
                if (!self) return;
                bool fail = !self->m_simPickFail[(size_t)k];
                self->m_simPickFail[(size_t)k] = fail;
                if (auto io = SimIo()) io->SimSetPickFail(k, fail);
                ui::SetTint(self->m_simPickFailButtons[(size_t)k], fail ? ui::Tint::Red : ui::Tint::None);
            });
            m_simPickFailButtons[(size_t)k] = button;
            SimPickFailPanel().Children().Append(button);
        }
    }

    // Đèn I/O: "X000  tên" / "Y000  tên" — đánh số kênh như bản vẽ điện (IoMap::DiLabel/DoLabel), đối chiếu thẳng không
    // phải quy đổi. DO là nút: bấm để đảo ngõ ra (ghi thẳng qua McControl::Io nên dùng được cả máy thật lẫn mô phỏng).
    void ManualPage::BuildIoPanels()
    {
        auto makeRow = [](ui::ManualLamp& lamp, ui::ManualLampBrushes const& brushes, std::string const& caption) {
            StackPanel row;
            row.Orientation(Orientation::Horizontal);
            row.Margin(ThicknessHelper::FromLengths(4, 3, 4, 3));
            row.Children().Append(lamp.Build(11, brushes));
            TextBlock text;
            text.Text(ui::H(caption));
            text.FontSize(11);
            text.FontFamily(Media::FontFamily(L"Consolas"));
            text.Margin(ThicknessHelper::FromLengths(8, 0, 0, 0));
            text.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(text);
            return row;
        };

        const auto& diNames = scs::IoMap::DiNames();
        m_diLamps.resize(scs::IoMap::DiCount);
        for (int bit = 0; bit < scs::IoMap::DiCount; ++bit)
            DiList().Children().Append(makeRow(m_diLamps[bit], m_lampBrushes, scs::IoMap::DiLabel(bit) + "  " + diNames[bit]));

        const auto& doNames = scs::IoMap::DoNames();
        auto rowStyle = Resources().Lookup(box_value(L"IoRowButtonStyle")).as<Microsoft::UI::Xaml::Style>();
        m_doLamps.resize(scs::IoMap::DoCount);
        for (int bit = 0; bit < scs::IoMap::DoCount; ++bit)
        {
            Button button;
            button.Style(rowStyle);
            button.HorizontalAlignment(HorizontalAlignment::Stretch);
            button.Margin(ThicknessHelper::FromLengths(0, 0, 6, 0));
            button.Content(makeRow(m_doLamps[bit], m_lampBrushes, scs::IoMap::DoLabel(bit) + "  " + doNames[bit]));
            // Đọc-đảo-ghi ngay trong hàng đợi lệnh tay: SetDo trên EtherCAT chặn ngắn; bấm hai lần liên tiếp đảo đúng hai lần.
            // Đèn tháp/còi/đèn nút do vòng quét ghi lại mỗi chu kỳ nên đảo tay sẽ bị ghi đè ngay.
            button.Click([bit](auto&&, auto&&) {
                ui::PostMotionCommand([bit] {
                    if (auto io = scs::McControl::Io()) io->SetDo(bit, !io->GetDo(bit));
                });
            });
            DoList().Children().Append(button);
        }
    }

    // Lỗi driver giả lập: mỗi trục mô phỏng một ô "tên | ALM | Ngắt" (bấm lại để gỡ).
    void ManualPage::BuildSimAxisGrid()
    {
        int index = 0;
        for (const auto& axis : scs::McControl::Motion().AxisList())
        {
            auto sim = std::dynamic_pointer_cast<scs::FakeAxis>(axis);
            if (!sim) continue;

            StackPanel cell;
            cell.Orientation(Orientation::Horizontal);
            cell.Margin(ThicknessHelper::FromLengths(0, 0, 16, 8));

            TextBlock name;
            name.Text(ui::H(sim->Name()));
            name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            name.Width(28);
            name.VerticalAlignment(VerticalAlignment::Center);
            cell.Children().Append(name);

            Button alarm = ui::ManualCompactButton(L"ALM", 56, L"Simulate driver alarm");
            alarm.Click([sim](auto&&, auto&&) { sim->SetSimDriverAlarm(!sim->SimDriverAlarm()); });
            cell.Children().Append(alarm);

            Button cut = ui::ManualCompactButton(L"LINK OFF", 72, L"Simulate link loss");
            cut.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0));
            cut.Click([sim](auto&&, auto&&) { sim->SetSimDisconnected(!sim->SimDisconnected()); });
            cell.Children().Append(cut);

            int row = index / 5;
            while ((int)SimAxisGrid().RowDefinitions().Size() <= row)
            {
                RowDefinition definition;
                definition.Height(GridLengthHelper::Auto());
                SimAxisGrid().RowDefinitions().Append(definition);
            }
            Grid::SetRow(cell, row);
            Grid::SetColumn(cell, index % 5);
            SimAxisGrid().Children().Append(cell);
            ++index;
        }
    }

    // ══════════ Cập nhật (10 Hz, chỉ lúc trang đang hiện) ══════════

    void ManualPage::OnTick()
    {
        if (!m_loaded) return;

        for (const auto& row : m_rows) row->Update();

        int busy = m_servoAllBusy.load() ? 1 : 0;
        if (busy != m_shownServoAllBusy)
        {
            m_shownServoAllBusy = busy;
            ServoOnAllButton().IsEnabled(!busy);
            ServoOffAllButton().IsEnabled(!busy);
        }

        std::string message = scs::McControl::Status().Message();
        if (message != m_shownMessage)
        {
            ui::Show(MessageBar(), !::base::IsBlank(message));
            SetIfChanged(MessageText(), m_shownMessage, std::move(message));
        }

        UpdateSafety();
        UpdateNozzles();
        UpdateIo();
    }

    // Nút nozzle xanh khi van mở, chấm đèn theo cảm biến; đèn CAM xanh khi đang bật; nút laser khoá trong lúc xung chạy.
    void ManualPage::UpdateNozzles()
    {
        auto io = scs::McControl::Io();
        for (int k = 1; k <= scs::IoMap::NozzleCount; ++k)
        {
            bool valve = io && io->GetDo(scs::IoMap::DoVacuum(k));
            bool sensor = io && io->GetDi(scs::IoMap::VacuumSensor(k));
            ui::SetTint(m_nozzleButtons[(size_t)k], valve ? ui::Tint::Green : ui::Tint::None);
            m_nozzleLamps[(size_t)k].Set(sensor);
        }
        ui::SetTint(CamLightButton(), io && io->GetDo(scs::IoMap::DoCamLight) ? ui::Tint::Green : ui::Tint::None);

        int laserBusy = m_laserBusy.load() ? 1 : 0;
        if (laserBusy != m_shownLaserBusy)
        {
            m_shownLaserBusy = laserBusy;
            LaserTestButton().IsEnabled(!laserBusy);
        }
        ui::SetTint(LaserTestButton(), io && io->GetDo(scs::IoMap::DoLaserTrigger) ? ui::Tint::Red : ui::Tint::None);
    }

    void ManualPage::UpdateSafety()
    {
        auto safety = scs::McControl::Safety();
        std::string block = (safety && !safety->SafeToMove()) ? safety->BlockReason() : std::string();
        if (block != m_shownBlockReason)
        {
            m_shownBlockReason = block;
            m_blockReasonRun.Text(ui::H(block));
            ui::Show(BlockBar(), !::base::IsBlank(block));
        }

        auto& motion = scs::McControl::Motion();
        int zSafe = motion.ZSafeOk() ? 1 : 0;
        if (zSafe != m_shownZSafe)
        {
            m_shownZSafe = zSafe;
            ZSafeLamp().Fill(ui::StatusBrush(zSafe != 0));
        }
        SetIfChanged(ZSafeText(), m_shownZText, motion.ZSafeInterlock()
            ? std::format("Z ≤ {:.1f} (safe height) — required for X, Y to move", motion.ZSafeLimit())
            : std::string("Z_SAFE interlock is OFF (Setting page)"));
    }

    void ManualPage::UpdateIo()
    {
        auto io = scs::McControl::Io();
        if (io.get() != m_shownIo)
        {
            m_shownIo = io.get();
            ui::Show(SimCard(), std::dynamic_pointer_cast<scs::FakeIo>(io) != nullptr);
        }
        if (!io) return;

        uint64_t di = 0, doo = 0;
        for (int bit = 0; bit < scs::IoMap::DiCount; ++bit)
        {
            bool on = io->GetDi(bit);
            m_diLamps[bit].Set(on);
            if (on && bit < 64) di |= 1ull << bit;
        }
        for (int bit = 0; bit < scs::IoMap::DoCount; ++bit)
        {
            bool on = io->GetDo(bit);
            m_doLamps[bit].Set(on);
            if (on && bit < 64) doo |= 1ull << bit;
        }
        UpdateDiagnostics(di, doo);
    }

    void ManualPage::UpdateDiagnostics(uint64_t di, uint64_t doo)
    {
        SetIfChanged(RawDiText(), m_shownRawDi, std::format("DI  0x{:016X}   {}", di, Bits(di, scs::IoMap::DiCount)));
        SetIfChanged(RawDoText(), m_shownRawDo, std::format("DO  0x{:08X}           {}", doo, Bits(doo, scs::IoMap::DoCount)));

        // Bus đang dùng + thống kê chu kỳ native (jitter/scan) — chỉ có số khi chạy SOEM thật.
        std::string busText;
        if (auto bus = scs::McControl::Motion().Bus(); !bus)
        {
            busText = "Simulation (FakeAxis / FakeIo) — no EtherCAT bus";
        }
        else
        {
            ecm::CyclicStats s = bus->CyclicStats();
            busText = bus->BusDescription() + (bus->IsConnected() ? "  · OPEN" : "  · NOT OPEN");
            if (s.SampleCount > 0)
                busText += std::format("  · scan {:.0f} µs (max {:.0f})  · jitter max {:.0f} µs",
                                       s.ScanTimeAvgUs, s.ScanTimeMaxUs, s.PeriodJitterMaxUs);
        }
        SetIfChanged(BusText(), m_shownBusText, std::move(busText));

        // Một dòng CiA402 mỗi trục EtherCAT (EtherCatAxis::DiagnosticText).
        while (m_driveLines.size() < m_etherCatAxes.size())
        {
            TextBlock line;
            line.FontFamily(Media::FontFamily(L"Consolas"));
            line.FontSize(12);
            line.TextWrapping(TextWrapping::Wrap);
            line.Margin(ThicknessHelper::FromLengths(0, 0, 0, 2));
            DriveStatusPanel().Children().Append(line);
            m_driveLines.push_back(line);
            m_shownDriveLines.emplace_back("\x01");
        }
        for (size_t i = 0; i < m_etherCatAxes.size(); ++i)
            SetIfChanged(m_driveLines[i], m_shownDriveLines[i], m_etherCatAxes[i]->DiagnosticText());
    }

    // ══════════ Lệnh chung cho tất cả trục ══════════

    // Đi cùng hàng đợi với lệnh jog: không có lệnh jog nào đang xếp hàng lại chạy SAU lệnh dừng.
    void ManualPage::OnStopAllClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        ui::PostMotionCommand([] { scs::McControl::Motion().StopAll(); });
    }

    // RESET ngay tại trang Manual — cùng McControl::RequestReset với nút RESET trang Main.
    void ManualPage::OnResetClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::RequestReset();
    }

    void ManualPage::OnOriginClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::RequestOrigin();
    }

    // SVO ON tất cả — bật servo từng trục chưa ServoOn, bỏ qua trục đang Alarm (phải RST trước). Song song: servo
    // EtherCAT có thể mất 1–2 s mới vào Operation Enabled.
    void ManualPage::OnServoOnAllClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_servoAllBusy.exchange(true)) return;

        std::vector<std::string> alarmed;
        std::vector<std::shared_ptr<scs::IAxis>> targets;
        for (const auto& axis : scs::McControl::Motion().AxisList())
        {
            if (!axis || axis->ServoOn()) continue;
            if (axis->Alarm()) alarmed.push_back(axis->Name());
            else targets.push_back(axis);
        }
        OnTick();   // khoá hai nút ngay, không đợi nhịp sau

        ui::RunBackgroundThenUi(
            [targets, alarmed] {
                try
                {
                    std::vector<std::function<bool()>> jobs;
                    for (const auto& axis : targets) jobs.push_back([axis] { return axis->SetServoOn(true); });
                    if (!jobs.empty()) ::base::WhenAll(std::move(jobs));
                }
                catch (const std::exception& ex)
                {
                    ui::Dialogs::Error(std::string("SERVO ON: ") + ex.what());
                }
                if (!alarmed.empty())
                    scs::McControl::Status().SetMessage("SERVO ON skipped: " + ::base::Join(alarmed, ", ") +
                                                        " in alarm — press RST on each axis first");
            },
            [weak = get_weak()] {
                if (auto self = weak.get()) self->m_servoAllBusy = false;
            });
    }

    void ManualPage::OnServoOffAllClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_servoAllBusy.exchange(true)) return;

        std::vector<std::shared_ptr<scs::IAxis>> targets;
        for (const auto& axis : scs::McControl::Motion().AxisList())
            if (axis && axis->ServoOn()) targets.push_back(axis);
        OnTick();

        ui::RunBackgroundThenUi(
            [targets] {
                try
                {
                    std::vector<std::function<bool()>> jobs;
                    for (const auto& axis : targets) jobs.push_back([axis] { return axis->SetServoOn(false); });
                    if (!jobs.empty()) ::base::WhenAll(std::move(jobs));
                }
                catch (const std::exception& ex)
                {
                    ui::Dialogs::Error(std::string("SERVO OFF: ") + ex.what());
                }
            },
            [weak = get_weak()] {
                if (auto self = weak.get()) self->m_servoAllBusy = false;
            });
    }

    // ══════════ Van chân không, đèn CAM, laser ══════════

    void ManualPage::OnVacuumAllOffClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        ui::PostMotionCommand([] {
            auto io = scs::McControl::Io();
            if (!io) return;
            for (int k = 1; k <= scs::IoMap::NozzleCount; ++k) io->SetDo(scs::IoMap::DoVacuum(k), false);
        });
    }

    void ManualPage::OnCamLightClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        ui::PostMotionCommand([] {
            if (auto io = scs::McControl::Io()) io->SetDo(scs::IoMap::DoCamLight, !io->GetDo(scs::IoMap::DoCamLight));
        });
    }

    // Một xung laser tại chỗ đang đứng — cùng Routine_Calib::TestMark với nút TEST MARK của trang Vision Edit (từ chối khi
    // trục đang chạy). Hàm chặn (giữ DO suốt MarkTimeMs) nên chạy trên luồng nền; kết quả hiện cạnh nút.
    void ManualPage::OnLaserTestClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_laserBusy.exchange(true)) return;
        OnTick();   // khoá nút ngay
        auto weak = get_weak();
        ui::RunBackgroundThenUi(
            [weak] {
                std::string text;
                try
                {
                    auto calib = scs::McControl::Calib();
                    scs::CalibResult r = calib ? calib->TestMark(std::stop_token{}) : scs::CalibResult::Fail("machine not initialized");
                    text = (r.Ok ? "" : "Refused: ") + r.Text;
                }
                catch (const std::exception& ex)
                {
                    text = std::string("ERROR: ") + ex.what();
                }
                ui::PostUi([weak, text] {
                    if (auto self = weak.get()) SetIfChanged(self->LaserTestText(), self->m_shownLaserText, text);
                });
            },
            [weak] {
                if (auto self = weak.get()) self->m_laserBusy = false;
            });
    }

    // ══════════ Bảng mô phỏng — nút cứng và tín hiệu an toàn (FakeIo) ══════════

    void ManualPage::OnSimStartClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto io = SimIo()) io->SimPress(scs::IoMap::BtnStart);
    }

    void ManualPage::OnSimStopClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto io = SimIo()) io->SimPress(scs::IoMap::BtnStop);
    }

    void ManualPage::OnSimResetClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto io = SimIo()) io->SimPress(scs::IoMap::BtnReset);
    }

    void ManualPage::OnSimEmgClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto io = SimIo()) io->SimToggle(scs::IoMap::EmgOk);
    }

    void ManualPage::OnSimCurtainClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto io = SimIo()) io->SimToggle(scs::IoMap::LightCurtainOk);
    }

    void ManualPage::OnSimAirClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto io = SimIo()) io->SimToggle(scs::IoMap::AirOk);
    }

    // Cảm biến giữ mức của chu trình: hai tray.
    void ManualPage::OnSimTray1Click(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto io = SimIo()) io->SimToggle(scs::IoMap::TrayPresent(1));
    }

    void ManualPage::OnSimTray2Click(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto io = SimIo()) io->SimToggle(scs::IoMap::TrayPresent(2));
    }
}
