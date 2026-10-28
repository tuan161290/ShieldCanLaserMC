#pragma once
#include "TeachingPage.g.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Communication/IAxis.h"
#include "Data/Model/ModelData.h"
#include "Ui/Page.h"

namespace winrt::ShieldCanLaser::implementation
{
    /// <summary>
    /// Trang Teaching — model, tham số chu trình Auto và điểm teach của máy Laser Marking (PLAN-LASER.md §3): quản lý
    /// model (nạp/lưu/xoá/tạo mới/sao chép), jog 3 trục X, Y, Z, và các điểm teach của model đang dùng.
    ///
    /// <para>Sửa ghi chú/toạ độ/tham số ghi THẲNG vào model đang nạp (như binding LostFocus của bản WPF); "SAVE" mới ghi
    /// xuống file. Các ô sửa chỉ được nạp lại khi đổi điểm chọn / nạp lại danh sách / trang được mở — OnTick() không bao
    /// giờ ghi đè chúng.</para>
    ///
    /// <para><b>Card Teach Points</b> có 4 tab: <b>Pick</b> (tray 1 / 2 — teach hàng 1 và hàng cuối: X, Y, Z, các hàng
    /// giữa chia đều theo ModelData::PickPoint), <b>CAM</b> (nozzle 1 và nozzle 7 giữa khung CAM: X, Z — ModelData::CamPoint),
    /// <b>Laser</b> (nozzle 1 và nozzle 7 ở tâm trường laser: X, Z — ModelData::LaserPoint) và <b>Others</b> (danh sách điểm
    /// có tên tự thêm, không gồm 8 điểm của ba tab kia). Ba tab đầu dùng chung một bố cục: hai khung đầu/cuối + bảng các
    /// điểm suy ra có nút MOVE TO.</para>
    /// </summary>
    struct TeachingPage : TeachingPageT<TeachingPage>, ui::IPage
    {
        TeachingPage() = default;

