#pragma once
#include "AppBackdrop.g.h"

namespace winrt::ShieldCanLaser::implementation
{
    /// <summary>
    /// Nền cửa sổ: Mica (mặc định) hoặc Acrylic Thin, không tắt khi mất focus (Tuấn 2026-10-06: "giống app GFP Test UI";
    /// thử Acrylic rồi quay về Mica). Dùng cấu hình riêng thay cho cấu hình mặc định của XAML — cấu hình mặc định theo
    /// focus cửa sổ và thay nền bằng màu đặc mỗi khi cửa sổ khác (trình xem ảnh, công cụ của hãng) đang active. Theme /
    /// tương phản cao vẫn chép từ cấu hình mặc định. Hệ thống tắt hiệu ứng trong suốt, tiết kiệm pin hay RDP thì
    /// controller tự về màu đặc.
    /// </summary>
    struct AppBackdrop : AppBackdropT<AppBackdrop>
    {
        AppBackdrop() = default;

        bool UseAcrylic() const { return m_useAcrylic; }
        void UseAcrylic(bool value) { m_useAcrylic = value; }

        void OnTargetConnected(Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
                               Microsoft::UI::Xaml::XamlRoot const& xamlRoot);
        void OnTargetDisconnected(Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target);
        void OnDefaultSystemBackdropConfigurationChanged(Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
                                                         Microsoft::UI::Xaml::XamlRoot const& xamlRoot);

    private:
        void CopyTheme(Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
                       Microsoft::UI::Xaml::XamlRoot const& xamlRoot);

        bool m_useAcrylic{ false };
        Microsoft::UI::Composition::SystemBackdrops::MicaController m_mica{ nullptr };
        Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController m_acrylic{ nullptr };
        Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration m_config{ nullptr };
    };
}

namespace winrt::ShieldCanLaser::factory_implementation
{
    struct AppBackdrop : AppBackdropT<AppBackdrop, implementation::AppBackdrop>
    {
    };
}
