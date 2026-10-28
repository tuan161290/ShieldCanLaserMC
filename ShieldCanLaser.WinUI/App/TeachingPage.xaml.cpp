#include "pch.h"
#include "TeachingPage.xaml.h"
#if __has_include("TeachingPage.g.cpp")
#include "TeachingPage.g.cpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>

#include "Base/Async.h"
#include "Base/Text.h"
#include "Controller/McControl.h"
#include "Data/McDataManager.h"
#include "Infor/AxisId.h"
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
    /// <summary>Math.Round(x, 4) của C# — làm tròn về số chẵn gần nhất ở điểm giữa (chế độ làm tròn mặc định).</summary>
    double Round4(double v) { return std::nearbyint(v * 10000.0) / 10000.0; }

    void SetEnabled(Control const& c, bool enabled)
    {
        if (c && c.IsEnabled() != enabled) c.IsEnabled(enabled);
    }

    /// <summary>Path.GetInvalidFileNameChars() trên Windows: ký tự điều khiển và " &lt; &gt; | : * ? \ /.</summary>
    bool ValidModelName(const std::string& name)
    {
        if (::base::IsBlank(name))
        {
            ui::Dialogs::Warn("Enter a model name.");
            return false;
        }
        for (unsigned char c : name)
        {
            if (c < 32 || std::strchr("\"<>|:*?\\/", c) != nullptr)
            {
                ui::Dialogs::Warn("Model name must not contain special characters (\\ / : * ? \" < > |).");
                return false;
            }
        }
        return true;
    }

    /// <summary>Tên file model trên Windows không phân biệt hoa thường — so trùng tên cũng vậy.</summary>
    bool ContainsName(const std::vector<std::string>& names, const std::string& name)
    {
        return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return ::base::EqualsIgnoreCase(n, name); });
    }

    /// <summary>Tóm tắt một dòng cho danh sách điểm: trục nào, đã teach lúc nào (TeachPointVm.Summary).</summary>
    std::string SummaryOf(const scs::TeachPosition& p)
    {
        std::string text;
        if (p.Pos.empty())
        {
            text = "no axes";
        }
        else
        {
            std::vector<std::string> axes;
            for (const char* a : scs::AxisId::All)
                if (p.Has(a)) axes.push_back(a);
            text = ::base::Join(axes, " ");
        }
        text += p.TaughtAt.has_value() ? "  ·  " + scs::FormatLocal(*p.TaughtAt, "dd/MM HH:mm") : std::string("  ·  not taught");
        return text;
    }

    std::string WhenText(const std::optional<scs::TimePoint>& at)
    {
        return at ? "taught " + scs::FormatLocal(*at, "dd/MM HH:mm") : std::string("not taught");
    }

    Border DividedRow()
    {
        Border row;
        row.BorderBrush(ui::Res(L"DividerBrush"));
        row.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
        row.Padding(ThicknessHelper::FromLengths(0, 4, 0, 4));
        return row;
    }

    /// <summary>Nút hẹp của bản WPF (CompactButtonStyle: Padding 4,3; JogButtonStyle: cao 32).</summary>
    Button CompactButton(wchar_t const* text, double width)
    {
        Button b;
        b.Content(box_value(text));
        b.Width(width);
        b.Height(32);
        b.MinHeight(32);
        b.Padding(ThicknessHelper::FromLengths(4, 3, 4, 3));
        return b;
    }

    int TagOf(WF::IInspectable const& sender, int fallback)
    {
        if (auto b = sender.try_as<Button>())
        {
            try { return std::stoi(winrt::to_string(winrt::unbox_value<hstring>(b.Tag()))); }
            catch (...) {}
        }
        return fallback;
    }

    void Guarded(const char* what, std::function<void()> const& fn)
    {
        try { fn(); }
        catch (const std::exception& ex)
        {
            scs::LogWorker::Instance().RecordLog(std::string("Teaching: ") + what + " failed: " + ex.what(), scs::LogLevel::Error, "UI");
            ui::Dialogs::Error(std::string(what) + " failed: " + ex.what());
        }
        catch (winrt::hresult_error const& ex)
        {
            ui::Dialogs::Error(std::string(what) + " failed: " + winrt::to_string(ex.message()));
        }
    }
}

namespace winrt::ShieldCanLaser::implementation
{
    // ───────────────────────── WrapFlow ─────────────────────────

    void TeachingPage::WrapFlow::Attach(StackPanel const& host)
    {
        Host = host;
        Items.clear();
        for (auto const& line : host.Children())
        {
            if (auto panel = line.try_as<Panel>())
                for (auto const& child : panel.Children())
                    Items.push_back(child.as<FrameworkElement>());
        }
    }

    void TeachingPage::WrapFlow::Reflow(double width)
    {
        if (!Host || Items.empty() || width <= 0) return;

        // Như WrapPanel: phần tử (kể cả lề) không còn vừa chỗ trên hàng thì sang hàng mới.
        std::vector<size_t> breaks;
        double x = 0;
        for (size_t i = 0; i < Items.size(); ++i)
        {
            auto m = Items[i].Margin();
            double w = Items[i].Width();
            if (std::isnan(w)) w = Items[i].ActualWidth();
            double footprint = w + m.Left + m.Right;
            if (i > 0 && x + footprint > width + 0.5)
            {
                breaks.push_back(i);
                x = 0;
            }
            x += footprint;
        }
        if (breaks == Breaks) return;
        Breaks = breaks;

        for (auto const& line : Host.Children())
            if (auto panel = line.try_as<Panel>()) panel.Children().Clear();
        Host.Children().Clear();

        StackPanel line{ nullptr };
        for (size_t i = 0; i < Items.size(); ++i)
        {
            if (i == 0 || std::find(breaks.begin(), breaks.end(), i) != breaks.end())
            {
                line = StackPanel();
                line.Orientation(Orientation::Horizontal);
                Host.Children().Append(line);
            }
            line.Children().Append(Items[i]);
        }
    }

    // ───────────────────────── dựng trang ─────────────────────────

    void TeachingPage::OnRootLoaded(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loaded) return;
        m_loaded = true;

        BuildJogRows();
        BuildAxisRows();
        BuildLayoutEditors();

        m_fieldsFlow.Attach(FieldsWrap());
        m_buttonsFlow.Attach(ButtonsWrap());
        auto weak = get_weak();
        FieldsWrap().SizeChanged([weak](auto&&, SizeChangedEventArgs const& e) {
            if (auto self = weak.get()) self->m_fieldsFlow.Reflow(e.NewSize().Width);
        });
        ButtonsWrap().SizeChanged([weak](auto&&, SizeChangedEventArgs const& e) {
            if (auto self = weak.get()) self->m_buttonsFlow.Reflow(e.NewSize().Width);
        });

