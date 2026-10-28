#include "pch.h"
#include "Ui/AlarmTable.h"

#include "Ui/StatusColors.h"
#include "Ui/UiUtil.h"
#include "Utility/DateTimeText.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace ui
{
    namespace
    {
        TextBlock Cell(std::string_view text, bool wrap = false)
        {
            TextBlock t;
            t.Text(H(text));
            t.FontSize(13);
            t.VerticalAlignment(VerticalAlignment::Center);
            t.Margin(ThicknessHelper::FromLengths(8, 0, 8, 0));
            t.TextTrimming(wrap ? TextTrimming::None : TextTrimming::CharacterEllipsis);
            t.TextWrapping(wrap ? TextWrapping::Wrap : TextWrapping::NoWrap);
            return t;
        }

        long long KeyOf(scs::TimePoint t)
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
        }
    }

    std::vector<AlarmTable::Column> AlarmTable::Columns() const
    {
        if (m_kind == AlarmTableKind::Active)
            return { { L"Time", 76 }, { L"Code", 60 }, { L"Level", 76 }, { L"Source", 76 }, { L"Root", 52 },
                     { L"Cause active", 124 }, { L"Message", 0 }, { L"Details", 200 } };
        return { { L"Raised", 116 }, { L"Cleared", 116 }, { L"Code", 60 }, { L"Level", 76 }, { L"Source", 76 },
                 { L"Root", 52 }, { L"Message", 0 }, { L"Details", 200 } };
    }

    Grid AlarmTable::MakeRowGrid() const
    {
        Grid grid;
        for (const Column& c : Columns())
        {
            ColumnDefinition def;
            def.Width(c.Width > 0 ? GridLengthHelper::FromPixels(c.Width) : GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            grid.ColumnDefinitions().Append(def);
        }
        return grid;
    }

    FrameworkElement AlarmTable::Build(AlarmTableKind kind)
    {
        m_kind = kind;

        Grid root;
        RowDefinition headerRow; headerRow.Height(GridLengthHelper::Auto());
        RowDefinition dividerRow; dividerRow.Height(GridLengthHelper::Auto());
        RowDefinition bodyRow; bodyRow.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        root.RowDefinitions().Append(headerRow);
        root.RowDefinitions().Append(dividerRow);
        root.RowDefinitions().Append(bodyRow);

        Grid header = MakeRowGrid();
        header.MinHeight(34);
        int column = 0;
        for (const Column& c : Columns())
        {
            TextBlock t = Cell("");
            t.Text(c.Header);
            t.FontSize(12);
            t.Foreground(Res(L"TextSecondaryBrush"));
            Grid::SetColumn(t, column++);
            header.Children().Append(t);
        }

        Border divider;
        divider.Style(ResStyle(L"DividerStyle"));

        m_rows = StackPanel();
        ScrollViewer scroll;
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroll.Content(m_rows);

        Grid::SetRow(header, 0);
        Grid::SetRow(divider, 1);
        Grid::SetRow(scroll, 2);
        root.Children().Append(header);
        root.Children().Append(divider);
        root.Children().Append(scroll);
        return root;
    }

    void AlarmTable::Update(const std::vector<scs::AlarmData>& alarms)
    {
        if (!m_rows) return;
        m_rows.Children().Clear();

        // Bỏ dấu "đang mở" của những dòng không còn trong danh sách.
        std::set<std::pair<int, long long>> present;
        for (const auto& a : alarms) present.insert({ a.Code, KeyOf(a.RaisedAt) });
        for (auto it = m_expanded.begin(); it != m_expanded.end();)
            it = present.count(*it) ? std::next(it) : m_expanded.erase(it);

        for (const scs::AlarmData& a : alarms)
        {
            auto key = std::make_pair(a.Code, KeyOf(a.RaisedAt));

            StackPanel item;
            item.BorderBrush(Res(L"DividerBrush"));
            item.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
            item.Background(Solid(0x00FFFFFF));   // trong suốt nhưng vẫn bắt được chuột trên cả dòng

            Grid row = MakeRowGrid();
            row.MinHeight(34);
            int column = 0;
            auto add = [&](FrameworkElement const& e) {
                Grid::SetColumn(e, column++);
                row.Children().Append(e);
            };

            Border level;
            level.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
            level.Padding(ThicknessHelper::FromLengths(6, 1, 6, 1));
            level.Margin(ThicknessHelper::FromLengths(8, 0, 8, 0));
            level.HorizontalAlignment(HorizontalAlignment::Left);
            level.VerticalAlignment(VerticalAlignment::Center);
            level.Background(Solid(AlarmLevelColor(a.Level)));
            TextBlock levelText;
            levelText.Text(H(scs::ToString(a.Level)));
            levelText.Foreground(Solid(0xFFFFFFFF));
            levelText.FontSize(11);
            level.Child(levelText);

            if (m_kind == AlarmTableKind::Active)
            {
                add(Cell(scs::FormatLocal(a.RaisedAt, "HH:mm:ss")));
                add(Cell(std::to_string(a.Code)));
                add(level);
                add(Cell(a.Source));
                add(Cell(a.IsRootCause ? "True" : "False"));
                add(Cell(a.ConditionActive ? "True" : "False"));
                add(Cell(a.Message(), true));
                add(Cell(a.Detail, true));
            }
            else
            {
                add(Cell(scs::FormatLocal(a.RaisedAt, "HH:mm:ss dd/MM")));
                add(Cell(a.ClearedAt.has_value() ? scs::FormatLocal(*a.ClearedAt, "HH:mm:ss dd/MM") : std::string()));
                add(Cell(std::to_string(a.Code)));
                add(level);
                add(Cell(a.Source));
                add(Cell(a.IsRootCause ? "True" : "False"));
                add(Cell(a.Message(), true));
                add(Cell(a.Detail, true));
            }

            StackPanel details;
            details.Margin(ThicknessHelper::FromLengths(20, 4, 8, 10));
            TextBlock cause = Cell("Cause: " + a.CauseText(), true);
            cause.Margin(ThicknessHelper::FromUniformLength(0));
            cause.Foreground(Res(L"TextSecondaryBrush"));
            TextBlock remedy = Cell("Remedy: " + a.RemedyText(), true);
            remedy.Margin(ThicknessHelper::FromLengths(0, 3, 0, 0));
            details.Children().Append(cause);
            details.Children().Append(remedy);
            details.Visibility(m_expanded.count(key) ? Visibility::Visible : Visibility::Collapsed);

            item.Children().Append(row);
            item.Children().Append(details);

            item.Tapped([this, key, details](auto&&, auto&&) {
                bool open = details.Visibility() != Visibility::Visible;
                details.Visibility(open ? Visibility::Visible : Visibility::Collapsed);
                if (open) m_expanded.insert(key);
                else m_expanded.erase(key);
            });

            m_rows.Children().Append(item);
        }
    }
}
