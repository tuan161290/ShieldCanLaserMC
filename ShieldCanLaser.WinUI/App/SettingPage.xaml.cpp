#include "pch.h"
#include "SettingPage.xaml.h"
#if __has_include("SettingPage.g.cpp")
#include "SettingPage.g.cpp"
#endif

#include <format>
#include <shellapi.h>

#include "Base/Text.h"
#include "Controller/McControl.h"
#include "Data/McDataManager.h"
#include "Infor/AlarmCatalog.h"
#include "SysConfig/AppPaths.h"
#include "Ui/Dialogs.h"
#include "Ui/UiUtil.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;
namespace WF = winrt::Windows::Foundation;
namespace sp = ui::settingpage;

namespace
{
    std::shared_ptr<scs::SystemData> Sys() { return scs::McDataManager::SysData(); }

    void SetEnabled(Control const& control, bool enabled)
    {
        if (control.IsEnabled() != enabled) control.IsEnabled(enabled);
    }

    /// <summary>Báo cáo của Core xuống dòng bằng "\r\n" (như bản C#); TextBlock chỉ cần "\n".</summary>
    std::string NormalizeNewLines(const std::string& text) { return ::base::Replace(text, "\r\n", "\n"); }
}

namespace winrt::ShieldCanLaser::implementation
{
    // ══════════ Dựng trang ══════════

    void SettingPage::OnRootLoaded(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loaded) return;
        m_loaded = true;

        BindSystemFields();
        BuildPositionTable();
        LogHost().Child(m_log.Build());

