#pragma once

#include "TransparentBackdrop.g.h"

namespace winrt::GhosttyWin32::implementation
{
    // See TransparentBackdrop.idl. The mechanics: a SystemBackdrop's
    // job is to hand the composition target a brush; handing it a
    // transparent CompositionColorBrush means DWM composites the
    // window's alpha directly against whatever is behind the window
    // — no blur, no tint. Same technique WinUIEx's
    // TransparentTintBackdrop uses.
    struct TransparentBackdrop : TransparentBackdropT<TransparentBackdrop>
    {
        TransparentBackdrop() = default;

        void OnTargetConnected(
            winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
            winrt::Microsoft::UI::Xaml::XamlRoot const& xamlRoot);
        void OnTargetDisconnected(
            winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target);
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct TransparentBackdrop : TransparentBackdropT<TransparentBackdrop, implementation::TransparentBackdrop>
    {
    };
}

#include "ClearAcrylicBackdrop.g.h"

namespace winrt::GhosttyWin32::implementation
{
    // See ClearAcrylicBackdrop in the IDL: DesktopAcrylicController
    // with TintOpacity / LuminosityOpacity forced to 0 — pure blur,
    // no color wash, so the terminal's own alpha stays visible
    // through it.
    struct ClearAcrylicBackdrop : ClearAcrylicBackdropT<ClearAcrylicBackdrop>
    {
        ClearAcrylicBackdrop() = default;

        void OnTargetConnected(
            winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
            winrt::Microsoft::UI::Xaml::XamlRoot const& xamlRoot);
        void OnTargetDisconnected(
            winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target);

    private:
        winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController m_controller{ nullptr };
        winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration m_configuration{ nullptr };
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct ClearAcrylicBackdrop : ClearAcrylicBackdropT<ClearAcrylicBackdrop, implementation::ClearAcrylicBackdrop>
    {
    };
}
