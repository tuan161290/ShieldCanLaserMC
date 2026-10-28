#pragma once
#include "MainPage.g.h"

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>

#include "Communication/ImageData.h"
#include "Infor/Enums.h"
#include "Infor/IoMap.h"
#include "Ui/Page.h"
#include "Ui/VmPane.h"
#include "Utility/Log/LogData.h"

namespace scs { class VisionMasterDevice; }

namespace winrt::ShieldCanLaser::implementation
{
    /// <summary>
    /// Trang Main — máy Laser Marking: nút vận hành (AUTO/MANUAL, RUN, STOP, Về gốc, Init, RESET, Tắt còi), sản lượng +
    /// cycle time, tiến độ 2 tray (RESET TRAY), sơ đồ 2 tray TrayRows × 7 ô, trạng thái 7 nozzle (độ lệch đo ở CAM), một khung
    /// ảnh CAM và nhật ký trực tiếp. Không binding: mọi thứ đọc từ Core trong OnTick().
    ///
    /// <para>Khung ảnh CAM: ảnh Bitmap chụp gần nhất của Vision (đổi sang WriteableBitmap khi Vision thay ảnh mới) và — chỉ
    /// khi vision là VisionMaster thật (cả lúc mô phỏng trục/IO), không chụp ảnh — một VmRenderControl của VisionMaster đặt
    /// TRÊN ảnh đó, gắn vào Flow/module cấu hình ở trang Setting (VisionOutputNames::RenderCam).</para>
    /// </summary>
    struct MainPage : MainPageT<MainPage>, ui::IPage
    {
        MainPage() = default;

        void OnRootLoaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAutoClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnManualClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnRunClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStopClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOriginClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnInitDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void OnResetClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMuteClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnClearCountersClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnResetTrayClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCamToolbarClick(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const&);

        void OnTick() override;
        void OnShown(bool visible) override;
        void OnWindowMoved() override;
        void OnModalChanged(bool modalOpen) override;
        void OnClosing() override;

    private:
        static constexpr int NozzleCount = scs::IoMap::NozzleCount;
        static constexpr int TrayCount = scs::IoMap::TrayCount;

        /// <summary>Một sơ đồ tray: lưới ô Border dựng lại khi số ô / số cột đổi, tô màu theo ECellState mỗi nhịp.</summary>
        struct TrayMap
        {
            Microsoft::UI::Xaml::Controls::Grid Host{ nullptr };
            std::vector<Microsoft::UI::Xaml::Controls::Border> Cells;
            std::vector<int> Shown;   // ECellState đang tô; -1 = chưa tô
            int Cols{ -1 };
        };