        Guarded("Load Teaching page", [&] {
            RefreshModelList();
            ReloadPoints();
            SelectTab(m_tab);
        });
        OnTick();
    }

    // Một dòng mỗi trục (JogRowTemplate của bản WPF): tên, toạ độ + đơn vị, Jog −/+, đèn ORG.
    void TeachingPage::BuildJogRows()
    {
        for (const auto& axis : scs::McControl::Motion().AxisList())
        {
            if (!axis) continue;
            JogRow jog;
            jog.Axis = axis;

            Border row = DividedRow();
            StackPanel line;
            line.Orientation(Orientation::Horizontal);

            TextBlock name;
            name.Text(ui::H(axis->Name()));
            name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            name.Width(30);
            name.VerticalAlignment(VerticalAlignment::Center);
            line.Children().Append(name);

            TextBlock pos;
            pos.Width(104);
            pos.VerticalAlignment(VerticalAlignment::Center);
            pos.FontFamily(Media::FontFamily(L"Consolas"));
            pos.FontSize(14);
            jog.PosRun = Documents::Run();
            jog.UnitRun = Documents::Run();
            jog.UnitRun.FontSize(10);
            jog.UnitRun.Foreground(ui::Res(L"TextSecondaryBrush"));
            pos.Inlines().Append(jog.PosRun);
            pos.Inlines().Append(jog.UnitRun);
            line.Children().Append(pos);

            for (int dir : { -1, 1 })
            {
                Button b = CompactButton(dir < 0 ? L"−" : L"+", 44);
                b.Margin(ThicknessHelper::FromLengths(2, 0, 2, 0));
                ui::AttachJog(b, axis->Name(), dir);   // giữ để chạy, nhả / kéo ra ngoài / mất chuột là dừng
                line.Children().Append(b);
            }

            jog.OrgLamp = Shapes::Ellipse();
            jog.OrgLamp.Width(9);
            jog.OrgLamp.Height(9);
            jog.OrgLamp.Margin(ThicknessHelper::FromLengths(12, 0, 4, 0));
            jog.OrgLamp.VerticalAlignment(VerticalAlignment::Center);
            line.Children().Append(jog.OrgLamp);

            TextBlock org;
            org.Text(L"ORG");
            org.FontSize(11);
            org.VerticalAlignment(VerticalAlignment::Center);
            org.Foreground(ui::Res(L"TextSecondaryBrush"));
            line.Children().Append(org);

            row.Child(line);
            JogRows().Children().Append(row);
            m_jogRows.push_back(std::move(jog));
        }
    }

    // Một hàng mỗi trục trong khung chi tiết (tab Others): ô chọn dùng trục, toạ độ (sửa tay được), đơn vị, GET POS.
    void TeachingPage::BuildAxisRows()
    {
        static_assert(scs::AxisId::All.size() == AxisCount, "TeachingPage::AxisCount phải khớp AxisId::All");
        auto weak = get_weak();
        for (size_t i = 0; i < AxisCount; ++i)
        {
            const char* axisName = scs::AxisId::All[i];
            AxisEditRow& r = m_axisRows[i];

            Border row = DividedRow();
            StackPanel line;
            line.Orientation(Orientation::Horizontal);

            r.Use = CheckBox();
            r.Use.Width(68);
            r.Use.MinWidth(0);   // CheckBox của WinUI có MinWidth 120
            r.Use.VerticalAlignment(VerticalAlignment::Center);
            TextBlock label;
            label.Text(ui::H(axisName));
            label.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            r.Use.Content(label);
            r.Use.Click([weak, i](auto&&, auto&&) {
                if (auto self = weak.get()) self->OnAxisUseClick(i);
            });
            line.Children().Append(r.Use);

            r.Value = TextBox();
            r.Value.Width(110);
            r.Value.VerticalAlignment(VerticalAlignment::Center);
            r.Value.LostFocus([weak, i](auto&&, auto&&) {
                if (auto self = weak.get()) self->CommitAxisValue(i);
            });
            line.Children().Append(r.Value);

            r.Unit = TextBlock();
            r.Unit.Width(34);
            r.Unit.Margin(ThicknessHelper::FromLengths(6, 0, 0, 0));
            r.Unit.VerticalAlignment(VerticalAlignment::Center);
            r.Unit.FontSize(11);
            r.Unit.Foreground(ui::Res(L"TextSecondaryBrush"));
            line.Children().Append(r.Unit);

            Button take = CompactButton(L"GET POS", 96);
            take.VerticalAlignment(VerticalAlignment::Center);
            ToolTipService::SetToolTip(take, box_value(L"Store this axis's current position in the point (also checks the axis)"));
            take.Click([weak, i](auto&&, auto&&) {
                if (auto self = weak.get()) self->OnTakeAxisClick(i);
            });
            line.Children().Append(take);

            row.Child(line);
            AxisRows().Children().Append(row);
        }
    }

    // Khung điểm đầu / cuối: 3 dòng trục (tên, ô toạ độ, đơn vị) — tên trục đổi theo tab ở LoadLayout.
    void TeachingPage::BuildLayoutEditors()
    {
        auto weak = get_weak();
        std::array<Grid, EndCount> hosts{ FirstAxes(), LastAxes() };
        for (int end = 0; end < EndCount; ++end)
        {
            Grid host = hosts[(size_t)end];
            for (int c : { 36, 120, 40 })
            {
                ColumnDefinition col;
                col.Width(GridLengthHelper::FromPixels(c));
                host.ColumnDefinitions().Append(col);
            }
            for (int i = 0; i < MaxEndAxes; ++i)
            {
                RowDefinition rowDef;
                rowDef.Height(GridLengthHelper::Auto());
                host.RowDefinitions().Append(rowDef);

                EndAxisRow& r = m_ends[(size_t)end][(size_t)i];
                r.Label = TextBlock();
                r.Label.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                r.Label.VerticalAlignment(VerticalAlignment::Center);
                r.Box = TextBox();
                r.Box.TextAlignment(TextAlignment::Right);
                r.Box.VerticalAlignment(VerticalAlignment::Center);
                r.Box.LostFocus([weak, end, i](auto&&, auto&&) {
                    if (auto self = weak.get()) self->CommitEndValue(end, i);
                });
                r.Unit = TextBlock();
                r.Unit.FontSize(11);
                r.Unit.VerticalAlignment(VerticalAlignment::Center);
                r.Unit.Foreground(ui::Res(L"TextSecondaryBrush"));
                int col = 0;
                for (FrameworkElement e : { r.Label.as<FrameworkElement>(), r.Box.as<FrameworkElement>(), r.Unit.as<FrameworkElement>() })
                {
                    Grid::SetRow(e, i);
                    Grid::SetColumn(e, col++);
                    host.Children().Append(e);
                }
            }
        }
    }

    // ───────────────────────── nhịp cập nhật ─────────────────────────

    void TeachingPage::OnTick()
    {
        if (!m_loaded) return;

        // Model được thay ở nơi khác (nạp từ trang khác…): danh sách điểm đang giữ là của đối tượng cũ.
        if (scs::McDataManager::Model() != m_model) Guarded("Reload point list", [&] { ReloadPoints(); });

        ui::SetText(CurrentModelText(), m_model ? m_model->Name : std::string());

        // Mức jog dùng chung với trang Manual / Vision Edit — có thể vừa đổi ở đó.
        auto func = scs::McControl::Func();
        bool fast = func && func->JogFast();
        if (m_jogFastShown != (fast ? 1 : 0))
        {
            m_jogFastShown = fast ? 1 : 0;
            ui::SetTint(JogSlowButton(), fast ? ui::Tint::None : ui::Tint::Accent);
            ui::SetTint(JogFastButton(), fast ? ui::Tint::Accent : ui::Tint::None);
            JogSlowButton().FontWeight(fast ? Windows::UI::Text::FontWeights::Normal() : Windows::UI::Text::FontWeights::SemiBold());
            JogFastButton().FontWeight(fast ? Windows::UI::Text::FontWeights::SemiBold() : Windows::UI::Text::FontWeights::Normal());
        }

        for (JogRow& jog : m_jogRows)
        {
            hstring pos = ui::Num(jog.Axis->CurrentPos(), 3);
            if (pos != jog.PosShown) { jog.PosShown = pos; jog.PosRun.Text(pos); }
            auto cfg = jog.Axis->Config();
            hstring unit = ui::H(" " + (cfg ? cfg->Unit : std::string()));
            if (unit != jog.UnitShown) { jog.UnitShown = unit; jog.UnitRun.Text(unit); }
            int org = jog.Axis->OrgOk() ? 1 : 0;
            if (org != jog.OrgShown) { jog.OrgShown = org; jog.OrgLamp.Fill(ui::StatusBrush(org != 0)); }
        }

        // Tên + tóm tắt từng điểm (tên vừa sửa, trục vừa bật/tắt, vừa teach).
        for (PointRow& row : m_points)
        {
            ui::SetText(row.NameText, row.Point->Name);
            ui::SetText(row.SummaryText, SummaryOf(*row.Point));
        }

        bool hasPoint = SelectedRow() != nullptr;
        SetEnabled(DeletePointButton(), hasPoint);
        SetEnabled(TeachButton(), hasPoint);
        SetEnabled(MoveButton(), hasPoint && !m_busy);
        SetEnabled(MoveSlowButton(), hasPoint && !m_busy);

        std::string message = scs::McControl::Status().Message();
        ui::Show(MessageBar(), !::base::IsBlank(message));
        ui::SetText(MessageText(), message);
    }

    void TeachingPage::OnShown(bool visible)
    {
        if (!m_loaded) return;
        if (visible)
        {
            Guarded("Open Teaching page", [&] {
                RefreshModelList();
                if (scs::McDataManager::Model() != m_model) ReloadPoints();
                else { LoadEditor(); LoadLayout(); }
            });
        }
        else
        {
            CommitEditor();   // rời trang giữa lúc đang sửa một ô
            CommitLayout();
            CommitAutoParams();
            CommitDescription();
        }
    }

    // ───────────────────────── model ─────────────────────────

    void TeachingPage::RefreshModelList()
    {
        std::vector<std::string> names = scs::McDataManager::ListModels();
        if (names != m_modelNames)
        {
            m_modelNames = names;
            m_suppressModelSelection = true;
            ModelList().Items().Clear();
            for (const std::string& n : m_modelNames)
            {
                ListViewItem item;
                item.Content(box_value(ui::H(n)));
                item.MinHeight(32);
                ModelList().Items().Append(item);
            }
            m_suppressModelSelection = false;
        }
        if (m_selectedModelName.empty() ||
            std::find(m_modelNames.begin(), m_modelNames.end(), m_selectedModelName) == m_modelNames.end())
            m_selectedModelName = m_modelNames.empty() ? std::string() : m_modelNames.front();
        SelectModelInList();
    }

    void TeachingPage::SelectModelInList()
    {
        auto it = std::find(m_modelNames.begin(), m_modelNames.end(), m_selectedModelName);
        int index = it == m_modelNames.end() ? -1 : (int)(it - m_modelNames.begin());
        if (ModelList().SelectedIndex() != index)
        {
            m_suppressModelSelection = true;
            ModelList().SelectedIndex(index);
            m_suppressModelSelection = false;
        }
        if (index >= 0) ModelList().ScrollIntoView(ModelList().Items().GetAt((uint32_t)index));
    }

    void TeachingPage::OnModelSelectionChanged(WF::IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_suppressModelSelection) return;
        int index = ModelList().SelectedIndex();
        m_selectedModelName = (index >= 0 && index < (int)m_modelNames.size()) ? m_modelNames[(size_t)index] : std::string();
    }

    void TeachingPage::OnLoadModelClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        CommitEditor();
        CommitLayout();
        Guarded("Load model", [&] {
            std::string name = m_selectedModelName;
            if (::base::IsBlank(name)) return;

            auto state = scs::McControl::Status().State();
            if (state == scs::EMcState::Running || state == scs::EMcState::Held || state == scs::EMcState::Homing)
            {
                ui::Dialogs::Warn("Cannot load a model while the machine is running or homing.");
                return;
            }

            if (scs::McDataManager::LoadModel(name))
            {
                scs::McControl::Status().SetModelName(scs::McDataManager::Model()->Name);
                ReloadPoints();
            }
            else
            {
                ui::Dialogs::Error("Load model '" + name + "' failed — see log.");
            }
        });
    }

    void TeachingPage::OnSaveModelClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        CommitEditor();
        CommitLayout();
        CommitAutoParams();
        CommitDescription();
        Guarded("Save model", [&] {
            auto model = scs::McDataManager::Model();

            // Nhóm trùng tên (không phân biệt hoa thường) xuất hiện sớm nhất — GroupBy(...).FirstOrDefault(Count > 1).
            const auto& list = model->TeachPositions;
            for (size_t i = 0; i < list.size(); ++i)
            {
                if (!list[i]) continue;
                for (size_t j = i + 1; j < list.size(); ++j)
                {
                    if (list[j] && ::base::EqualsIgnoreCase(list[i]->Name, list[j]->Name))
                    {
                        ui::Dialogs::Warn("Two points share the name '" + list[i]->Name + "' — rename before saving.");
                        return;
                    }
                }
            }

            if (scs::McDataManager::SaveModel())
            {
                RefreshModelList();
                m_selectedModelName = scs::McDataManager::Model()->Name;
                SelectModelInList();
                scs::McControl::Status().SetMessage("Model '" + scs::McDataManager::Model()->Name + "' saved");
            }
            else
            {
                ui::Dialogs::Error("Save model failed — see log.");
            }
        });
    }

    winrt::fire_and_forget TeachingPage::OnDeleteModelClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        auto self = get_strong();
        CommitEditor();
        std::string name = m_selectedModelName;
        if (::base::IsBlank(name)) co_return;

        if (::base::EqualsIgnoreCase(name, scs::McDataManager::Model()->Name))
        {
            ui::Dialogs::Warn("Cannot delete the model in use — load another model first.");
            co_return;
        }

        if (!co_await ui::Dialogs::ConfirmAsync("Permanently delete model '" + name + "'?")) co_return;

        Guarded("Delete model", [&] {
            if (scs::McDataManager::DeleteModel(name)) RefreshModelList();
            else ui::Dialogs::Error("Delete model failed — see log.");
        });
    }

    void TeachingPage::OnNewModelClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        CommitEditor();
        Guarded("New model", [&] {
            std::string name = ::base::Trim(ui::U8(NewModelNameBox().Text()));
            if (!ValidModelName(name)) return;

            if (ContainsName(m_modelNames, name))
            {
                ui::Dialogs::Warn("Model '" + name + "' already exists.");
                return;
            }

            auto model = scs::ModelData::CreateDefault(name);
            scs::McDataManager::UseModel(model, true);
            scs::McDataManager::SaveModel(name);
            scs::McControl::Status().SetModelName(name);

            NewModelNameBox().Text(L"");
            RefreshModelList();
            m_selectedModelName = name;
            SelectModelInList();
            ReloadPoints();
        });
    }

    void TeachingPage::OnCopyModelClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        CommitEditor();
        Guarded("Copy model", [&] {
            std::string source = m_selectedModelName;
            std::string target = ::base::Trim(ui::U8(NewModelNameBox().Text()));

            if (::base::IsBlank(source)) { ui::Dialogs::Warn("Select a source model first."); return; }
            if (!ValidModelName(target)) return;
            if (ContainsName(m_modelNames, target)) { ui::Dialogs::Warn("Model '" + target + "' already exists."); return; }

            if (scs::McDataManager::CopyModel(source, target))
            {
                NewModelNameBox().Text(L"");
                RefreshModelList();
                m_selectedModelName = target;
                SelectModelInList();
            }
            else
            {
                ui::Dialogs::Error("Copy model failed — see log.");
            }
        });
    }

    void TeachingPage::CommitDescription()
    {
        if (!m_model) return;
        hstring text = DescriptionBox().Text();
        if (text == m_descriptionShown) return;
        m_model->Description = ::base::Trim(ui::U8(text));
        m_descriptionShown = ui::H(m_model->Description);
        if (m_descriptionShown != text) DescriptionBox().Text(m_descriptionShown);
    }

    void TeachingPage::OnDescriptionLostFocus(WF::IInspectable const&, RoutedEventArgs const&) { CommitDescription(); }

    // ───────────────────────── jog ─────────────────────────

    void TeachingPage::OnJogSlowClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto func = scs::McControl::Func()) func->SetJogFast(false);
        OnTick();
    }

    void TeachingPage::OnJogFastClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (auto func = scs::McControl::Func()) func->SetJogFast(true);
        OnTick();
    }

    // "STOP ALL" (khu Jog) và "STOP" (khung chi tiết) — cùng hàng đợi với lệnh jog: không lệnh jog nào đang
    // xếp hàng lại chạy SAU lệnh dừng. Không khoá theo Busy (dừng được cả lúc đang MOVE TO).
    void TeachingPage::OnStopAllClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        ui::PostMotionCommand([] { scs::McControl::Motion().StopAll(); });
    }

    // ───────────────────────── tham số chu trình ─────────────────────────

    void TeachingPage::LoadAutoParams()
    {
        auto model = scs::McDataManager::Model();
        if (!model) return;
        const auto& a = model->Auto;
        AutoTrayRowsBox().Text(winrt::to_hstring(a.TrayRows));
        AutoSlowLengthBox().Text(ui::Num(a.PickSlowLength, 3));
        AutoVacuumOnBox().Text(winrt::to_hstring(a.VacuumOnMs));
        AutoVacuumOffBox().Text(winrt::to_hstring(a.VacuumOffMs));
        AutoMarkTimeBox().Text(winrt::to_hstring(a.MarkTimeMs));
        AutoVisionCheck().IsChecked(a.UseVision);
        AutoLaserCheck().IsChecked(a.UseLaser);
    }

    void TeachingPage::CommitAutoParams()
    {
        auto model = scs::McDataManager::Model();
        if (!model || !m_loaded) return;
        auto& a = model->Auto;
        const int rowsBefore = a.TrayRows;
        double v = 0;
        auto num = [&](TextBox const& box, double lo, double hi, double& field) {
            if (ui::ParseDouble(box.Text(), v) && v >= lo && v <= hi) field = v;
        };
        auto integer = [&](TextBox const& box, int lo, int hi, int& field) {
            if (ui::ParseDouble(box.Text(), v) && v >= lo && v <= hi && v == std::floor(v)) field = (int)v;
        };
        integer(AutoTrayRowsBox(), 1, 50, a.TrayRows);
        num(AutoSlowLengthBox(), 0, 50, a.PickSlowLength);
        integer(AutoVacuumOnBox(), 0, 5000, a.VacuumOnMs);
        integer(AutoVacuumOffBox(), 0, 5000, a.VacuumOffMs);
        integer(AutoMarkTimeBox(), 0, 60000, a.MarkTimeMs);
        LoadAutoParams();   // ô sai hiện lại số đang dùng
        if (a.TrayRows != rowsBefore && m_tab == TabPick) LoadLayout();   // tiêu đề "Last row (row N)" và bảng đổi theo
    }

    void TeachingPage::OnAutoParamLostFocus(WF::IInspectable const&, RoutedEventArgs const&) { CommitAutoParams(); }

    void TeachingPage::OnAutoFlagClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        auto model = scs::McDataManager::Model();
        if (!model) return;
        auto on = [](CheckBox const& c) { auto v = c.IsChecked(); return v && v.Value(); };
        model->Auto.UseVision = on(AutoVisionCheck());
        model->Auto.UseLaser = on(AutoLaserCheck());
    }

    // ───────────────────────── tab Others: điểm có tên ─────────────────────────

    TeachingPage::PointRow* TeachingPage::SelectedRow()
    {
        if (m_selected < 0 || m_selected >= (int)m_points.size()) return nullptr;
        return &m_points[(size_t)m_selected];
    }

    void TeachingPage::ReloadPoints()
    {
        m_model = scs::McDataManager::Model();
        m_points.clear();
        if (m_model)
        {
            m_model->EnsureAutoPoints();
            for (const auto& p : m_model->TeachPositions)
            {
                if (!p || scs::AutoPoints::IsLayoutPoint(p->Name)) continue;   // teach ở các tab Pick / CAM / Laser
                PointRow row;
                row.Point = p;
                for (size_t i = 0; i < AxisCount; ++i) row.Values[i] = p->Get(scs::AxisId::All[i]);
                m_points.push_back(std::move(row));
            }
        }
        m_selected = m_points.empty() ? -1 : 0;
        m_descriptionShown = ui::H(m_model ? m_model->Description : std::string());
        DescriptionBox().Text(m_descriptionShown);
        RebuildPointList();
        LoadEditor();
        LoadAutoParams();
        LoadLayout();
    }

    // Danh sách đổi THÀNH PHẦN (nạp model, thêm/xoá điểm): dựng lại toàn bộ. Tên/tóm tắt cập nhật tại chỗ ở OnTick.
    void TeachingPage::RebuildPointList()
    {
        m_suppressPointSelection = true;
        PointList().Items().Clear();
        ui::Show(PointListEmptyText(), m_points.empty());
        for (PointRow& row : m_points)
        {
            StackPanel content;
            content.Margin(ThicknessHelper::FromLengths(0, 2, 0, 2));
            row.NameText = TextBlock();
            row.NameText.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            row.NameText.FontSize(13);
            row.NameText.Text(ui::H(row.Point->Name));
            row.SummaryText = TextBlock();
            row.SummaryText.FontSize(11);
            row.SummaryText.Foreground(ui::Res(L"TextSecondaryBrush"));
            row.SummaryText.Text(ui::H(SummaryOf(*row.Point)));
            content.Children().Append(row.NameText);
            content.Children().Append(row.SummaryText);

            row.Item = ListViewItem();
            row.Item.Content(content);
            PointList().Items().Append(row.Item);
        }
        m_suppressPointSelection = false;
        SelectPointInList();
    }

    void TeachingPage::SelectPointInList()
    {
        if (PointList().SelectedIndex() != m_selected)
        {
            m_suppressPointSelection = true;
            PointList().SelectedIndex(m_selected);
            m_suppressPointSelection = false;
        }
        if (auto row = SelectedRow()) PointList().ScrollIntoView(row->Item);
    }

    void TeachingPage::OnPointSelectionChanged(WF::IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_suppressPointSelection) return;
        int index = PointList().SelectedIndex();
        if (index == m_selected) return;
        CommitEditor();   // ô đang sửa thuộc điểm cũ
        m_selected = (index >= 0 && index < (int)m_points.size()) ? index : -1;
        LoadEditor();
    }

    // Nạp khung chi tiết từ điểm đang chọn — CHỈ ở đây (đổi điểm, nạp danh sách, mở trang), không ở OnTick.
    void TeachingPage::LoadEditor()
    {
        PointRow* row = SelectedRow();
        ui::Show(Editor(), row != nullptr);
        if (!row) return;

        m_nameShown = ui::H(row->Point->Name);
        PointNameBox().Text(m_nameShown);
        m_noteShown = ui::H(row->Point->Note);
        PointNoteBox().Text(m_noteShown);
        for (size_t i = 0; i < AxisCount; ++i) ShowAxisRow(*row, i);
    }

    void TeachingPage::ShowAxisRow(PointRow const& row, size_t i)
    {
        const char* axisName = scs::AxisId::All[i];
        AxisEditRow& r = m_axisRows[i];
        bool use = row.Point->Has(axisName);
        r.Use.IsChecked(use);
        r.Shown = ui::Num(row.Values[i], 3);
        r.Value.Text(r.Shown);
        SetEnabled(r.Value, use);

        auto axis = scs::McControl::Motion().Axis(axisName);
        auto cfg = axis ? axis->Config() : nullptr;
        ui::SetText(r.Unit, cfg ? cfg->Unit : std::string());
    }

    // Ghi các ô đang sửa vào điểm — tương đương binding LostFocus của bản WPF. Gọi ở đầu mọi lệnh và trước khi
    // đổi điểm, vì thứ tự LostFocus / Click không được đảm bảo ở mọi đường thao tác.
    void TeachingPage::CommitEditor()
    {
        if (!SelectedRow()) return;
        CommitName();
        CommitNote();
        for (size_t i = 0; i < AxisCount; ++i) CommitAxisValue(i);
    }

    void TeachingPage::CommitName()
    {
        PointRow* row = SelectedRow();
        if (!row) return;
        hstring text = PointNameBox().Text();
        if (text == m_nameShown) return;
        std::string name = ::base::Trim(ui::U8(text));   // TeachPointVm.Name: (value ?? "").Trim()
        if (scs::AutoPoints::IsLayoutPoint(name))
        {
            ui::Dialogs::Warn("'" + name + "' is a cycle point of the Pick / CAM / Laser tabs — choose another name.");
            PointNameBox().Text(m_nameShown);
            return;
        }
        row->Point->Name = name;
        m_nameShown = ui::H(row->Point->Name);
        if (m_nameShown != text) PointNameBox().Text(m_nameShown);
    }

    void TeachingPage::CommitNote()
    {
        PointRow* row = SelectedRow();
        if (!row) return;
        hstring text = PointNoteBox().Text();
        if (text == m_noteShown) return;
        row->Point->Note = ui::U8(text);
        m_noteShown = text;
    }

    void TeachingPage::CommitAxisValue(size_t i)
    {
        PointRow* row = SelectedRow();
        if (!row || i >= AxisCount) return;
        AxisEditRow& r = m_axisRows[i];
        hstring text = r.Value.Text();
        if (text == r.Shown) return;   // người vận hành chưa sửa — không ghi số làm tròn 3 chữ số đè số thật

        const char* axisName = scs::AxisId::All[i];
        double value = 0;
        if (!ui::ParseDouble(text, value))
        {
            r.Value.Text(r.Shown);
            ui::Dialogs::Warn(std::string("Invalid ") + axisName + " axis value: '" + ui::U8(text) + "'.");
            return;
        }
        row->Values[i] = value;
        if (row->Point->Has(axisName)) row->Point->Set(axisName, value);   // TeachAxisValueVm.Value: if (Use) Pos[axis] = value
        r.Shown = ui::Num(value, 3);
        r.Value.Text(r.Shown);
    }

    void TeachingPage::OnPointNameLostFocus(WF::IInspectable const&, RoutedEventArgs const&) { CommitName(); }

    void TeachingPage::OnPointNoteLostFocus(WF::IInspectable const&, RoutedEventArgs const&) { CommitNote(); }

    // TeachAxisValueVm.Use: bật = ghi toạ độ đang hiện vào điểm; tắt = xoá trục khỏi điểm ("MOVE TO" không đụng trục đó).
    void TeachingPage::OnAxisUseClick(size_t i)
    {
        PointRow* row = SelectedRow();
        if (!row || i >= AxisCount) return;
        CommitAxisValue(i);

        const char* axisName = scs::AxisId::All[i];
        auto checkedRef = m_axisRows[i].Use.IsChecked();
        bool use = checkedRef && checkedRef.Value();
        if (use != row->Point->Has(axisName))
        {
            if (use) row->Point->Set(axisName, row->Values[i]);
            else row->Point->Remove(axisName);
        }
        SetEnabled(m_axisRows[i].Value, row->Point->Has(axisName));
    }

    /// <summary>TeachAxisValueVm.TakeCurrent: lấy vị trí hiện tại của trục vào điểm, bật luôn cờ dùng trục.</summary>
    void TeachingPage::TakeCurrent(PointRow& row, size_t i)
    {
        const char* axisName = scs::AxisId::All[i];
        auto axis = scs::McControl::Motion().Axis(axisName);
        if (!axis) return;
        row.Values[i] = Round4(axis->CurrentPos());
        row.Point->Set(axisName, row.Values[i]);
        ShowAxisRow(row, i);
    }

    // "GET POS" của một trục (TakeAxisCommand): không đòi đã về gốc, như bản WPF.
    void TeachingPage::OnTakeAxisClick(size_t i)
    {
        PointRow* row = SelectedRow();
        if (!row || i >= AxisCount) return;
        CommitEditor();
        TakeCurrent(*row, i);
        row->Point->TaughtAt = std::chrono::system_clock::now();   // MarkTaught
    }

    /// <summary>Teach: lấy vị trí hiện tại của MỌI trục đang được chọn trong điểm. Điểm chưa chọn trục nào thì lấy
    /// cả 3 trục.</summary>
    void TeachingPage::OnTeachClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        PointRow* row = SelectedRow();
        if (!row) return;
        CommitEditor();
        if (!CheckHomedForTeach()) return;

        std::vector<size_t> targets;
        for (size_t i = 0; i < AxisCount; ++i)
            if (row->Point->Has(scs::AxisId::All[i])) targets.push_back(i);
        if (targets.empty())
            for (size_t i = 0; i < AxisCount; ++i) targets.push_back(i);

        std::vector<std::string> names;
        for (size_t i : targets)
        {
            TakeCurrent(*row, i);
            names.push_back(scs::AxisId::All[i]);
        }
        row->Point->TaughtAt = std::chrono::system_clock::now();   // MarkTaught
        scs::McControl::Status().SetMessage("Taught '" + row->Point->Name + "' (" + ::base::Join(names, ", ") +
                                            ") — press SAVE (Model card) to write to file");
    }

    void TeachingPage::OnMoveDoubleTapped(WF::IInspectable const&, Input::DoubleTappedRoutedEventArgs const&) { MoveToSelected(false); }

    void TeachingPage::OnMoveSlowDoubleTapped(WF::IInspectable const&, Input::DoubleTappedRoutedEventArgs const&) { MoveToSelected(true); }

    // Chạy tới điểm đang chọn trên luồng nền (MoveToPosition chặn tới khi tới nơi). Chạy theo BẢN SAO của điểm: sửa
    // ô / xoá điểm trong lúc trục đang chạy không chạm vào dữ liệu luồng nền đang đọc.
    void TeachingPage::MoveToSelected(bool slow)
    {
        PointRow* row = SelectedRow();
        if (!row || m_busy) return;
        CommitEditor();
        MoveToPoint(std::make_shared<scs::TeachPosition>(*row->Point), slow);
    }

    void TeachingPage::MoveToPoint(std::shared_ptr<scs::TeachPosition> snapshot, bool slow)
    {
        if (!snapshot || m_busy) return;
        std::string name = snapshot->Name;
        m_busy = true;
        OnTick();

        ui::RunBackgroundThenUi(
            [snapshot, name, slow] {
                auto func = scs::McControl::Func();
                auto safety = scs::McControl::Safety();
                if (!func || !safety) return;
                try
                {
                    bool ok = func->MoveToPosition(snapshot.get(), safety->AbortToken(), slow);
                    if (ok) scs::McControl::Status().SetMessage("Reached point '" + name + "'");
                }
                catch (const ::base::OperationCanceled&)
                {
                    scs::McControl::Status().SetMessage("Move to point '" + name + "' cancelled");
                }
            },
            [weak = get_weak()] {
                if (auto self = weak.get())
                {
                    self->m_busy = false;
                    self->OnTick();
                }
            });
    }

    void TeachingPage::OnAddPointClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        CommitEditor();
        Guarded("Add point", [&] {
            auto model = scs::McDataManager::Model();
            if (model != m_model) ReloadPoints();

            std::string name = ::base::Trim(ui::U8(NewPointNameBox().Text()));
            if (name.empty()) name = "Point" + std::to_string(m_points.size() + 1);
            if (model->FindPosition(name) || scs::AutoPoints::IsLayoutPoint(name))
            {
                ui::Dialogs::Warn("Point '" + name + "' already exists.");
                return;
            }

            auto p = std::make_shared<scs::TeachPosition>();
            p->Name = name;
            model->TeachPositions.push_back(p);

            PointRow row;
            row.Point = p;
            m_points.push_back(std::move(row));
            m_selected = (int)m_points.size() - 1;
            RebuildPointList();
            LoadEditor();
            NewPointNameBox().Text(L"");
        });
    }

    winrt::fire_and_forget TeachingPage::OnDeletePointClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        auto self = get_strong();
        CommitEditor();
        PointRow* row = SelectedRow();
        if (!row) co_return;
        std::shared_ptr<scs::TeachPosition> point = row->Point;

        if (!co_await ui::Dialogs::ConfirmAsync("Delete point '" + point->Name + "' from the model? (press SAVE (Model card) to write to file)"))
            co_return;

        // Trong lúc hộp thoại mở, danh sách có thể đã được nạp lại — xoá đúng điểm đã hỏi, nếu nó còn.
        auto it = std::find_if(m_points.begin(), m_points.end(), [&](const PointRow& r) { return r.Point == point; });
        if (it == m_points.end()) co_return;

        if (m_model)
        {
            auto& list = m_model->TeachPositions;
            list.erase(std::remove(list.begin(), list.end(), point), list.end());
        }
        m_points.erase(it);
        m_selected = m_points.empty() ? -1 : 0;   // SelectedPoint = Points.FirstOrDefault()
        RebuildPointList();
        LoadEditor();
    }

    // ───────────────────────── tab Pick / CAM / Laser ─────────────────────────

    void TeachingPage::OnTabClick(WF::IInspectable const& sender, RoutedEventArgs const&)
    {
        SelectTab(TagOf(sender, 0));
    }

    void TeachingPage::SelectTab(int tab)
    {
        if (tab < 0 || tab >= TabCount) return;
        CommitLayout();
        CommitEditor();
        m_tab = tab;
        std::array<Button, TabCount> tabs{ TabButton0(), TabButton1(), TabButton2(), TabButton3() };
        for (int i = 0; i < TabCount; ++i) ui::SetTint(tabs[(size_t)i], i == tab ? ui::Tint::Accent : ui::Tint::None);
        ui::Show(LayoutPanel(), tab != TabOther);
        ui::Show(OtherPanel(), tab == TabOther);
        if (tab != TabOther) LoadLayout();
    }

    void TeachingPage::OnSelectorClick(WF::IInspectable const& sender, RoutedEventArgs const&)
    {
        CommitLayout();
        if (m_tab == TabPick) m_tray = std::clamp(TagOf(sender, 1), 1, 2);
        LoadLayout();
    }

    std::vector<std::string> TeachingPage::EndAxes() const
    {
        if (m_tab == TabPick) return { scs::AxisId::X, scs::AxisId::Y, scs::AxisId::Z };
        return { scs::AxisId::X, scs::AxisId::Z };
    }

    std::string TeachingPage::EndName(int end) const
    {
        bool last = end == 1;
        if (m_tab == TabPick) return scs::AutoPoints::PickRow(m_tray, last);
        if (m_tab == TabCam) return scs::AutoPoints::Cam(last);
        return scs::AutoPoints::Laser(last);
    }

    std::shared_ptr<scs::TeachPosition> TeachingPage::EndPoint(int end)
    {
        if (!m_model || m_tab == TabOther) return nullptr;
        std::string name = EndName(end);
        auto p = m_model->FindPosition(name);
        if (!p)
        {
            p = std::make_shared<scs::TeachPosition>();
            p->Name = name;
            m_model->TeachPositions.push_back(p);
        }
        return p;
    }

    int TeachingPage::DerivedCount() const
    {
        if (!m_model) return 0;
        return m_tab == TabPick ? std::max(1, m_model->Auto.TrayRows) : scs::AutoPoints::NozzleCount;
    }

    std::shared_ptr<scs::TeachPosition> TeachingPage::DerivedPoint(int n, std::string& why) const
    {
        if (!m_model) { why = "no model loaded"; return nullptr; }
        if (m_tab == TabPick) return m_model->PickPoint(m_tray, n, why);
        if (m_tab == TabCam) return m_model->CamPoint(n, why);
        return m_model->LaserPoint(n, why);
    }

    // Nạp tab Pick / CAM / Laser — CHỈ khi đổi tab/tray, nạp model, teach (không ở OnTick: ô đang sửa không bị đè).
    void TeachingPage::LoadLayout()
    {
        if (!m_loaded || !m_model || m_tab == TabOther) return;
        const bool pick = m_tab == TabPick, cam = m_tab == TabCam;
        const int rows = std::max(1, m_model->Auto.TrayRows);

        ui::Show(SelectorPanel(), pick);
        ui::SetTint(SelButton1(), m_tray == 1 ? ui::Tint::Accent : ui::Tint::None);
        ui::SetTint(SelButton2(), m_tray == 2 ? ui::Tint::Accent : ui::Tint::None);
        ui::SetText(SelectorHint(), std::format("{} rows per tray — set in Cycle Parameters", rows));
        ui::SetText(LayoutHint(), pick
            ? std::string("Nozzle 1 exactly over column 1 of the row, Z at pick height. Teach Row 1 and the Last row; rows in between are interpolated. One tray row = 7 cans picked at once.")
            : cam ? std::string("Nozzle 1 / nozzle 7 centered in the CAM image (X, Z). N2…N6 are interpolated. Each nozzle is brought here for the LocateCan Flow.")
                  : std::string("Nozzle 1 / nozzle 7 at the laser field center (X, Z). N2…N6 are interpolated; per-nozzle trim and the measured X/Z offset are added at run time (Vision Edit)."));

        ui::SetText(FirstTitle(), pick ? std::string("Row 1") : std::string("Nozzle 1 (N1)"));
        ui::SetText(LastTitle(), pick ? (rows > 1 ? std::format("Last row (row {})", rows) : std::string("Last row (not used: 1 row)"))
                                      : std::string("Nozzle 7 (N7)"));

        std::vector<std::string> axes = EndAxes();
        std::array<TextBlock, EndCount> whens{ FirstWhen(), LastWhen() };
        for (int end = 0; end < EndCount; ++end)
        {
            auto p = EndPoint(end);
            for (int i = 0; i < MaxEndAxes; ++i)
            {
                EndAxisRow& r = m_ends[(size_t)end][(size_t)i];
                bool used = i < (int)axes.size();
                ui::Show(r.Label, used);
                ui::Show(r.Box, used);
                ui::Show(r.Unit, used);
                if (!used) continue;
                const std::string& axisName = axes[(size_t)i];
                ui::SetText(r.Label, axisName);
                auto axis = scs::McControl::Motion().Axis(axisName.c_str());
                auto cfg = axis ? axis->Config() : nullptr;
                ui::SetText(r.Unit, cfg ? cfg->Unit : std::string());
                r.Shown = p && p->Has(axisName) ? ui::Num(p->Get(axisName), 3) : hstring();
                r.Box.Text(r.Shown);
            }
            ui::SetText(whens[(size_t)end], WhenText(p && p->Taught() ? p->TaughtAt : std::optional<scs::TimePoint>{}));
        }
        RebuildDerived();
    }

    // Bảng các điểm chia đều của tab đang mở: số thứ tự, toạ độ từng trục, MOVE TO (bấm đúp, chạy theo bản sao).
    void TeachingPage::RebuildDerived()
    {
        DerivedRows().Children().Clear();
        if (!m_model || m_tab == TabOther) return;
        const bool pick = m_tab == TabPick;
        const int count = DerivedCount();
        ui::SetText(DerivedTitle(), pick ? std::string("Tray rows (interpolated)")
                                         : m_tab == TabCam ? std::string("CAM points per nozzle") : std::string("Laser points per nozzle (before trim)"));
        ui::SetText(DerivedHint(), pick ? std::format("tray {} · {} rows × 7 cans", m_tray, count) : std::format("{} nozzles", count));

        std::vector<std::string> axes = EndAxes();
        auto makeRow = [&](std::vector<hstring> const& cells, bool header) {
            Grid g;
            g.ColumnSpacing(8);
            ColumnDefinition first;
            first.Width(GridLengthHelper::FromPixels(64));
            g.ColumnDefinitions().Append(first);
            for (size_t i = 0; i < axes.size(); ++i)
            {
                ColumnDefinition c;
                c.Width(GridLengthHelper::FromPixels(100));
                g.ColumnDefinitions().Append(c);
            }
            ColumnDefinition rest;
            rest.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            g.ColumnDefinitions().Append(rest);
            for (size_t i = 0; i < cells.size(); ++i)
            {
                TextBlock t;
                t.Text(cells[i]);
                t.VerticalAlignment(VerticalAlignment::Center);
                t.FontSize(header ? 12 : 13);
                if (header) t.Foreground(ui::Res(L"TextSecondaryBrush"));
                else if (i > 0) t.FontFamily(Media::FontFamily(L"Consolas"));
                if (i > 0) t.TextAlignment(TextAlignment::Right);
                Grid::SetColumn(t, (int32_t)i);
                g.Children().Append(t);
            }
            return g;
        };

        std::vector<hstring> header{ ui::H(pick ? "Row" : "Nozzle") };
        for (const auto& a : axes) header.push_back(ui::H(a));
        Border head = DividedRow();
        head.Child(makeRow(header, true));
        DerivedRows().Children().Append(head);

        for (int n = 1; n <= count; ++n)
        {
            if (n > MaxDerivedRows)
            {
                TextBlock more;
                more.FontSize(12);
                more.Foreground(ui::Res(L"TextSecondaryBrush"));
                more.Margin(ThicknessHelper::FromLengths(0, 8, 0, 0));
                more.Text(ui::H(std::format("… {} more rows (table shows first {} only)", count - MaxDerivedRows, MaxDerivedRows)));
                DerivedRows().Children().Append(more);
                return;
            }
            std::string why;
            auto p = DerivedPoint(n, why);
            if (!p)
            {
                TextBlock t;
                t.FontSize(12);
                t.TextWrapping(TextWrapping::Wrap);
                t.Foreground(ui::Res(L"TextSecondaryBrush"));
                t.Margin(ThicknessHelper::FromLengths(0, 8, 0, 0));
                t.Text(ui::H("Cannot interpolate: " + why));
                DerivedRows().Children().Append(t);
                return;
            }
            std::vector<hstring> cells{ ui::H(pick ? std::to_string(n) : std::format("N{}", n)) };
            for (const auto& a : axes) cells.push_back(p->Has(a) ? ui::Num(p->Get(a), 3) : hstring(L"—"));
            Grid g = makeRow(cells, false);

            Button move = CompactButton(L"MOVE TO", 96);
            move.HorizontalAlignment(HorizontalAlignment::Right);
            ToolTipService::SetToolTip(move, box_value(ui::H("Double-click: raise Z to safe, move to " + p->Name + " (low Auto speed), lower Z last")));
            move.DoubleTapped([weak = get_weak(), p](auto&&, auto&&) {
                if (auto self = weak.get()) self->MoveToPoint(std::make_shared<scs::TeachPosition>(*p), true);
            });
            Grid::SetColumn(move, (int32_t)axes.size() + 1);
            g.Children().Append(move);

            Border line = DividedRow();
            line.Child(g);
            DerivedRows().Children().Append(line);
        }
    }

    // Ô toạ độ của điểm đầu/cuối: số gõ tay coi như teach trục đó (đánh dấu đã teach) — chỉ khi chữ đã khác chữ đang hiện.
    void TeachingPage::CommitEndValue(int end, int i)
    {
        if (!m_model || m_tab == TabOther || end < 0 || end >= EndCount || i < 0 || i >= MaxEndAxes) return;
        std::vector<std::string> axes = EndAxes();
        if (i >= (int)axes.size()) return;
        EndAxisRow& r = m_ends[(size_t)end][(size_t)i];
        hstring text = r.Box.Text();
        if (text == r.Shown) return;
        double value = 0;
        if (!ui::ParseDouble(text, value))
        {
            r.Box.Text(r.Shown);
            ui::Dialogs::Warn("Invalid " + axes[(size_t)i] + " axis value: '" + ui::U8(text) + "'.");
            return;
        }
        auto now = std::chrono::system_clock::now();
        auto p = EndPoint(end);
        p->Set(axes[(size_t)i], value);
        p->TaughtAt = now;
        r.Shown = ui::Num(value, 3);
        r.Box.Text(r.Shown);
        std::array<TextBlock, EndCount> whens{ FirstWhen(), LastWhen() };
        ui::SetText(whens[(size_t)end], WhenText(now));
        RebuildDerived();
    }

    void TeachingPage::CommitLayout()
    {
        if (!m_loaded || !m_model || m_tab == TabOther) return;
        for (int end = 0; end < EndCount; ++end)
            for (int i = 0; i < MaxEndAxes; ++i) CommitEndValue(end, i);
    }

    bool TeachingPage::CheckHomedForTeach()
    {
        if (scs::McControl::Motion().AllHomed()) return true;
        ui::Dialogs::Warn("Not homed — current positions are meaningless, teaching now would store garbage. Home first.");
        return false;
    }

    /// <summary>Teach khung điểm Tag (0 đầu: hàng 1 / N1, 1 cuối: hàng cuối / N7): lấy vị trí hiện tại của các trục của tab.</summary>
    void TeachingPage::OnEndTeachClick(WF::IInspectable const& sender, RoutedEventArgs const&)
    {
        int end = std::clamp(TagOf(sender, 0), 0, EndCount - 1);
        CommitLayout();
        if (!m_model || m_tab == TabOther || !CheckHomedForTeach()) return;
        auto p = EndPoint(end);
        std::vector<std::string> axes = EndAxes();
        for (const auto& name : axes)
            if (auto axis = scs::McControl::Motion().Axis(name.c_str())) p->Set(name, Round4(axis->CurrentPos()));
        p->TaughtAt = std::chrono::system_clock::now();
        scs::McControl::Status().SetMessage("Taught '" + p->Name + "' (" + ::base::Join(axes, ", ") + ") — press SAVE (Model card) to write to file");
        LoadLayout();
    }

    void TeachingPage::OnEndMoveDoubleTapped(WF::IInspectable const& sender, Input::DoubleTappedRoutedEventArgs const&)
    {
        int end = std::clamp(TagOf(sender, 0), 0, EndCount - 1);
        CommitLayout();
        if (!m_model || m_tab == TabOther) return;
        auto p = EndPoint(end);
        if (!p || !p->Taught()) { ui::Dialogs::Warn("Point '" + EndName(end) + "' not taught."); return; }
        MoveToPoint(std::make_shared<scs::TeachPosition>(*p), true);
    }
}
