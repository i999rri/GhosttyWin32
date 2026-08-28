#pragma once

#include "Tabs/Panes/Tree.h"
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
    // each tab's pane tree. ActiveControl alone would miss surfaces in
    // non-active panes once splits exist; the tree walk is correct for
    // both the single-pane case and any split configuration.
    Tab* FindBySurface(ghostty_surface_t surface) const {
        return FindPaneBySurface(surface).tab;
    }

    // Look up the Tab + pane carrying `id`. Used by the close_surface_cb
    // callback path: ghostty hands us back the ID we placed in
    // cfg.userdata, and the dispatched lambda calls this on the UI
    // thread. Returns { nullptr, nullptr } if the user already closed
    // the surface via the UI before the dispatched close arrived (or
    // if the ID is otherwise unknown), making stale callbacks a safe
    // no-op.
    struct PaneLookup {
        Tab* tab;
        Pane* pane;
    };
    PaneLookup FindByPaneId(PaneId id) const {
        if (!id) return { nullptr, nullptr };
        for (auto& t : m_tabs) {
            if (!t) continue;
            auto* panelImpl = winrt::get_self<implementation::SplitPanel>(t->Panel());
            if (!panelImpl) continue;
            if (auto* pane = panelImpl->Tree().FindPaneBy(
                    [id](Pane const& p) { return p.id == id; })) {
                return { t.get(), pane };
            }
        }
        return { nullptr, nullptr };
    }

    // Surface-driven pane lookup. Mirrors FindBySurface but returns the
    // specific pane in addition to the owning Tab, so callers that need
    // both (e.g. desktop notification routing) don't repeat the walk.
    PaneLookup FindPaneBySurface(ghostty_surface_t surface) const {
        if (!surface) return { nullptr, nullptr };
        for (auto& t : m_tabs) {
            if (!t) continue;
            auto* panelImpl = winrt::get_self<implementation::SplitPanel>(t->Panel());
            if (!panelImpl) continue;
            if (auto* pane = panelImpl->Tree().FindPaneBy(
                    [surface](Pane const& p) {
                        auto const* tc = p.Impl();
                        return tc && tc->Surface().Owns(surface);
                    })) {
                return { t.get(), pane };
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

    // Transfer ownership of the Tab wrapping `item` out of this
    // collection without destroying it. Tab tear-out moves a live Tab
    // — surfaces, swap chains, panel tree and all — into a sibling
    // window's collection, so unlike Remove() the Tab must survive
    // its departure from this list. Returns null when no tab here
    // wraps `item`.
    std::unique_ptr<Tab> Extract(Microsoft::UI::Xaml::Controls::TabViewItem const& item) {
        for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it) {
            if (*it && (*it)->Item() == item) {
                auto tab = std::move(*it);
                m_tabs.erase(it);
                return tab;
            }
        }
        return nullptr;
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
    std::vector<std::unique_ptr<Tab>> m_tabs;
};

}  // namespace winrt::GhosttyWin32::implementation
