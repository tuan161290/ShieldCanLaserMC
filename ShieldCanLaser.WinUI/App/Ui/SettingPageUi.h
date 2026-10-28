#pragma once
// Phần dùng riêng của trang Setting:
//   * FieldGroup  — ô nhập / hộp chọn / hộp đánh dấu gắn với một giá trị trong dữ liệu SỐNG (SystemData,
//                   AxisConfig) — thay binding hai chiều của bản WPF;
//   * bảng dựng trong code (thay DataGrid) và bảng nhật ký;
//   * Background  — co_await một việc chặn của Core trên luồng nền (base::RunDetached), chạy tiếp trên luồng UI.
#include <climits>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Utility/Log/LogData.h"

namespace ui::settingpage
{
    // ── Số ──

    /// <summary>Số thực dạng ngắn nhất vẫn đọc lại đúng giá trị ("10", "0.2", "3333.3333333333335") — như
    /// DataGrid của bản WPF hiện double.ToString(), không cắt bớt chữ số của CountsPerUnit.</summary>
    std::string FormatDouble(double value);

    // ── Ô nhập gắn với dữ liệu sống ──

    /// <summary>
    /// Nhóm control gắn với các giá trị trong dữ liệu sống, thay binding của bản WPF:
    /// <list type="bullet">
    /// <item>Ô nhập: GHI vào dữ liệu khi người vận hành rời ô (LostFocus) hoặc bấm Enter — như TextBox/ô DataGrid
    /// của bản WPF. Chữ không đọc được thành số thì ô viền đỏ (tooltip nói lý do) và giá trị cũ giữ nguyên; các nút
    /// Lưu / Connect All Axis / Nạp lại solution gọi <see cref="CommitAll"/> và từ chối chạy khi còn ô sai.</item>
    /// <item>Hộp đánh dấu / hộp chọn: ghi ngay khi người vận hành đổi (sự kiện Click / SelectionChanged; đặt giá
    /// trị từ code không ghi ngược lại).</item>
    /// </list>
    /// </summary>
    class FieldGroup
    {
    public:
        using Getter = std::function<std::string()>;
        /// <summary>Đọc chữ, ghi vào dữ liệu. False + <paramref name="error"/> nếu chữ không hợp lệ (không ghi gì).</summary>
        using Setter = std::function<bool(const std::string& text, std::string& error)>;

        FieldGroup();

        void BindText(winrt::Microsoft::UI::Xaml::Controls::TextBox const& box, std::string label, Getter get, Setter set);
        void BindInt(winrt::Microsoft::UI::Xaml::Controls::TextBox const& box, std::string label,
                     std::function<int()> get, std::function<void(int)> set, int min = INT_MIN, int max = INT_MAX);
        void BindDouble(winrt::Microsoft::UI::Xaml::Controls::TextBox const& box, std::string label,
                        std::function<double()> get, std::function<void(double)> set);
        /// <summary>Ô trống = không có giá trị (nullopt) — TargetNullValue='' của bản WPF.</summary>
        void BindOptionalDouble(winrt::Microsoft::UI::Xaml::Controls::TextBox const& box, std::string label,
                                std::function<std::optional<double>()> get, std::function<void(std::optional<double>)> set);
        void BindString(winrt::Microsoft::UI::Xaml::Controls::TextBox const& box, std::string label,
                        std::function<std::string()> get, std::function<void(std::string)> set);
        void BindCheck(winrt::Microsoft::UI::Xaml::Controls::CheckBox const& box,
                       std::function<bool()> get, std::function<void(bool)> set);
        /// <summary>Hộp chọn một trong các chuỗi cố định; giá trị đang lưu không có trong danh sách thì được thêm
        /// vào cuối (không âm thầm đổi cấu hình).</summary>
        void BindChoice(winrt::Microsoft::UI::Xaml::Controls::ComboBox const& box, std::vector<std::string> values,
                        std::function<std::string()> get, std::function<void(std::string)> set);

        /// <summary>Nạp lại control từ dữ liệu. force = false: bỏ qua ô người vận hành đã sửa mà chưa ghi được
        /// (chữ khác lần nạp trước) để không xoá mất chữ đang gõ.</summary>
        void LoadAll(bool force);

        /// <summary>Ghi mọi ô đã sửa. Trả danh sách lỗi "Nhãn: lý do" — rỗng là tất cả hợp lệ.</summary>
        std::vector<std::string> CommitAll();

        /// <summary>Nạp lại MỘT ô từ dữ liệu, bỏ cả chữ sai đang có (vd. ô số quay chờ khi chuyển sang "cả chu kỳ").</summary>
        void ReloadField(winrt::Microsoft::UI::Xaml::Controls::TextBox const& box);

        /// <summary>Ẩn nút xoá (X) của ô nhập hẹp trong bảng — nó chiếm gần hết bề ngang ô khi đang gõ.</summary>
        static void HideDeleteButton(winrt::Microsoft::UI::Xaml::Controls::TextBox const& box);

