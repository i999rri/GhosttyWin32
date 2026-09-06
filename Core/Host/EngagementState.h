#pragma once

namespace core::host {

// Decides what to tell the OS about which text field it types into,
// from two facts that change independently: what the owner wants
// (Want) and whether a context exists to tell it to (ContextCreated /
// ContextReleased). Pure — every call returns the one action to
// perform now and performs nothing itself, so the rules can be
// tested without a text-services stack:
//
//   * a wish made before a context exists is remembered and applied
//     when one is created
//   * repeating the current state is a no-op
//   * releasing the context gives back focus only if it was taken
//   * the wish survives a release; the next context applies it again
//
// EditContext owns one of these and maps Enter / Leave onto
// CoreTextEditContext::NotifyFocusEnter / NotifyFocusLeave.
class EngagementState {
public:
    enum class Action { None, Enter, Leave };

    // The owner's wish. Returns what to do now — None while no
    // context exists (remembered) or when nothing changes.
    Action Want(bool engaged) noexcept {
        m_want = engaged;
        return Reconcile();
    }

    // A context now exists; apply the remembered wish.
    Action ContextCreated() noexcept {
        m_hasContext = true;
        return Reconcile();
    }

    // The context is going away. Leave only if this one was engaged.
    Action ContextReleased() noexcept {
        m_hasContext = false;
        if (!m_engaged) return Action::None;
        m_engaged = false;
        return Action::Leave;
    }

    bool HasContext() const noexcept { return m_hasContext; }
    // What the owner asked for.
    bool Wants() const noexcept { return m_want; }
    // What the OS has actually been told.
    bool IsEngaged() const noexcept { return m_engaged; }

private:
    Action Reconcile() noexcept {
        if (!m_hasContext || m_want == m_engaged) return Action::None;
        m_engaged = m_want;
        return m_want ? Action::Enter : Action::Leave;
    }

    bool m_hasContext = false;
    bool m_want = false;
    bool m_engaged = false;
};

}  // namespace core::host
