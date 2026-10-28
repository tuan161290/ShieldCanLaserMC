#pragma once
#include "EtherCatPage.g.h"

#include "Communication/AxisConfig.h"
#include "Ui/Page.h"
#include "Ui/SettingPageUi.h"

namespace winrt::ShieldCanLaser::implementation
{
    /// <summary>
    /// Trang EtherCAT (Tuấn 2026-10-09: tách khỏi trang Setting): bus SOEM + Remote IO (card mạng, Scan, Connect All
    /// Axis), cấu hình servo 14 trục, luồng chu kỳ EtherCAT. Mỗi phần một mục ở cột trái; nút Lưu system.json dùng chung.
    ///
    /// <para>Ô nhập gắn thẳng vào dữ liệu sống (SystemData / AxisConfig) qua ui::settingpage::FieldGroup — ghi khi rời ô,
    /// như binding của bản WPF. OnTick chỉ cập nhật phần chỉ đọc (trạng thái bus, kết quả Connect, thống kê RT) và nút
    /// bật/tắt.</para>
    /// </summary>
    struct EtherCatPage : EtherCatPageT<EtherCatPage>, ui::IPage
    {
        EtherCatPage() = default;

        void OnRootLoaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnNavClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSaveClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        // ── Bus + Remote IO ──
        void OnAdapterSelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        winrt::fire_and_forget OnDisconnectBusClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnScanClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnApplyScanClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnConnectAllClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        // ── Luồng chu kỳ EtherCAT ──
        void OnRtCpuSelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnRtSpinAllClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        // ── Bảng servo 14 trục: ba ScrollViewer cuộn đồng bộ (cột "Trục" cố định) ──
        void OnAxisBodyViewChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::ScrollViewerViewChangedEventArgs const&);
        void OnAxisNamesWheel(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);

        void OnTick() override;
        void OnShown(bool visible) override;
        int ShotVariants() const override { return SectionCount; }
        void ShowShotVariant(int index) override { SelectSection(index); }

    private:
        enum { SectionBus = 0, SectionServo, SectionRt, SectionCount };

        /// <summary>Một slave tìm thấy khi Scan — một dòng ở bảng "Kết quả Scan" (ScannedSlaveVm của bản WPF).</summary>
        struct ScannedSlave
        {
            int SlavePosition{};
            uint32_t VendorId{}, ProductCode{}, Revision{};
            std::string DriverName;     // tên driver theo ESI; "(không có trong ESI)" nếu không khớp
            std::string EncoderText;    // xung/vòng (0x608F) — trống nếu không đọc được
            std::string AssignedTo;     // slot cấu hình ở vị trí này ("X", "A3", "Remote IO") — trống nếu không có
            bool Matches{};
            std::string IdText() const;
            std::string MatchText() const;
        };

        /// <summary>Dòng của bảng servo: các ô chỉ đọc (định danh driver) cập nhật trong OnTick.</summary>
        struct AxisRow
        {
            std::shared_ptr<scs::AxisConfig> Config;
            Microsoft::UI::Xaml::Controls::TextBlock Driver{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock Id{ nullptr };
        };

        struct AdapterOption
        {
            std::string Display;
            std::string Value;
        };

        void SelectSection(int section);
        void BindFields();
        void BuildAxisTable();
        void BuildAdapterOptions();
        void BuildRtCpuOptions();
        void BuildRtHistogram();
        void LoadAll(bool force);
        void LoadRtSpin();
        void ShowSaveResult();

        void RefreshBusInfo();
        void UpdateRtStats();
        void UpdateCommandStates();
        void ShowScanResults();

        /// <summary>Ghi mọi ô đã sửa của trang vào dữ liệu sống trước Lưu / Connect All Axis. Còn ô sai thì cảnh báo
        /// (kèm hành động bị từ chối) và trả false.</summary>
        bool CommitEdits(const std::string& action);

        void SetEtherCatMessage(std::string text);

        bool m_loaded{ false };
        int m_section{ SectionBus };
        /// <summary>Đang chạy Scan / Connect All Axis / Ngắt bus (Busy của ViewModel cũ).</summary>
        bool m_busy{ false };
        /// <summary>Đang đặt control từ code — SelectionChanged lúc đó không phải người vận hành.</summary>
        bool m_loading{ false };

        ui::settingpage::FieldGroup m_fields;       // Remote IO, Rt*
        ui::settingpage::FieldGroup m_axisFields;   // bảng servo 14 trục
        std::vector<AxisRow> m_axisRows;

        std::vector<AdapterOption> m_adapterOptions;
        std::vector<int> m_rtCpuValues;             // song song với RtCpuCombo: -1 = tự chọn
        std::vector<Microsoft::UI::Xaml::Controls::TextBlock> m_histogramCounts;

        std::vector<ScannedSlave> m_scanned;
        ui::settingpage::SimpleTable m_scanTable;

        std::string m_etherCatMessage;
        std::string m_connectResultsKey;
        std::string m_saveResult;
    };
}

namespace winrt::ShieldCanLaser::factory_implementation
{
    struct EtherCatPage : EtherCatPageT<EtherCatPage, implementation::EtherCatPage>
    {
    };
}
