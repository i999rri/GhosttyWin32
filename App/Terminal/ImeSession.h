#pragma once

#include "Host/ImeBuffer.h"
#include "Interop/Encoding.h"
#include "Terminal/EditContextHandlers.h"
#include <winrt/Windows.Foundation.h>
#include <functional>
#include <optional>
#include <string>

namespace winrt::GhosttyWin32::implementation
{
    // One surface's side of the IME conversation: the composition
    // buffer (core::host::ImeBuffer) and what to do on each of the
    // edit context's events. It never touches the WinRT context — it
    // only installs handlers on an IEditContextEvents (EditContext
    // in the app, a plain struct in tests) — and never sees the
    // surface. What the composed text should do leaves through three
    // callbacks the owner (SurfaceHost) supplies:
    //
    //   onPreedit(text)   the composition changed; empty = clear it
    //   onCommit(text)    a composition was committed
    //   caretRect()       where the candidate window should anchor, in
    //                     screen coordinates; nullopt = don't know yet
    //
    // Engagement is the owner's policy and goes straight to the
    // EditContext (Engage / Disengage); this class only cares about
    // text.
    //
    // UI thread only. Plain value: the handlers it installs capture
    // `this`, so the events object must outlive the session — the destructor
    // takes them off again.
    class ImeSession
    {
    public:
        using TextCallback = std::function<void(std::string const& utf8)>;
        using CaretRectCallback =
            std::function<std::optional<winrt::Windows::Foundation::Rect>()>;

        explicit ImeSession(IEditContextEvents& events)
            : m_events(events)
        {
            m_events.SetHandlers({
                .textRequested = [this]() {
                    return winrt::hstring(m_buffer.paddedText());
                },
                .selectionRequested = [this]() {
                    return m_buffer.selectionPosition();
                },
                .textUpdating = [this](int32_t start, int32_t end, winrt::hstring const& text) {
                    m_buffer.applyTextUpdate(start, end, text.c_str(), text.size());
                    if (m_buffer.composing() && m_onPreedit) {
                        m_onPreedit(core::interop::Encoding::toUtf8(m_buffer.text()));
                    }
                },
                .compositionStarted = [this]() {
                    m_buffer.compositionStarted();
                },
                .compositionCompleted = [this]() {
                    if (m_onCommit) m_onCommit(core::interop::Encoding::toUtf8(m_buffer.text()));
                    m_buffer.compositionCompleted();
                },
                .layoutRequested = [this]() -> std::optional<winrt::Windows::Foundation::Rect> {
                    return m_caretRect ? m_caretRect() : std::nullopt;
                },
                .focusRemoved = [this]() {
                    // The OS took input away mid-composition: drop the
                    // half-typed text rather than leave a stale preedit
                    // on screen.
                    if (m_buffer.composing()) {
                        m_buffer.reset();
                        if (m_onPreedit) m_onPreedit(std::string{});
                    }
                },
            });
        }

        ~ImeSession()
        {
            // Handlers capture `this`; take them off before this object
            // is gone (the events object may outlive us by a moment).
            m_events.SetHandlers({});
        }

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
        IEditContextEvents& m_events;
        core::host::ImeBuffer m_buffer;

        TextCallback m_onPreedit;
        TextCallback m_onCommit;
        CaretRectCallback m_caretRect;
    };
}
