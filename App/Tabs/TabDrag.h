#pragma once

#include <cassert>
#include <utility>

namespace winrt::GhosttyWin32::implementation {

// The process-wide tab drag. At most one exists at a time: tab
// drag-and-drop rides OLE's DoDragDrop, which runs a modal loop on
// the UI thread all windows share, so a second drag cannot start
// before the first ends. Begin() asserts that assumption.
//
// One drag carries two lifetimes, which is why this is a type and
// not a bare field:
//
//  - InFlight() / DraggedTab() are live only between Begin and End.
//    They gate TabStripDragOver / TabStripDrop so a foreign OLE drag
//    (files, text) is never mistaken for one of our tabs.
//  - TakeLastDraggedTab() consumes the identity kept past End.
//    TabView raises TabDragCompleted BEFORE TabDroppedOutside
//    (hardcoded order in its OnListViewDragItemsCompleted), so the
//    tear-out handler runs after the in-flight window has already
//    closed — it claims the tab from here instead.
//
// Begin receives its item from the press slot (BasicPressedTab)
// because TabView's own event args can't identify the dragged tab
// in this app: with TabViewItem elements stored directly in TabItems
// (Content unset — see #77), TabView's FindTabViewItemFromDragItem
// falls through to a Content()==item scan that matches the FIRST
// tab no matter which one was grabbed. If microsoft-ui-xaml ever
// fixes that, the press slot and TakeLastDraggedTab can be retired —
// TabDroppedOutside could read args.Tab() directly. InFlight /
// DraggedTab stay regardless: OLE cannot marshal the item across
// windows, and the in-flight gate is what keeps foreign OLE drags
// out of the tab-merge path.
//
// Templated on the item type so the lifecycle logic stays free of
// the WinUI projection and unit-testable; production instantiates
// it with TabViewItem (see App).
template <class TItem>
class BasicTabDrag {
public:
    // A null item is allowed: it means a drag started but the tab
    // could not be identified. The drag then stays NOT in flight
    // (the merge gate stays closed) and the kept identity is
    // cleared, so a stale one can't leak into this drag's tear-out.
    void Begin(TItem item) {
        assert(!m_inFlight && "TabDrag::Begin while a drag is in flight");
        m_item = std::move(item);
        m_inFlight = static_cast<bool>(m_item);
    }

    // Deliberately leaves m_item alone — see TakeLastDraggedTab.
    void End() noexcept { m_inFlight = false; }

    bool InFlight() const noexcept { return m_inFlight; }

    // The dragged item while the drag is in flight; null otherwise.
    TItem DraggedTab() const { return m_inFlight ? m_item : TItem{ nullptr }; }

    // Claims the most recently dragged item — one-shot: a second
    // call returns null. Taking mid-flight would rip the identity
    // out from under DraggedTab, so that's a caller bug.
    TItem TakeLastDraggedTab() {
        assert(!m_inFlight && "TakeLastDraggedTab during a drag");
        return std::exchange(m_item, TItem{ nullptr });
    }

private:
    TItem m_item{ nullptr };
    bool m_inFlight{ false };
};

}  // namespace winrt::GhosttyWin32::implementation
