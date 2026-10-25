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

        // Every pointer event but the wheel comes through here: a
        // move, a press, a release, and a press or release that rode
        // in on a move because another button was already held. The
        // position goes first so a change lands on the cell under the
        // pointer.
        void Handle(input::PointerEvent const& ev)
        {
            m_surface.MousePos(ev.position.X, ev.position.Y, core::host::currentMods());
            if (!ev.change) return;

            auto const [button, state] = *ev.change;
            if (state == GHOSTTY_MOUSE_PRESS) Press(button, ev.pointer);
            else                              Release(button, ev.pointer);
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
        void Press(ghostty_input_mouse_button_e button,
                   Microsoft::UI::Xaml::Input::Pointer const& pointer)
        {
            bool const firstButtonDown = !m_ledger.AnyOutstanding();

            // Right-click copies a selection instead of reaching the
            // terminal. libghostty hears no press, which is also what
            // keeps it from hearing the release. Only for a right
            // button pressed on its own: under a held left one, copying
            // would clear the selection the left one is drawing.
            if (button == GHOSTTY_MOUSE_RIGHT && firstButtonDown && m_surface.HasSelection()) {
                if (m_copySelection) m_copySelection();
                return;
            }
            if (!m_ledger.Press(button)) return;

            // Capture so the release still arrives when the pointer has
            // left the panel by then, which is also what lets a
            // selection keep growing past the pane's edge: ghostty
            // reads a negative position as a drag that should scroll
            // (external/ghostty/src/Surface.zig:4555). Best effort -- a
            // refused capture costs the drag, not the click.
            if (firstButtonDown) m_panel.CapturePointer(pointer);

            m_surface.MouseButton(GHOSTTY_MOUSE_PRESS, button, core::host::currentMods());
        }

        void Release(ghostty_input_mouse_button_e button,
                     Microsoft::UI::Xaml::Input::Pointer const& pointer)
        {
            // Nothing to decide here beyond taking back a press that
            // went out: a button released without one -- the
            // right-click that copied, a press that landed before the
            // owner had a surface -- is not reported.
            bool const takenBack = m_ledger.Release(button);
            if (!m_ledger.AnyOutstanding()) m_panel.ReleasePointerCapture(pointer);
            if (!takenBack) return;

            m_surface.MouseButton(GHOSTTY_MOUSE_RELEASE, button, core::host::currentMods());
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
