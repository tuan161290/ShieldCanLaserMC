#pragma once
#include "ManualPage.g.h"

#include <array>

#include "Infor/IoMap.h"
#include "Ui/ManualPageParts.h"
#include "Ui/Page.h"

namespace scs { class IAxis; class EtherCatAxis; }

namespace winrt::ShieldCanLaser::implementation
{
    /// <summary>
    /// Trang Manual — máy Laser Marking: điều khiển đầy đủ 3 trục X, Y, Z (SVO/RST/Home/Jog/Inching/Move/Stop), 7 van chân
    /// không + đèn CAM + xung laser thử, bảng đèn I/O, bảng mô phỏng và chẩn đoán EtherCAT.
    ///
    /// <para>Bảng mô phỏng không phải đồ chơi: nó là cách duy nhất chạy được các nhánh EMG, rèm sáng và mất kết nối mà
    /// không phải rút cáp máy thật.</para>
    /// </summary>
    struct ManualPage : ManualPageT<ManualPage>, ui::IPage
    {
        ManualPage() = default;

        void OnRootLoaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        void OnStopAllClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnResetClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnServoOnAllClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnServoOffAllClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOriginClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        void OnVacuumAllOffClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCamLightClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnLaserTestClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        void OnSimStartClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSimStopClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSimResetClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSimEmgClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSimCurtainClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSimAirClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSimTray1Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSimTray2Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        void OnTick() override;

    private:
        void BuildAxisRows();
        void BuildNozzlePanel();
        void BuildIoPanels();
        void BuildSimAxisGrid();
        void BuildSimPickFail();
        void UpdateSafety();
        void UpdateNozzles();
        void UpdateIo();
        void UpdateDiagnostics(uint64_t di, uint64_t doo);

        bool m_loaded{ false };
        ui::ManualLampBrushes m_lampBrushes;

        // ── Bảng trục ──
        std::vector<std::shared_ptr<ui::ManualAxisRow>> m_rows;
        /// <summary>SVO ON / SVO OFF tất cả đang chạy trên luồng nền — khoá hai nút đó.</summary>
        std::atomic<bool> m_servoAllBusy{ false };
        int m_shownServoAllBusy{ -1 };

        // ── 7 nozzle, đèn CAM, laser ── (chỉ số 1…7, phần tử 0 bỏ trống)
        std::array<Microsoft::UI::Xaml::Controls::Button, scs::IoMap::NozzleCount + 1> m_nozzleButtons{};
        std::array<ui::ManualLamp, scs::IoMap::NozzleCount + 1> m_nozzleLamps;
        std::array<Microsoft::UI::Xaml::Controls::Button, scs::IoMap::NozzleCount + 1> m_simPickFailButtons{};
        std::array<bool, scs::IoMap::NozzleCount + 1> m_simPickFail{};
        /// <summary>Xung laser thử đang chạy trên luồng nền — khoá nút.</summary>
        std::atomic<bool> m_laserBusy{ false };
        int m_shownLaserBusy{ -1 };
        std::string m_shownLaserText{ "\x01" };

        // ── InfoBar + Z an toàn ── (chuỗi đã hiện lần trước; "\x01" = chưa hiện)
        Microsoft::UI::Xaml::Documents::Run m_blockReasonRun{ nullptr };
        std::string m_shownBlockReason{ "\x01" };
        std::string m_shownMessage{ "\x01" };
        std::string m_shownZText{ "\x01" };
        int m_shownZSafe{ -1 };

        // ── Bảng I/O ──
        std::vector<ui::ManualLamp> m_diLamps;
        std::vector<ui::ManualLamp> m_doLamps;
        /// <summary>Io đã xét lần trước — đổi thì xét lại có phải FakeIo không (hiện/ẩn Bảng mô phỏng).</summary>
        const void* m_shownIo{ reinterpret_cast<const void*>(1) };

        // ── Chẩn đoán ──
        std::string m_shownBusText{ "\x01" };
        std::string m_shownRawDi{ "\x01" };
        std::string m_shownRawDo{ "\x01" };
        /// <summary>Trục EtherCAT thật (không có khi mô phỏng) — mỗi trục một dòng CiA402.</summary>
        std::vector<std::shared_ptr<scs::EtherCatAxis>> m_etherCatAxes;
        std::vector<Microsoft::UI::Xaml::Controls::TextBlock> m_driveLines;
        std::vector<std::string> m_shownDriveLines;
    };
}

namespace winrt::ShieldCanLaser::factory_implementation
{
    struct ManualPage : ManualPageT<ManualPage, implementation::ManualPage>
    {
    };
}
