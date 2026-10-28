#include "pch.h"
#include "EtherCatPage.xaml.h"
#if __has_include("EtherCatPage.g.cpp")
#include "EtherCatPage.g.cpp"
#endif

#include "Base/Text.h"
#include "Communication/EtherCat/EtherCatBusManager.h"
#include "Communication/EtherCat/EtherCatIoDevice.h"
#include "Controller/McControl.h"
#include "Data/McDataManager.h"
#include "EtherCat/Esi/EsiCatalog.h"
#include "EtherCat/Soem/RtCpu.h"
#include "EtherCat/Soem/SoemEtherCatBus.h"
#include "Ui/Dialogs.h"
#include "Ui/UiUtil.h"
#include "Utility/Log/LogWorker.h"

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

    constexpr double AxisRowHeight = 40;

    // Bảng servo 14 trục — cột của DataGrid bản WPF (cột "Trục" nằm riêng, cố định) thêm cột "Kênh" (driver hai trục
    // WE2x); vị trí chờ / giới hạn mềm / offset gốc đã sang trang Setting → Vị trí làm việc. Riêng "Đơn vị" rộng hơn
    // (70 → 84): ComboBox WinUI cần chỗ cho mũi tên. Pulse/Rotation + Travel/Rotation đứng ngay trước Counts/đơn vị vì
    // nhập đủ hai số đó thì Counts/đơn vị tự tính (AxisConfig::RecomputeCountsPerUnit). Acc/Dec nhập theo ms (Tuấn
    // 2026-10-18), AxisConfig vẫn lưu giây.
    const std::vector<sp::Column> AxisColumns = {
        { L"Slave", 54 }, { L"Ch", 52 }, { L"Driver", 110 }, { L"Vendor/Product", 116 }, { L"Unit", 84 },
        { L"Pulse/Rotation", 112 }, { L"Travel/Rotation", 114 }, { L"Counts/unit", 110 },
        { L"Wrap 360°", 86 }, { L"Homing method", 120 }, { L"Home speed", 88 }, { L"Jog slow", 80 }, { L"Jog fast", 86 },
        { L"Auto high", 78 }, { L"Auto mid", 78 }, { L"Auto low", 86 }, { L"Acc (ms)", 76 }, { L"Dec (ms)", 76 },
        { L"In-pos tol", 104 }, { L"Home timeout (ms)", 124 }, { L"Move timeout (ms)", 124 },
    };

    /// <summary>Tooltip của tiêu đề cột bảng servo — cột không có trong bảng thì không có tooltip.</summary>
    const wchar_t* AxisColumnTip(std::wstring_view header)
    {
        if (header == L"Pulse/Rotation") return L"Counts per motor revolution. Enter together with Travel/Rotation and Counts/unit is auto-calculated.";
        if (header == L"Travel/Rotation")
            return L"Load travel per motor revolution (mm or deg per Unit column). Written to the driver (0x6092:1) at "
                   L"Connect All Axis; blank = keep the value currently in the driver.";
        if (header == L"Counts/unit")
            return L"Counts per mm/deg of load. Auto = Pulse/Rotation ÷ Travel/Rotation when both are entered; can still be typed manually.";
        if (header == L"Acc (ms)" || header == L"Dec (ms)") return L"Accel/decel time from 0 to commanded speed, ms (10–10000).";
        return nullptr;
    }

    /// <summary>Acc/Dec: AxisConfig lưu giây, ô nhập theo ms.</summary>
    constexpr int RampMsMin = 10, RampMsMax = 10000;
    int ToMs(double seconds) { return (int)std::llround(seconds * 1000.0); }

    /// <summary>Danh mục driver từ ESI trong DriverInfo — tra tên driver cho kết quả Scan. Hỏng thì danh sách rỗng
    /// (như bản WPF): Scan vẫn hiện, chỉ là "(not in ESI)".</summary>
    std::vector<ecm::EsiDeviceInfo> LoadEsiDevices()
    {
        try
        {
            std::filesystem::path dir = scs::EtherCatBusManager::DriverInfoDir();
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) return {};
            return ecm::EsiCatalog::LoadFromFolder(::base::PathToU8(dir));
        }
        catch (...)
        {
            return {};
        }
    }
}

namespace winrt::ShieldCanLaser::implementation
{
    // ══════════ ScannedSlave ══════════

    std::string EtherCatPage::ScannedSlave::IdText() const { return std::format("0x{:X}/0x{:X}", VendorId, ProductCode); }

    std::string EtherCatPage::ScannedSlave::MatchText() const
    {
        return AssignedTo.empty() ? "—" : (Matches ? "match" : "MISMATCH");
    }

    // ══════════ Dựng trang ══════════

    void EtherCatPage::OnRootLoaded(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loaded) return;
        m_loaded = true;

        BindFields();
        BuildAxisTable();

        m_scanTable.Build({ { L"Slave", 54 }, { L"Vendor/Product", 130 }, { L"Rev", 90 }, { L"Driver (ESI)", 0 },
                            { L"Pulse/rev", 84 }, { L"Slot", 84 }, { L"Identity", 110 } }, 30);
        ScanTableHost().Child(m_scanTable.Root);

