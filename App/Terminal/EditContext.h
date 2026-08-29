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
    //   what you can do   SetEngaged — make it (or stop it being) the
    //                     context the OS routes input to; SetOn… —
    //                     say what happens on each of the seven
    //                     events; Release — drop it
    //   what can happen   the seven SetOn… events, on the UI thread,
    //                     only while a context exists
    //
    // Timing is owned here too: a context can only register once its
    // element is in the live visual tree (one made earlier silently
    // fails — the "first tab can't toggle 半角/全角" bug), so the
    // wrapper waits for Loaded and creates it then. Anything asked
    // before that (SetEngaged, SetOn…) is remembered and applied on
    // creation. A reparent's second Loaded is a no-op.
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

        explicit EditContext(Microsoft::UI::Xaml::FrameworkElement element);
        ~EditContext();

        EditContext(EditContext const&) = delete;
        EditContext& operator=(EditContext const&) = delete;

        // Whether the WinRT context exists (Loaded has fired and
        // Release has not).
        bool HasContext() const noexcept { return m_context != nullptr; }

        // Ask for (true) or give up (false) being the context the OS
        // routes input to. Remembered if no context exists yet.
        void SetEngaged(bool engaged);
        // What the OS has actually been told.
        bool IsEngaged() const noexcept { return m_engaged; }

        // What can happen. Each takes effect at once — the subscription
        // on the WinRT context is made when the context is created and
        // reads the current handler at fire time; an empty function
        // means "not interested". ClearHandlers drops all seven.
        //
        // The OS wants the field's full text (padded composition
        // buffer).
        void SetOnTextRequested(TextRequestedHandler h) noexcept { m_handlers.textRequested = std::move(h); }
        // The OS wants the caret position within that text.
        void SetOnSelectionRequested(SelectionRequestedHandler h) noexcept { m_handlers.selectionRequested = std::move(h); }
        // The OS replaced [start, end) with `text`.
        void SetOnTextUpdating(TextUpdatingHandler h) noexcept { m_handlers.textUpdating = std::move(h); }
        void SetOnCompositionStarted(CompositionHandler h) noexcept { m_handlers.compositionStarted = std::move(h); }
        void SetOnCompositionCompleted(CompositionHandler h) noexcept { m_handlers.compositionCompleted = std::move(h); }
        // Where to anchor the candidate window, in screen coordinates;
        // nullopt = unknown, leave it where it is.
        void SetOnLayoutRequested(LayoutRequestedHandler h) noexcept { m_handlers.layoutRequested = std::move(h); }
        // The OS moved input elsewhere mid-composition.
        void SetOnFocusRemoved(FocusRemovedHandler h) noexcept { m_handlers.focusRemoved = std::move(h); }
        void ClearHandlers() noexcept { m_handlers = {}; }

        // Drop the context, releasing OS focus if this one holds it.
        // Handlers and the engagement wish are kept; a later Loaded
        // recreates the context and applies both.
        void Release();

    private:
        using Context = winrt::Windows::UI::Text::Core::CoreTextEditContext;

        struct Handlers
        {
            TextRequestedHandler textRequested;
            SelectionRequestedHandler selectionRequested;
            TextUpdatingHandler textUpdating;
            CompositionHandler compositionStarted;
            CompositionHandler compositionCompleted;
            LayoutRequestedHandler layoutRequested;
            FocusRemovedHandler focusRemoved;
        };

        void OnLoaded();
        void BindHandlers();
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
