#include "pch.h"
#include "AppBackdrop.h"
#if __has_include("AppBackdrop.g.cpp")
#include "AppBackdrop.g.cpp"
#endif

using namespace winrt;
using namespace winrt::Microsoft::UI::Composition;
using namespace winrt::Microsoft::UI::Composition::SystemBackdrops;
using namespace winrt::Microsoft::UI::Xaml;

namespace winrt::ShieldCanLaser::implementation
{
    void AppBackdrop::OnTargetConnected(ICompositionSupportsSystemBackdrop const& target, XamlRoot const& xamlRoot)
    {
        AppBackdropT<AppBackdrop>::OnTargetConnected(target, xamlRoot);

        // Không có composition GPU (vài phiên RDP): không có nền, cửa sổ giữ màu đặc.
        if (m_useAcrylic ? !DesktopAcrylicController::IsSupported() : !MicaController::IsSupported())
            return;

        // Cấu hình riêng, IsInputActive luôn true: nền không tắt khi cửa sổ mất focus.
        m_config = SystemBackdropConfiguration();
        m_config.IsInputActive(true);
        CopyTheme(target, xamlRoot);

        if (m_useAcrylic)
        {
            m_acrylic = DesktopAcrylicController();
            m_acrylic.Kind(DesktopAcrylicKind::Thin);
            m_acrylic.AddSystemBackdropTarget(target);
            m_acrylic.SetSystemBackdropConfiguration(m_config);
        }
        else
        {
            m_mica = MicaController();
            m_mica.Kind(MicaKind::Base);   // như MicaBackdrop mặc định của XAML
            m_mica.AddSystemBackdropTarget(target);
            m_mica.SetSystemBackdropConfiguration(m_config);
        }
    }

    void AppBackdrop::OnTargetDisconnected(ICompositionSupportsSystemBackdrop const& target)
    {
        AppBackdropT<AppBackdrop>::OnTargetDisconnected(target);
        if (m_acrylic)
        {
            m_acrylic.RemoveSystemBackdropTarget(target);
            m_acrylic.Close();
            m_acrylic = nullptr;
        }
        if (m_mica)
        {
            m_mica.RemoveSystemBackdropTarget(target);
            m_mica.Close();
            m_mica = nullptr;
        }
        m_config = nullptr;
    }

    void AppBackdrop::OnDefaultSystemBackdropConfigurationChanged(ICompositionSupportsSystemBackdrop const& target, XamlRoot const& xamlRoot)
    {
        if (m_config)
            CopyTheme(target, xamlRoot);
    }

    void AppBackdrop::CopyTheme(ICompositionSupportsSystemBackdrop const& target, XamlRoot const& xamlRoot)
    {
        SystemBackdropConfiguration def = GetDefaultSystemBackdropConfiguration(target, xamlRoot);
        m_config.Theme(def.Theme());
        m_config.IsHighContrast(def.IsHighContrast());
        m_config.HighContrastBackgroundColor(def.HighContrastBackgroundColor());
    }
}