        /// <summary>Một hàng của dải nozzle: ô màu trạng thái + chữ trạng thái + độ lệch.</summary>
        struct NozzleRow
        {
            Microsoft::UI::Xaml::Controls::Border Cell{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock Text{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock Dev{ nullptr };
            int Shown{ -1 };   // ECellState đang tô
        };

        /// <summary>Khung ảnh CAM: ảnh dự phòng + control VisionMaster (máy thật).</summary>
        struct CamFrame
        {
            Microsoft::UI::Xaml::Controls::TextBlock Captured{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock Placeholder{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock VmText{ nullptr };
            Microsoft::UI::Xaml::Controls::Image Picture{ nullptr };
            Microsoft::UI::Xaml::Controls::Border Host{ nullptr };
            Microsoft::UI::Xaml::Controls::Button ToolbarButton{ nullptr };
            Microsoft::UI::Xaml::Media::Imaging::WriteableBitmap Bitmap{ nullptr };
            /// <summary>Thanh công cụ (tối) của VmRenderControl đang hiện — mặc định ẩn, nút ở tiêu đề card bật/tắt.</summary>
            bool ToolbarShown{ false };

            /// <summary>Ảnh Vision đang hiện — chỉ đổi lại khi Vision thay bằng ảnh MỚI (so con trỏ).</summary>
            std::shared_ptr<const scs::ImageData> LastSrc;
            /// <summary>Tăng mỗi lần có ảnh mới; kết quả đổi ảnh trên luồng nền mang số cũ thì bỏ.</summary>
            uint64_t Generation{ 0 };

            ui::VmPane Pane;
            /// <summary>"lần nạp|đường dẫn" đã gắn thành công vào control; rỗng = chưa gắn.</summary>
            std::string BoundKey;
            /// <summary>Popup của control đang hiện (1) / ẩn (0); -1 = chưa đặt lần nào.</summary>
            int PaneShown{ -1 };
            /// <summary>Đường dẫn ứng với lần SetParamsInfo hỏng gần nhất đã ghi log (tránh ghi lặp mỗi giây).</summary>
            std::string FailLoggedKey;
        };

        void BuildTrayLegend();
        void BuildNozzleStrip();
        /// <summary>Sơ đồ 2 tray: lấy Routine_Auto::Trays(), thiếu / sai kích thước thì suy từ RunningData.TrayRowsDone.</summary>
        void UpdateTrays();
        /// <summary>Dải 7 nozzle: Routine_Auto::Nozzles().</summary>
        void UpdateNozzles();
        /// <summary>Dựng lưới <paramref name="count"/> ô, <paramref name="cols"/> cột, cột giãn kín bề ngang khung;
        /// <paramref name="cellHeight"/> = 0 thì hàng cũng giãn kín chiều cao. <paramref name="labels"/>: ghi số thứ tự trong ô.</summary>
        void LayoutMap(TrayMap& map, int count, int cols, double cellHeight, bool labels);
        void PaintMap(TrayMap& map, std::vector<int> const& states);
        void UpdateRunButton(scs::EMcState state);
        void UpdateCounters();
        void UpdateLog();
        void SyncImage(std::shared_ptr<const scs::ImageData> src);
        void ShowConverted(uint64_t generation, int width, int height, std::vector<uint8_t> const& bgra);

        /// <summary>Xếp hàng EnsureVmPane + UpdateRenderBinding chạy ở lượt sau của hàng đợi UI — mọi lời gọi vào control
        /// VisionMaster đi qua đây, không gọi thẳng trong Loaded/Tick/bấm tab (xem Ui/VmPane.h: XAML fail-fast khi bị gọi
        /// lồng).</summary>
        void QueueVmWork(bool force);
        void EnsureVmPane();
        void UpdateRenderBinding(bool force);
        bool BindCam(scs::VisionMasterDevice& device, std::string const& path, int loadCount);
        void ApplyPaneVisibility();

        bool m_loaded{ false };
        bool m_visible{ false };
        bool m_closing{ false };

        std::array<TrayMap, TrayCount> m_trayMaps;
        std::array<NozzleRow, NozzleCount + 1> m_nozzles;   // chỉ số 1…7
        int m_lastNgStreak{ -1 };   // NG liên tiếp > 0 (1) / = 0 (0) đã tô; -1 = chưa tô
        CamFrame m_cam;
        int m_camNozzle{ -1 };      // nozzle của ảnh CAM đang hiện trên tiêu đề; -1 = chưa đặt

        // Nút RUN: 0 = đứng (xám), 1 = sẵn sàng (xanh nháy), 2 = đang chạy (xanh + quầng sáng); -1 = chưa đặt.
        int m_runVisual{ -1 };
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_armedBlink{ nullptr };

        // Nhật ký: bản chụp LogWorker đang hiện (mới nhất đứng đầu, cùng thứ tự với LogPanel).
        uint64_t m_logVersion{ ~0ull };
        std::vector<scs::LogData> m_shownLogs;

        // Control VisionMaster — tạo lần đầu trang hiện (không tạo khi vision giả / chụp ảnh).
        bool m_vmAttempted{ false };
        bool m_vmActive{ false };
        bool m_vmQueued{ false };
        bool m_vmForce{ false };
        bool m_vmBusy{ false };   // đang trong một lời gọi VM (có thể bơm thông điệp) — việc xếp hàng lồng vào thì bỏ
        int m_lastLoadCount{ -1 };
        std::chrono::steady_clock::time_point m_lastBindTry{};
        /// <summary>Chuỗi chẩn đoán đã ghi log lần gần nhất — chỉ ghi lại khi đổi (như _loggedVisionDiagnostic bản WPF).</summary>
        std::string m_loggedVisionDiagnostic;
    };
}

namespace winrt::ShieldCanLaser::factory_implementation
{
    struct MainPage : MainPageT<MainPage, implementation::MainPage>
    {
    };
}
