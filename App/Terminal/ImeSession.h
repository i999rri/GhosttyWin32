#pragma once

#include "Host/ImeBuffer.h"
#include "Terminal/EditContext.h"
#include <winrt/Windows.Foundation.h>
#include <functional>
#include <optional>
#include <string>

namespace winrt::GhosttyWin32::implementation
{
    // One surface's side of the IME conversation: the composition
    // buffer (core::host::ImeBuffer) and what to do on each of the
    // EditContext's events. It never touches the WinRT context — that
    // is EditContext's job — and never sees the surface. What the
    // composed text should do leaves through three callbacks the
    // owner (SurfaceHost) supplies:
    //
    //   onPreedit(text)   the composition changed; empty = clear it
    //   onCommit(text)    a composition was committed
    //   caretRect()       where the candidate window should anchor, in
    //                     screen coordinates; nullopt = don't know yet
    //
    // Engagement is the owner's policy and goes straight to the
    // EditContext (SetEngaged); this class only cares about text.
    //
    // UI thread only. Plain value: the handlers it installs on the
    // EditContext capture `this`, and the EditContext revokes them
    // before either object can be destroyed (SurfaceHost owns both,
    // context first).
    class ImeSession
    {
    public:
        using TextCallback = std::function<void(std::string const& utf8)>;
        using CaretRectCallback =
            std::function<std::optional<winrt::Windows::Foundation::Rect>()>;

        // Installs this session's handlers on `context`. `context`
        // must outlive the session.
        explicit ImeSession(EditContext& context);
        ~ImeSession();

        ImeSession(ImeSession const&) = delete;
        ImeSession& operator=(ImeSession const&) = delete;

        void SetOnPreedit(TextCallback cb) noexcept { m_onPreedit = std::move(cb); }
        void SetOnCommit(TextCallback cb) noexcept { m_onCommit = std::move(cb); }
        void SetCaretRect(CaretRectCallback cb) noexcept { m_caretRect = std::move(cb); }

        // True while a composition is in flight — keystrokes then
        // belong to the IME, not the terminal.
        bool Composing() const noexcept { return m_buffer.composing(); }

        // Forget the composition in progress, if any.
        void Reset() { m_buffer.reset(); }

    private:
        EditContext& m_context;
        core::host::ImeBuffer m_buffer;

        TextCallback m_onPreedit;
        TextCallback m_onCommit;
        CaretRectCallback m_caretRect;
    };
}
