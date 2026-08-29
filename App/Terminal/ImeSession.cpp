#include "pch.h"
#include "Terminal/ImeSession.h"
#include "Interop/Encoding.h"

namespace winrt::GhosttyWin32::implementation
{
    namespace interop = core::interop;

    ImeSession::ImeSession(EditContext& context)
        : m_context(context)
    {
        EditContext::Handlers h;
        h.textRequested = [this]() {
            return winrt::hstring(m_buffer.paddedText());
        };
        h.selectionRequested = [this]() {
            return m_buffer.selectionPosition();
        };
        h.textUpdating = [this](int32_t start, int32_t end, winrt::hstring const& text) {
            m_buffer.applyTextUpdate(start, end, text.c_str(), text.size());
            if (m_buffer.composing() && m_onPreedit) {
                m_onPreedit(interop::Encoding::toUtf8(m_buffer.text()));
            }
        };
        h.compositionStarted = [this]() {
            m_buffer.compositionStarted();
        };
        h.compositionCompleted = [this]() {
            if (m_onCommit) m_onCommit(interop::Encoding::toUtf8(m_buffer.text()));
            m_buffer.compositionCompleted();
        };
        h.layoutRequested = [this]() -> std::optional<winrt::Windows::Foundation::Rect> {
            return m_caretRect ? m_caretRect() : std::nullopt;
        };
        h.focusRemoved = [this]() {
            // The OS took input away mid-composition: drop the
            // half-typed text rather than leave a stale preedit on
            // screen.
            if (m_buffer.composing()) {
                m_buffer.reset();
                if (m_onPreedit) m_onPreedit(std::string{});
            }
        };
        m_context.SetHandlers(std::move(h));
    }

    ImeSession::~ImeSession()
    {
        // Handlers capture `this`; take them off before this object
        // is gone (the EditContext may outlive us by a moment).
        m_context.SetHandlers({});
    }
}
