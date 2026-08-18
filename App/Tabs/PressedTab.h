#pragma once

#include <utility>

namespace winrt::GhosttyWin32::implementation {

// The most recent pointer press on a tab header — a single slot,
// deliberately separate from the drag lifecycle (BasicTabDrag).
//
// The press is the only trustworthy identity of a grabbed tab:
// pressing a non-selected tab starts a drag WITHOUT moving the
// selection (ListView commits selection on release), and TabView's
// own drag event args are broken in this app (see BasicTabDrag).
// Every drag starts with a press, so TabDragStarting takes the slot
// and feeds it to TabDrag::Begin.
//
// Living outside TabDrag also gives the slot its own lifetime rule:
// a press that never became a drag would otherwise pin the
// TabViewItem until the next press, so the close paths Forget the
// item and release the reference immediately.
template <class TItem>
class BasicPressedTab {
public:
    void Record(TItem item) { m_item = std::move(item); }

    // One press feeds at most one drag.
    TItem Take() { return std::exchange(m_item, TItem{ nullptr }); }

    // Drops the slot if it names `item` — a closed tab must not be
    // kept alive by a stale press record.
    void Forget(TItem const& item) {
        if (m_item == item) m_item = TItem{ nullptr };
    }

private:
    TItem m_item{ nullptr };
};

}  // namespace winrt::GhosttyWin32::implementation
