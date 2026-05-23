#pragma once

#include "GhosttyActions.h"
#include "IMainWindowView.h"
#include "ghostty.h"
#include <memory>

namespace winrt::GhosttyWin32::implementation {

// Routing layer for ghostty's action_cb. Owns no implementation
// logic of its own — every case in the switch resolves to a
// GhosttyActions method (or returns true directly when the
// action is an intentional ack-only with no handler at all,
// which is a routing decision, not an implementation one).
//
// Splitting "what to do" (GhosttyActions) from "which method to
// call" (this class) means the routing table stays scannable for
// missing tags, and GhosttyActions stays exercisable in isolation
// — no need to thread a fake ghostty_action_s through the switch
// just to drive one handler.
//
// Construction via Create() so future fixture variants (test
// mocks, mode switches) can be added without rewriting every
// call site — the option-value is the signature, not the body.
class ActionDispatcher {
public:
    static std::unique_ptr<ActionDispatcher> Create(IMainWindowView& view);

    // Returns true when the action was handled (matches the
    // ghostty action_cb contract: false leaves the action eligible
    // for fallthrough / "unhandled" telemetry on libghostty's
    // side).
    bool Dispatch(ghostty_target_s target, ghostty_action_s action);

private:
    explicit ActionDispatcher(IMainWindowView& view) noexcept
        : m_actions(view) {}

    GhosttyActions m_actions;
};

}  // namespace winrt::GhosttyWin32::implementation
