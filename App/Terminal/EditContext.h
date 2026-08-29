#pragma once

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Text.Core.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace winrt::GhosttyWin32::implementation
{
    // Wraps one CoreTextEditContext — the object through which the
    // Windows text-services stack (IME) talks to a text field that
    // draws itself. This is the only place the raw WinRT context is
    // touched; everything the rest of the app can do with it, or can
    // have happen to it, is spelled out here:
    //
    //   what it does      registers this element as a text field with
    //                     the text-services manager
    //   what you can do   Engage / Disengage — make it (or stop it
    //                     being) the field the OS types into;
    //                     SetHandlers — say what happens on each of
    //                     the seven events, as one unit; Release —
    //                     drop it
    //   what can happen   the seven events in Handlers, on the UI
    //                     thread, only while a context exists
    //
    // Timing is owned here too: a context can only register once its
    // element is in the live visual tree (one made earlier silently
    // fails — the "first tab can't toggle 半角/全角" bug), so the
    // wrapper waits for Loaded and creates it then. Anything asked
    // before that (Engage / Disengage, SetHandlers) is remembered and
    // applied on creation. A reparent's second Loaded is a no-op.
    //
    // UI thread only. Event handlers are revoked on Release and in the
    // destructor, so they never fire into a dead owner.
    class EditContext
    {
    public:
        using TextRequestedHandler = std::function<winrt::hstring()>;
        using SelectionRequestedHandler = std::function<int32_t()>;
        using TextUpdatingHandler =
            std::function<void(int32_t start, int32_t end, winrt::hstring const& text)>;
        using CompositionHandler = std::function<void()>;
        using LayoutRequestedHandler =
            std::function<std::optional<winrt::Windows::Foundation::Rect>()>;
        using FocusRemovedHandler = std::function<void()>;

        // Everything that can happen, as one unit: the seven only make
        // sense together, so they are installed together (SetHandlers)
        // and a missing one is a visibly empty field at the call site,
        // not a forgotten call. An empty function means "not
        // interested".
        struct Handlers
        {
            // The OS wants the field's full text (padded composition
            // buffer).
            TextRequestedHandler textRequested;
            // The OS wants the caret position within that text.
            SelectionRequestedHandler selectionRequested;
            // The OS replaced [start, end) with `text`.
            TextUpdatingHandler textUpdating;
            CompositionHandler compositionStarted;
            CompositionHandler compositionCompleted;
            // Where to anchor the candidate window, in screen
            // coordinates; nullopt = unknown, leave it where it is.
            LayoutRequestedHandler layoutRequested;
            // The OS moved input elsewhere mid-composition.
            FocusRemovedHandler focusRemoved;
        };

        explicit EditContext(Microsoft::UI::Xaml::FrameworkElement element);
        ~EditContext();

        EditContext(EditContext const&) = delete;
        EditContext& operator=(EditContext const&) = delete;

        // Whether the WinRT context exists (Loaded has fired and
        // Release has not).
        bool HasContext() const noexcept { return m_context != nullptr; }

        // Become the field the OS types into: keystrokes that the IME
        // turns into text, and the composition itself, come to this
        // context's handlers. Only one context per view is engaged at
        // a time — the last Engage wins. Remembered if no context
        // exists yet.
        void Engage() { SetEngaged(true); }
        // Stop being that field. The OS routes text to whichever
        // context engages next, or nowhere.
        void Disengage() { SetEngaged(false); }
        // What the OS has actually been told (Engage before Loaded is
        // remembered, not yet applied).
        bool IsEngaged() const noexcept { return m_engaged; }

        // Install the handlers. Takes effect at once — the subscription
        // on the WinRT context is made when the context is created and
        // reads the current handlers at fire time. Replaces any earlier
        // set; `{}` clears all seven.
        void SetHandlers(Handlers handlers) noexcept { m_handlers = std::move(handlers); }

        // Drop the context, releasing OS focus if this one holds it.
        // Handlers and the engagement wish are kept; a later Loaded
        // recreates the context and applies both.
        void Release();

    private:
        using Context = winrt::Windows::UI::Text::Core::CoreTextEditContext;

        void OnLoaded();
        void BindHandlers();
        void SetEngaged(bool engaged);
        void ApplyEngagement();

        Microsoft::UI::Xaml::FrameworkElement m_element{ nullptr };
        winrt::event_token m_loadedToken{};

        Context m_context{ nullptr };
        Handlers m_handlers;
        bool m_wantEngaged = false;
        bool m_engaged = false;

        Context::TextRequested_revoker m_textRequested;
        Context::SelectionRequested_revoker m_selectionRequested;
        Context::TextUpdating_revoker m_textUpdating;
        Context::CompositionStarted_revoker m_compositionStarted;
        Context::CompositionCompleted_revoker m_compositionCompleted;
        Context::LayoutRequested_revoker m_layoutRequested;
        Context::FocusRemoved_revoker m_focusRemoved;
    };
}
