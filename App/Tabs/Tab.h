#pragma once

#include "Host/TitleSource.h"
#include "Tabs/Panes/Tree.h"
#include "Tabs/SplitPanel.h"
#include "Terminal/TerminalControl.xaml.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
        Microsoft::UI::Xaml::Controls::TabViewItem item,
        std::string command = {})
        : m_panel(std::move(panel))
        , m_item(std::move(item))
        , m_command(std::move(command))
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
        return m_activePane ? ControlOf(*m_activePane) : nullptr;
    }

    Microsoft::UI::Xaml::Controls::TabViewItem const& Item() const noexcept { return m_item; }

    // The shell command this tab was created with (empty = the
    // configured default). A tab opened from this one inherits it, so
    // a WSL tab begets WSL tabs.
    std::string const& Command() const noexcept { return m_command; }

    // Who last named this tab. Every header write site asks this
    // before touching Item().Header() — see Host/TitleSource.h for
    // the priority rule between the foreground-pid poll, shell
    // SET_TITLE / SET_TAB_TITLE, and the rename prompt.
    // (Qualified return type: the method name shadows the type
    // inside this class.)
    core::host::TitleSource TitleSource() const noexcept { return m_titleSource; }
    void SetTitleSource(core::host::TitleSource source) noexcept { m_titleSource = source; }

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
        auto needs = [](Pane const& p) {
            return p.view && p.view->Surface().NeedsConfirmQuit();
        };
        if (panelImpl->Tree().AnyPaneMatches(needs)) return true;
        return std::any_of(m_inPlace.begin(), m_inPlace.end(),
                           [&](InPlaceSession const& s) { return s.parked && needs(*s.parked); });
    }

    // ----- in-place WSL sessions (#217) -----
    // A shell in this tab asked for WSL in its own pane: the WSL pane
    // now sits in the tree where the shell's pane was, and the shell's
    // pane waits here, alive and unparented, to go back when WSL ends.
    // `onFinished` answers the shim that is holding the shell's
    // prompt; it gets WSL's exit code. A dropped session (tab closed
    // mid-way) destroys the callable, which closes the shim's pipe
    // instead of answering.
    struct InPlaceSession {
        PaneId overlay;
        std::optional<Pane> parked;
        std::function<void(uint32_t exitCode)> onFinished;
    };

    void BeginInPlace(InPlaceSession session) {
        m_inPlace.push_back(std::move(session));
    }

    // The session whose WSL pane is `overlay`, taken out of the tab.
    std::optional<InPlaceSession> TakeInPlaceByOverlay(PaneId overlay) {
        auto it = std::find_if(m_inPlace.begin(), m_inPlace.end(),
                               [overlay](InPlaceSession const& s) { return s.overlay == overlay; });
        if (it == m_inPlace.end()) return std::nullopt;
        InPlaceSession session = std::move(*it);
        m_inPlace.erase(it);
        return session;
    }

    // Whether `pane` is the WSL half of a session. A shim request
    // from inside one is refused rather than stacked.
    bool IsInPlaceOverlay(PaneId pane) const noexcept {
        return std::any_of(m_inPlace.begin(), m_inPlace.end(),
                           [pane](InPlaceSession const& s) { return s.overlay == pane; });
    }

    // A parked shell that died while waiting: hand it out for detach.
    // Its session stays, and closes like a plain pane when the WSL
    // side ends, since there is nothing left to put back.
    std::optional<Pane> TakeParkedById(PaneId id) {
        for (auto& s : m_inPlace) {
            if (s.parked && s.parked->id == id) {
                Pane pane = *s.parked;
                s.parked.reset();
                return pane;
            }
        }
        return std::nullopt;
    }

    // The parked shell whose surface is `surface`, or null. A parked
    // pane's surface stays live and keeps talking to its control
    // (mouse visibility after the focus change, title, shape), so the
    // window's surface directory has to find it here, or the pane
    // comes back with stale state.
    Pane* FindParkedBySurface(ghostty_surface_t surface) noexcept {
        for (auto& s : m_inPlace) {
            if (s.parked && s.parked->view && s.parked->view->Surface().Owns(surface)) {
                return &*s.parked;
            }
        }
        return nullptr;
    }

    // Parked panes are outside the tree, so every tree-wide walk that
    // must reach every control (rehost on tear-out, appearance
    // restate) covers them through this.
    template <class F>
    void ForEachParkedPane(F&& visit) {
        for (auto& s : m_inPlace) {
            if (s.parked) visit(*s.parked);
        }
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
                if (p.view) p.view->ApplyFocusVisual(&p == m_activePane);
            });
        }
    }

    // Apply the window's background-opacity mode to every pane in
    // the tree (#69). See TerminalControl::SetOpaqueBackground.
    void ApplyBackgroundOpacity(bool opaque, winrt::Windows::UI::Color bg) {
        if (auto* panelImpl = winrt::get_self<implementation::SplitPanel>(m_panel)) {
            panelImpl->Tree().ForEachPane([&](Pane& p) {
                if (p.view) p.view->SetOpaqueBackground(opaque, bg);
            });
        }
        ForEachParkedPane([&](Pane& p) {
            if (p.view) p.view->SetOpaqueBackground(opaque, bg);
        });
    }

    // Detach every TerminalControl in the tree (surface free, swap
    // chain release, composition handle close, SizeChanged unhook),
    // and every parked one. Must run while the SplitPanel is still in
    // the live visual tree — see MainWindow close handlers for the AV
    // that happens if a SwapChainPanel is unparented before its swap
    // chain handle is cleared.
    void DetachAll() {
        if (auto* panelImpl = winrt::get_self<implementation::SplitPanel>(m_panel)) {
            panelImpl->Tree().ForEachPane([](Pane& p) {
                if (p.view) p.view->Detach();
            });
        }
        ForEachParkedPane([](Pane& p) {
            if (p.view) p.view->Detach();
        });
        // The shims of dropped sessions read end-of-file, not an
        // exit code, so their shells (gone with the surfaces above)
        // never see a false success.
        m_inPlace.clear();
    }

    // Whether XAML accepted the focus request. The active pane's
    // TerminalControl is a UserControl with IsTabStop=true, so unlike
    // a bare SwapChainPanel this Focus call actually moves focus
    // reliably.
    bool Focus() {
        if (!m_activePane || !m_activePane->view) return false;
        return m_activePane->view->TakeFocus();
    }

private:
    // SplitPanel owns the pane tree. The host parents it under
    // AppContent — Tab just borrows it via Panel() for tree walks and
    // Detach plumbing.
    winrt::GhosttyWin32::SplitPanel m_panel{ nullptr };
    Microsoft::UI::Xaml::Controls::TabViewItem m_item{ nullptr };
    // See Command().
    std::string m_command;
    // Borrowed pointer into the SplitPanel's tree — never owning.
    // Reset to nullptr or another pane on tree mutations before any
    // pane is destroyed.
    Pane* m_activePane{ nullptr };

    // See TitleSource(). Starts Automatic: nobody has spoken yet, so
    // the foreground-pid poll owns the header.
    core::host::TitleSource m_titleSource{ core::host::TitleSource::Automatic() };

    // Foreground-pid poll cache. Zero means "not yet resolved".
    uint32_t         m_lastForegroundPid{ 0 };
    winrt::hstring   m_lastForegroundName{};

    // See InPlaceSession. One entry per pane currently showing WSL in
    // place of its shell; a split tab can hold several.
    std::vector<InPlaceSession> m_inPlace;
};

}  // namespace winrt::GhosttyWin32::implementation
