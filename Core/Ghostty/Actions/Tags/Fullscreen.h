#pragma once

namespace core::ghostty::actions::tags {

// TOGGLE_FULLSCREEN action state: whether this window is in
// borderless fullscreen, and which way the next toggle goes. Named
// after the ghostty action tag so the relationship with action_cb is
// obvious; the class is "the fullscreen thing", not "something that
// controls fullscreen".
//
// Pure value object. Spanning the monitor and coming back — the
// style strip, the saved placement — is win32::NativeWindow's job
// (EnterFullscreen / LeaveFullscreen); this class only answers
// `Active` and turns a keypress into Enter or Leave.
//
// Caveat carried over from the first implementation: the custom
// title bar lives in the XAML content tree, so it stays visible at
// the top of the surface in fullscreen. Hiding it is a follow-up;
// the window itself fills the monitor correctly.
class Fullscreen {
public:
    enum class Transition { Enter, Leave };

    Fullscreen() = default;

    // Apply the user's TOGGLE keypress; returns what the window has
    // to do about it.
    Transition Toggle() noexcept {
        m_active = !m_active;
        return m_active ? Transition::Enter : Transition::Leave;
    }

    // Whether borderless fullscreen is currently active. Used by
    // guards that must no-op while fullscreen (e.g. background-
    // opacity toggling, mirroring upstream macOS).
    bool Active() const noexcept { return m_active; }

private:
    bool m_active = false;
};

}  // namespace core::ghostty::actions::tags
