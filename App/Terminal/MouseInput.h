#pragma once

#include "Input/MouseButtonLedger.h"
#include "Input/MouseTransition.h"
#include "Input/PointerEvent.h"
#include "ghostty.h"
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <functional>
#include <utility>
#include <variant>
#include <vector>

namespace winrt::GhosttyWin32::implementation
{
    // One surface's side of the mouse: what the pointer tells the
    // terminal, and the pointer capture that keeps those reports whole.
    // The capture is held for exactly as long as the ledger has a press
    // outstanding -- it is what brings a release outside the panel back
    // here -- so the two are kept in one class.
    //
    // Nothing is decided here. Each event is handed, with the ledger as
    // it stands, to core::input::OnPress / OnRelease / OnCaptureLost /
    // OnCanceled, which return a MouseTransition: the ledger as it will
    // be, and what goes out. CarryOut then does exactly that.
    //
    // UI thread only, like every pointer event it is fed, so nothing
    // here needs a lock. The owner calls it only while a surface stands
    // behind Terminal.
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

        void CaptureLost() { CarryOut(core::input::OnCaptureLost(m_ledger), nullptr); }

        void Canceled(Microsoft::UI::Xaml::Input::Pointer const& pointer)
        {
            CarryOut(core::input::OnCanceled(m_ledger), pointer);
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
        using Transition = core::input::MouseTransition;

        void Handle(input::PointerMove const& e)
        {
            m_terminal.reportPosition(e.position.X, e.position.Y);
        }

        // The position goes first so the change lands on the cell
        // under the pointer.
        void Handle(input::PointerPress const& e)
        {
            m_terminal.reportPosition(e.position.X, e.position.Y);
            CarryOut(core::input::OnPress(m_ledger, e.button, m_terminal.hasSelection()), e.pointer);
        }

        void Handle(input::PointerRelease const& e)
        {
            m_terminal.reportPosition(e.position.X, e.position.Y);
            CarryOut(core::input::OnRelease(m_ledger, e.button), e.pointer);
        }

        // The deciding lives in core::input, where the tests can reach
        // it; this is the one place its decisions touch the world. So
        // every line here does what the transition says and asks
        // nothing of its own.
        void CarryOut(Transition const& t, Microsoft::UI::Xaml::Input::Pointer const& pointer)
        {
            AdoptLedger(t.ledger);
            if (t.capture.IsTake()) m_panel.CapturePointer(pointer);
            ReportButtonsToTerminal(t.reports);
            if (t.capture.IsDrop()) DropCaptureAfterThisEvent(pointer);
            if (t.copySelection) m_terminal.copySelection();
        }

        // The transition's ledger becomes this class's: from here on,
        // this is what libghostty has been told.
        void AdoptLedger(core::input::MouseButtonLedger const& ledger)
        {
            m_ledger = ledger;
        }

        // The presses and releases this transition owes libghostty,
        // in order. One for a press or a release of our own; one per
        // outstanding button when the capture is lost; none when the
        // event changed nothing libghostty should know about.
        void ReportButtonsToTerminal(std::vector<core::input::ButtonReport> const& reports)
        {
            for (auto const& r : reports) {
                m_terminal.reportButton(r.state, r.button.ToGhosttyEnum());
            }
        }

        // ReleasePointerCapture raises PointerCaptureLost at once, on
        // this thread, inside the call, which would run a handler
        // inside this one. Queued, it arrives as an event of its own
        // after this one has finished, so no handler ever nests.
        void DropCaptureAfterThisEvent(Microsoft::UI::Xaml::Input::Pointer const& pointer)
        {
            m_panel.DispatcherQueue().TryEnqueue([panel = m_panel, pointer]() {
                panel.ReleasePointerCapture(pointer);
            });
        }

        Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };
        Terminal m_terminal;
        core::input::MouseButtonLedger m_ledger;
    };
}
