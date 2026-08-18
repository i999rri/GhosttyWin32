#pragma once

#include "Tabs/Panes/Tree.h"
#include "SplitPanel.h"
#include "TerminalControl.xaml.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <memory>

namespace winrt::GhosttyWin32::implementation {

// One tab in the window's TabView.
//
// Each Tab references:
//   * A SplitPanel (`m_panel`) that owns the Tree of Branches
//     describing how this tab's content is partitioned across one or
//     more Panes (single terminals). The host (MainWindow) parents
//     the panel under AppContent alongside every other tab's panel;
//     selection drives per-panel Visibility, which is why
//     TabViewItem.Content stays unset. The panel keeps its
//     Children() collection in sync with the tree's Pane set so
//     framework input routing, hit-testing, and measure / arrange
//     work end-to-end.
//   * A pointer to the currently active Pane (`m_activePane`). All
//     "focused terminal" operations (key events, IME, clipboard,
//     action targets) flow through this. On tree mutations (NEW_SPLIT
//     / CLOSE_PANE) `m_activePane` must be reset before any Pane it
//     points at is destroyed.
//
// The tree lives inside the SplitPanel — Tab borrows it via
// `panel.Tree()` for Pane walks and reaches into individual Panes
// for TerminalControl bridge calls.
//
// Construction is just validation + member init — failable setup
// (creating the surface handle, attaching it, calling
// ghostty_surface_new) lives in TabFactory::Make. If you have a Tab*,
// you can operate on it freely without worrying about half-built
// state.
class Tab {
public:
    Tab(winrt::GhosttyWin32::SplitPanel panel,
        Microsoft::UI::Xaml::Controls::TabViewItem item)
        : m_panel(std::move(panel))
        , m_item(std::move(item))
    {
        if (!m_panel || !m_item) {
            throw winrt::hresult_error(E_INVALIDARG, L"Tab: missing resource");
        }
        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(m_panel);
        if (!panelImpl || !panelImpl->Tree().HasRoot()) {
            throw winrt::hresult_error(E_INVALIDARG, L"Tab: SplitPanel has no root");
        }
        // Initial active pane is the first pane found in depth-first
        // order — SetActivePane keeps the per-tab dim invariant
        // consistent (active bright, everything else dim).
        auto* firstPane = panelImpl->Tree().FindPaneBy(
            [](Pane const&) { return true; });
        if (!firstPane) {
            throw winrt::hresult_error(E_INVALIDARG, L"Tab: pane tree has no pane");
        }
        SetActivePane(firstPane);
    }

    ~Tab() {
        // Catch-all teardown: any panes still attached at destruction
        // get released here. The host's close paths (TabCloseRequested,
        // CLOSE_TAB action, close_surface_cb) all call DetachAll()
        // explicitly first so the framework's panel unparenting doesn't
        // run against a still-bound swap chain handle (the AV at +0x1F8
        // documented in MainWindow's close handlers). This destructor
        // is idempotent against those calls — Detach itself is a no-op
        // on an already-detached control.
        DetachAll();
    }

    Tab(const Tab&) = delete;
    Tab& operator=(const Tab&) = delete;
    Tab(Tab&&) = delete;
    Tab& operator=(Tab&&) = delete;

    // Returns the currently-focused TerminalControl in this tab —
    // the impl paired with the active pane. Callers that need the
    // surface, composition handle, or inner SwapChainPanel should go
    // through here so they keep working when the tree gains additional
    // panes and the active pane shifts on GOTO_SPLIT.
    implementation::TerminalControl* ActiveControl() const noexcept {
        if (!m_activePane) return nullptr;
        return PaneToTerminalControl(*m_activePane);
    }

    Microsoft::UI::Xaml::Controls::TabViewItem const& Item() const noexcept { return m_item; }

    // True once the shell has set an explicit title via SET_TITLE /
    // SET_TAB_TITLE (OSC 0/2 or `set_title` action). The foreground-
    // pid poll uses this to decide whether the tab header is theirs
    // to overwrite: shell-supplied titles win, auto-computed process
    // names only fill in when the shell has said nothing.
    bool HasExplicitTitle() const noexcept { return m_hasExplicitTitle; }
    void MarkExplicitTitle() noexcept { m_hasExplicitTitle = true; }

    // Last PID resolved for this tab's active pane's foreground
    // process, and the basename cached from it. The poll updates both
    // when the PID changes so QueryFullProcessImageNameW only runs on
    // transitions (running `git` for a while doesn't re-open the
    // handle every tick).
    uint32_t LastForegroundPid() const noexcept { return m_lastForegroundPid; }
    winrt::hstring const& LastForegroundName() const noexcept { return m_lastForegroundName; }
    void SetForegroundCache(uint32_t pid, winrt::hstring name) noexcept {
        m_lastForegroundPid = pid;
        m_lastForegroundName = std::move(name);
    }

