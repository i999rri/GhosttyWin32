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

#include "FrostedBackdrop.g.h"

namespace winrt::GhosttyWin32::implementation
{
    // See FrostedBackdrop in the IDL: the raw host backdrop brush,
    // no material layered on top. Requires the window to opt in to
    // host-backdrop sampling (DWMWA_USE_HOSTBACKDROPBRUSH — set in
    // MainWindow::ApplyBackgroundOpacityAppearance, which owns the
    // HWND); without it the brush renders empty.
    struct FrostedBackdrop : FrostedBackdropT<FrostedBackdrop>
    {
        FrostedBackdrop() = default;

        void OnTargetConnected(
            winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
            winrt::Microsoft::UI::Xaml::XamlRoot const& xamlRoot);
        void OnTargetDisconnected(
            winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target);
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct FrostedBackdrop : FrostedBackdropT<FrostedBackdrop, implementation::FrostedBackdrop>
    {
    };
}
