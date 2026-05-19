#pragma once

#include "Pane.h"
#include "SplitPanel.h"
#include "TabId.h"
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
//     panes, and is the TabViewItem's Content. The Panel keeps its
//     Children() collection in sync with the tree's leaf set so
//     framework input routing, hit-testing, and measure / arrange work
//     end-to-end. Today the tree is always a single leaf — NEW_SPLIT
//     plumbing comes in the next phase — but the structure is in place
//     so callers (Tabs::FindBySurface, action callbacks) walk the tree
//     instead of assuming a single TerminalControl per tab.
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
        Microsoft::UI::Xaml::Controls::TabViewItem item,
        TabId id)
        : m_panel(std::move(panel))
        , m_item(std::move(item))
        , m_id(id)
    {
        if (!m_panel || !m_item) {
            throw winrt::hresult_error(E_INVALIDARG, L"Tab: missing resource");
        }
        auto* panelImpl = winrt::get_self<implementation::SplitPanel>(m_panel);
        if (!panelImpl || !panelImpl->Root()) {
            throw winrt::hresult_error(E_INVALIDARG, L"Tab: SplitPanel has no root");
        }
        // Initial active leaf is the first (and currently only) leaf
        // found via depth-first descent.
        m_activeLeaf = FindFirstLeaf(panelImpl->Root());
        if (!m_activeLeaf) {
            throw winrt::hresult_error(E_INVALIDARG, L"Tab: pane tree has no leaf");
        }
    }

    ~Tab() {
        // Detach every TerminalControl in the tree — surface free,
        // swap chain release, composition handle close, SizeChanged
        // unhook all live on the control. Walking the tree handles
        // the post-split case naturally; with a single leaf it
        // collapses to one Detach call.
        if (auto* panelImpl = winrt::get_self<implementation::SplitPanel>(m_panel)) {
            DetachAllLeaves(panelImpl->Root());
        }
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
    TabId Id() const noexcept { return m_id; }

    // Read-only access to the SplitPanel hosting this tab's tree, used
    // by Tabs::FindBySurface to walk every leaf and locate the one
    // whose TerminalControl owns a given ghostty_surface_t.
    winrt::GhosttyWin32::SplitPanel const& Panel() const noexcept { return m_panel; }
    Pane* ActiveLeaf() const noexcept { return m_activeLeaf; }

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

    // SplitPanel owns the Pane tree (via its own m_root) and is set as
    // the TabViewItem's Content by TabFactory.
    winrt::GhosttyWin32::SplitPanel m_panel{ nullptr };
    Microsoft::UI::Xaml::Controls::TabViewItem m_item{ nullptr };
    TabId m_id{};
    // Borrowed pointer into the SplitPanel's tree — never owning.
    // Reset to nullptr or another leaf on tree mutations before any
    // leaf is destroyed.
    Pane* m_activeLeaf{ nullptr };
};

}  // namespace winrt::GhosttyWin32::implementation
