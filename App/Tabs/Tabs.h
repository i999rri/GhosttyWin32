#pragma once

#include "Tabs/Panes/Pane.h"
#include "SplitPanel.h"
#include "Tabs/Tab.h"
#include "ghostty.h"
#include <vector>
#include <memory>

namespace winrt::GhosttyWin32::implementation {

// Owning collection of Tab. Provides the lookups MainWindow needs (by
// TabViewItem reference, by ghostty_surface_t pointer, by current TabView
// selection) so the surrounding code doesn't have to repeat vector iteration
// and identity checks every time.
//
// Pure aggregation — Tab itself owns its panel/surface/handle, this class
// just owns the unique_ptrs and exposes find/insert/remove primitives.
class Tabs {
public:
    Tabs() = default;
    Tabs(const Tabs&) = delete;
    Tabs& operator=(const Tabs&) = delete;
    Tabs(Tabs&&) = delete;
    Tabs& operator=(Tabs&&) = delete;

    void Add(std::unique_ptr<Tab> tab) {
        if (tab) m_tabs.push_back(std::move(tab));
    }

    // Returns the matching Tab, or nullptr. Both lookups are O(N); N here is
    // the number of open tabs which is small enough that linear search is
    // strictly cheaper than maintaining an index.
    Tab* FindByItem(Microsoft::UI::Xaml::Controls::TabViewItem const& item) const {
        for (auto& t : m_tabs) {
            if (t && t->Item() == item) return t.get();
        }
        return nullptr;
    }

    // Resolve a ghostty_surface_t back to the owning Tab by walking
    // each tab's Pane tree. ActiveControl alone would miss surfaces in
    // non-active leaves once splits exist; the tree walk is correct
    // for both the single-leaf case (today) and any future split
    // configuration. O(N * leaves) — both factors are small enough
    // that a flat scan is strictly cheaper than maintaining an index.
    Tab* FindBySurface(ghostty_surface_t surface) const {
        return FindLeafBySurface(surface).tab;
    }

    // Look up the Tab + leaf for the pane carrying `id`. Used by the
    // close_surface_cb callback path: ghostty hands us back the ID we
    // placed in cfg.userdata, and the dispatched lambda calls this on
    // the UI thread. Returns { nullptr, nullptr } if the user already
    // closed the surface via the UI before the dispatched close
    // arrived (or if the ID is otherwise unknown), making stale
    // callbacks a safe no-op.
    struct PaneLookup {
        Tab* tab;
        Pane* leaf;
    };
    PaneLookup FindByPaneId(PaneId id) const {
        if (!id) return { nullptr, nullptr };
        for (auto& t : m_tabs) {
            if (!t) continue;
            auto* panelImpl = winrt::get_self<implementation::SplitPanel>(t->Panel());
            if (!panelImpl) continue;
            if (auto* leaf = FindLeafByPaneId(panelImpl->Root(), id)) {
                return { t.get(), leaf };
            }
        }
        return { nullptr, nullptr };
    }

    // Surface-driven leaf lookup. Mirrors FindBySurface but returns the
    // specific leaf in addition to the owning Tab, so callers that need
    // both (e.g. desktop notification routing — find the pane that
    // emitted the toast and embed its PaneId in the click argument)
    // don't repeat the tree walk.
    PaneLookup FindLeafBySurface(ghostty_surface_t surface) const {
        if (!surface) return { nullptr, nullptr };
        for (auto& t : m_tabs) {
            if (!t) continue;
            auto* panelImpl = winrt::get_self<implementation::SplitPanel>(t->Panel());
            if (!panelImpl) continue;
            if (auto* leaf = FindLeafBySurfaceRecursive(panelImpl->Root(), surface)) {
                return { t.get(), leaf };
            }
        }
        return { nullptr, nullptr };
    }

    // The Tab whose TabViewItem is currently selected in the given TabView,
    // or nullptr if there is no selection / no match. Encapsulates the
    // SelectedItem → TabViewItem → match dance the input handlers all need.
    Tab* Active(Microsoft::UI::Xaml::Controls::TabView const& tv) const {
        auto sel = tv.SelectedItem();
        if (!sel) return nullptr;
        auto item = sel.try_as<Microsoft::UI::Xaml::Controls::TabViewItem>();
        if (!item) return nullptr;
        return FindByItem(item);
    }

    // Remove a tab by TabViewItem. The Tab destructor handles teardown
    // (panel detach, ghostty_surface_free, CloseHandle), so callers don't
    // need to do anything extra.
    bool Remove(Microsoft::UI::Xaml::Controls::TabViewItem const& item) {
        for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
            if (*it && (*it)->Item() == item) {
                m_tabs.erase(it);
                return true;
            }
        }
        return false;
    }

    void Clear() noexcept { m_tabs.clear(); }

    bool Empty() const noexcept { return m_tabs.empty(); }
    size_t Size() const noexcept { return m_tabs.size(); }

    // Range-for support for the few places that genuinely need to walk every
    // tab (DPI change broadcast, crash-time handle cleanup).
    auto begin() noexcept { return m_tabs.begin(); }
    auto end() noexcept { return m_tabs.end(); }
    auto begin() const noexcept { return m_tabs.begin(); }
    auto end() const noexcept { return m_tabs.end(); }

private:
    // Depth-first search for the leaf whose TerminalControl owns
    // `surface`. Returns the leaf node (mutable so callers updating
    // the active-leaf pointer can do so) or nullptr. Used by both
    // FindBySurface (which only cares whether the leaf exists) and
    // FindLeafBySurface (which needs the leaf itself).
    static Pane* FindLeafBySurfaceRecursive(Pane* node, ghostty_surface_t surface) {
        if (!node) return nullptr;
        if (node->IsLeaf()) {
            if (auto* c = Tab::LeafToTerminalControl(*node); c && c->Surface().Owns(surface)) {
                return node;
            }
            return nullptr;
        }
        if (auto* p = FindLeafBySurfaceRecursive(node->First(), surface)) return p;
        return FindLeafBySurfaceRecursive(node->Second(), surface);
    }

    // Depth-first search for a leaf carrying `id`. Returns the leaf
    // node (mutable so callers can update active-leaf pointers when
    // collapsing the tree on close) or nullptr.
    static Pane* FindLeafByPaneId(Pane* node, PaneId id) {
        if (!node) return nullptr;
        if (node->IsLeaf()) {
            return node->Id() == id ? node : nullptr;
        }
        if (auto* p = FindLeafByPaneId(node->First(), id)) return p;
        return FindLeafByPaneId(node->Second(), id);
    }

    std::vector<std::unique_ptr<Tab>> m_tabs;
};

}  // namespace winrt::GhosttyWin32::implementation