    // Read-only access to the SplitPanel hosting this tab's tree — the
    // starting point for pane walks (`tab->Panel()` → `.Tree()` →
    // walker methods).
    winrt::GhosttyWin32::SplitPanel const& Panel() const noexcept { return m_panel; }
    Pane* ActivePane() const noexcept { return m_activePane; }

    // Per-tab confirmation predicate: does any pane in this tab's tree
    // currently report ghostty_surface_needs_confirm_quit? Owns the
    // walk here because Tab already owns the tree via its SplitPanel
    // — callers (close-flow gate for tab scope, MainWindow iterating
    // tabs for window scope) get a straight bool.
    bool NeedsConfirmClose() const {
        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(m_panel);
        if (!panelImpl) return false;
        return panelImpl->Tree().AnyPaneMatches([](Pane const& p) {
            auto const* tc = PaneToTerminalControl(p);
            return tc && tc->Surface().NeedsConfirmQuit();
        });
    }

    // Retarget the active pane — used by NEW_SPLIT (focus shifts to
    // the freshly-created pane), GOTO_SPLIT (direction-based pane
    // nav), and the pointer-focus path (a click on a non-active pane).
    // `pane` must reach back to a pane currently inside this tab's
    // SplitPanel tree, or be nullptr to indicate "no active pane yet"
    // during a tree mutation.
    //
    // Owns the per-tab dim invariant: the active pane's UnfocusedDim
    // is Collapsed (bright), every other pane's is Visible (dim).
    // Walking the tree on every SetActivePane call is the canonical
    // path — the dim state is a property of which pane the tab thinks
    // is active, not of XAML keyboard focus, so tab switches and
    // alt-tabs don't disturb it. Pointer clicks and keybind navigation
    // funnel through here too; the overlay stays untouched by
    // TerminalControl's GotFocus / LostFocus hooks.
    void SetActivePane(Pane* pane) {
        m_activePane = pane;
        if (auto* panelImpl = winrt::get_self<implementation::SplitPanel>(m_panel)) {
            panelImpl->Tree().ForEachPane([this](Pane& p) {
                if (auto* tc = PaneToTerminalControl(p)) {
                    tc->ApplyFocusVisual(&p == m_activePane);
                }
            });
        }
    }

    // Detach every TerminalControl in the tree (surface free, swap
    // chain release, composition handle close, SizeChanged unhook).
    // Must run while the SplitPanel is still in the live visual tree
    // — see MainWindow close handlers for the AV that happens if a
    // SwapChainPanel is unparented before its swap chain handle is
    // cleared.
    void DetachAll() {
        if (auto* panelImpl = winrt::get_self<implementation::SplitPanel>(m_panel)) {
            panelImpl->Tree().ForEachPane([](Pane& p) {
                if (auto* tc = PaneToTerminalControl(p)) {
                    tc->Detach();
                }
            });
        }
    }

    // Whether XAML accepted the focus request. The active pane's
    // TerminalControl is a UserControl with IsTabStop=true, so unlike
    // a bare SwapChainPanel this Focus call actually moves focus
    // reliably.
    bool Focus() {
        if (!m_activePane) return false;
        auto element = m_activePane->content;
        if (!element) return false;
        if (auto tc = element.try_as<winrt::GhosttyWin32::TerminalControl>()) {
            return tc.Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
        }
        return false;
    }

    // Extracts the impl pointer for a pane's TerminalControl, or
    // returns nullptr if the pane hosts something else (Phase 1
    // scaffolding accepts any UIElement, but in practice every pane
    // in a real Tab is a TerminalControl). Kept public-static so
    // helpers outside Tab can walk the tree consistently.
    static implementation::TerminalControl* PaneToTerminalControl(Pane const& pane) noexcept {
        auto element = pane.content;
        if (!element) return nullptr;
        if (auto tc = element.try_as<winrt::GhosttyWin32::TerminalControl>()) {
            return winrt::get_self<implementation::TerminalControl>(tc);
        }
        return nullptr;
    }

private:
    // SplitPanel owns the pane tree. The host parents it under
    // AppContent — Tab just borrows it via Panel() for tree walks and
    // Detach plumbing.
    winrt::GhosttyWin32::SplitPanel m_panel{ nullptr };
    Microsoft::UI::Xaml::Controls::TabViewItem m_item{ nullptr };
    // Borrowed pointer into the SplitPanel's tree — never owning.
    // Reset to nullptr or another pane on tree mutations before any
    // pane is destroyed.
    Pane* m_activePane{ nullptr };

    // See HasExplicitTitle. Sticky: once the shell sets a title the
    // poll leaves it alone for the rest of the tab's life.
    bool m_hasExplicitTitle{ false };

    // Foreground-pid poll cache. Zero means "not yet resolved".
    uint32_t         m_lastForegroundPid{ 0 };
    winrt::hstring   m_lastForegroundName{};
};

}  // namespace winrt::GhosttyWin32::implementation
