#pragma once

// Hand-written IGraphicsEffect description of the D2D1 Gaussian
// blur, so GaussianBlurBackdrop can build a variable-radius blur
// without pulling in the Win2D NuGet — Win2D's only role in the
// usual recipe is authoring exactly this description object.
//
// The composition engine consumes the description through the
// classic-COM IGraphicsEffectD2D1Interop interface: it asks for the
// D2D effect CLSID, its property values (boxed as IPropertyValue),
// and the source chain. CompositionEffectFactory validates all of
// it at creation time, so a mistake here fails fast with an hresult
// rather than rendering garbage.

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <windows.foundation.h>
#include <windows.graphics.effects.interop.h>
#include <d2d1effects.h>

namespace winrt::GhosttyWin32::implementation
{
    struct GaussianBlurEffect : winrt::implements<GaussianBlurEffect,
        winrt::Windows::Graphics::Effects::IGraphicsEffect,
        winrt::Windows::Graphics::Effects::IGraphicsEffectSource,
        ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop>
    {
        // D2D takes a standard deviation; callers set this directly
        // (the radius-to-sigma conversion lives at the call site,
        // next to the config semantics it belongs to).
        float StandardDeviation = 3.0f;
        winrt::Windows::Graphics::Effects::IGraphicsEffectSource Source{ nullptr };

        // ----- IGraphicsEffect -----
        winrt::hstring Name() const { return m_name; }
        void Name(winrt::hstring const& name) { m_name = name; }

        // ----- IGraphicsEffectD2D1Interop -----
        HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept final
        {
            if (!id) return E_POINTER;
            *id = CLSID_D2D1GaussianBlur;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(
            LPCWSTR name, UINT* index,
            ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept final
        {
            if (!name || !index || !mapping) return E_POINTER;
            if (_wcsicmp(name, L"BlurAmount") == 0) {
                *index = D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION;
                *mapping = ABI::Windows::Graphics::Effects::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
                return S_OK;
            }
            return E_INVALIDARG;
        }

        HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept final
        {
            if (!count) return E_POINTER;
            // The full D2D1 Gaussian property set, in enum order:
            // standard deviation, optimization, border mode.
            *count = 3;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetProperty(
            UINT index, ABI::Windows::Foundation::IPropertyValue** value) noexcept final
        {
            if (!value) return E_POINTER;
            try {
                winrt::Windows::Foundation::IInspectable boxed{ nullptr };
                namespace wf = winrt::Windows::Foundation;
                switch (index) {
                case D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION:
                    boxed = wf::PropertyValue::CreateSingle(StandardDeviation);
                    break;
                case D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION:
                    boxed = wf::PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
                    break;
                case D2D1_GAUSSIANBLUR_PROP_BORDER_MODE:
                    // HARD keeps the blur from sampling transparent
                    // black outside the window bounds, which would
                    // darken the edges.
                    boxed = wf::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD);
                    break;
                default:
                    return E_BOUNDS;
                }
                *value = boxed.as<ABI::Windows::Foundation::IPropertyValue>().detach();
                return S_OK;
            } catch (...) {
                return winrt::to_hresult();
            }
        }

        HRESULT STDMETHODCALLTYPE GetSource(
            UINT index, ABI::Windows::Graphics::Effects::IGraphicsEffectSource** source) noexcept final
        {
            if (!source) return E_POINTER;
            if (index != 0 || !Source) return E_BOUNDS;
            try {
                *source = Source.as<ABI::Windows::Graphics::Effects::IGraphicsEffectSource>().detach();
                return S_OK;
            } catch (...) {
                return winrt::to_hresult();
            }
        }

        HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept final
        {
            if (!count) return E_POINTER;
            *count = 1;
            return S_OK;
        }

    private:
        winrt::hstring m_name{ L"GaussianBlur" };
    };
}
