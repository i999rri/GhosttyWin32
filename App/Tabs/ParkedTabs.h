#pragma once

#include "Tabs/Tab.h"
#include "Win32/DebugTrace.h"
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace winrt::GhosttyWin32::implementation {

// Undo support for tab closes (#151): the parked-tab stack, its
// expiry timers, and the redo bookkeeping — state and operations in
// one place, same aggregation style as Tabs / WindowCloseGate.
//
// A "closed" tab is parked here alive — surfaces, pty processes,
// swap chains all intact — because a torn-down shell process cannot
// be resurrected from any snapshot. Memory stays bounded by the
// timeout: each entry self-destructs via its timer, and the window
// is SLIDING (a deliberate refinement over upstream's per-entry
// expiry): every new park re-arms every existing timer, so a
// closing streak stays fully restorable and the clock only counts
// down once the user stops closing. Each extension costs a
// deliberate close, so the parked set stays bounded in practice.
//
// What deliberately stays OUTSIDE: everything XAML-window-shaped.
// The owner (MainWindow) removes/reinserts tab-strip items, hides
// panels, restates appearance, and supplies the expiry teardown via
// the onExpire callback — this class never touches the visual tree.
class ParkedTabs {
public:
    ParkedTabs() = default;
    ParkedTabs(const ParkedTabs&) = delete;
    ParkedTabs& operator=(const ParkedTabs&) = delete;
    ParkedTabs(ParkedTabs&&) = delete;
    ParkedTabs& operator=(ParkedTabs&&) = delete;

    ~ParkedTabs() { Shutdown(); }

    // Take ownership of a closed-but-alive Tab for timeoutMs. When
    // the clock runs out, onExpire receives the ownership back on
    // the dispatcher thread (the owner runs the real teardown); a
    // tab reclaimed by PopNewest first never expires. Sliding
    // window: every Park re-arms all existing entries' timers.
    void Park(std::unique_ptr<Tab> tab, uint32_t index, uint64_t timeoutMs,
              Microsoft::UI::Dispatching::DispatcherQueue const& dq,
              std::function<void(std::unique_ptr<Tab>)> onExpire)
    {
        if (!tab || !dq) return;

        // Set to 0 to restore upstream semantics (macOS
        // ExpiringUndoManager: each entry expires on its own clock,
        // counted from its own close) — upstream behavior is simply
        // this block's absence.
#if 1  // 1 = sliding window, 0 = upstream per-entry expiry
        for (auto& entry : m_entries) {
            if (entry.timer) {
                entry.timer.Stop();
                entry.timer.Interval(std::chrono::milliseconds{ timeoutMs });
                entry.timer.Start();
            }
        }
        if (!m_entries.empty()) {
            DEBUG_TRACE(L"UndoPark[%llu]: re-armed %zu parked timer(s)\n",
                            GetTickCount64() % 100'000, m_entries.size());
        }
#endif

        Entry entry;
        entry.tab = std::move(tab);
        entry.index = index;
        entry.onExpire = std::move(onExpire);
        Tab* key = entry.tab.get();
        auto timer = dq.CreateTimer();
        timer.Interval(std::chrono::milliseconds{ timeoutMs });
        timer.IsRepeating(false);
        // The alive token outlives neither this object nor the
        // dispatcher thread's serialization: a tick that fires
        // after ~ParkedTabs finds an expired weak_ptr and no-ops.
        // `key` is only ever a lookup token — if PopNewest already
        // reclaimed the tab, the lookup misses and the tick no-ops.
        timer.Tick([alive = std::weak_ptr<int>(m_alive), this, key](
                       auto const&, auto const&) {
            if (alive.lock()) Expire(key);
        });
        entry.timer = timer;
        m_entries.push_back(std::move(entry));
        timer.Start();
        DEBUG_TRACE(L"UndoPark[%llu]: parked tab=%p idx=%u timeout=%llums "
                        L"(parked total: %zu)\n",
                        GetTickCount64() % 100'000, static_cast<void*>(key),
                        index, timeoutMs, m_entries.size());
    }

    // Reclaim the most recently parked tab (LIFO), stopping its
    // timer. Empty stack returns nullopt.
    struct Restored {
        std::unique_ptr<Tab> tab;
        uint32_t index;
    };
    std::optional<Restored> PopNewest()
    {
        if (m_entries.empty()) {
            DEBUG_TRACE(L"UndoPark[%llu]: undo requested, stack empty\n",
                            GetTickCount64() % 100'000);
            return std::nullopt;
        }
        Entry entry = std::move(m_entries.back());
        m_entries.pop_back();
        if (entry.timer) entry.timer.Stop();
        DEBUG_TRACE(L"UndoPark[%llu]: undo tab=%p (parked left: %zu)\n",
                        GetTickCount64() % 100'000,
                        static_cast<void*>(entry.tab.get()), m_entries.size());
        return Restored{ std::move(entry.tab), entry.index };
    }

    bool Empty() const noexcept { return m_entries.empty(); }
    size_t Size() const noexcept { return m_entries.size(); }

    // ----- redo bookkeeping -----
    // Redo = "close that tab again": remember the items most
    // recently restored by an undo. Weak because the user can close
    // the tab through any normal path meanwhile; a fresh
    // user-initiated close invalidates the whole history (standard
    // undo semantics — a new action clears redo).

    void RememberRedoCandidate(Microsoft::UI::Xaml::Controls::TabViewItem const& item)
    {
        m_redoItems.push_back(winrt::make_weak(item));
    }

    void ClearRedoCandidates() noexcept { m_redoItems.clear(); }

    // Newest still-resolvable redo candidate, consuming dead ones.
    // The caller re-checks that the item still maps to a live tab.
    std::optional<Microsoft::UI::Xaml::Controls::TabViewItem> PopRedoCandidate()
    {
        while (!m_redoItems.empty()) {
            auto weakItem = m_redoItems.back();
            m_redoItems.pop_back();
            if (auto item = weakItem.get()) return item;
        }
        return std::nullopt;
    }

    // Stop every timer and drop the entries (each ~Tab runs its
    // DetachAll catch-all). Called from the owner's destructor for
    // explicit ordering; idempotent.
    void Shutdown() noexcept
    {
        for (auto& entry : m_entries) {
            if (entry.timer) entry.timer.Stop();
        }
        m_entries.clear();
        m_redoItems.clear();
    }

private:
    struct Entry {
        std::unique_ptr<Tab> tab;
        uint32_t index{ 0 };
        Microsoft::UI::Dispatching::DispatcherQueueTimer timer{ nullptr };
        std::function<void(std::unique_ptr<Tab>)> onExpire;
    };

    void Expire(Tab* key)
    {
        auto it = std::find_if(
            m_entries.begin(), m_entries.end(),
            [key](Entry const& e) { return e.tab.get() == key; });
        if (it == m_entries.end()) return;
        if (it->timer) it->timer.Stop();
        auto tab = std::move(it->tab);
        auto onExpire = std::move(it->onExpire);
        m_entries.erase(it);
        DEBUG_TRACE(L"UndoPark[%llu]: expired tab=%p (parked left: %zu)\n",
                        GetTickCount64() % 100'000, static_cast<void*>(key),
                        m_entries.size());
        if (onExpire) onExpire(std::move(tab));
    }

    std::vector<Entry> m_entries;
    std::vector<winrt::weak_ref<Microsoft::UI::Xaml::Controls::TabViewItem>> m_redoItems;
    // Liveness token for timer ticks; see Park.
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
};

}  // namespace winrt::GhosttyWin32::implementation
