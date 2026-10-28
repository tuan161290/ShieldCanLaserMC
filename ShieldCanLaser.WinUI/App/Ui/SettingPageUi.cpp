#include "pch.h"
#include "Ui/SettingPageUi.h"

#include <shobjidl.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>

#include "Base/Text.h"
#include "Ui/UiUtil.h"
#include "Ui/VmPane.h"
#include "Utility/DateTimeText.h"
#include "Utility/Log/LogWorker.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
namespace WF = winrt::Windows::Foundation;

namespace ui::settingpage
{
    namespace
    {
        FrameworkElement FindNamed(DependencyObject const& parent, std::wstring_view name, int depth)
        {
            int count = VisualTreeHelper::GetChildrenCount(parent);
            for (int i = 0; i < count; ++i)
            {
                auto child = VisualTreeHelper::GetChild(parent, i);
                if (auto fe = child.try_as<FrameworkElement>(); fe && fe.Name() == name) return fe;
                if (depth > 0)
                    if (auto found = FindNamed(child, name, depth - 1)) return found;
            }
            return nullptr;
        }

        bool ParseIntText(const std::string& text, int min, int max, int& value, std::string& error)
        {
            int parsed = 0;
            if (!ui::ParseInt(ui::H(text), parsed))
            {
                error = "\"" + text + "\" is not an integer";
                return false;
            }
            if (parsed < min || parsed > max)
            {
                error = std::format("must be {} to {} (now {})", min, max, parsed);
                return false;
            }
            value = parsed;
            return true;
        }

        bool ParseDoubleText(const std::string& text, double& value, std::string& error)
        {
            if (!ui::ParseDouble(ui::H(text), value))
            {
                error = "\"" + text + "\" is not a number";
                return false;
            }
            return true;
        }

        // Hộp thoại Win32 — dùng khi FileOpenPicker không chạy được (tiến trình chạy quyền admin…).
        hstring PickWithShellDialog(hstring const& currentPath)
        {
            com_ptr<IFileOpenDialog> dialog;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
                return {};

            COMDLG_FILTERSPEC types[] = {
                { L"VisionMaster solution (*.solw)", L"*.solw" },
                { L"All files", L"*.*" },
            };
            dialog->SetFileTypes(ARRAYSIZE(types), types);
            dialog->SetFileTypeIndex(1);
            DWORD options = 0;
            if (SUCCEEDED(dialog->GetOptions(&options)))
                dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);

