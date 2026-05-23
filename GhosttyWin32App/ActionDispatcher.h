#pragma once

#include "IMainWindowView.h"
#include "ghostty.h"
#include <memory>

namespace winrt::GhosttyWin32::implementation {

// Owns the action_cb dispatch table that previously lived as a
// ~470-line lambda inside MainWindow::InitGhostty. Construction
// goes through Create() so future fixture variants (test mocks,
// mode switches) can be added without rewriting every call site
// — the option-value is the signature, not the body.
//
// Dispatch is a flat switch over ghostty_action_tag_e; compilers
// turn closed-enum switches into jump tables, so the visitor /
// function-table machinery isn't worth the ceremony for ~30 cases.
// Per-handler state migrates from MainWindow into this class as
// handlers are moved across.
class ActionDispatcher {
public:
    // Factory entry point. Body today is just make_unique; keeping
    // it as a static helper means call sites stay stable when a
    // CreateForTest(mockView) or Create(view, AppMode::ReadOnly)
    // sibling gets added later.
    static std::unique_ptr<ActionDispatcher> Create(IMainWindowView& view);

    // Returns true when the action was handled (matching the
    // ghostty action_cb contract: false leaves the action eligible
    // for fallthrough / "unhandled" telemetry on libghostty's side).
    bool Dispatch(ghostty_target_s target, ghostty_action_s action);

private:
    explicit ActionDispatcher(IMainWindowView& view) noexcept : m_view(view) {}

    IMainWindowView& m_view;

    // Initial window size from GHOSTTY_ACTION_INITIAL_SIZE (physical
    // pixels). Zero means "not yet received" — RESET_WINDOW_SIZE
    // falls back to a DPI-scaled 1280x720 in that case. Lives on the
    // dispatcher because INITIAL_SIZE writes it and RESET_WINDOW_SIZE
    // reads it; no other code outside this class participates.
    uint32_t m_initialWidth = 0;
    uint32_t m_initialHeight = 0;
};

}  // namespace winrt::GhosttyWin32::implementation
