#pragma once

#include "TabId.h"
#include "TerminalControl.xaml.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace winrt::GhosttyWin32::implementation {

// One tab in the window's TabView.
//
// Conceptually a tab is the *root of a pane tree*: a container holding
// one or more TerminalControls (one per pane) plus the splitter
// metadata that describes how they're laid out. Every other terminal
// emulator that supports split panes (Windows Terminal, Alacritty,
// macOS Terminal, GTK Ghostty) follows this shape:
//
//   Tab
//    └── PaneTree
//          ├── Leaf: TerminalControl
//          └── Branch: split{H/V, child1, child2}
//                ├── Leaf: TerminalControl
//                └── Leaf: TerminalControl
//
// Today we don't yet implement splits, so each Tab holds exactly one
// TerminalControl directly. The class deliberately exposes only the
// "tab-level" API (active control, focus, id, item) and avoids
// delegating per-control accessors like Surface() — once panes land,
// those would silently change meaning ("the surface" becomes "which
// surface?") and every caller would need re-auditing. By forcing
// callers to go through ActiveControl() now, the eventual switch to
// "the focused leaf in the pane tree" is mechanical: only ActiveControl
// changes implementation, no caller-side audit needed.
//
// Construction is just validation + member init — all failable work
// (creating the surface handle, attaching it, calling
// ghostty_surface_new) lives in TabFactory::Make. If you have a Tab*,
// you can operate on it freely without worrying about half-built state.
class Tab {
public:
    Tab(winrt::GhosttyWin32::TerminalControl control,
        Microsoft::UI::Xaml::Controls::TabViewItem item,
        TabId id)
        : m_control(std::move(control))
        , m_item(std::move(item))
        , m_id(id)
    {
        if (!m_control || !m_item) {
            throw winrt::hresult_error(E_INVALIDARG, L"Tab: missing resource");
        }
    }

    ~Tab() {
        // Detach every TerminalControl in the tab — surface free, swap
        // chain release, composition handle close, SizeChanged unhook
        // all live on the control. Today there's a single control;
        // when pane support lands this becomes a tree walk.
        if (auto* c = ActiveControl()) {
            c->Detach();
        }
    }

    Tab(const Tab&) = delete;
    Tab& operator=(const Tab&) = delete;
    Tab(Tab&&) = delete;
    Tab& operator=(Tab&&) = delete;

    // Returns the currently-focused TerminalControl in this tab. Right
    // now that's the single control; once panes land it'll be the
    // focused leaf in the pane tree. Callers that need the surface,
    // composition handle, or inner SwapChainPanel should go through
    // here so they keep working when the tree gains additional leaves.
    implementation::TerminalControl* ActiveControl() const noexcept {
        if (!m_control) return nullptr;
        return winrt::get_self<implementation::TerminalControl>(m_control);
    }

    Microsoft::UI::Xaml::Controls::TabViewItem const& Item() const noexcept { return m_item; }
    TabId Id() const noexcept { return m_id; }

    // Returns whether XAML accepted the focus request (used in
    // diagnostics for the tab-switch focus path). TerminalControl is a
    // UserControl with IsTabStop=true, so unlike a bare SwapChainPanel
    // this Focus call actually moves focus reliably. Future: focus the
    // active leaf of the pane tree, not just the single control.
    bool Focus() {
        if (!m_control) return false;
        return m_control.Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
    }

private:
    // Today: the single TerminalControl that fills the tab content.
    // Future: the root of a pane tree (variant<TerminalControl, Pane>),
    // where Pane itself holds two children + split orientation.
    winrt::GhosttyWin32::TerminalControl m_control{ nullptr };
    Microsoft::UI::Xaml::Controls::TabViewItem m_item{ nullptr };
    TabId m_id{};
};

}  // namespace winrt::GhosttyWin32::implementation
