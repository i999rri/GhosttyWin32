#include "pch.h"
#include "TransparentBackdrop.h"
// Neither is in pch. Note the namespace split: the target interface
// lives in Microsoft.UI.Composition, but its SystemBackdrop property
// (WinAppSDK 1.8 projection) takes a brush from the SYSTEM
// compositor's family — Windows.UI.Composition — so the brush has to
// be created by a Windows::UI::Composition::Compositor.
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Windows.UI.Composition.h>
#if __has_include("TransparentBackdrop.g.cpp")
#include "TransparentBackdrop.g.cpp"
#endif
#if __has_include("FrostedBackdrop.g.cpp")
#include "FrostedBackdrop.g.cpp"
#endif

namespace winrt::GhosttyWin32::implementation
{
    void TransparentBackdrop::OnTargetConnected(
        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
        winrt::Microsoft::UI::Xaml::XamlRoot const& /*xamlRoot*/)
    {
        // Deliberately no base call: the base class's bookkeeping
        // feeds SystemBackdropConfiguration policy updates (theme,
        // activation), and a constant transparent brush has no
        // policy to react to.
        //
        // The brush must come from the system compositor family
        // (Windows.UI.Composition); the brush keeps its compositor
        // alive, so a local is fine.
        winrt::Windows::UI::Composition::Compositor compositor;
        target.SystemBackdrop(compositor.CreateColorBrush(
            winrt::Windows::UI::Color{ 0, 0, 0, 0 }));
    }

    void TransparentBackdrop::OnTargetDisconnected(
        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target)
    {
        target.SystemBackdrop(nullptr);
    }

    void FrostedBackdrop::OnTargetConnected(
        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
        winrt::Microsoft::UI::Xaml::XamlRoot const& /*xamlRoot*/)
    {
        // No base call, same reasoning as TransparentBackdrop: a
        // constant brush has no theme/activation policy to react to.
        //
        // The host backdrop brush is the behind-window imagery DWM
        // provides — pre-blurred at a fixed strength by design
        // (apps must not be able to read other windows' pixels; see
        // the #165 investigation). That fixed frost is exactly what
        // background-blur wants, without the noise texture and
        // milky wash the acrylic MATERIAL adds on top of the same
        // source.
        winrt::Windows::UI::Composition::Compositor compositor;
        target.SystemBackdrop(compositor.CreateHostBackdropBrush());
    }

    void FrostedBackdrop::OnTargetDisconnected(
        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target)
    {
        target.SystemBackdrop(nullptr);
    }
}
