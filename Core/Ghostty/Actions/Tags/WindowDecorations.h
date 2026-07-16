#pragma once

namespace core::ghostty::actions::tags {

// TOGGLE_WINDOW_DECORATIONS action state. Per-window override of the
// config-level `window-decoration` setting, mirroring upstream GTK's
// 3-state semantics:
//
//   * No override (= use config default)
//   * Forced decorated
//   * Forced undecorated
//
// Toggle behaviour (also matches upstream GTK class/window.zig):
//
//   * From "no override": flip to the opposite of the config default
//     (config says decorated → force undecorated, and vice versa)
//   * From a forced state: clear back to "no override" so subsequent
//     CONFIG_CHANGE re-reads pick up the user's current value
//
// The state is in-memory per-window; not persisted, not cleared by
// CONFIG_CHANGE (a config reload that flips `window-decoration` while
// an override is in place leaves the override winning, same as GTK).
//
// Pure value object — the view side (MainWindow) reads `Effective`
// to know whether to show the caption buttons + drag region, and
// calls `Toggle` from the action handler. No Win32 / XAML in here so
// the toggle/effective logic is trivially unit-testable.
class WindowDecorations {
public:
    WindowDecorations() = default;
    WindowDecorations(const WindowDecorations&) = delete;
    WindowDecorations& operator=(const WindowDecorations&) = delete;

    // Apply the user's TOGGLE keypress. `configDecorated` is what the
    // current config says (`Config::WindowDecoratedByConfig()`).
    // Returns the new effective state so the caller can apply it
    // without a follow-up `Effective` call.
    bool Toggle(bool configDecorated) noexcept {
        switch (m_override) {
            case Override::None:
                m_override = configDecorated
                    ? Override::ForceUndecorated
                    : Override::ForceDecorated;
                break;
            case Override::ForceDecorated:
            case Override::ForceUndecorated:
                m_override = Override::None;
                break;
        }
        return Effective(configDecorated);
    }

    // Whether window chrome should be visible right now, given the
    // current config default and any active override.
    bool Effective(bool configDecorated) const noexcept {
        switch (m_override) {
            case Override::ForceDecorated:   return true;
            case Override::ForceUndecorated: return false;
            case Override::None:             return configDecorated;
        }
        return configDecorated;
    }

    // Whether the user has installed an override. Useful for tests
    // and for diagnostics ("config says X but the user toggled it").
    bool HasOverride() const noexcept { return m_override != Override::None; }

private:
    enum class Override {
        None,              // follow the config
        ForceDecorated,    // ignore config, show chrome
        ForceUndecorated,  // ignore config, hide chrome
    };
    Override m_override = Override::None;
};

}  // namespace core::ghostty::actions::tags
