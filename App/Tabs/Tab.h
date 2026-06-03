#pragma once

#include "Tabs/Panes/Pane.h"
#include "SplitPanel.h"
#include "TerminalControl.xaml.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <memory>

namespace winrt::GhosttyWin32::implementation {

// One tab in the window's TabView.
//
// Each Tab references:
//   * A SplitPanel (`m_panel`) that owns the Pane tree describing how
//     this tab's content is partitioned across one or more terminal
//     panes. The host (MainWindow) parents the panel under AppContent
//     alongside every other tab's panel; selection drives per-panel
//     Visibility, which is why TabViewItem.Content stays unset. The
//     panel keeps its Children() collection in sync with the tree's
//     leaf set so framework input routing, hit-testing, and measure /
//     arrange work end-to-end. Today the tree is always a single leaf
//     — NEW_SPLIT plumbing comes in the next phase — but the structure
//     is in place so callers (Tabs::FindBySurface, action callbacks)
//     walk the tree instead of assuming a single TerminalControl per
//     tab.
//   * A pointer to the currently active leaf (`m_activeLeaf`). All
//     "focused terminal" operations (key events, IME, clipboard,
//     action targets) flow through this. Today there is exactly one
//     leaf so it never moves; phase 4 (#13) will retarget it on
//     GOTO_SPLIT / pointer focus.
//
// The Pane tree itself lives inside the SplitPanel (one unique_ptr
// owner) — Tab borrows it via panel.Root() and stores `m_activeLeaf`
// as a non-owning pointer into that tree. On tree mutations (future
// NEW_SPLIT / CLOSE_PANE) `m_activeLeaf` must be reset before any leaf
// is destroyed.
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
        if (!panelImpl || !panelImpl->Root()) {
            throw winrt::hresult_error(E_INVALIDARG, L"Tab: SplitPanel has no root");
        }
        // Initial active leaf is the first (and currently only) leaf
        // found via depth-first descent. Route through SetActiveLeaf so
        // the per-tab dim state is applied consistently: active leaf
        // bright, everything else dim.
        auto* firstLeaf = FindFirstLeaf(panelImpl->Root());
        if (!firstLeaf) {
            throw winrt::hresult_error(E_INVALIDARG, L"Tab: pane tree has no leaf");
        }
        SetActiveLeaf(firstLeaf);
    }

    ~Tab() {
        // Catch-all teardown: any leaves still attached at destruction
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
    // i.e. the impl of the active leaf in the pane tree. Callers that
    // need the surface, composition handle, or inner SwapChainPanel
    // should go through here so they keep working when the tree gains
    // additional leaves and the active leaf shifts on GOTO_SPLIT.
    implementation::TerminalControl* ActiveControl() const noexcept {
        if (!m_activeLeaf) return nullptr;
        return LeafToTerminalControl(*m_activeLeaf);
    }

    Microsoft::UI::Xaml::Controls::TabViewItem const& Item() const noexcept { return m_item; }

    // Read-only access to the SplitPanel hosting this tab's tree, used
    // by Tabs::FindBySurface to walk every leaf and locate the one
    // whose TerminalControl owns a given ghostty_surface_t.
    winrt::GhosttyWin32::SplitPanel const& Panel() const noexcept { return m_panel; }
    Pane* ActiveLeaf() const noexcept { return m_activeLeaf; }

    // Retarget the active leaf — used by NEW_SPLIT (focus shifts to
    // the freshly-created pane), GOTO_SPLIT (direction-based pane
    // nav), and the pointer-focus path (a click on a non-active pane).
    // `leaf` must reach back to a leaf currently inside this tab's
    // SplitPanel tree, or be nullptr to indicate "no active leaf yet"
    // during a tree mutation. Callers verify with FindLeafByPaneId or
    // build the leaf themselves before the tree mutation.
    //
    // Owns the per-tab dim invariant: the active leaf's UnfocusedDim
    // is Collapsed (bright), every other leaf's is Visible (dim).
    // Walking the tree on every SetActiveLeaf call is the canonical
    // path — the dim state is a property of which leaf the tab thinks
    // is active, not of XAML keyboard focus, so tab switches and
    // alt-tabs don't disturb it (they don't change m_activeLeaf).
    // Pointer clicks and keybind navigation come in through here too;
    // the corresponding TerminalControl GotFocus / LostFocus hooks no
    // longer touch the overlay.
    void SetActiveLeaf(Pane* leaf) {
        m_activeLeaf = leaf;
        if (auto* panelImpl = winrt::get_self<implementation::SplitPanel>(m_panel)) {
            ApplyDimRecursive(panelImpl->Root(), leaf);
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
            DetachAllLeaves(panelImpl->Root());
        }
    }

    // Whether XAML accepted the focus request. The active leaf's
    // TerminalControl is a UserControl with IsTabStop=true, so unlike
    // a bare SwapChainPanel this Focus call actually moves focus
    // reliably.
    bool Focus() {
        if (!m_activeLeaf) return false;
        auto element = m_activeLeaf->Content();
        if (!element) return false;
        if (auto tc = element.try_as<winrt::GhosttyWin32::TerminalControl>()) {
            return tc.Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
        }
        return false;
    }

    // Extracts the impl pointer for a leaf's TerminalControl, or
    // returns nullptr if the leaf hosts something else (Phase 1
    // scaffolding accepts any UIElement, but in practice every leaf
    // in a real Tab is a TerminalControl). Kept public-static so
    // helpers outside Tab can walk the tree consistently.
    static implementation::TerminalControl* LeafToTerminalControl(Pane const& leaf) noexcept {
        auto element = leaf.Content();
        if (!element) return nullptr;
        if (auto tc = element.try_as<winrt::GhosttyWin32::TerminalControl>()) {
            return winrt::get_self<implementation::TerminalControl>(tc);
        }
        return nullptr;
    }

private:
    static Pane* FindFirstLeaf(Pane* node) noexcept {
        if (!node) return nullptr;
        if (node->IsLeaf()) return node;
        if (auto* p = FindFirstLeaf(node->First())) return p;
        return FindFirstLeaf(node->Second());
    }

    // Walk the tree and apply the per-tab dim invariant: the leaf
    // whose Pane* matches `active` gets its UnfocusedDim Collapsed
    // (bright); every other leaf's gets Visible (dim). Static because
    // it doesn't touch this instance's mutable state beyond the
    // leaves it visits.
    static void ApplyDimRecursive(Pane* node, Pane const* active) {
        if (!node) return;
        if (node->IsLeaf()) {
            if (auto* tc = LeafToTerminalControl(*node)) {
                tc->ApplyFocusVisual(node == active);
            }
            return;
        }
        ApplyDimRecursive(node->First(), active);
        ApplyDimRecursive(node->Second(), active);
    }

    static void DetachAllLeaves(Pane* node) {
        if (!node) return;
        if (node->IsLeaf()) {
            if (auto* tc = LeafToTerminalControl(*node)) {
                tc->Detach();
            }
            return;
        }
        DetachAllLeaves(node->First());
        DetachAllLeaves(node->Second());
    }

    // SplitPanel owns the Pane tree (via its own m_root). The host
    // parents it under AppContent — Tab just borrows it via Panel()
    // for tree walks and Detach plumbing.
    winrt::GhosttyWin32::SplitPanel m_panel{ nullptr };
    Microsoft::UI::Xaml::Controls::TabViewItem m_item{ nullptr };
    // Borrowed pointer into the SplitPanel's tree — never owning.
    // Reset to nullptr or another leaf on tree mutations before any
    // leaf is destroyed.
    Pane* m_activeLeaf{ nullptr };
};

}  // namespace winrt::GhosttyWin32::implementation
