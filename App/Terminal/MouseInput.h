#pragma once

#include "Ghostty/Surface.h"
#include "Host/KeyModifiers.h"
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
    // A right-click over a selection copies instead of reaching the
    // terminal. When that happens is decided here; how is the owner's
    // (SurfaceHost), through copySelection, because copying needs the
    // window and the clipboard and this class sees neither.
    //
    // Handling is in three parts. The event arrives already read into
    // a move, a press or a release (input::PointerEvent). A press is
    // judged first -- Judge reads the host's state and names one of
    // three outcomes -- and only then acted on, so the acting code
    // decides nothing.
    //
    // UI thread only, like every pointer event it is fed, so nothing
    // here needs a lock. The owner calls it only while it has a
    // surface.
    class MouseInput
    {
    public:
        using CopySelection = std::function<void()>;

        MouseInput(Microsoft::UI::Xaml::Controls::SwapChainPanel panel,
                   core::ghostty::Surface& surface,
                   CopySelection copySelection)
            : m_panel(std::move(panel))
            , m_surface(surface)
            , m_copySelection(std::move(copySelection)) {}

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
            ghostty_input_scroll_mods_t smods = {};
            m_surface.MouseScroll(0, static_cast<double>(wheelDelta) / 120.0, smods);
        }

        // The surface is going away and its presses with it, so there
        // is nothing left to take back.
        void Reset() noexcept { m_ledger = {}; }

    private:
        void Handle(input::PointerMove const& e)
        {
            m_surface.MousePos(e.position.X, e.position.Y, core::host::currentMods());
        }

        // The position goes first so the press lands on the cell under
        // the pointer.
        void Handle(input::PointerPress const& e)
        {
            m_surface.MousePos(e.position.X, e.position.Y, core::host::currentMods());
            switch (Judge(e)) {
            case PressVerdict::Copy:        if (m_copySelection) m_copySelection(); break;
            case PressVerdict::Relay:       Relay(e);                               break;
            case PressVerdict::AlreadyDown:                                         break;
            }
        }

        void Handle(input::PointerRelease const& e)
        {
            m_surface.MousePos(e.position.X, e.position.Y, core::host::currentMods());
            // A release whose press never went out -- the right-click
            // that copied, a press that landed before the owner had a
            // surface -- is not reported.
            if (m_ledger.Release(e.button)) {
                m_surface.MouseButton(GHOSTTY_MOUSE_RELEASE, e.button, core::host::currentMods());
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
                && m_surface.HasSelection()) {
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

            m_surface.MouseButton(GHOSTTY_MOUSE_PRESS, e.button, core::host::currentMods());
        }

        // Before libghostty reads the next move as a drag of a button
        // nobody is holding.
        void TakeBackEveryPress()
        {
            while (auto button = m_ledger.ReleaseAny()) {
                m_surface.MouseButton(GHOSTTY_MOUSE_RELEASE, *button, core::host::currentMods());
            }
        }

        Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
        // The owner's member, so it follows Attach and Detach.
        core::ghostty::Surface& m_surface;
        CopySelection m_copySelection;
        core::input::MouseButtonLedger m_ledger;
    };
}
