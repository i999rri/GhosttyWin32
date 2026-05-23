#pragma once

#include <string>
#include <algorithm>
#include <cstdint>

// Pure IME buffer logic, separated from UI for testability.
// Tracks the composition buffer and an offset that accounts for
// CoreTextEditContext's accumulated caret positions across compositions.
class ImeBuffer {
public:
    // Called by TextUpdating — translates context range to local buffer and applies the edit.
    void applyTextUpdate(int32_t rangeStart, int32_t rangeEnd,
                         const wchar_t* text, size_t textLen) {
        int32_t localStart = rangeStart - m_baseOffset;
        int32_t localEnd = rangeEnd - m_baseOffset;
        int32_t bufLen = static_cast<int32_t>(m_buffer.size());

        localStart = std::clamp(localStart, 0, bufLen);
        localEnd = std::clamp(localEnd, localStart, bufLen);

        m_buffer.replace(localStart, localEnd - localStart, text, textLen);
    }

    // Called by CompositionStarted
    void compositionStarted() {
        m_composing = true;
        m_buffer.clear();
    }

    // Called by CompositionCompleted — advances offset past committed text.
    void compositionCompleted() {
        m_composing = false;
        m_baseOffset += static_cast<int32_t>(m_buffer.size());
        m_buffer.clear();
    }

    // Called by FocusRemoved or NotifyFocusEnter — full reset.
    void reset() {
        m_composing = false;
        m_buffer.clear();
        m_baseOffset = 0;
    }

    // For TextRequested — returns padded text matching context positions.
    std::wstring paddedText() const {
        std::wstring padded(m_baseOffset, L' ');
        padded += m_buffer;
        return padded;
    }

    // For SelectionRequested — caret at end of current buffer in context coords.
    int32_t selectionPosition() const {
        return m_baseOffset + static_cast<int32_t>(m_buffer.size());
    }

    const std::wstring& text() const { return m_buffer; }
    bool composing() const { return m_composing; }
    int32_t baseOffset() const { return m_baseOffset; }

private:
    std::wstring m_buffer;
    int32_t m_baseOffset = 0;
    bool m_composing = false;
};