        void OnRootLoaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        // ── Model ──
        void OnModelSelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnLoadModelClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSaveModelClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnDeleteModelClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnNewModelClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCopyModelClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnDescriptionLostFocus(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        // ── Jog / tham số ──
        void OnJogSlowClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnJogFastClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStopAllClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAutoParamLostFocus(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAutoFlagClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        // ── Tab Others: điểm có tên ──
        void OnPointSelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnAddPointClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnDeletePointClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnPointNameLostFocus(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnPointNoteLostFocus(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTeachClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMoveDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void OnMoveSlowDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);

        // ── Tab Pick / CAM / Laser (điểm đầu + điểm cuối) ──
        void OnTabClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSelectorClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnEndTeachClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnEndMoveDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);

        void OnTick() override;
        void OnShown(bool visible) override;
        /// <summary>--shot chụp cả 4 tab của card Teach Points (3-teaching, 3-teaching-2 … -4).</summary>
        int ShotVariants() const override { return TabCount; }
        void ShowShotVariant(int index) override { SelectTab(index); }

    private:
        static constexpr size_t AxisCount = 3;   // scs::AxisId::All.size()
        enum Tab : int { TabPick = 0, TabCam = 1, TabLaser = 2, TabOther = 3, TabCount = 4 };
        /// <summary>Số trục tối đa của một điểm đầu/cuối — Pick: X, Y, Z; CAM / Laser: X, Z.</summary>
        static constexpr int MaxEndAxes = 3;
        /// <summary>Hai khung điểm: đầu (hàng 1 / nozzle 1), cuối (hàng cuối / nozzle 7).</summary>
        static constexpr int EndCount = 2;
        /// <summary>Bảng điểm suy ra hiện tối đa ngần này dòng.</summary>
        static constexpr int MaxDerivedRows = 60;

        /// <summary>Một điểm trong danh sách Others (TeachPointVm của bản WPF). Values = toạ độ đang hiện của từng trục
        /// theo thứ tự AxisId::All — giữ cả khi trục bị bỏ chọn, bật lại thì ghi đúng số đó vào điểm.</summary>
        struct PointRow
        {
            std::shared_ptr<scs::TeachPosition> Point;
            std::array<double, AxisCount> Values{};
            Microsoft::UI::Xaml::Controls::ListViewItem Item{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock NameText{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock SummaryText{ nullptr };
        };

        /// <summary>Một dòng trục trong khung chi tiết điểm (TeachAxisValueVm). Shown = chữ trang này đặt vào ô
        /// toạ độ lần cuối — ô chỉ được ghi vào điểm khi chữ đã khác (người vận hành đã sửa), như binding WPF.</summary>
        struct AxisEditRow
        {
            Microsoft::UI::Xaml::Controls::CheckBox Use{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBox Value{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock Unit{ nullptr };
            winrt::hstring Shown;
        };

        /// <summary>Một trục của cột điểm đầu / điểm cuối. Shown như AxisEditRow: chỉ ghi khi người vận hành đã sửa.</summary>
        struct EndAxisRow
        {
            Microsoft::UI::Xaml::Controls::TextBlock Label{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBox Box{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock Unit{ nullptr };
            winrt::hstring Shown;
        };

        /// <summary>Một dòng của khu Jog.</summary>
        struct JogRow
        {
            std::shared_ptr<scs::IAxis> Axis;
            Microsoft::UI::Xaml::Documents::Run PosRun{ nullptr };
            Microsoft::UI::Xaml::Documents::Run UnitRun{ nullptr };
            Microsoft::UI::Xaml::Shapes::Ellipse OrgLamp{ nullptr };
            winrt::hstring PosShown, UnitShown;
            int OrgShown{ -1 };
        };

        /// <summary>Thay WrapPanel của bản WPF (WinUI không có): các phần tử bề rộng cố định xếp thành từng hàng
        /// ngang trong một StackPanel dọc, hết chỗ thì xuống hàng. Sắp lại khi bề ngang đổi.</summary>
        struct WrapFlow
        {
            Microsoft::UI::Xaml::Controls::StackPanel Host{ nullptr };
            std::vector<Microsoft::UI::Xaml::FrameworkElement> Items;
            std::vector<size_t> Breaks{ static_cast<size_t>(-1) };   // giá trị canh: chưa sắp lần nào
            void Attach(Microsoft::UI::Xaml::Controls::StackPanel const& host);
            void Reflow(double width);
        };

        void BuildJogRows();
        void BuildAxisRows();

        // Model
        void RefreshModelList();
        void SelectModelInList();
        void CommitDescription();

        // Điểm teach
        void ReloadPoints();
        /// <summary>Card "Cycle Parameters": nạp từ model hiện tại / ghi các ô vào model (ô sai thì trả lại số cũ).</summary>
        void LoadAutoParams();
        void CommitAutoParams();
        void RebuildPointList();
        void SelectPointInList();
        PointRow* SelectedRow();
        void LoadEditor();
        void CommitEditor();
        void CommitName();
        void CommitNote();
        void CommitAxisValue(size_t axisIndex);
        void OnAxisUseClick(size_t axisIndex);
        void OnTakeAxisClick(size_t axisIndex);
        void TakeCurrent(PointRow& row, size_t axisIndex);
        void ShowAxisRow(PointRow const& row, size_t axisIndex);
        void MoveToSelected(bool slow);
        /// <summary>Chạy tới BẢN SAO một điểm trên luồng nền (nâng Z an toàn, các trục khác song song, Z hạ sau cùng).</summary>
        void MoveToPoint(std::shared_ptr<scs::TeachPosition> snapshot, bool slow);

        // Tab Pick / CAM / Laser
        void BuildLayoutEditors();
        void SelectTab(int tab);
        /// <summary>Nạp tab Pick / CAM / Laser từ model: chọn tray, hai khung điểm teach, bảng chia đều.</summary>
        void LoadLayout();
        /// <summary>Ghi các ô đang sửa của tab Pick / CAM / Laser vào model.</summary>
        void CommitLayout();
        void CommitEndValue(int end, int axisIndex);
        void RebuildDerived();
        /// <summary>Trục của điểm đầu/cuối ở tab đang mở: Pick X, Y, Z — CAM / Laser X, Z.</summary>
        std::vector<std::string> EndAxes() const;
        /// <summary>Tên điểm khung <paramref name="end"/> (0 đầu, 1 cuối) của tab đang mở — AutoPoints::PickRow / Cam / Laser.</summary>
        std::string EndName(int end) const;
        /// <summary>Điểm khung <paramref name="end"/> của tab đang mở; model thiếu thì thêm vào.</summary>
        std::shared_ptr<scs::TeachPosition> EndPoint(int end);
        /// <summary>Số điểm suy ra của tab đang mở: Pick = số hàng tray, CAM / Laser = 7 nozzle.</summary>
        int DerivedCount() const;
        /// <summary>Điểm suy ra thứ <paramref name="n"/> (1-based) của tab đang mở — bản sao mới, nullptr + why nếu thiếu teach.</summary>
        std::shared_ptr<scs::TeachPosition> DerivedPoint(int n, std::string& why) const;

        /// <summary>Teach lúc chưa về gốc là lưu số rác — báo và trả false.</summary>
        static bool CheckHomedForTeach();

        bool m_loaded{ false };
        bool m_busy{ false };

        // Model
        std::vector<std::string> m_modelNames;
        std::string m_selectedModelName;
        bool m_suppressModelSelection{ false };
        winrt::hstring m_descriptionShown;

        // Điểm teach của model đang hiện (m_model = đối tượng model lúc nạp danh sách; đổi đối tượng = nạp lại).
        std::shared_ptr<scs::ModelData> m_model;
        std::vector<PointRow> m_points;
        int m_selected{ -1 };
        bool m_suppressPointSelection{ false };

        // Khung chi tiết
        std::array<AxisEditRow, AxisCount> m_axisRows;
        winrt::hstring m_nameShown, m_noteShown;
        WrapFlow m_fieldsFlow, m_buttonsFlow;

        // Jog
        std::vector<JogRow> m_jogRows;
        int m_jogFastShown{ -1 };

        // Tab điểm teach
        int m_tab{ TabPick };
        int m_tray{ 1 };   // tab Pick
        std::array<std::array<EndAxisRow, MaxEndAxes>, EndCount> m_ends;
    };
}

namespace winrt::ShieldCanLaser::factory_implementation
{
    struct TeachingPage : TeachingPageT<TeachingPage, implementation::TeachingPage>
    {
    };
}
