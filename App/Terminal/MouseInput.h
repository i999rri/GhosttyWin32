#pragma once

#include "Input/MouseButtonLedger.h"
#include "Input/PointerEvent.h"
#include "ghostty.h"
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <functional>
#include <utility>
#include <variant>

namespace winrt::GhosttyWin32::implementation
{
    // One surface's side of the mouse: everything the pointer tells
    // libghostty, and the pointer capture that keeps those reports
    // whole.
    //
    // The two live together because they describe one fact. The record
    // (core::input::MouseButtonLedger) says which buttons libghostty
    // has been told are down, and the capture is held for exactly as
    // long as that record is not empty -- it is what brings a release
    // outside the panel back here. Kept apart, the rule linking them
    // was split across two handlers that each held half of it.
    //
    // Everything this class says to the terminal, and the one thing it
    // asks of it, goes through Terminal, which the owner (SurfaceHost)
    // fills in when it builds the class. The decisions are here; the
    // acts are named at that one call site, so reading it says what a
    // relayed press does, what a move does and what the right-click
    // does, without descending into the handlers. The owner is also
    // where the live modifier state is read, and where the window and
    // the clipboard are. This class holds no surface.
    //
    // Handling is in three parts. The event arrives already read into
    // a move, a press or a release (input::PointerEvent). A press is
    // judged first -- Judge reads the host's state and names one of
    // three outcomes -- and only then acted on, so the acting code
    // decides nothing.
    //
    // UI thread only, like every pointer event it is fed, so nothing
    // here needs a lock. The owner calls it only while it has a
    // surface behind Terminal.
    class MouseInput
    {
    public:
        // The terminal as this class sees it: what it can be told,
        // and the one thing it can be asked.
        struct Terminal {
            // A press or release libghostty should hear.
            std::function<void(ghostty_input_mouse_state_e, ghostty_input_mouse_button_e)> reportButton;
            // Where the pointer is, relative to the panel.
            std::function<void(double x, double y)> reportPosition;
            // Wheel movement, in lines; positive is up.
            std::function<void(double lines)> scroll;
            // The right-click gesture: copy what is selected.
            std::function<void()> copySelection;
            // Whether there is anything to copy.
            std::function<bool()> hasSelection;
        };

        MouseInput(Microsoft::UI::Xaml::Controls::SwapChainPanel panel, Terminal terminal)
            : m_panel(std::move(panel))
            , m_terminal(std::move(terminal)) {}

        MouseInput(MouseInput const&) = delete;
        MouseInput& operator=(MouseInput const&) = delete;

        // Every pointer event but the wheel comes through here.
        void Handle(input::PointerEvent const& ev)
        {
            std::visit([this](auto const& e) { Handle(e); }, ev);
        }

        // The capture is what was going to deliver the releases, so
        // once it is gone they are not coming. The release path gets
        // here too, by its own ReleasePointerCapture call, and finds
        // the record already empty.
        void CaptureLost() { TakeBackEveryPress(); }

        // A canceled pointer ends its press with no release at all.
        // The capture goes after the record, so the CaptureLost it
        // raises finds nothing left to take back.
        void Canceled(Microsoft::UI::Xaml::Input::Pointer const& pointer)
        {
            TakeBackEveryPress();
            m_panel.ReleasePointerCapture(pointer);
        }

        void Wheel(Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args)
        {
            ScrollBy(args.GetCurrentPoint(m_panel).Properties().MouseWheelDelta());
        }

        // A raw wheel delta, from the panel or from an overlay above
        // it (SurfaceHost::ScrollByWheel).
        void ScrollBy(int wheelDelta)
        {
            // WHEEL_DELTA is one notch; libghostty counts in lines.
            m_terminal.scroll(static_cast<double>(wheelDelta) / 120.0);
        }

        // The surface is going away and its presses with it, so there
        // is nothing left to take back.
        void Reset() noexcept { m_ledger = {}; }

    private:
        void Handle(input::PointerMove const& e)
        {
            m_terminal.reportPosition(e.position.X, e.position.Y);
        }

        // The position goes first so the press lands on the cell under
        // the pointer.
        void Handle(input::PointerPress const& e)
        {
            m_terminal.reportPosition(e.position.X, e.position.Y);
            switch (Judge(e)) {
            case PressVerdict::Copy:        m_terminal.copySelection(); break;
            case PressVerdict::Relay:       Relay(e);                               break;
            case PressVerdict::AlreadyDown:                                         break;
            }
        }

        void Handle(input::PointerRelease const& e)
        {
            m_terminal.reportPosition(e.position.X, e.position.Y);
            // A release whose press never went out -- the right-click
            // that copied, a press that landed before the owner had a
            // surface -- is not reported.
            if (m_ledger.Release(e.button)) {
                m_terminal.reportButton(GHOSTTY_MOUSE_RELEASE, e.button);
            }
            if (!m_ledger.AnyOutstanding()) m_panel.ReleasePointerCapture(e.pointer);
        }

        // What this host does with a press, decided from its own state
        // alone: the ledger and whether the terminal has a selection.
        enum class PressVerdict {
            Copy,          // this terminal's copy gesture; libghostty hears nothing
            Relay,         // record it, hold the capture, tell libghostty
            AlreadyDown,   // a press libghostty has already heard
        };

        PressVerdict Judge(input::PointerPress const& e) const
        {
            if (m_ledger.IsOutstanding(e.button)) return PressVerdict::AlreadyDown;

            // A right button pressed on its own, over a selection,
            // copies instead of reaching the terminal. Under a held
            // left one it reaches the terminal, because copying would
            // clear the selection the left one is drawing.
            if (e.button == GHOSTTY_MOUSE_RIGHT && !m_ledger.AnyOutstanding()
                && m_terminal.hasSelection()) {
                return PressVerdict::Copy;
            }
            return PressVerdict::Relay;
        }

        // Record the press, hold the capture if it is the first, tell
        // libghostty. Judge has already said this is the thing to do.
        void Relay(input::PointerPress const& e)
        {
            bool const firstButtonDown = !m_ledger.AnyOutstanding();
            m_ledger.Press(e.button);

            // Capture so the release still arrives when the pointer has
            // left the panel by then, which is also what lets a
            // selection keep growing past the pane's edge: ghostty
            // reads a negative position as a drag that should scroll
            // (external/ghostty/src/Surface.zig:4555). Best effort -- a
            // refused capture costs the drag, not the click.
            if (firstButtonDown) m_panel.CapturePointer(e.pointer);

            m_terminal.reportButton(GHOSTTY_MOUSE_PRESS, e.button);
        }

        // Before libghostty reads the next move as a drag of a button
        // nobody is holding.
        void TakeBackEveryPress()
        {
            while (auto button = m_ledger.ReleaseAny()) {
                m_terminal.reportButton(GHOSTTY_MOUSE_RELEASE, *button);
            }
        }

        Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
        Terminal m_terminal;
        core::input::MouseButtonLedger m_ledger;
    };
}