            // Mở ở thư mục của file đang cấu hình (nếu còn tồn tại).
            if (!currentPath.empty())
            {
                std::error_code ec;
                std::filesystem::path folder = std::filesystem::path(std::wstring(currentPath)).parent_path();
                if (!folder.empty() && std::filesystem::is_directory(folder, ec))
                {
                    com_ptr<IShellItem> item;
                    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(item.put()))))
                        dialog->SetFolder(item.get());
                }
            }

            if (FAILED(dialog->Show(ui::MainHwnd()))) return {};   // huỷ = HRESULT_FROM_WIN32(ERROR_CANCELLED)
            com_ptr<IShellItem> result;
            if (FAILED(dialog->GetResult(result.put()))) return {};
            PWSTR path = nullptr;
            if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) || path == nullptr) return {};
            hstring text{ path };
            CoTaskMemFree(path);
            return text;
        }
    }

    std::string FormatDouble(double value)
    {
        return std::format("{}", value);
    }

    // ══════════ FieldGroup ══════════

    FieldGroup::FieldGroup() : m_loading(std::make_shared<bool>(false)) {}

    void FieldGroup::BindText(TextBox const& box, std::string label, Getter get, Setter set)
    {
        auto field = std::make_shared<TextField>();
        field->Box = box;
        field->Label = std::move(label);
        field->Get = std::move(get);
        field->Set = std::move(set);
        field->ToolTip = ToolTipService::GetToolTip(box);
        m_texts.push_back(field);

        // weak_ptr: handler nằm trên chính ô nhập, giữ shared_ptr thì thành vòng tham chiếu ô ↔ field.
        std::weak_ptr<TextField> weak = field;
        box.LostFocus([weak](WF::IInspectable const&, RoutedEventArgs const&) {
            if (auto f = weak.lock()) Commit(*f, nullptr);
        });
        box.KeyDown([weak](WF::IInspectable const&, Input::KeyRoutedEventArgs const& e) {
            if (e.Key() != winrt::Windows::System::VirtualKey::Enter) return;
            if (auto f = weak.lock()) Commit(*f, nullptr);
        });
    }

    void FieldGroup::BindInt(TextBox const& box, std::string label, std::function<int()> get, std::function<void(int)> set,
                             int min, int max)
    {
        BindText(box, std::move(label),
            [get] { return std::to_string(get()); },
            [set, min, max](const std::string& text, std::string& error) {
                int value = 0;
                if (!ParseIntText(text, min, max, value, error)) return false;
                set(value);
                return true;
            });
    }

    void FieldGroup::BindDouble(TextBox const& box, std::string label, std::function<double()> get, std::function<void(double)> set)
    {
        BindText(box, std::move(label),
            [get] { return FormatDouble(get()); },
            [set](const std::string& text, std::string& error) {
                double value = 0;
                if (!ParseDoubleText(text, value, error)) return false;
                set(value);
                return true;
            });
    }

    void FieldGroup::BindOptionalDouble(TextBox const& box, std::string label, std::function<std::optional<double>()> get,
                                        std::function<void(std::optional<double>)> set)
    {
        BindText(box, std::move(label),
            [get] {
                auto v = get();
                return v.has_value() ? FormatDouble(*v) : std::string();
            },
            [set](const std::string& text, std::string& error) {
                if (::base::IsBlank(text))
                {
                    set(std::nullopt);
                    return true;
                }
                double value = 0;
                if (!ParseDoubleText(text, value, error)) return false;
                set(value);
                return true;
            });
    }

    void FieldGroup::BindString(TextBox const& box, std::string label, std::function<std::string()> get,
                                std::function<void(std::string)> set)
    {
        BindText(box, std::move(label), std::move(get),
            [set](const std::string& text, std::string&) {
                set(text);
                return true;
            });
    }

    void FieldGroup::BindCheck(CheckBox const& box, std::function<bool()> get, std::function<void(bool)> set)
    {
        m_checks.push_back({ box, std::move(get) });
        // Click chỉ bắn khi người vận hành bấm — đặt IsChecked từ code (LoadAll) không ghi ngược lại.
        box.Click([set = std::move(set)](WF::IInspectable const& sender, RoutedEventArgs const&) {
            auto value = sender.as<CheckBox>().IsChecked();
            set(value && value.Value());
        });
    }

    void FieldGroup::BindChoice(ComboBox const& box, std::vector<std::string> values, std::function<std::string()> get,
                                std::function<void(std::string)> set)
    {
        auto list = std::make_shared<std::vector<std::string>>(std::move(values));
        for (const auto& v : *list) box.Items().Append(box_value(ui::H(v)));
        m_choices.push_back({ box, list, std::move(get) });

        box.SelectionChanged([list, loading = m_loading, set = std::move(set)](WF::IInspectable const& sender,
                                                                                 SelectionChangedEventArgs const&) {
            if (*loading) return;
            int index = sender.as<ComboBox>().SelectedIndex();
            if (index >= 0 && index < (int)list->size()) set((*list)[index]);
        });
    }

    void FieldGroup::LoadAll(bool force)
    {
        *m_loading = true;
        for (auto& f : m_texts)
        {
            if (!force && f->Box.Text() != f->Loaded) continue;   // đang sửa dở / chữ sai chưa ghi được
            f->Loaded = ui::H(f->Get());
            if (f->Box.Text() != f->Loaded) f->Box.Text(f->Loaded);
            if (f->Invalid)
            {
                f->Invalid = false;
                f->Box.ClearValue(Control::BorderBrushProperty());
                ToolTipService::SetToolTip(f->Box, f->ToolTip);
            }
        }
        for (auto& c : m_checks)
        {
            bool value = c.Get();
            auto current = c.Box.IsChecked();
            if (!current || current.Value() != value) c.Box.IsChecked(value);
        }
        for (auto& c : m_choices)
        {
            std::string value = c.Get();
            auto it = std::find(c.Values->begin(), c.Values->end(), value);
            int index = (int)(it - c.Values->begin());
            if (it == c.Values->end())
            {
                c.Values->push_back(value);
                c.Box.Items().Append(box_value(ui::H(value)));
            }
            if (c.Box.SelectedIndex() != index) c.Box.SelectedIndex(index);
        }
        *m_loading = false;
    }

    void FieldGroup::ReloadField(TextBox const& box)
    {
        for (auto& f : m_texts)
        {
            if (f->Box != box) continue;
            f->Loaded = ui::H(f->Get());
            if (f->Box.Text() != f->Loaded) f->Box.Text(f->Loaded);
            if (f->Invalid)
            {
                f->Invalid = false;
                f->Box.ClearValue(Control::BorderBrushProperty());
                ToolTipService::SetToolTip(f->Box, f->ToolTip);
            }
        }
    }

    std::vector<std::string> FieldGroup::CommitAll()
    {
        std::vector<std::string> errors;
        for (auto& f : m_texts)
        {
            std::string line;
            if (!Commit(*f, &line)) errors.push_back(line);
        }
        return errors;
    }

    bool FieldGroup::Commit(TextField& f, std::string* errorLine)
    {
        hstring text = f.Box.Text();
        if (text == f.Loaded)
        {
            if (f.Invalid)
            {
                f.Invalid = false;
                f.Box.ClearValue(Control::BorderBrushProperty());
                ToolTipService::SetToolTip(f.Box, f.ToolTip);
            }
            return true;
        }

        std::string error;
        bool ok = false;
        try
        {
            ok = f.Set(ui::U8(text), error);
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }
        if (!ok)
        {
            SetInvalid(f, error);
            if (errorLine) *errorLine = f.Label + ": " + error;
            return false;
        }

        if (f.Invalid)
        {
            f.Invalid = false;
            f.Box.ClearValue(Control::BorderBrushProperty());
            ToolTipService::SetToolTip(f.Box, f.ToolTip);
        }
        // Viết lại theo dạng chuẩn ("1,50" → "1.5") để lần sau so được "đã sửa hay chưa".
        f.Loaded = ui::H(f.Get());
        if (f.Box.Text() != f.Loaded)
        {
            f.Box.Text(f.Loaded);
            f.Box.SelectionStart((int32_t)f.Loaded.size());
        }
        return true;
    }

    void FieldGroup::SetInvalid(TextField& f, const std::string& error)
    {
        f.Invalid = true;
        f.Box.BorderBrush(ui::Res(L"CriticalBrush"));
        ToolTipService::SetToolTip(f.Box, box_value(ui::H(error + " — not applied, old value kept")));
    }

    void FieldGroup::HideDeleteButton(TextBox const& box)
    {
        // Lúc Loaded template có thể chưa dựng (trang đang Collapsed) — làm ở lần nhận focus, khi chắc chắn đã có.
        // VisualState "ButtonVisible" chỉ đổi Visibility nên bề rộng 0 giữ nguyên.
        box.GotFocus([](WF::IInspectable const& sender, RoutedEventArgs const&) {
            if (auto del = FindNamed(sender.as<TextBox>(), L"DeleteButton", 4); del && del.Width() != 0)
            {
                del.MinWidth(0);
                del.Width(0);
                del.Margin(ThicknessHelper::FromUniformLength(0));
            }
        });
    }

    // ══════════ Bảng ══════════

    Grid MakeRowGrid(const std::vector<Column>& columns)
    {
        Grid grid;
        for (const Column& c : columns)
        {
            ColumnDefinition def;
            def.Width(c.Width > 0 ? GridLengthHelper::FromPixels(c.Width) : GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            grid.ColumnDefinitions().Append(def);
        }
        return grid;
    }

    TextBlock HeaderCell(const wchar_t* text)
    {
        TextBlock t;
        t.Text(text);
        t.FontSize(12);
        t.Foreground(ui::Res(L"TextSecondaryBrush"));
        t.VerticalAlignment(VerticalAlignment::Center);
        t.Margin(ThicknessHelper::FromLengths(8, 0, 6, 0));
        t.TextTrimming(TextTrimming::CharacterEllipsis);
        return t;
    }

    TextBlock TextCell(std::string_view text, double fontSize)
    {
        TextBlock t;
        t.Text(ui::H(text));
        t.FontSize(fontSize);
        t.VerticalAlignment(VerticalAlignment::Center);
        t.Margin(ThicknessHelper::FromLengths(8, 0, 6, 0));
        t.TextWrapping(TextWrapping::NoWrap);
        t.TextTrimming(TextTrimming::CharacterEllipsis);
        return t;
    }

    void PlaceInColumn(Grid const& row, FrameworkElement const& cell, int column)
    {
        Grid::SetColumn(cell, column);
        row.Children().Append(cell);
    }

    TextBox CellBox()
    {
        TextBox box;
        box.MinWidth(0);
        box.FontSize(13);
        box.Margin(ThicknessHelper::FromLengths(3, 4, 3, 4));
        box.Padding(ThicknessHelper::FromLengths(6, 5, 4, 5));
        box.VerticalAlignment(VerticalAlignment::Center);
        FieldGroup::HideDeleteButton(box);
        return box;
    }

    CheckBox CellCheck()
    {
        CheckBox check;
        check.MinWidth(0);
        check.Padding(ThicknessHelper::FromUniformLength(0));
        check.HorizontalAlignment(HorizontalAlignment::Center);
        check.VerticalAlignment(VerticalAlignment::Center);
        return check;
    }

    void SimpleTable::Build(std::vector<Column> columns, double headerHeight)
    {
        Columns = std::move(columns);

        Root = Grid();
        RowDefinition headerRow; headerRow.Height(GridLengthHelper::Auto());
        RowDefinition dividerRow; dividerRow.Height(GridLengthHelper::Auto());
        RowDefinition bodyRow; bodyRow.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        Root.RowDefinitions().Append(headerRow);
        Root.RowDefinitions().Append(dividerRow);
        Root.RowDefinitions().Append(bodyRow);

        Grid header = MakeRowGrid(Columns);
        header.Height(headerHeight);
        for (int i = 0; i < (int)Columns.size(); ++i) PlaceInColumn(header, HeaderCell(Columns[i].Header), i);

        Border divider;
        divider.Style(ui::ResStyle(L"DividerStyle"));

        Rows = StackPanel();
        ScrollViewer scroll;
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroll.Content(Rows);

        Grid::SetRow(header, 0);
        Grid::SetRow(divider, 1);
        Grid::SetRow(scroll, 2);
        Root.Children().Append(header);
        Root.Children().Append(divider);
        Root.Children().Append(scroll);
    }

    Grid SimpleTable::NewRow(double minHeight) const
    {
        Grid row = MakeRowGrid(Columns);
        row.MinHeight(minHeight);
        row.BorderBrush(ui::Res(L"DividerBrush"));
        row.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
        return row;
    }

    // ══════════ Nhật ký ══════════

    FrameworkElement LogTable::Build()
    {
        m_table.Build({ { L"Time", 72 }, { L"Level", 62 }, { L"Source", 70 }, { L"Message", 0 } });
        return m_table.Root;
    }

    LogTable::Key LogTable::KeyOf(const scs::LogData& d)
    {
        return { std::chrono::duration_cast<std::chrono::milliseconds>(d.UpdateTime.time_since_epoch()).count(),
                 (int)d.Level, d.Source, d.Message };
    }

    Grid LogTable::MakeRow(const scs::LogData& d) const
    {
        Grid row = m_table.NewRow(26);
        PlaceInColumn(row, TextCell(scs::FormatLocal(d.UpdateTime, "HH:mm:ss")), 0);
        PlaceInColumn(row, TextCell(scs::ToString(d.Level)), 1);
        PlaceInColumn(row, TextCell(d.Source), 2);
        TextBlock message = TextCell(d.Message);
        ToolTipService::SetToolTip(message, box_value(ui::H(d.Message)));
        PlaceInColumn(row, message, 3);
        return row;
    }

    void LogTable::Update()
    {
        if (!m_table.Rows) return;
        auto& log = scs::LogWorker::Instance();
        uint64_t version = log.Version();
        if (version == m_version) return;
        m_version = version;

        std::vector<scs::LogData> list = log.LogList();   // mới nhất ở đầu
        auto children = m_table.Rows.Children();

        // Danh sách chỉ thêm ở đầu và cắt ở cuối: tìm dòng mới nhất đang hiện, các dòng đứng trước nó là dòng mới.
        size_t fresh = list.size();
        if (!m_keys.empty())
        {
            fresh = SIZE_MAX;
            for (size_t i = 0; i < list.size(); ++i)
            {
                if (KeyOf(list[i]) == m_keys.front())
                {
                    fresh = i;
                    break;
                }
            }
            if (fresh == SIZE_MAX)
            {
                children.Clear();
                m_keys.clear();
                fresh = list.size();
            }
        }

        for (size_t i = fresh; i-- > 0;)
        {
            children.InsertAt(0, MakeRow(list[i]));
            m_keys.push_front(KeyOf(list[i]));
        }
        while (m_keys.size() > list.size())
        {
            children.RemoveAtEnd();
            m_keys.pop_back();
        }
    }

    // ══════════ Việc nền ══════════

    void Background::await_suspend(std::coroutine_handle<> handle)
    {
        ui::RunBackground([work = std::move(m_work), handle]() {
            try
            {
                if (work) work();
            }
            catch (const std::exception& e)
            {
                scs::LogWorker::Instance().RecordLog(std::string("Setting page: background task error — ") + e.what(),
                                                     scs::LogLevel::Error, "UI");
            }
            catch (...)
            {
                scs::LogWorker::Instance().RecordLog("Setting page: unknown background task error", scs::LogLevel::Error, "UI");
            }
            ui::PostUi([handle] { handle.resume(); });
        });
    }

    WF::IAsyncOperation<hstring> PickSolutionFileAsync(hstring currentPath)
    {
        try
        {
            winrt::Windows::Storage::Pickers::FileOpenPicker picker;
            // App unpackaged: picker phải biết cửa sổ chủ, không thì ném lỗi.
            picker.as<::IInitializeWithWindow>()->Initialize(ui::MainHwnd());
            picker.ViewMode(winrt::Windows::Storage::Pickers::PickerViewMode::List);
            picker.FileTypeFilter().Append(L".solw");
            picker.FileTypeFilter().Append(L"*");
            winrt::Windows::Storage::StorageFile file = co_await picker.PickSingleFileAsync();
            co_return file ? file.Path() : hstring{};
        }
        catch (...)
        {
            scs::LogWorker::Instance().RecordLog("FileOpenPicker unavailable — falling back to Win32 dialog",
                                                 scs::LogLevel::Warning, "UI");
        }
        co_return PickWithShellDialog(currentPath);
    }
}
