#pragma once

#include <cstdint>
#include <utility>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace winrt::GhosttyWin32::implementation::display {

// ghostty exposes its swap-chain size as raw physical pixels — both
// the initial-size config and `ghostty_surface_set_size`. XAML reports
// element bounds in DIPs; multiplying by the active DPI scale gets the
// buffer resolution ghostty wants.
//
// Centralising the conversion catches two recurring mistakes:
//
//   1. Forgetting the multiplication entirely. Passing DIPs straight
//      through halves the swap-chain resolution on a 200 % display,
//      so the rendered content lands in a buffer half the panel's
//      physical footprint and shows up shrunk into the corner with a
//      black margin.
//
//   2. Treating a transient 0 scale as the real value. Both
//      `SwapChainPanel.CompositionScale*` and
//      `XamlRoot.RasterizationScale` can report 0 before composition
//      has picked up the element (first frame on RDP, before the
//      `CompositionScaleChanged` hook fires). Multiplying width by 0
//      yields a 0-pixel swap chain, which crashes some DXGI paths.
//      The fallback to 1.0 here matches what every existing call site
//      already did ad-hoc; the eventual scale-change event re-publishes
//      the correct value via `ghostty_surface_set_size`.
//
// Type dispatch:
//
//   - `EffectiveScales(SwapChainPanel)` uses the panel's per-panel
//     `CompositionScaleX/Y`. Granular and matches what gets passed to
//     `ghostty_surface_set_content_scale` from the panel's own
//     `CompositionScaleChanged` event, so we stay consistent with that
//     code path.
//
//   - `EffectiveScales(FrameworkElement)` (the generic overload) uses
//     `XamlRoot.RasterizationScale`. Same value as composition scale
//     in normal monitor-DPI scenarios; works for any element that's
//     attached to a XamlRoot.
//
//   - `ToPhysicalPixels` / `MeasuredPhysical` are templates that
//     instantiate over whatever the caller passes — the right
//     `EffectiveScales` overload is picked at compile time based on
//     the argument's static type. No runtime `try_as` cost.
struct PhysicalSize {
    uint32_t width;
    uint32_t height;
};

// Specialised resolver: `SwapChainPanel` has its own per-panel
// composition scale, which can transiently differ from the root
// rasterization scale (e.g. during the first frame on RDP before
// composition settles). Use it directly when the caller knows the
// element is a SwapChainPanel.
inline std::pair<double, double> EffectiveScales(
    winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel)
{
    double sx = panel.CompositionScaleX();
    double sy = panel.CompositionScaleY();
    if (sx > 0.0 && sy > 0.0) return { sx, sy };
    if (auto root = panel.XamlRoot()) {
        double s = root.RasterizationScale();
        if (s > 0.0) return { s, s };
    }
    return { 1.0, 1.0 };
}

// Generic resolver: any other `FrameworkElement` (Grid, ContentPresenter,
// etc.) doesn't expose composition scale; the root's rasterization
// scale is the correct equivalent.
inline std::pair<double, double> EffectiveScales(
    winrt::Microsoft::UI::Xaml::FrameworkElement const& element)
{
    if (auto root = element.XamlRoot()) {
        double s = root.RasterizationScale();
        if (s > 0.0) return { s, s };
    }
    return { 1.0, 1.0 };
}

// Convert an explicit (width, height) DIP pair to physical pixels
// using `element`'s scale. Useful when the caller has a DIP
// measurement from a different source (e.g. SizeChanged's NewSize, or
// a halved source-pane width) than the element's own ActualWidth.
//
// Templated so the right `EffectiveScales` overload is selected at
// compile time from the argument's static type.
template <typename TElement>
inline PhysicalSize ToPhysicalPixels(
    TElement const& element,
    double widthDips,
    double heightDips)
{
    auto [sx, sy] = EffectiveScales(element);
    return {
        static_cast<uint32_t>(widthDips  * sx),
        static_cast<uint32_t>(heightDips * sy),
    };
}

// `element`'s own measured size (ActualWidth/Height) converted to
// physical pixels. The common case for "size this swap chain at the
// pane's current bounds". 0 when the element hasn't been measured
// yet.
template <typename TElement>
inline PhysicalSize MeasuredPhysical(TElement const& element)
{
    return ToPhysicalPixels(element, element.ActualWidth(), element.ActualHeight());
}

}  // namespace winrt::GhosttyWin32::implementation::display