    private:
        struct TextField
        {
            winrt::Microsoft::UI::Xaml::Controls::TextBox Box{ nullptr };
            std::string Label;
            Getter Get;
            Setter Set;
            winrt::hstring Loaded;
            winrt::Windows::Foundation::IInspectable ToolTip{ nullptr };
            bool Invalid{ false };
        };
        struct CheckField
        {
            winrt::Microsoft::UI::Xaml::Controls::CheckBox Box{ nullptr };
            std::function<bool()> Get;
        };
        struct ChoiceField
        {
            winrt::Microsoft::UI::Xaml::Controls::ComboBox Box{ nullptr };
            std::shared_ptr<std::vector<std::string>> Values;
            std::function<std::string()> Get;
        };

        static bool Commit(TextField& field, std::string* errorLine);
        static void SetInvalid(TextField& field, const std::string& error);

        std::vector<std::shared_ptr<TextField>> m_texts;
        std::vector<CheckField> m_checks;
        std::vector<ChoiceField> m_choices;
        /// <summary>Đang đặt SelectedIndex từ code — SelectionChanged lúc đó không phải người vận hành chọn.</summary>
        std::shared_ptr<bool> m_loading;
    };

    // ── Bảng dựng trong code ──

    struct Column
    {
        const wchar_t* Header;
        /// <summary>Bề rộng DIP; 0 = phần còn lại (*).</summary>
        double Width;
    };

    winrt::Microsoft::UI::Xaml::Controls::Grid MakeRowGrid(const std::vector<Column>& columns);
    winrt::Microsoft::UI::Xaml::Controls::TextBlock HeaderCell(const wchar_t* text);
    winrt::Microsoft::UI::Xaml::Controls::TextBlock TextCell(std::string_view text, double fontSize = 12);
    void PlaceInColumn(winrt::Microsoft::UI::Xaml::Controls::Grid const& row,
                       winrt::Microsoft::UI::Xaml::FrameworkElement const& cell, int column);
    /// <summary>Ô nhập hẹp trong bảng (bảng servo ở trang EtherCAT, bảng vị trí chờ ở trang Setting): chữ 13, không nút xoá.</summary>
    winrt::Microsoft::UI::Xaml::Controls::TextBox CellBox();
    /// <summary>Hộp đánh dấu nằm giữa ô bảng.</summary>
    winrt::Microsoft::UI::Xaml::Controls::CheckBox CellCheck();

    /// <summary>Khung bảng chỉ đọc: hàng tiêu đề | đường kẻ | các dòng trong ScrollViewer dọc.</summary>
    struct SimpleTable
    {
        std::vector<Column> Columns;
        winrt::Microsoft::UI::Xaml::Controls::Grid Root{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::StackPanel Rows{ nullptr };

        void Build(std::vector<Column> columns, double headerHeight = 32);
        /// <summary>Một dòng mới (chưa thêm vào bảng), viền dưới 1 px.</summary>
        winrt::Microsoft::UI::Xaml::Controls::Grid NewRow(double minHeight) const;
    };

    /// <summary>
    /// Bảng "Nhật ký (bộ nhớ)" — LogWorker::LogList (mới nhất ở đầu, tối đa 500 dòng). Chỉ dựng lại khi
    /// LogWorker::Version đổi, và chỉ thêm các dòng MỚI lên đầu (không viết lại cả 500 dòng mỗi lần có log).
    /// </summary>
    class LogTable
    {
    public:
        winrt::Microsoft::UI::Xaml::FrameworkElement Build();
        void Update();

    private:
        struct Key
        {
            long long TimeMs{};
            int Level{};
            std::string Source;
            std::string Message;
            bool operator==(const Key&) const = default;
        };
        static Key KeyOf(const scs::LogData& data);
        winrt::Microsoft::UI::Xaml::Controls::Grid MakeRow(const scs::LogData& data) const;

        SimpleTable m_table;
        std::deque<Key> m_keys;   // song song với m_table.Rows.Children(), mới nhất ở đầu
        uint64_t m_version{ ~0ull };
    };

    // ── Việc nền ──

    /// <summary>
    /// <c>co_await Background([&amp;] { ...hàm chặn của Core... });</c> — chạy trên luồng nền qua base::RunDetached
    /// (lúc tắt app được chờ cùng các việc nền khác), xong thì coroutine chạy tiếp trên luồng UI. Việc nền phải tự
    /// bắt exception của nó và để lại kết quả/lỗi cho phần chạy tiếp; exception lọt ra chỉ được ghi log.
    /// </summary>
    struct Background
    {
        explicit Background(std::function<void()> work) : m_work(std::move(work)) {}
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle);
        void await_resume() const noexcept {}

    private:
        std::function<void()> m_work;
    };

    /// <summary>Hộp chọn file .solw. FileOpenPicker gắn HWND cửa sổ chính; picker không dùng được (vd. app chạy
    /// quyền admin) thì rơi về hộp thoại Win32 IFileOpenDialog. Trả rỗng nếu người vận hành huỷ.</summary>
    winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> PickSolutionFileAsync(winrt::hstring currentPath);
}
