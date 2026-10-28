#include "pch.h"
#include "AlarmPage.xaml.h"
#if __has_include("AlarmPage.g.cpp")
#include "AlarmPage.g.cpp"
#endif

#include "Controller/McControl.h"
#include "Infor/AlarmManager.h"
#include "Ui/UiUtil.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
namespace WF = winrt::Windows::Foundation;

namespace winrt::ShieldCanLaser::implementation
{
    void AlarmPage::OnRootLoaded(WF::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loaded) return;
        m_loaded = true;
        ActiveHost().Child(m_active.Build(ui::AlarmTableKind::Active));
        HistoryHost().Child(m_history.Build(ui::AlarmTableKind::History));
        OnTick();
    }

    void AlarmPage::OnTick()
    {
        if (!m_loaded) return;

        uint64_t version = scs::AlarmManager::Version();
        if (version != m_shownVersion)
        {
            m_shownVersion = version;
            m_active.Update(scs::AlarmManager::ActiveAlarms());
            m_history.Update(scs::AlarmManager::History());
        }

        ui::SetText(MessageText(), scs::McControl::Status().Message());
    }

    // Cùng đường với nút RESET trên thanh tab và nút RESET vật lý: McControl quyết định xoá được alarm nào.
    void AlarmPage::OnResetClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::RequestReset();
    }

    void AlarmPage::OnMuteClick(WF::IInspectable const&, RoutedEventArgs const&)
    {
        scs::McControl::RequestMuteBuzzer();
    }
}
