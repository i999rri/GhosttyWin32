#include "pch.h"
#include "TransparentBackdrop.h"
// Neither is in pch. Note the namespace split: the target interface
// lives in Microsoft.UI.Composition, but its SystemBackdrop property
// (WinAppSDK 1.8 projection) takes a brush from the SYSTEM
// compositor's family — Windows.UI.Composition — so the brush has to
// be created by a Windows::UI::Composition::Compositor.
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Windows.UI.Composition.h>
#if __has_include("TransparentBackdrop.g.cpp")
#include "TransparentBackdrop.g.cpp"
#endif
#if __has_include("ClearAcrylicBackdrop.g.cpp")
#include "ClearAcrylicBackdrop.g.cpp"
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

    void ClearAcrylicBackdrop::OnTargetConnected(
        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
        winrt::Microsoft::UI::Xaml::XamlRoot const& /*xamlRoot*/)
    {
        namespace sb = winrt::Microsoft::UI::Composition::SystemBackdrops;
        if (!m_controller) {
            m_controller = sb::DesktopAcrylicController();
            // Zero out the material: no tint, no luminosity wash.
            // What remains is the blur of whatever is behind the
            // window, and the terminal's own background-opacity
            // provides the color on top of it.
            m_controller.TintOpacity(0.0f);
            m_controller.LuminosityOpacity(0.0f);
        }
        if (!m_configuration) {
            m_configuration = sb::SystemBackdropConfiguration();
            m_controller.SetSystemBackdropConfiguration(m_configuration);
        }
        m_controller.AddSystemBackdropTarget(target);
    }

    void ClearAcrylicBackdrop::OnTargetDisconnected(
        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target)
    {
        if (m_controller) m_controller.RemoveSystemBackdropTarget(target);
    }
}
