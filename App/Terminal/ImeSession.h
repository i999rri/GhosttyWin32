#pragma once

#include "Host/ImeBuffer.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Text.Core.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace winrt::GhosttyWin32::implementation
{
    // One surface's conversation with the Windows text-services stack:
    // a CoreTextEditContext, its seven event handlers, and the
    // composition buffer (core::host::ImeBuffer) they maintain. It
    // speaks the IME protocol and nothing else — it never sees the
    // surface. What the composed text should do leaves through three
    // callbacks the owner (SurfaceHost) supplies:
    //
    //   onPreedit(text)   the composition changed; empty = clear it
    //   onCommit(text)    a composition was committed
    //   caretRect()       where the candidate window should anchor, in
    //                     screen coordinates; nullopt = don't know yet
    //
    // Engagement (which EditContext the OS routes input to) is the
    // owner's policy too — it calls SetEngaged from its focus logic.
    //
    // The one dependency it has, it waits for itself: a
    // CoreTextEditContext only registers with the text-services
    // manager once the view's XAML is live, so the session subscribes
    // to its element's Loaded and brings the context up then. Until
    // that moment it remembers what was asked of it (SetEngaged) and
    // applies it on arrival, so nothing the owner does before Loaded
    // is lost — and the owner never has to know the rule.
    //
    // Lifetime: owned through a shared_ptr (use Create); every handler
    // captures a weak_ptr and no-ops once the session is gone (the OS
    // can deliver a queued event after Reset).
    class ImeSession : public std::enable_shared_from_this<ImeSession>
    {
    public:
        using TextCallback = std::function<void(std::string const& utf8)>;
        using CaretRectCallback =
            std::function<std::optional<winrt::Windows::Foundation::Rect>()>;

        // `element` is what the session waits on: the context is
        // created when it fires Loaded (again after a reparent, which
        // is a no-op once the context exists).
        static std::shared_ptr<ImeSession> Create(
            Microsoft::UI::Xaml::FrameworkElement const& element);
        ~ImeSession();

        ImeSession(ImeSession const&) = delete;
        ImeSession& operator=(ImeSession const&) = delete;

        void SetOnPreedit(TextCallback cb) noexcept { m_onPreedit = std::move(cb); }
        void SetOnCommit(TextCallback cb) noexcept { m_onCommit = std::move(cb); }
        void SetCaretRect(CaretRectCallback cb) noexcept { m_caretRect = std::move(cb); }

        // Route input to this context (true) or release it (false).
        // Remembered and applied when the context comes up if asked
        // before Loaded.
        void SetEngaged(bool engaged);

        // True while a composition is in flight — keystrokes then
        // belong to the IME, not the terminal.
        bool Composing() const noexcept { return m_buffer.composing(); }

        // Release the context and forget the composition. Idempotent.
        // A later Loaded (reparent) brings the context back.
        void Reset();

    private:
        explicit ImeSession(Microsoft::UI::Xaml::FrameworkElement element) noexcept
            : m_element(std::move(element)) {}
        // Builds a context with all seven handlers bound to `weak`.
        // A function of its input: it touches no member, so
        // EnsureContext is just "if none, take one".
        static winrt::Windows::UI::Text::Core::CoreTextEditContext
        CreateContext(std::weak_ptr<ImeSession> weak);
        void EnsureContext();
        void ApplyEngagement();

        Microsoft::UI::Xaml::FrameworkElement m_element{ nullptr };
        winrt::event_token m_loadedToken{};
        // What the owner asked for (its focus policy — never changed
        // here) versus what has been told to the OS. They differ
        // before Loaded and are reconciled by ApplyEngagement.
        bool m_wantEngaged = false;
        bool m_engaged = false;

        core::host::ImeBuffer m_buffer;
        winrt::Windows::UI::Text::Core::CoreTextEditContext m_context{ nullptr };

        TextCallback m_onPreedit;
        TextCallback m_onCommit;
        CaretRectCallback m_caretRect;
    };
}
