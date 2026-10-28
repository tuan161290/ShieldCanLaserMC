#pragma once
#include "AlarmPage.g.h"

#include "Ui/AlarmTable.h"
#include "Ui/Page.h"

namespace winrt::ShieldCanLaser::implementation
{
    struct AlarmPage : AlarmPageT<AlarmPage>, ui::IPage
    {
        AlarmPage() = default;

        void OnRootLoaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMuteClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnResetClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        void OnTick() override;

    private:
        bool m_loaded{ false };
        ui::AlarmTable m_active;
        ui::AlarmTable m_history;
        /// <summary>Bản AlarmManager::Version() đã vẽ — danh sách không đổi thì không dựng lại bảng.</summary>
        uint64_t m_shownVersion{ ~0ull };
    };
}

namespace winrt::ShieldCanLaser::factory_implementation
{
    struct AlarmPage : AlarmPageT<AlarmPage, implementation::AlarmPage>
    {
    };
}
