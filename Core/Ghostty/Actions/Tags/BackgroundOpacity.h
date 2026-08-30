#pragma once

namespace core::ghostty::actions::tags {

// TOGGLE_BACKGROUND_OPACITY action state (#69). Per-window flip
// between "as configured" (translucent when `background-opacity`
// < 1.0, which is also the launch state) and "fully opaque", with
// the same two guards as upstream macOS
// (BaseTerminalController.toggleBackgroundOpacity):
//
//   * nothing to toggle when the config is already opaque
//   * never while fullscreen (transparency doesn't apply there)
//
// The Windows-specific part is how the mode becomes pixels. The
// window backdrop mirrors Windows Terminal's two transparency modes
// and maps 1:1 onto ghostty config:
//
//   translucent + background-blur  -> ClearAcrylic (pure blur of
//     whatever is behind the window; the stock DesktopAcrylic tint
//     would swallow the terminal's own translucency)
//   translucent, no blur           -> Transparent (crisp see-through,
//     WT's "vintage opacity" look). DWM composites a window as opaque
//     by default, so this one also needs per-pixel alpha switched on
//     (DwmEnableBlurBehindWindow with an empty region)
//   opaque                         -> Mica, as before. Mica alone was
//     tried first and reads as "slightly gray", not transparent — it
//     only tints toward the wallpaper
//
// Pure value object: `Toggle` applies the keypress, `Effective`
// turns the current config + override into an Appearance the view
// side (MainWindow) just applies. No Win32 / XAML in here so the
// guards and the backdrop decision are unit-testable.
class BackgroundOpacity {
public:
    enum class Backdrop {
        Transparent,   // crisp see-through
        ClearAcrylic,  // blurred see-through
        Mica,          // opaque
    };

    // What the window should look like right now. Every field is a
    // decision already made; the view only carries it out.
    struct Appearance {
        Backdrop backdrop;
        // Ask DWM to honour the window's per-pixel alpha. Only the
        // crisp mode needs it — Acrylic / Mica are DWM materials that
        // composite on their own.
        bool dwmPerPixelAlpha;
        // Paint the XAML root with the terminal background. In the
        // translucent modes the root stays unpainted so the backdrop
        // shows through behind the panes.
        bool paintRoot;
        // Show each pane's opaque underlay. Only earns its pixel cost
        // when it changes the result: config transparency present AND
        // the user toggled opaque (with opacity 1.0 the swap chain is
        // opaque anyway).
        bool paneUnderlay;

        bool operator==(const Appearance&) const noexcept = default;
    };

    BackgroundOpacity() = default;
    BackgroundOpacity(const BackgroundOpacity&) = delete;
    BackgroundOpacity& operator=(const BackgroundOpacity&) = delete;

    // Apply the user's TOGGLE keypress. `configOpacity` is the current
    // `background-opacity` (Config::BackgroundOpacity()), `fullscreen`
    // whether borderless fullscreen is active. Returns whether the
    // mode flipped — false means a guard fired and there is nothing
    // to re-apply.
    bool Toggle(double configOpacity, bool fullscreen) noexcept {
        if (configOpacity >= 1.0) return false;
        if (fullscreen) return false;
        m_opaque = !m_opaque;
        return true;
    }

    // Whether the user has toggled to fully opaque. Meaningless while
    // the config opacity is 1.0 — `Effective` ignores it there.
    bool Opaque() const noexcept { return m_opaque; }

    // The look for the current config and override. `configBlur` is
    // Config::BackgroundBlurEnabled().
    Appearance Effective(double configOpacity, bool configBlur) const noexcept {
        const bool transparentConfig = configOpacity < 1.0;
        const bool translucent = transparentConfig && !m_opaque;
        const bool blur = translucent && configBlur;
        return Appearance{
            .backdrop = !translucent ? Backdrop::Mica
                      : blur         ? Backdrop::ClearAcrylic
                                     : Backdrop::Transparent,
            .dwmPerPixelAlpha = translucent && !blur,
            .paintRoot = !translucent,
            .paneUnderlay = transparentConfig && m_opaque,
        };
    }

private:
    bool m_opaque = false;
};

}  // namespace core::ghostty::actions::tags
