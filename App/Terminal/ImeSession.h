#pragma once

#include "Host/ImeBuffer.h"
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
    // Lifetime: owned through a shared_ptr; every handler captures a
    // weak_ptr and no-ops once the session is gone (the OS can deliver
    // a queued event after Reset).
    class ImeSession : public std::enable_shared_from_this<ImeSession>
    {
    public:
        using TextCallback = std::function<void(std::string const& utf8)>;
        using CaretRectCallback =
            std::function<std::optional<winrt::Windows::Foundation::Rect>()>;

        ImeSession() = default;
        ~ImeSession() { Reset(); }

        ImeSession(ImeSession const&) = delete;
        ImeSession& operator=(ImeSession const&) = delete;

        void SetOnPreedit(TextCallback cb) noexcept { m_onPreedit = std::move(cb); }
        void SetOnCommit(TextCallback cb) noexcept { m_onCommit = std::move(cb); }
        void SetCaretRect(CaretRectCallback cb) noexcept { m_caretRect = std::move(cb); }

        // Create the EditContext and wire its handlers. Must run once
        // the owning element is in the live visual tree — registration
        // with the text-services manager silently fails earlier.
        // Idempotent.
        void Ensure();

        // Route input to this context (true) or release it (false).
        // No-op before Ensure.
        void SetEngaged(bool engaged);

        // True while a composition is in flight — keystrokes then
        // belong to the IME, not the terminal.
        bool Composing() const noexcept { return m_buffer.composing(); }

        // Release the context and forget the composition. Idempotent;
        // Ensure may be called again afterwards.
        void Reset();

    private:
        core::host::ImeBuffer m_buffer;
        winrt::Windows::UI::Text::Core::CoreTextEditContext m_context{ nullptr };

        TextCallback m_onPreedit;
        TextCallback m_onCommit;
        CaretRectCallback m_caretRect;
    };
}
