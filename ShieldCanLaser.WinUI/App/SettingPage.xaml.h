#pragma once
#include "SettingPage.g.h"

#include "Communication/AxisConfig.h"
#include "Ui/Page.h"
#include "Ui/SettingPageUi.h"

namespace winrt::ShieldCanLaser::implementation
{
    /// <summary>
    /// Trang Setting — máy Laser Marking: hệ thống, vị trí làm việc (3 trục), time delay, vision (solution + tên Flow/biến +
    /// bù laser + lưới hiệu chuẩn), nhật ký — mỗi phần một mục
    /// ở cột trái, nút Lưu system.json dùng chung. Bus EtherCAT, cấu hình servo và luồng chu kỳ ở trang EtherCAT riêng
    /// (Tuấn 2026-10-09). Port từ SettingView/SettingViewModel của bản WPF.
    ///
    /// <para>Ô nhập gắn thẳng vào dữ liệu sống (SystemData / AxisConfig) qua ui::settingpage::FieldGroup — ghi khi rời ô,
    /// như binding của bản WPF; riêng Z_SAFE là giá trị riêng của trang, chỉ áp khi bấm Lưu (như ViewModel cũ).
    /// OnTick chỉ cập nhật phần chỉ đọc (vị trí trục hiện tại, nhật ký) và nút bật/tắt.</para>
    /// </summary>
    struct SettingPage : SettingPageT<SettingPage>, ui::IPage
    {
        SettingPage() = default;

        void OnRootLoaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnNavClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        // ── Lưu, hệ thống, vị trí làm việc ──
        void OnZSafeLostFocus(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSaveClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenDataFolderClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        // ── Vision ──
        void OnSolutionPathChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        winrt::fire_and_forget OnBrowseSolutionClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnReloadSolutionClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCheckGlobalVarsClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCreateGlobalVarsClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnCheckFlowOutputsClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        void OnTick() override;
        void OnShown(bool visible) override;
        int ShotVariants() const override { return SectionCount; }
        void ShowShotVariant(int index) override { SelectSection(index); }

    private:
        enum { SectionSystem = 0, SectionPosition, SectionTime, SectionVision, SectionLog, SectionCount };

        /// <summary>Dòng của bảng "Vị trí chờ và giới hạn mềm": đơn vị và vị trí hiện tại chỉ đọc.</summary>
        struct PositionRow
        {
            std::shared_ptr<scs::AxisConfig> Config;
            Microsoft::UI::Xaml::Controls::TextBlock Unit{ nullptr };
            Microsoft::UI::Xaml::Controls::TextBlock Current{ nullptr };
        };

        void SelectSection(int section);
        void BindSystemFields();
        void BuildPositionTable();
        void LoadAll(bool force);
        void LoadZSafe();
        void UpdateStaticInfo();
        void UpdatePositions();
        void UpdateCommandStates();

        /// <summary>Ghi mọi ô đã sửa vào dữ liệu sống trước Lưu / Nạp lại solution. Còn ô sai thì cảnh báo (kèm hành
        /// động bị từ chối) và trả false.</summary>
        bool CommitEdits(bool includeZSafe, const std::string& action);
        bool ParseZSafe(double& value, std::string& error);

        void SetReport(Microsoft::UI::Xaml::Controls::TextBlock const& block, const std::string& text);
        winrt::fire_and_forget RunGlobalVarSync(bool apply);

        bool m_loaded{ false };
        int m_section{ SectionSystem };
        /// <summary>Đang chạy Kiểm tra biến / Tạo biến / Kiểm tra Flow — không cho chạy chồng.</summary>
        bool m_visionBusy{ false };

        ui::settingpage::FieldGroup m_fields;       // hệ thống, time delay, tên Flow/biến
        ui::settingpage::FieldGroup m_posFields;    // bảng vị trí chờ / giới hạn mềm
        ui::settingpage::SimpleTable m_posTable;
        std::vector<PositionRow> m_posRows;

        hstring m_zSafeLoaded;

        ui::settingpage::LogTable m_log;
        std::string m_saveResult;
    };
}

namespace winrt::ShieldCanLaser::factory_implementation
{
    struct SettingPage : SettingPageT<SettingPage, implementation::SettingPage>
    {
    };
}
