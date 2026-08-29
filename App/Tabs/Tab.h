#pragma once

#include "Tabs/Panes/Tree.h"
#include "Tabs/SplitPanel.h"
#include "Terminal/TerminalControl.xaml.h"
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
        return m_activePane ? m_activePane->Impl() : nullptr;
    }

    Microsoft::UI::Xaml::Controls::TabViewItem const& Item() const noexcept { return m_item; }

    // True once the shell has set an explicit title via SET_TITLE /
    // SET_TAB_TITLE (OSC 0/2 or `set_title` action). The foreground-
    // pid poll uses this to decide whether the tab header is theirs
    // to overwrite: shell-supplied titles win, auto-computed process
    // names only fill in when the shell has said nothing.
    bool HasExplicitTitle() const noexcept { return m_hasExplicitTitle; }
    void MarkExplicitTitle() noexcept { m_hasExplicitTitle = true; }

    // True once the USER named this tab via the rename prompt
    // (PROMPT_TITLE). One level stronger than the shell latch above:
    // upstream documents that a prompt-set title "overrides any
    // title set by the terminal", so SET_TITLE / SET_TAB_TITLE must
    // skip a user-titled tab (the shell keeps re-asserting its OSC
    // title on every prompt, which would instantly undo the rename).
    // Marking a user title implies the explicit latch too, so the
    // pid poll stays out without callers having to set both.
    bool HasUserTitle() const noexcept { return m_hasUserTitle; }
    void MarkUserTitle() noexcept {
        m_hasUserTitle = true;
        m_hasExplicitTitle = true;
    }

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
            auto const* tc = p.Impl();
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
                if (auto* tc = p.Impl()) {
                    tc->ApplyFocusVisual(&p == m_activePane);
                }
            });
        }
    }

    // Apply the window's background-opacity mode to every pane in
    // the tree (#69). See TerminalControl::SetOpaqueBackground.
    void ApplyBackgroundOpacity(bool opaque, winrt::Windows::UI::Color bg) {
        if (auto* panelImpl = winrt::get_self<implementation::SplitPanel>(m_panel)) {
            panelImpl->Tree().ForEachPane([&](Pane& p) {
                if (auto* tc = p.Impl()) {
                    tc->SetOpaqueBackground(opaque, bg);
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
                if (auto* tc = p.Impl()) {
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
        if (!m_activePane || !m_activePane->control) return false;
        return m_activePane->control.Focus(
            Microsoft::UI::Xaml::FocusState::Programmatic);
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

    // See HasUserTitle. Sticky for the tab's lifetime — there is no
    // inverse yet (the rename prompt treats empty input as cancel
    // for exactly this reason).
    bool m_hasUserTitle{ false };
    // See HasExplicitTitle. Sticky: once the shell sets a title the
    // poll leaves it alone for the rest of the tab's life.
    bool m_hasExplicitTitle{ false };

    // Foreground-pid poll cache. Zero means "not yet resolved".
    uint32_t         m_lastForegroundPid{ 0 };
    winrt::hstring   m_lastForegroundName{};
};

}  // namespace winrt::GhosttyWin32::implementation
