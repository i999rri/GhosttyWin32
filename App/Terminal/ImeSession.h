#pragma once

#include "Host/ImeBuffer.h"
#include "Terminal/EditContextSource.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Text.Core.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace winrt::GhosttyWin32::implementation
{
    // One surface's conversation with the Windows text-services stack:
    // the seven CoreTextEditContext event handlers and the composition
    // buffer (core::host::ImeBuffer) they maintain. It speaks the IME
    // protocol and nothing else — it never sees the surface, and it
    // does not know when a context can exist (EditContextSource does;
    // this session binds to whatever context the source delivers).
    //
    // What the composed text should do leaves through three callbacks
    // the owner (SurfaceHost) supplies:
    //
    //   onPreedit(text)   the composition changed; empty = clear it
    //   onCommit(text)    a composition was committed
    //   caretRect()       where the candidate window should anchor, in
    //                     screen coordinates; nullopt = don't know yet
    //
    // Engagement (which context the OS routes input to) is the owner's
    // policy: it calls SetEngaged from its focus logic. Asked before a
    // context exists, the wish is remembered and applied on delivery.
    //
    // Lifetime: owned through a shared_ptr (use Create); every handler
    // captures a weak_ptr and no-ops once the session is gone (the OS
    // can deliver a queued event after Reset).
    class ImeSession : public std::enable_shared_from_this<ImeSession>
    {
    public:
        using Context = EditContextSource::Context;
        using TextCallback = std::function<void(std::string const& utf8)>;
        using CaretRectCallback =
            std::function<std::optional<winrt::Windows::Foundation::Rect>()>;

        // A session that takes whatever context `source` delivers (now
        // or on Loaded) through AttachContext. `source` must outlive
        // the session.
        static std::shared_ptr<ImeSession> Create(EditContextSource& source);
        ~ImeSession() { Reset(); }

        ImeSession(ImeSession const&) = delete;
        ImeSession& operator=(ImeSession const&) = delete;

        // The context is supplied from outside (EditContextSource); the
        // session never makes one. Binds the seven handlers to it and
        // applies any engagement asked for before it arrived. Attaching
        // a second context replaces the first.
        void AttachContext(Context const& context);
        // Whether a context has been attached (and not yet Reset).
        // Before that, SetEngaged is remembered rather than applied
        // and Composing() is necessarily false.
        bool HasContext() const noexcept { return m_context != nullptr; }

        void SetOnPreedit(TextCallback cb) noexcept { m_onPreedit = std::move(cb); }
        void SetOnCommit(TextCallback cb) noexcept { m_onCommit = std::move(cb); }
        void SetCaretRect(CaretRectCallback cb) noexcept { m_caretRect = std::move(cb); }

        // Route input to this context (true) or release it (false).
        void SetEngaged(bool engaged);

        // True while a composition is in flight — keystrokes then
        // belong to the IME, not the terminal.
        bool Composing() const noexcept { return m_buffer.composing(); }

        // Let go of the context (releasing OS focus if this session
        // holds it) and forget the composition. Idempotent. The owner's
        // engagement wish is left alone.
        void Reset();

    private:
        ImeSession() = default;
        void ApplyEngagement();

        // What the owner asked for (its focus policy — never changed
        // here) versus what has been told to the OS. They differ
        // before a context exists and are reconciled by
        // ApplyEngagement.
        bool m_wantEngaged = false;
        bool m_engaged = false;

        core::host::ImeBuffer m_buffer;
        Context m_context{ nullptr };

        TextCallback m_onPreedit;
        TextCallback m_onCommit;
        CaretRectCallback m_caretRect;
    };
}