        LoadAll(true);
        LoadZSafe();
        UpdateStaticInfo();
        SelectSection(m_section);
        OnTick();
    }

    // ══════════ Mục ở cột trái ══════════

    void SettingPage::SelectSection(int section)
    {
        if (section < 0 || section >= SectionCount) return;
        m_section = section;
        std::array<ToggleButton, SectionCount> nav{ NavButton0(), NavButton1(), NavButton2(), NavButton3(), NavButton4() };
        // Bấm vào mục đang chọn làm ToggleButton tự bỏ chọn — luôn đặt lại theo mục thật.
        for (int i = 0; i < SectionCount; ++i) nav[(size_t)i].IsChecked(i == section);
        ui::Show(SystemSection(), section == SectionSystem);
        ui::Show(PositionSection(), section == SectionPosition);
        ui::Show(TimeSection(), section == SectionTime);
        ui::Show(VisionSection(), section == SectionVision);
        ui::Show(LogSection(), section == SectionLog);
        OnTick();
    }

    void SettingPage::OnNavClick(WF::IInspectable const& sender, RoutedEventArgs const&)
    {
        int section = m_section;
        if (auto b = sender.try_as<ToggleButton>()) section = std::stoi(winrt::to_string(winrt::unbox_value<hstring>(b.Tag())));
        SelectSection(section);
    }

    void SettingPage::BindSystemFields()
    {
        auto& f = m_fields;

        // ── Tham số hệ thống (Z_SAFE riêng — xem LoadZSafe/OnSaveClick) ──
        f.BindString(MachineNameBox(), "Machine name", [] { return Sys()->MachineName; }, [](std::string v) { Sys()->MachineName = std::move(v); });
        f.BindString(MachineCodeBox(), "Machine code", [] { return Sys()->MachineCode; }, [](std::string v) { Sys()->MachineCode = std::move(v); });
        f.BindInt(ControlTickBox(), "Control loop period (ms)",
                  [] { return Sys()->ControlTickMs; }, [](int v) { Sys()->ControlTickMs = v; });
        f.BindInt(WatchdogBox(), "Watchdog (ms)",
                  [] { return Sys()->WatchdogTimeoutMs; }, [](int v) { Sys()->WatchdogTimeoutMs = v; });
        f.BindInt(VisionSettleBox(), "Settle delay before capture (ms)",
                  [] { return Sys()->VisionSettleMs; }, [](int v) { Sys()->VisionSettleMs = v; });
        f.BindInt(LampSettleBox(), "Lamp settle delay before capture (ms)",
                  [] { return Sys()->LampSettleMs; }, [](int v) { Sys()->LampSettleMs = v; });
        f.BindInt(VisionTimeoutBox(), "Timeout Flow (ms)",
                  [] { return Sys()->VisionTriggerTimeoutMs; }, [](int v) { Sys()->VisionTriggerTimeoutMs = v; });

        f.BindCheck(SimModeCheck(), [] { return Sys()->SimulationMode; }, [](bool v) { Sys()->SimulationMode = v; });
        // Đọc một lần lúc McControl::Init như SimulationMode → cần khởi động lại; lưu cùng nút Lưu system.json.
        f.BindCheck(VisionSimCheck(), [] { return Sys()->VisionSimulation; }, [](bool v) { Sys()->VisionSimulation = v; });
        f.BindCheck(ZSafeInterlockCheck(), [] { return Sys()->ZSafeInterlock; }, [](bool v) { Sys()->ZSafeInterlock = v; });
        f.BindCheck(LightCurtainCheck(), [] { return Sys()->HasLightCurtain; }, [](bool v) { Sys()->HasLightCurtain = v; });
        f.BindCheck(TraySensorsCheck(), [] { return Sys()->HasTraySensors; }, [](bool v) { Sys()->HasTraySensors = v; });
        f.BindCheck(AirSensorCheck(), [] { return Sys()->HasAirSensor; }, [](bool v) { Sys()->HasAirSensor = v; });

        // ── Vision — tên Flow và biến (một camera, một Flow LocateCan) ──
        auto bindName = [&f](TextBox const& box, const char* label, std::string scs::VisionOutputNames::*member) {
            f.BindString(box, label,
                         [member] { return Sys()->VisionOutputs.*member; },
                         [member](std::string v) { Sys()->VisionOutputs.*member = std::move(v); });
        };
        bindName(FlowLocateCanBox(), "Flow LocateCan", &scs::VisionOutputNames::FlowLocateCan);
        bindName(OutFoundBox(), "Output Found", &scs::VisionOutputNames::OutFound);
        bindName(OutImageBox(), "Output image", &scs::VisionOutputNames::OutImage);
        bindName(RenderCamBox(), "CAM frame", &scs::VisionOutputNames::RenderCam);
        bindName(GlobalVarModuleBox(), "Module Global Variable", &scs::VisionOutputNames::GlobalVarModule);
        bindName(GvAxisXBox(), "Axis X var (mm)", &scs::VisionOutputNames::GvAxisX);
        bindName(GvAxisZBox(), "Axis Z var (mm)", &scs::VisionOutputNames::GvAxisZ);
        bindName(GvNozzleBox(), "Nozzle var", &scs::VisionOutputNames::GvNozzle);
        bindName(GvIsLocateBox(), "Branch var", &scs::VisionOutputNames::GvIsLocate);

        // ── Bù ở trạm laser (PLAN-LASER.md §4.3) — ghi thẳng vào SystemData sống, Routine_Auto đọc ở lần khắc kế tiếp ──
        f.BindDouble(GainXBox(), "Gain X", [] { return Sys()->LaserComp.GainX; }, [](double v) { Sys()->LaserComp.GainX = v; });
        f.BindDouble(GainZBox(), "Gain Z", [] { return Sys()->LaserComp.GainZ; }, [](double v) { Sys()->LaserComp.GainZ = v; });
        f.BindDouble(MaxCompBox(), "Max compensation (mm)", [] { return Sys()->LaserComp.MaxCompMm; }, [](double v) { Sys()->LaserComp.MaxCompMm = v; });
        f.BindInt(LaserPulseMinBox(), "Laser pulse minimum (ms)",
                  [] { return Sys()->LaserComp.LaserPulseMinMs; }, [](int v) { Sys()->LaserComp.LaserPulseMinMs = v; }, 0, 60000);

        // ── Lưới hiệu chuẩn X × Z (tâm lưới do trang Vision Edit lưu — chỉ hiện) ──
        f.BindDouble(GridStepXBox(), "Grid step X (mm)", [] { return Sys()->VisionCalib.GridStepX; }, [](double v) { Sys()->VisionCalib.GridStepX = v; });
        f.BindDouble(GridStepZBox(), "Grid step Z (mm)", [] { return Sys()->VisionCalib.GridStepZ; }, [](double v) { Sys()->VisionCalib.GridStepZ = v; });
        f.BindInt(CalibNozzleBox(), "Calibration nozzle",
                  [] { return Sys()->VisionCalib.CalibNozzle; }, [](int v) { Sys()->VisionCalib.CalibNozzle = v; }, 1, scs::VisionOutputNames::NozzleCount);
        f.BindDouble(VerifyStepBox(), "Verify travel (mm)", [] { return Sys()->VisionCalib.VerifyStepMm; }, [](double v) { Sys()->VisionCalib.VerifyStepMm = v; });
        f.BindDouble(ResidualTolBox(), "Verify tolerance (mm)", [] { return Sys()->VisionCalib.ResidualTolMm; }, [](double v) { Sys()->VisionCalib.ResidualTolMm = v; });
        f.BindInt(ShotsPerPointBox(), "Images per measurement",
                  [] { return Sys()->VisionCalib.ShotsPerPoint; }, [](int v) { Sys()->VisionCalib.ShotsPerPoint = v; }, 1, 100);
        f.BindInt(CalibValidDaysBox(), "Calibration valid (days)",
                  [] { return Sys()->CalibValidDays; }, [](int v) { Sys()->CalibValidDays = v; }, 0, 10000);
    }

    // Bảng vị trí chờ / giới hạn mềm / offset gốc — 3 dòng, vừa bề ngang nên không cần cột cố định hay cuộn ngang. Ô ghi
    // thẳng vào AxisConfig sống: vị trí chờ và giới hạn mềm ăn ngay từ lệnh kế tiếp; "Ghi limit" ghi xuống driver ở lần
    // Connect All Axis kế tiếp.
    void SettingPage::BuildPositionTable()
    {
        m_posTable.Build({ { L"Axis", 64 }, { L"Unit", 64 }, { L"Current", 100 }, { L"Standby", 110 }, { L"Soft −", 110 },
                           { L"Soft +", 110 }, { L"Write limit", 84 }, { L"Home offset", 110 }, { L"", 0 } }, 34);
        PositionTableHost().Child(m_posTable.Root);

        m_posRows.clear();
        auto& f = m_posFields;
        for (const auto& cfg : Sys()->Axes)
        {
            if (!cfg) continue;
            auto a = cfg;
            std::string name = a->Name;
            auto label = [name](const char* column) { return "Axis " + name + " — " + column; };

            Grid row = m_posTable.NewRow(40);
            auto box = [&row](int column) {
                TextBox b = sp::CellBox();
                sp::PlaceInColumn(row, b, column);
                return b;
            };

            TextBlock nameText = sp::TextCell(name, 13);
            nameText.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            sp::PlaceInColumn(row, nameText, 0);
            TextBlock unit = sp::TextCell(a->Unit);
            unit.Foreground(ui::Res(L"TextSecondaryBrush"));
            sp::PlaceInColumn(row, unit, 1);
            TextBlock current = sp::TextCell("");
            sp::PlaceInColumn(row, current, 2);

            f.BindDouble(box(3), label("Standby"), [a] { return a->StandbyPos; }, [a](double v) { a->StandbyPos = v; });
            f.BindDouble(box(4), label("Soft −"), [a] { return a->SoftLimitNeg; }, [a](double v) { a->SoftLimitNeg = v; });
            f.BindDouble(box(5), label("Soft +"), [a] { return a->SoftLimitPos; }, [a](double v) { a->SoftLimitPos = v; });
            CheckBox write = sp::CellCheck();
            sp::PlaceInColumn(row, write, 6);
            f.BindCheck(write, [a] { return a->WriteSoftLimitToDrive; }, [a](bool v) { a->WriteSoftLimitToDrive = v; });
            f.BindDouble(box(7), label("Home offset"), [a] { return a->HomeOffset; }, [a](double v) { a->HomeOffset = v; });

            m_posTable.Rows.Children().Append(row);
            m_posRows.push_back({ cfg, unit, current });
        }
    }

    // ══════════ Nạp ô nhập từ dữ liệu ══════════

    void SettingPage::LoadAll(bool force)
    {
        m_fields.LoadAll(force);
        m_posFields.LoadAll(force);
        for (auto& r : m_posRows) ui::SetText(r.Unit, r.Config->Unit);   // đơn vị sửa ở trang EtherCAT

        hstring path = ui::H(Sys()->VisionSolutionPath);
        if (SolutionPathBox().Text() != path) SolutionPathBox().Text(path);
    }

    /// <summary>Z_SAFE là giá trị riêng của trang (ZSafe của ViewModel cũ): nạp lúc mở trang lần đầu và sau Lưu,
    /// chỉ áp vào SystemData + MotionConnect khi bấm Lưu.</summary>
    void SettingPage::LoadZSafe()
    {
        m_zSafeLoaded = ui::H(sp::FormatDouble(Sys()->ZSafe));
        ZSafeBox().Text(m_zSafeLoaded);
        ZSafeBox().ClearValue(Control::BorderBrushProperty());
        ToolTipService::SetToolTip(ZSafeBox(), nullptr);
    }

    void SettingPage::UpdateStaticInfo()
    {
        const auto& c = Sys()->VisionCalib;
        ui::SetText(GridCenterText(), c.CenterSet
            ? std::format("Grid center (saved by SET CENTER on Vision Edit): X {:.3f}, Z {:.3f} mm", c.CenterX, c.CenterZ)
            : std::string("Grid center not set yet — Vision Edit page, SET CENTER."));
        ui::SetText(DataRootText(), ::base::PathToU8(scs::AppPaths::Root()));
        ui::Show(FallbackRootText(), scs::AppPaths::UsingFallbackRoot());
        ui::Show(AlarmCatalogText(), !scs::AlarmCatalog::Loaded());
        ui::SetText(SaveResultText(), m_saveResult);
        ui::Show(SaveResultText(), !m_saveResult.empty());
    }

    bool SettingPage::ParseZSafe(double& value, std::string& error)
    {
        if (ui::ParseDouble(ZSafeBox().Text(), value)) return true;
        error = "Z_SAFE (mm): \"" + ui::U8(ZSafeBox().Text()) + "\" is not a number";
        return false;
    }

    void SettingPage::OnZSafeLostFocus(WF::IInspectable const&, RoutedEventArgs const&)
    {
        double value = 0;
        std::string error;
        if (ParseZSafe(value, error))
        {
            ZSafeBox().ClearValue(Control::BorderBrushProperty());
            ToolTipService::SetToolTip(ZSafeBox(), nullptr);
        }
        else
        {
            ZSafeBox().BorderBrush(ui::Res(L"CriticalBrush"));
            ToolTipService::SetToolTip(ZSafeBox(), box_value(ui::H(error + " — SAVE will be rejected")));
        }
    }

    bool SettingPage::CommitEdits(bool includeZSafe, const std::string& action)
    {
        std::vector<std::string> errors;
        if (includeZSafe)
        {
            double z = 0;
            std::string error;
            if (!ParseZSafe(z, error))
            {
                errors.push_back(error);
                ZSafeBox().BorderBrush(ui::Res(L"CriticalBrush"));
            }
        }
        for (auto& e : m_fields.CommitAll()) errors.push_back(std::move(e));
        for (auto& e : m_posFields.CommitAll()) errors.push_back(std::move(e));
        if (errors.empty()) return true;

        ui::Dialogs::Warn(action + " — invalid fields (red border):\n• " + ::base::Join(errors, "\n• ") +
                              "\n\nCorrect them (or re-enter the old value), then try again.",
                          "Invalid Value");
        return false;
    }

    // ══════════ Lưu ══════════

    void SettingPage::OnSaveClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (!CommitEdits(true, "system.json not saved")) return;
        try
        {
            double z = 0;
            std::string error;
            ParseZSafe(z, error);
            auto sys = Sys();
            sys->ZSafe = z;
            scs::McControl::Motion().SetZSafe(z);
            scs::McControl::Motion().SetZSafeInterlock(sys->ZSafeInterlock);
            m_saveResult = scs::McDataManager::SaveSystemData()
                ? "system.json saved. Simulation mode change: restart required."
                : "Save failed — see alarm.";
        }
        catch (const std::exception& e)
        {
            m_saveResult = std::string("Save failed — ") + e.what();
        }
        LoadAll(true);
        LoadZSafe();
        UpdateStaticInfo();
    }

    void SettingPage::OnOpenDataFolderClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        try
        {
            std::wstring root = scs::AppPaths::Root().wstring();
            ShellExecuteW(nullptr, L"open", L"explorer.exe", root.c_str(), nullptr, SW_SHOWNORMAL);
        }
        catch (...) {}
    }

    // ══════════ Trạng thái ══════════

    /// <summary>Cột "Hiện tại" của bảng vị trí chờ — để chép vị trí đang đứng sang ô Vị trí chờ / Soft.</summary>
    void SettingPage::UpdatePositions()
    {
        const auto& axes = scs::McControl::Motion().AxisList();
        for (auto& r : m_posRows)
        {
            hstring text = L"—";
            for (const auto& axis : axes)
            {
                if (axis && axis->Name() == r.Config->Name)
                {
                    text = ui::Num(axis->CurrentPos(), 3);
                    break;
                }
            }
            ui::SetText(r.Current, text);
        }
    }

    void SettingPage::UpdateCommandStates()
    {
        SetEnabled(CheckGlobalVarsButton(), !m_visionBusy);
        SetEnabled(CreateGlobalVarsButton(), !m_visionBusy);
        SetEnabled(CheckFlowOutputsButton(), !m_visionBusy);
        // "Vision giả" chỉ có nghĩa khi đang chọn mô phỏng (giá trị vẫn giữ, chỉ mờ đi).
        SetEnabled(VisionSimCheck(), Sys()->SimulationMode);
    }

    // ══════════ Vision ══════════

    void SettingPage::SetReport(TextBlock const& block, const std::string& text)
    {
        ui::SetText(block, NormalizeNewLines(text));
        ui::Show(block == GlobalVarReportText() ? GlobalVarReportScroll() : FlowOutputReportScroll(), !text.empty());
    }

    // UpdateSourceTrigger=PropertyChanged của bản WPF: ghi ngay vào SystemData (Nạp lại solution đọc từ đó).
    void SettingPage::OnSolutionPathChanged(WF::IInspectable const&, TextChangedEventArgs const&)
    {
        if (!m_loaded) return;
        std::string path = ui::U8(SolutionPathBox().Text());
        if (Sys()->VisionSolutionPath != path) Sys()->VisionSolutionPath = path;
    }

    winrt::fire_and_forget SettingPage::OnBrowseSolutionClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        auto self = get_strong();
        hstring path;
        try
        {
            path = co_await sp::PickSolutionFileAsync(SolutionPathBox().Text());
        }
        catch (...)
        {
            path = {};
        }
        if (!path.empty()) SolutionPathBox().Text(path);   // TextChanged ghi vào SystemData
    }

    // Đường dẫn solution chỉ được VmSolution đọc MỘT LẦN lúc Init nên phải chủ động nạp lại.
    void SettingPage::OnReloadSolutionClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (!CommitEdits(false, "Solution not reloaded")) return;
        Sys()->VisionSolutionPath = ui::U8(SolutionPathBox().Text());
        try
        {
            scs::McDataManager::SaveSystemData();
            std::string refused = scs::McControl::ReloadVisionSolution();
            ui::SetText(VisionReloadText(), refused.empty() ? "Reload requested — see Vision log/alarm for the result." : refused);
        }
        catch (const std::exception& e)
        {
            ui::SetText(VisionReloadText(), std::string("ERROR: ") + e.what());
        }
    }

    void SettingPage::OnCheckGlobalVarsClick(WF::IInspectable const&, RoutedEventArgs const&) { RunGlobalVarSync(false); }

    void SettingPage::OnCreateGlobalVarsClick(WF::IInspectable const&, RoutedEventArgs const&) { RunGlobalVarSync(true); }

    /// <summary>Kiểm tra hoặc tạo biến toàn cục cho solution đang nạp. Tạo thì kiểm tra trước để hỏi xác nhận kèm số
    /// biến sẽ tạo và file .solw sẽ bị lưu.</summary>
    winrt::fire_and_forget SettingPage::RunGlobalVarSync(bool apply)
    {
        auto self = get_strong();
        if (m_visionBusy) co_return;
        m_visionBusy = true;
        UpdateCommandStates();
        SetReport(GlobalVarReportText(), apply ? "Checking before creating…" : "Checking…");

        std::string text;
        bool done = false;
        if (apply)
        {
            scs::GlobalVarSyncReport dry;
            std::string error;
            co_await sp::Background([&dry, &error] {
                try { dry = scs::McControl::SyncVisionGlobalVariables(false); }
                catch (const std::exception& e) { error = e.what(); }
            });

            if (!error.empty())
            {
                text = "ERROR: " + error;
                done = true;
            }
            else if (!dry.Error.empty())
            {
                text = dry.ToText();
                done = true;
            }
            else
            {
                int missing = dry.Count(scs::EGlobalVarState::Missing);
                if (missing == 0)
                {
                    text = "No missing variables.\n" + dry.ToText();
                    done = true;
                }
                else
                {
                    bool yes = false;
                    try
                    {
                        yes = co_await ui::Dialogs::ConfirmAsync(
                            std::format("Create {} missing global variables in the loaded solution, then SAVE the solution:\n{}\n\n"
                                        "Existing variables keep their value and type. Try on a copy of the .solw first. Continue?",
                                        missing, Sys()->VisionSolutionPath),
                            "Create Global Variables");
                    }
                    catch (...)
                    {
                        yes = false;
                    }
                    if (!yes)
                    {
                        text = "Cancelled — no variables created.";
                        done = true;
                    }
                    else
                    {
                        SetReport(GlobalVarReportText(), std::format("Creating {} variables and saving solution…", missing));
                    }
                }
            }
        }

        if (!done)
        {
            scs::GlobalVarSyncReport report;
            std::string error;
            co_await sp::Background([&report, &error, apply] {
                try { report = scs::McControl::SyncVisionGlobalVariables(apply); }
                catch (const std::exception& e) { error = e.what(); }
            });
            text = error.empty() ? report.ToText() : "ERROR: " + error;
        }

        SetReport(GlobalVarReportText(), text);
        m_visionBusy = false;
        UpdateCommandStates();
    }

    /// <summary>Đối chiếu Output Params của các Flow với hợp đồng. Chỉ đọc khai báo — không chạy Flow nào.</summary>
    winrt::fire_and_forget SettingPage::OnCheckFlowOutputsClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        auto self = get_strong();
        if (m_visionBusy) co_return;
        m_visionBusy = true;
        UpdateCommandStates();
        SetReport(FlowOutputReportText(), "Reading Flow Output Params…");

        scs::VmOutCheckReport report;
        std::string error;
        co_await sp::Background([&report, &error] {
            try { report = scs::McControl::CheckVisionFlowOutputs(); }
            catch (const std::exception& e) { error = e.what(); }
        });
        SetReport(FlowOutputReportText(), error.empty() ? report.ToText() : "ERROR: " + error);

        m_visionBusy = false;
        UpdateCommandStates();
    }

    // ══════════ Vòng cập nhật ══════════

    void SettingPage::OnTick()
    {
        if (!m_loaded) return;

        UpdateCommandStates();
        if (m_section == SectionPosition) UpdatePositions();
        if (m_section == SectionLog) m_log.Update();
    }

    void SettingPage::OnShown(bool visible)
    {
        if (!visible || !m_loaded) return;
        // Ô người vận hành đang sửa dở (chữ sai chưa ghi được) giữ nguyên; Z_SAFE là giá trị riêng của trang, không nạp lại.
        LoadAll(false);
        UpdateStaticInfo();
    }
}
