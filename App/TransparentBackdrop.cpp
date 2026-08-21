#include "pch.h"
// initguid.h makes the DEFINE_GUID in d2d1effects.h (reached via
// GaussianBlurEffect.h) emit the actual CLSID_D2D1GaussianBlur
// definition in this TU, so no extra GUID lib needs linking. It has
// to come before the include chain that pulls in d2d1effects.h.
#include <initguid.h>
#include "TransparentBackdrop.h"
#include "GaussianBlurEffect.h"
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
#if __has_include("GaussianBlurBackdrop.g.cpp")
#include "GaussianBlurBackdrop.g.cpp"
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

    void GaussianBlurBackdrop::OnTargetConnected(
        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target,
        winrt::Microsoft::UI::Xaml::XamlRoot const& /*xamlRoot*/)
    {
        // No base call, same reasoning as TransparentBackdrop: a
        // constant blur has no theme/activation policy to react to.
        namespace wuc = winrt::Windows::UI::Composition;
        try {
            wuc::Compositor compositor;

            auto effect = winrt::make_self<GaussianBlurEffect>();
            // ghostty's radius follows the macOS CGS blur-radius
            // convention (pixels); D2D takes a standard deviation with
            // the documented relation radius = 3 * sigma.
            effect->StandardDeviation = m_radius / 3.0f;
            effect->Source = wuc::CompositionEffectSourceParameter{ L"backdrop" };

            auto factory = compositor.CreateEffectFactory(
                effect.as<winrt::Windows::Graphics::Effects::IGraphicsEffect>());
            auto brush = factory.CreateBrush();
            // Host backdrop = what is behind the WINDOW (desktop,
            // other apps), as opposed to CreateBackdropBrush's
            // behind-the-visual-within-this-window.
            brush.SetSourceParameter(L"backdrop", compositor.CreateHostBackdropBrush());
#if 1
            // DIAGNOSTIC step 2 (pre-merge): the effect brush connects
            // but the picture doesn't change with sigma. Substitute an
            // unconditional red brush: red on screen = the SystemBackdrop
            // brush is what's visible (effect binding is the bug); the
            // same fixed blur = DWM's host-backdrop layer is what's
            // visible and our brush contributes nothing (wiring is the
            // bug).
            target.SystemBackdrop(compositor.CreateColorBrush(
                winrt::Windows::UI::Color{ 128, 255, 0, 0 }));
            OutputDebugStringW(L"GaussianBlurBackdrop: DIAGNOSTIC red brush connected\n");
#else
            target.SystemBackdrop(brush);
            OutputDebugStringW(L"GaussianBlurBackdrop: effect brush connected\n");
#endif
        } catch (winrt::hresult_error const& e) {
            // DIAGNOSTIC (pre-merge): an unmissable red backdrop +
            // debug output instead of a silent fallback, so a failed
            // effect chain can't masquerade as "blur looks the same".
            OutputDebugStringW(
                (L"GaussianBlurBackdrop FAILED: " + e.message() + L"\n").c_str());
            wuc::Compositor fallback;
            target.SystemBackdrop(fallback.CreateColorBrush(
                winrt::Windows::UI::Color{ 128, 255, 0, 0 }));
        }
    }

    void GaussianBlurBackdrop::OnTargetDisconnected(
        winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop const& target)
    {
        target.SystemBackdrop(nullptr);
    }
}