        BuildAdapterOptions();
        BuildRtCpuOptions();
        BuildRtHistogram();

        LoadAll(true);
        SelectSection(m_section);
        ShowSaveResult();
        OnTick();
    }

    // ══════════ Mục ở cột trái ══════════

    void EtherCatPage::SelectSection(int section)
    {
        if (section < 0 || section >= SectionCount) return;
        m_section = section;
        std::array<ToggleButton, SectionCount> nav{ NavButton0(), NavButton1(), NavButton2() };
        // Bấm vào mục đang chọn làm ToggleButton tự bỏ chọn — luôn đặt lại theo mục thật.
        for (int i = 0; i < SectionCount; ++i) nav[(size_t)i].IsChecked(i == section);
        ui::Show(BusSection(), section == SectionBus);
        ui::Show(ServoSection(), section == SectionServo);
        ui::Show(RtSection(), section == SectionRt);
    }

    void EtherCatPage::OnNavClick(WF::IInspectable const& sender, RoutedEventArgs const&)
    {
        int section = m_section;
        if (auto b = sender.try_as<ToggleButton>()) section = std::stoi(winrt::to_string(winrt::unbox_value<hstring>(b.Tag())));
        SelectSection(section);
    }

    void EtherCatPage::BindFields()
    {
        auto& f = m_fields;

        // ── Remote IO ──
        f.BindInt(IoSlaveBox(), "Remote IO — Slave position",
                  [] { return Sys()->IoModule->SlavePosition; }, [](int v) { Sys()->IoModule->SlavePosition = v; });

        // ── Luồng chu kỳ EtherCAT ──
        f.BindCheck(RtReserveCheck(), [] { return Sys()->RtReserveCore; }, [](bool v) { Sys()->RtReserveCore = v; });
        f.BindInt(RtCycleBox(), "PDO cycle (µs)",
                  [] { return Sys()->RtCycleUs; }, [](int v) { Sys()->RtCycleUs = v; }, 500, 20000);
        // RtSpinUs = -1 (cả chu kỳ) do hộp "Cả chu kỳ (không ngủ)" quyết định; ô số chỉ dùng khi bỏ đánh dấu.
        f.BindText(RtSpinBox(), "Spin-wait before each deadline (µs)",
            [] {
                int spin = Sys()->RtSpinUs;
                return spin < 0 ? std::string() : std::to_string(spin);
            },
            [](const std::string& text, std::string& error) {
                if (::base::IsBlank(text))
                {
                    error = "blank — enter µs (0–20000) or tick \"Full cycle (no sleep)\"";
                    return false;
                }
                int value = 0;
                if (!ui::ParseInt(ui::H(text), value))
                {
                    error = "\"" + text + "\" is not an integer";
                    return false;
                }
                if (value < 0 || value > 20000)
                {
                    error = std::format("must be 0 to 20000 (now {})", value);
                    return false;
                }
                Sys()->RtSpinUs = value;
                return true;
            });
    }

    // Bảng servo 14 trục: hàng tiêu đề + phần thân trong ScrollViewer hai chiều; cột "Trục" nằm riêng bên trái và cuộn dọc
    // theo phần thân (OnAxisBodyViewChanged). Mỗi ô sửa được là một TextBox/ComboBox/CheckBox gắn thẳng vào AxisConfig.
    void EtherCatPage::BuildAxisTable()
    {
        auto header = AxisHeaderGrid();
        for (const auto& c : AxisColumns)
        {
            ColumnDefinition def;
            def.Width(GridLengthHelper::FromPixels(c.Width));
            header.ColumnDefinitions().Append(def);
        }
        for (int i = 0; i < (int)AxisColumns.size(); ++i)
        {
            TextBlock cell = sp::HeaderCell(AxisColumns[i].Header);
            if (auto tip = AxisColumnTip(AxisColumns[i].Header)) ToolTipService::SetToolTip(cell, box_value(hstring(tip)));
            sp::PlaceInColumn(header, cell, i);
        }

        m_axisRows.clear();
        auto divider = ui::Res(L"DividerBrush");
        for (const auto& cfg : Sys()->Axes)
        {
            if (!cfg) continue;
            auto a = cfg;
            std::string name = a->Name;
            auto label = [name](const char* column) { return "Axis " + name + " — " + column; };

            Border nameCell;
            nameCell.Height(AxisRowHeight);
            nameCell.BorderBrush(divider);
            nameCell.BorderThickness(ThicknessHelper::FromLengths(0, 0, 1, 1));
            TextBlock nameText = sp::TextCell(name, 13);
            nameText.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            nameCell.Child(nameText);
            AxisNamesPanel().Children().Append(nameCell);

            Grid row = sp::MakeRowGrid(AxisColumns);
            row.Height(AxisRowHeight);
            row.BorderBrush(divider);
            row.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));

            auto box = [&row](int column) {
                TextBox b = sp::CellBox();
                sp::PlaceInColumn(row, b, column);
                return b;
            };
            auto check = [&row](int column) {
                CheckBox c = sp::CellCheck();
                sp::PlaceInColumn(row, c, column);
                return c;
            };
            auto& f = m_axisFields;

            f.BindInt(box(0), label("Slave"), [a] { return a->SlavePosition; }, [a](int v) { a->SlavePosition = v; });
            // Kênh trục trong driver: 0 = driver một trục (MINAS A6B); driver hai trục dùng kênh 0 / 1.
            f.BindInt(box(1), label("Ch"), [a] { return a->AxisChannel; }, [a](int v) { a->AxisChannel = v; }, 0, 7);

            TextBlock driver = sp::TextCell("");
            sp::PlaceInColumn(row, driver, 2);
            TextBlock id = sp::TextCell("");
            sp::PlaceInColumn(row, id, 3);

            ComboBox unit;
            unit.MinWidth(0);
            unit.Width(76);
            unit.FontSize(13);
            unit.Margin(ThicknessHelper::FromLengths(4, 0, 4, 0));
            unit.VerticalAlignment(VerticalAlignment::Center);
            sp::PlaceInColumn(row, unit, 4);
            f.BindChoice(unit, { "mm", "deg" }, [a] { return a->Unit; }, [a](std::string v) { a->Unit = std::move(v); });

            // Pulse/Rotation, Travel/Rotation đổi → Counts/đơn vị tính lại và ô của nó nạp lại ngay.
            TextBox pulseBox = box(5), travelBox = box(6), countsBox = box(7);   // tạo theo thứ tự cột — thứ tự phím Tab
            f.BindOptionalDouble(pulseBox, label("Pulse/Rotation"), [a] { return a->PulsesPerRotation; },
                                 [this, a, countsBox](std::optional<double> v) {
                                     a->PulsesPerRotation = v;
                                     a->RecomputeCountsPerUnit();
                                     m_axisFields.ReloadField(countsBox);
                                 });
            f.BindOptionalDouble(travelBox, label("Travel/Rotation"), [a] { return a->TravelPerRotation; },
                                 [this, a, countsBox](std::optional<double> v) {
                                     a->TravelPerRotation = v;
                                     a->RecomputeCountsPerUnit();
                                     m_axisFields.ReloadField(countsBox);
                                 });
            f.BindDouble(countsBox, label("Counts/unit"), [a] { return a->CountsPerUnit; }, [a](double v) { a->CountsPerUnit = v; });
            f.BindCheck(check(8), [a] { return a->WrapDegrees; }, [a](bool v) { a->WrapDegrees = v; });
            f.BindInt(box(9), label("Homing method"), [a] { return (int)a->HomingMethod; },
                      [a](int v) { a->HomingMethod = (int8_t)v; }, -128, 127);   // sbyte ở bản C#
            f.BindDouble(box(10), label("Home speed"), [a] { return a->HomingVelocity; }, [a](double v) { a->HomingVelocity = v; });
            f.BindDouble(box(11), label("Jog slow"), [a] { return a->JogSpeedSlow; }, [a](double v) { a->JogSpeedSlow = v; });
            f.BindDouble(box(12), label("Jog fast"), [a] { return a->JogSpeedFast; }, [a](double v) { a->JogSpeedFast = v; });
            f.BindDouble(box(13), label("Auto high"), [a] { return a->MoveSpeed; }, [a](double v) { a->MoveSpeed = v; });
            f.BindDouble(box(14), label("Auto mid"), [a] { return a->MoveSpeedMid(); }, [a](double v) { a->SetMoveSpeedMid(v); });
            f.BindDouble(box(15), label("Auto low"), [a] { return a->MoveSpeedLow(); }, [a](double v) { a->SetMoveSpeedLow(v); });
            f.BindInt(box(16), label("Acc (ms)"), [a] { return ToMs(a->AccelTimeS); },
                      [a](int v) { a->AccelTimeS = v / 1000.0; }, RampMsMin, RampMsMax);
            f.BindInt(box(17), label("Dec (ms)"), [a] { return ToMs(a->DecelTimeS); },
                      [a](int v) { a->DecelTimeS = v / 1000.0; }, RampMsMin, RampMsMax);
            f.BindDouble(box(18), label("In-pos tol"), [a] { return a->InPositionTolerance; },
                         [a](double v) { a->InPositionTolerance = v; });
            f.BindInt(box(19), label("Home timeout (ms)"), [a] { return a->HomeTimeoutMs; }, [a](int v) { a->HomeTimeoutMs = v; });
            f.BindInt(box(20), label("Move timeout (ms)"), [a] { return a->MoveTimeoutMs; }, [a](int v) { a->MoveTimeoutMs = v; });

            AxisBodyPanel().Children().Append(row);
            m_axisRows.push_back({ cfg, driver, id });
        }
    }

    // NetworkAdapterOption.ListAll của bản WPF: "(chưa chọn)", bus mô phỏng, các card thật (chỉ LIỆT KÊ qua
    // GetAdaptersAddresses — không mở card nào), và card đã lưu mà giờ không thấy (giữ lại để bấm Lưu không âm
    // thầm xoá cấu hình).
    void EtherCatPage::BuildAdapterOptions()
    {
        std::string current = Sys()->NetworkAdapter;
        m_adapterOptions.clear();
        m_adapterOptions.push_back({ "(not selected)", "" });
        m_adapterOptions.push_back({ "Simulated EtherCAT bus (SimulatedEtherCatBus)", scs::EtherCatBusManager::SimulatedAdapterSentinel });
        try
        {
            for (const auto& nic : ecm::SoemEtherCatBus::ListAdapters())
                m_adapterOptions.push_back({ nic.FriendlyName + " (" + nic.Description + ")", nic.DeviceName });
        }
        catch (...) {}
        bool known = std::any_of(m_adapterOptions.begin(), m_adapterOptions.end(),
                                 [&](const AdapterOption& o) { return o.Value == current; });
        if (!current.empty() && !known) m_adapterOptions.push_back({ "(not found) " + current, current });

        int selected = 0;
        for (int i = 0; i < (int)m_adapterOptions.size(); ++i)
            if (m_adapterOptions[i].Value == current) { selected = i; break; }

        m_loading = true;
        AdapterCombo().Items().Clear();
        for (const auto& o : m_adapterOptions) AdapterCombo().Items().Append(box_value(ui::H(o.Display)));
        AdapterCombo().SelectedIndex(selected);
        m_loading = false;
    }

    // "Tự chọn" + một mục cho mỗi CPU logic. CPU lai (EfficiencyClass khác nhau): lớp thấp nhất là E-core.
    void EtherCatPage::BuildRtCpuOptions()
    {
        std::vector<ecm::rt::LogicalCpu> cpus;
        try { cpus = ecm::rt::EnumerateCpus(); }
        catch (...) {}

        uint8_t lowest = 255;
        for (const auto& c : cpus) lowest = std::min(lowest, c.EfficiencyClass);
        bool hybrid = std::any_of(cpus.begin(), cpus.end(), [&](const auto& c) { return c.EfficiencyClass != lowest; });

        m_rtCpuValues = { -1 };
        std::vector<std::string> labels = { "Auto" };
        for (const auto& c : cpus)
        {
            std::string text = std::format("CPU {}", c.LogicalIndex);
            if (hybrid) text += c.EfficiencyClass == lowest ? " (E-core)" : " (P-core)";
            if (c.Group != 0) text += std::format(" · group {}", c.Group);
            if (c.Allocated && !c.AllocatedToThisProcess) text += " · held by another process";
            m_rtCpuValues.push_back(c.LogicalIndex);
            labels.push_back(std::move(text));
        }

        m_loading = true;
        RtCpuCombo().Items().Clear();
        for (const auto& l : labels) RtCpuCombo().Items().Append(box_value(ui::H(l)));
        m_loading = false;
    }

    // Biểu đồ trễ phát khung: 7 ô theo ecm::rt::LatenessBucketLimitUs (<50, <100, … , ≥2000 µs).
    void EtherCatPage::BuildRtHistogram()
    {
        auto grid = RtHistogramGrid();
        m_histogramCounts.clear();
        for (int i = 0; i < ecm::rt::LatenessBuckets; ++i)
        {
            ColumnDefinition def;
            def.Width(GridLengthHelper::FromPixels(92));
            grid.ColumnDefinitions().Append(def);

            std::string range = i < ecm::rt::LatenessBuckets - 1
                ? std::format("< {} µs", ecm::rt::LatenessBucketLimitUs[i])
                : std::format("≥ {} µs", ecm::rt::LatenessBucketLimitUs[ecm::rt::LatenessBuckets - 2]);

            Border cell;
            cell.BorderBrush(ui::Res(L"DividerBrush"));
            cell.BorderThickness(ThicknessHelper::FromLengths(i > 0 ? 1 : 0, 0, 0, 0));
            cell.Padding(ThicknessHelper::FromLengths(10, 6, 10, 6));
            StackPanel stack;
            TextBlock label;
            label.Text(ui::H(range));
            label.FontSize(11);
            label.Foreground(ui::Res(L"TextSecondaryBrush"));
            TextBlock count;
            count.Text(L"0");
            count.FontSize(13);
            stack.Children().Append(label);
            stack.Children().Append(count);
            cell.Child(stack);
            sp::PlaceInColumn(grid, cell, i);
            m_histogramCounts.push_back(count);
        }
    }

    // ══════════ Nạp ô nhập từ dữ liệu ══════════

    void EtherCatPage::LoadAll(bool force)
    {
        m_loading = true;
        m_fields.LoadAll(force);
        m_axisFields.LoadAll(force);
        LoadRtSpin();

        int cpu = Sys()->RtCpuIndex;
        auto it = std::find(m_rtCpuValues.begin(), m_rtCpuValues.end(), cpu);
        if (it == m_rtCpuValues.end())
        {
            // Giá trị trong system.json không có trên máy này (chép cấu hình từ máy khác) — giữ, không âm thầm đổi.
            m_rtCpuValues.push_back(cpu);
            RtCpuCombo().Items().Append(box_value(ui::H(std::format("(not found) CPU {}", cpu))));
            it = m_rtCpuValues.end() - 1;
        }
        int index = (int)(it - m_rtCpuValues.begin());
        if (RtCpuCombo().SelectedIndex() != index) RtCpuCombo().SelectedIndex(index);
        m_loading = false;
    }

    void EtherCatPage::LoadRtSpin()
    {
        bool all = Sys()->RtSpinUs < 0;
        RtSpinAllCheck().IsChecked(all);
        SetEnabled(RtSpinBox(), !all);
    }

    bool EtherCatPage::CommitEdits(const std::string& action)
    {
        std::vector<std::string> errors = m_fields.CommitAll();
        for (auto& e : m_axisFields.CommitAll()) errors.push_back(std::move(e));
        if (errors.empty()) return true;

        ui::Dialogs::Warn(action + " — invalid input fields remain (red border):\n• " + ::base::Join(errors, "\n• ") +
                              "\n\nCorrect them (or re-enter the old value) and try again.",
                          "Invalid Value");
        return false;
    }

    // ══════════ Lưu ══════════

    void EtherCatPage::OnSaveClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (!CommitEdits("system.json not saved")) return;
        try
        {
            m_saveResult = scs::McDataManager::SaveSystemData()
                ? "system.json saved. NIC change requires app restart; slave, channel, identity, counts or "
                  "homing changes require Connect All Axis."
                : "Save failed — see alarm.";
        }
        catch (const std::exception& e)
        {
            m_saveResult = std::string("Save failed — ") + e.what();
        }
        LoadAll(true);
        ShowSaveResult();
    }

    void EtherCatPage::ShowSaveResult()
    {
        ui::SetText(SaveResultText(), m_saveResult);
        ui::Show(SaveResultText(), !m_saveResult.empty());
    }

    // ══════════ EtherCAT ══════════

    void EtherCatPage::SetEtherCatMessage(std::string text)
    {
        m_etherCatMessage = std::move(text);
        ui::SetText(EtherCatMessageText(), m_etherCatMessage);
        ui::Show(EtherCatMessageBar(), !m_etherCatMessage.empty());
    }

    void EtherCatPage::OnAdapterSelectionChanged(WF::IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_loading || !m_loaded) return;
        int index = AdapterCombo().SelectedIndex();
        if (index < 0 || index >= (int)m_adapterOptions.size()) return;
        const std::string& value = m_adapterOptions[index].Value;
        if (Sys()->NetworkAdapter == value) return;
        Sys()->NetworkAdapter = value;
        SetEtherCatMessage("Network adapter selected — press SAVE, then RESTART the app to use it.");
    }

    void EtherCatPage::RefreshBusInfo()
    {
        auto bus = scs::McControl::Motion().Bus();
        std::string busText;
        if (!bus)
        {
            busText = "Running in SIMULATION MODE (FakeAxis/FakeIo) — disable simulation, select NIC, save and restart to use EtherCAT.";
        }
        else if (bus->IsConnected())
        {
            busText = bus->BusDescription() + std::format("  ·  bus OPEN — {} slave(s)", bus->LastFoundSlaveCount());
        }
        else
        {
            std::string error = bus->LastError();
            busText = bus->BusDescription() + "  ·  bus NOT OPEN" + (error.empty() ? std::string() : " — " + error);
        }
        ui::SetText(BusText(), busText);

        // Module thật trong rack R1EC và số DI/DO lộ ra — đối chiếu với IoMap.
        std::string rack;
        if (auto io = std::dynamic_pointer_cast<scs::EtherCatIoDevice>(scs::McControl::Io()))
        {
            auto names = io->ModuleNames();
            rack = names.empty()
                ? std::string("Remote IO rack not read yet.")
                : "Rack: " + ::base::Join(names, " + ") + std::format("  →  {} DI / {} DO", io->MappedDiCount(), io->MappedDoCount());
        }
        ui::SetText(IoRackText(), rack);
        ui::Show(IoRackText(), !rack.empty());

        // Kết quả từng slot của lần Connect All Axis gần nhất — dựng lại chỉ khi đổi.
        std::vector<std::string> lines;
        if (bus)
        {
            for (const auto& r : bus->LastAxisResults()) lines.push_back((r.IsOk() ? "✓ " : "✗ ") + r.ToString());
            for (const auto& r : bus->LastIoResults()) lines.push_back((r.IsOk() ? "✓ " : "✗ ") + r.ToString());
        }
        std::string key = ::base::Join(lines, "\n");
        if (key != m_connectResultsKey)
        {
            m_connectResultsKey = key;
            auto panel = ConnectResultsPanel();
            panel.Children().Clear();
            for (const auto& line : lines)
            {
                TextBlock t;
                t.Text(ui::H(line));
                t.FontFamily(Media::FontFamily(L"Consolas"));
                t.FontSize(11);
                t.TextWrapping(TextWrapping::Wrap);
                t.Margin(ThicknessHelper::FromLengths(0, 0, 0, 2));
                panel.Children().Append(t);
            }
        }
    }

    void EtherCatPage::UpdateCommandStates()
    {
        auto bus = scs::McControl::Motion().Bus();
        bool open = bus && bus->IsConnected();
        SetEnabled(ScanButton(), bus && !open && !m_busy);
        SetEnabled(ApplyScanButton(), !m_scanned.empty());
        SetEnabled(ConnectAllButton(), bus && !m_busy);
        SetEnabled(DisconnectBusButton(), open && !m_busy);
    }

    winrt::fire_and_forget EtherCatPage::OnDisconnectBusClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        auto self = get_strong();
        if (m_busy) co_return;
        m_busy = true;
        UpdateCommandStates();

        std::string why;
        co_await sp::Background([&why] {
            try { why = scs::McControl::DisconnectEtherCat(); }
            catch (const std::exception& e) { why = std::string("Bus disconnect error: ") + e.what(); }
        });
        SetEtherCatMessage(why.empty() ? "Bus disconnected — ready to SCAN." : why);

        m_busy = false;
        RefreshBusInfo();
        UpdateCommandStates();
    }

    winrt::fire_and_forget EtherCatPage::OnScanClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        auto self = get_strong();
        auto bus = scs::McControl::Motion().Bus();
        if (m_busy || !bus) co_return;
        m_busy = true;
        UpdateCommandStates();
        SetEtherCatMessage("Scanning bus…");

        std::vector<ecm::DiscoveredSlaveInfo> found;
        std::vector<ecm::EsiDeviceInfo> catalog;
        std::string error;
        co_await sp::Background([&found, &catalog, &error, bus] {
            try
            {
                // SOEM chỉ có một ngữ cảnh — không quét được khi bus đang mở. Nút Scan vốn khoá lúc bus mở (phải bấm
                // Ngắt bus trước, như bản WPF); đây là chốt chặn nếu bus vừa được mở lại giữa chừng.
                if (bus->IsConnected())
                {
                    std::string why = scs::McControl::DisconnectEtherCat();
                    if (!why.empty())
                    {
                        error = why;
                        return;
                    }
                }
                found = bus->ScanSlaves();
                catalog = LoadEsiDevices();
            }
            catch (const std::exception& e)
            {
                error = e.what();
            }
        });

        if (!error.empty())
        {
            SetEtherCatMessage("Scan error: " + error);
        }
        else
        {
            auto sys = Sys();
            std::stable_sort(found.begin(), found.end(),
                             [](const auto& x, const auto& y) { return x.SlavePosition < y.SlavePosition; });

            m_scanned.clear();
            for (const auto& s : found)
            {
                auto esi = std::find_if(catalog.begin(), catalog.end(), [&](const ecm::EsiDeviceInfo& d) {
                    return d.VendorId == s.VendorId && d.ProductCode == s.ProductCode;
                });
                // Driver hai trục (WE2x) là MỘT slave cho HAI trục — gom mọi trục cùng vị trí slave.
                std::vector<std::shared_ptr<scs::AxisConfig>> onSlave;
                for (const auto& a : sys->Axes)
                    if (a && a->SlavePosition == s.SlavePosition) onSlave.push_back(a);
                std::shared_ptr<scs::AxisConfig> axis = onSlave.empty() ? nullptr : onSlave.front();
                bool isIo = !axis && sys->IoModule && sys->IoModule->SlavePosition == s.SlavePosition;
                uint32_t expV = axis ? axis->ExpectedVendorId : (isIo ? sys->IoModule->ExpectedVendorId : 0);
                uint32_t expP = axis ? axis->ExpectedProductCode : (isIo ? sys->IoModule->ExpectedProductCode : 0);
                bool axesAgree = std::all_of(onSlave.begin(), onSlave.end(), [&](const auto& a) {
                    return a->ExpectedVendorId == expV && a->ExpectedProductCode == expP;
                });
                std::vector<std::string> names;
                for (const auto& a : onSlave) names.push_back(a->Name);

                ScannedSlave v;
                v.SlavePosition = s.SlavePosition;
                v.VendorId = s.VendorId;
                v.ProductCode = s.ProductCode;
                v.Revision = s.RevisionNo;
                v.DriverName = esi != catalog.end() ? esi->DisplayName : "(not in ESI)";
                auto ppr = s.PulsesPerRotation();
                v.EncoderText = ppr.has_value() ? std::format("{:.0f}", *ppr) : std::string();
                v.AssignedTo = axis ? ::base::Join(names, " + ") : (isIo ? "Remote IO" : "");
                v.Matches = (axis || isIo) && axesAgree && expV == s.VendorId && expP == s.ProductCode;
                m_scanned.push_back(std::move(v));
            }
            ShowScanResults();

            // Cùng phép đếm với lúc Connect All Axis — lệch số này thì bus sẽ không mở.
            int expected = scs::EtherCatBusManager::ExpectedSlaveCount(*sys);
            std::string message = std::format("Scan: found {} slave(s), configuration needs {} (axes + Remote IO with assigned driver).",
                                              found.size(), expected);
            if ((int)found.size() != expected)
                message += " ⚠ SLAVE COUNT MISMATCH — Connect All Axis will be refused: check driver wiring/power.";
            if (std::any_of(m_scanned.begin(), m_scanned.end(), [](const ScannedSlave& s) { return !s.AssignedTo.empty() && !s.Matches; }))
                message += " Some slots MISMATCH the configuration — press \"APPLY SCAN\" if these are the installed drivers.";
            SetEtherCatMessage(message);
            scs::LogWorker::Instance().RecordLog(message, scs::LogLevel::Info, "EtherCAT");
        }

        m_busy = false;
        RefreshBusInfo();
        UpdateCommandStates();
    }

    void EtherCatPage::ShowScanResults()
    {
        auto rows = m_scanTable.Rows.Children();
        rows.Clear();
        for (const auto& s : m_scanned)
        {
            Grid row = m_scanTable.NewRow(28);
            sp::PlaceInColumn(row, sp::TextCell(std::to_string(s.SlavePosition)), 0);
            sp::PlaceInColumn(row, sp::TextCell(s.IdText()), 1);
            sp::PlaceInColumn(row, sp::TextCell(std::format("0x{:X}", s.Revision)), 2);
            sp::PlaceInColumn(row, sp::TextCell(s.DriverName), 3);
            sp::PlaceInColumn(row, sp::TextCell(s.EncoderText), 4);
            sp::PlaceInColumn(row, sp::TextCell(s.AssignedTo), 5);
            sp::PlaceInColumn(row, sp::TextCell(s.MatchText()), 6);
            rows.Append(row);
        }
    }

    // Gán định danh của slave tìm thấy vào MỌI slot cấu hình CÙNG vị trí slave — 14 trục (hai trục một slave với driver
    // WE2x) và Remote IO. Không đổi thứ tự trục: vị trí slave/kênh của từng trục sửa ở bảng bên dưới trước nếu đi dây
    // khác thứ tự mặc định.
    void EtherCatPage::OnApplyScanClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        auto sys = Sys();
        int n = 0;
        for (const auto& s : m_scanned)
        {
            bool anyAxis = false;
            for (const auto& axis : sys->Axes)
            {
                if (!axis || axis->SlavePosition != s.SlavePosition) continue;
                axis->ExpectedVendorId = s.VendorId;
                axis->ExpectedProductCode = s.ProductCode;
                axis->ExpectedRevision = s.Revision;
                axis->ExpectedDriverName = s.DriverName;
                ++n;
                anyAxis = true;
            }
            if (anyAxis) continue;
            if (sys->IoModule && sys->IoModule->SlavePosition == s.SlavePosition)
            {
                sys->IoModule->ExpectedVendorId = s.VendorId;
                sys->IoModule->ExpectedProductCode = s.ProductCode;
                sys->IoModule->ExpectedRevision = s.Revision;
                sys->IoModule->ExpectedDriverName = s.DriverName;
                ++n;
            }
        }
        m_axisFields.LoadAll(true);   // ReloadAxisRows của bản WPF
        OnTick();
        SetEtherCatMessage(std::format("Identity assigned to {} slot(s) by slave position — press SAVE, then CONNECT ALL.", n));
    }

    winrt::fire_and_forget EtherCatPage::OnConnectAllClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        auto self = get_strong();
        if (m_busy || !scs::McControl::Motion().Bus()) co_return;
        // "Lưu cấu hình, đóng rồi mở lại bus với cấu hình hiện tại" — ô đang sửa phải vào dữ liệu trước khi lưu.
        if (!CommitEdits("Connect All Axis not run")) co_return;
        m_busy = true;
        UpdateCommandStates();
        SetEtherCatMessage("Connect All Axis in progress…");

        std::string why;
        std::string error;
        try
        {
            scs::McDataManager::SaveSystemData();
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }
        if (error.empty())
        {
            co_await sp::Background([&why, &error] {
                try { why = scs::McControl::ReconnectEtherCat(); }
                catch (const std::exception& e) { error = e.what(); }
            });
        }

        if (!error.empty())
            SetEtherCatMessage("Connect All Axis error: " + error);
        else
            SetEtherCatMessage(why.empty() ? "Connect All Axis: all slots OK — servo on and home before running."
                                           : "Connect All Axis failed: " + why);

        m_busy = false;
        RefreshBusInfo();
        UpdateCommandStates();
    }

    // ══════════ Luồng chu kỳ EtherCAT ══════════

    void EtherCatPage::OnRtCpuSelectionChanged(WF::IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_loading || !m_loaded) return;
        int index = RtCpuCombo().SelectedIndex();
        if (index < 0 || index >= (int)m_rtCpuValues.size()) return;
        Sys()->RtCpuIndex = m_rtCpuValues[index];
    }

    void EtherCatPage::OnRtSpinAllClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        auto value = RtSpinAllCheck().IsChecked();
        bool all = value && value.Value();
        // Bỏ đánh dấu: gợi ý 1000 µs (con số ở chú thích của RtCyclicThread) — sửa tiếp trong ô số.
        Sys()->RtSpinUs = all ? -1 : 1000;
        m_fields.ReloadField(RtSpinBox());   // kể cả chữ sai còn sót: ô bị khoá thì không được chặn nút Lưu
        LoadRtSpin();
    }

    void EtherCatPage::UpdateRtStats()
    {
        auto bus = scs::McControl::Motion().Bus();
        // EtherCatBusManager không có cách hỏi thẳng "bus này là SOEM thật?" — BusDescription của bus SOEM bắt đầu
        // bằng "SOEM — " (xem EtherCatBusManager.cpp); bus mô phỏng thì RtStats toàn 0.
        bool soem = bus && ::base::StartsWith(bus->BusDescription(), "SOEM");
        ecm::rt::RtStats st{};
        if (soem && bus->IsConnected()) st = bus->RtStats();
        bool live = st.Cycles > 0;

        std::string message;
        if (!soem) message = "Available only with the real SOEM bus.";
        else if (!live) message = "SOEM bus not open — statistics available after Connect All Axis.";
        ui::SetText(RtStatsMessage(), message);
        ui::Show(RtStatsMessage(), !live);
        ui::Show(RtStatsPanel(), live);
        if (!live) return;

        ui::SetText(RtCpuText(), st.CpuIndex >= 0
            ? std::format("CPU {} · {} · {}", st.CpuIndex, st.Pinned ? "pinned" : "NOT pinned",
                          st.CoreReserved ? "core reserved" : "core NOT reserved")
            : std::string("not pinned to any CPU") + (st.CoreReserved ? " · core reserved" : ""));
        ui::SetText(RtPeriodText(), std::format("{} µs / {} µs{}", st.PeriodUs, st.SpinUs,
                                                st.SpinUs >= st.PeriodUs ? " (no sleep)" : ""));
        ui::SetText(RtCyclesText(), std::format("{}", st.Cycles));
        ui::SetText(RtJitterText(), std::format("{:+.1f} / {:+.1f} / {:+.1f}", st.PeriodJitterMinUs, st.PeriodJitterMaxUs,
                                                st.PeriodJitterAvgUs));
        ui::SetText(RtLatenessText(), std::format("{:.1f} / {:.1f}", st.LatenessMaxUs, st.LatenessAvgUs));
        ui::SetText(RtScanText(), std::format("{:.1f} / {:.1f} / {:.1f}", st.ScanTimeMinUs, st.ScanTimeMaxUs, st.ScanTimeAvgUs));
        ui::SetText(RtOverrunText(), std::format("{}", st.Overruns));
        ui::SetText(RtWkcText(), std::format("{}  (last {} / expected {})", st.WkcErrors, st.LastWkc, st.ExpectedWkc));
        ui::SetText(RtMigrationText(), std::format("{}", st.CpuMigrations));
        for (int i = 0; i < (int)m_histogramCounts.size() && i < ecm::rt::LatenessBuckets; ++i)
            ui::SetText(m_histogramCounts[i], std::format("{}", st.LatenessHistogram[i]));

        std::string info = bus->RealtimeInfo();
        info += std::format("{}High-resolution timer: {} · thread priority: {}", info.empty() ? "" : "\n",
                            st.HighResTimer ? "yes" : "NO", st.ThreadPriority);
        ui::SetText(RtInfoText(), info);
    }

    // ══════════ Bảng servo 14 trục: cuộn đồng bộ ══════════

    void EtherCatPage::OnAxisBodyViewChanged(WF::IInspectable const&, ScrollViewerViewChangedEventArgs const&)
    {
        auto body = AxisBodyScroll();
        AxisHeaderScroll().ChangeView(WF::IReference<double>(body.HorizontalOffset()), nullptr, nullptr, true);
        AxisNamesScroll().ChangeView(nullptr, WF::IReference<double>(body.VerticalOffset()), nullptr, true);
    }

    void EtherCatPage::OnAxisNamesWheel(WF::IInspectable const&, Input::PointerRoutedEventArgs const& e)
    {
        int delta = e.GetCurrentPoint(AxisNamesScroll()).Properties().MouseWheelDelta();
        auto body = AxisBodyScroll();
        double target = std::clamp(body.VerticalOffset() - delta * 0.4, 0.0, body.ScrollableHeight());
        body.ChangeView(nullptr, WF::IReference<double>(target), nullptr);
        e.Handled(true);
    }

    // ══════════ Vòng cập nhật ══════════

    void EtherCatPage::OnTick()
    {
        if (!m_loaded) return;

        RefreshBusInfo();
        UpdateCommandStates();
        UpdateRtStats();

        // Ô chỉ đọc — định danh driver chỉ đổi qua "APPLY SCAN".
        for (auto& r : m_axisRows)
        {
            ui::SetText(r.Driver, r.Config->ExpectedDriverName);
            ui::SetText(r.Id, r.Config->DriverIdText());
        }
        if (auto io = Sys()->IoModule)
        {
            ui::SetText(IoIdText(), io->DriverIdText());
            ui::SetText(IoDriverText(), io->ExpectedDriverName);
        }
    }

    void EtherCatPage::OnShown(bool visible)
    {
        if (!visible || !m_loaded) return;
        // Ô người vận hành đang sửa dở (chữ sai chưa ghi được) giữ nguyên.
        LoadAll(false);
    }
}
