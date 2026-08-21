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
            // DIAGNOSTIC step 3 (pre-merge): red proved the
            // SystemBackdrop brush is the visible layer. Now swap the
            // Gaussian for a saturation-0 (grayscale) effect over the
            // same host backdrop source. Grayscale on screen = the
            // effect pipeline (including property delivery) works, so
            // the sigma invisibility is the pre-blurred/downscaled
            // host backdrop source saturating perceptually. Colors
            // unchanged = property/effect application is broken.
            struct DiagnosticSaturationEffect : winrt::implements<DiagnosticSaturationEffect,
                winrt::Windows::Graphics::Effects::IGraphicsEffect,
                winrt::Windows::Graphics::Effects::IGraphicsEffectSource,
                ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop>
            {
                winrt::Windows::Graphics::Effects::IGraphicsEffectSource Source{ nullptr };
                winrt::hstring m_name{ L"Saturation" };
                winrt::hstring Name() const { return m_name; }
                void Name(winrt::hstring const& name) { m_name = name; }
                HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept final
                { if (!id) return E_POINTER; *id = CLSID_D2D1Saturation; return S_OK; }
                HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR, UINT*,
                    ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING*) noexcept final
                { return E_INVALIDARG; }
                HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept final
                { if (!count) return E_POINTER; *count = 1; return S_OK; }
                HRESULT STDMETHODCALLTYPE GetProperty(UINT index,
                    ABI::Windows::Foundation::IPropertyValue** value) noexcept final
                {
                    if (!value) return E_POINTER;
                    if (index != 0) return E_BOUNDS;
                    try {
                        *value = winrt::Windows::Foundation::PropertyValue::CreateSingle(0.0f)
                            .as<ABI::Windows::Foundation::IPropertyValue>().detach();
                        return S_OK;
                    } catch (...) { return winrt::to_hresult(); }
                }
                HRESULT STDMETHODCALLTYPE GetSource(UINT index,
                    ABI::Windows::Graphics::Effects::IGraphicsEffectSource** source) noexcept final
                {
                    if (!source) return E_POINTER;
                    if (index != 0 || !Source) return E_BOUNDS;
                    try {
                        *source = Source.as<ABI::Windows::Graphics::Effects::IGraphicsEffectSource>().detach();
                        return S_OK;
                    } catch (...) { return winrt::to_hresult(); }
                }
                HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept final
                { if (!count) return E_POINTER; *count = 1; return S_OK; }
            };
            auto sat = winrt::make_self<DiagnosticSaturationEffect>();
            sat->Source = wuc::CompositionEffectSourceParameter{ L"backdrop" };
            auto satFactory = compositor.CreateEffectFactory(
                sat.as<winrt::Windows::Graphics::Effects::IGraphicsEffect>());
            auto satBrush = satFactory.CreateBrush();
            satBrush.SetSourceParameter(L"backdrop", compositor.CreateHostBackdropBrush());
            target.SystemBackdrop(satBrush);
            OutputDebugStringW(L"GaussianBlurBackdrop: DIAGNOSTIC saturation brush connected\n");
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
