#pragma once

#include <winrt/Windows.Foundation.h>
#include <cstdint>
#include <functional>
#include <optional>

namespace winrt::GhosttyWin32::implementation
{
    // Everything that can happen on an edit context, as one unit: the
    // seven only make sense together, so they are installed together
    // (IEditContextEvents::SetHandlers) and a missing one is a
    // visibly empty field at the call site, not a forgotten call. An
    // empty function means "not interested". All fire on the UI
    // thread, only while a context exists.
    struct EditContextHandlers
    {
        using TextRequested = std::function<winrt::hstring()>;
        using SelectionRequested = std::function<int32_t()>;
        using TextUpdating =
            std::function<void(int32_t start, int32_t end, winrt::hstring const& text)>;
        using Composition = std::function<void()>;
        using LayoutRequested =
            std::function<std::optional<winrt::Windows::Foundation::Rect>()>;
        using FocusRemoved = std::function<void()>;

        // The OS wants the field's full text (padded composition
        // buffer).
        TextRequested textRequested;
        // The OS wants the caret position within that text.
        SelectionRequested selectionRequested;
        // The OS replaced [start, end) with `text`.
        TextUpdating textUpdating;
        Composition compositionStarted;
        Composition compositionCompleted;
        // Where to anchor the candidate window, in screen coordinates;
        // nullopt = unknown, leave it where it is.
        LayoutRequested layoutRequested;
        // The OS moved input elsewhere mid-composition.
        FocusRemoved focusRemoved;
    };

    // The events of an edit context, as seen by whoever handles them:
    // the one thing ImeSession needs from a context is somewhere to
    // install its handlers. EditContext implements it over the WinRT
    // type; tests implement it with a struct that just keeps what it
    // was given and fires the events by hand.
    class IEditContextEvents
    {
    public:
        virtual ~IEditContextEvents() = default;
        // Replaces any earlier set; `{}` clears all seven.
        virtual void SetHandlers(EditContextHandlers handlers) = 0;
    };
}
