#pragma once

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.UI.Text.Core.h>
#include <functional>
#include <vector>

namespace winrt::GhosttyWin32::implementation
{
    // Produces one CoreTextEditContext at the only moment it can be
    // made: after the element it belongs to is in the live visual
    // tree. A context created earlier silently fails to register with
    // the text-services manager (the "first tab can't toggle 半角/全角"
    // bug), so this class owns that timing and nothing else — what to
    // do with the context is the subscriber's business (ImeSession).
    //
    // WhenReady hands the context over as soon as it exists: at once
    // if Loaded already fired, otherwise when it does. A reparent
    // (tab tear-out / adopt) fires Loaded again; the existing context
    // is kept and nothing is re-delivered.
    //
    // UI thread only. The Loaded handler captures `this` raw: it is
    // revoked in the destructor on the same thread, so it cannot fire
    // afterwards.
    class EditContextSource
    {
    public:
        using Context = winrt::Windows::UI::Text::Core::CoreTextEditContext;

        explicit EditContextSource(Microsoft::UI::Xaml::FrameworkElement element);
        ~EditContextSource();

        EditContextSource(EditContextSource const&) = delete;
        EditContextSource& operator=(EditContextSource const&) = delete;

        void WhenReady(std::function<void(Context const&)> fn);

        // Drop the context. Subscribers keep whatever reference they
        // took; a later Loaded makes a fresh one and delivers it again.
        void Release();

    private:
        void OnLoaded();

        Microsoft::UI::Xaml::FrameworkElement m_element{ nullptr };
        winrt::event_token m_loadedToken{};
        Context m_context{ nullptr };
        std::vector<std::function<void(Context const&)>> m_waiting;
    };
}
