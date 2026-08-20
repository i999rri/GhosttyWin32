#pragma once

#include "ghostty.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <winrt/Windows.UI.h>

namespace core::ghostty {

// Typed, fallback-aware read-only view over `ghostty_config_t`.
//
// Centralises three things every consumer otherwise has to redo by
// hand against `ghostty_config_get`:
//
//   1. The 4th argument is the byte length of the KEY string, not
//      the size of the output buffer. Getting that wrong silently
//      truncates the key to its first 2-3 characters, which makes
//      every lookup miss. The templated `GetRaw` helper makes the
//      key length compile-time-correct by deriving it from the
//      string literal's type.
//
//   2. Conversion from the raw byte structs ghostty hands back
//      (`ghostty_config_color_s`) into the WinUI primitives the
//      host actually paints with (`winrt::Windows::UI::Color`).
//
//   3. Per-key fallback chains. `split-divider-color` falls back to
//      `background` darkened by 8% (light bg) or 40% (dark bg) —
//      matching upstream's macOS `splitDividerColor`. The
//      unfocused-split-fill chain is the same shape. Centralising
//      these in one place keeps the call sites at the use site
//      ("give me the divider colour") instead of the lookup site
//      ("read this key, then that key, then compute this fallback").
//
// Borrows the config handle; ownership lives on `GhosttyApp`. Cheap
// to construct, copy and pass by value.
class Config {
public:
    explicit Config(ghostty_config_t config) noexcept
        : m_config(config) {}

    // The terminal background colour as the user / theme configured
    // it. Falls back to black if even `background` isn't readable,
    // which would only happen with a broken or absent config.
    winrt::Windows::UI::Color Background() const noexcept {
        return ColorFrom(BackgroundRgb());
    }

    // Splitter divider colour. Honours `split-divider-color` when
    // set; otherwise derives from `background` darkened by 8% on
    // light backgrounds, 40% on dark ones — same algorithm as
    // upstream macOS `Ghostty.Config.splitDividerColor`.
    winrt::Windows::UI::Color SplitDividerColor() const noexcept {
        ghostty_config_color_s c{};
        if (GetRaw("split-divider-color", &c)) return ColorFrom(c);
        return DeriveDividerFromBackground(BackgroundRgb());
    }

    // Whether the user's config asks for window chrome (caption
    // buttons + drag region) to be shown. Reads the ghostty enum
    // `window-decoration` and returns true for any value except
    // `none` (i.e. auto / client / server all map to "decorated").
    // Defaults to true if the key isn't readable — matching ghostty's
    // own default of `.auto`. Used by the TOGGLE_WINDOW_DECORATIONS
    // tag (#68) to know what "the config default" means before
    // applying per-window overrides.
    // `background-opacity` as configured (clamped by ghostty to
    // [0,1] at load). 1.0 = fully opaque; the TOGGLE_BACKGROUND_
    // OPACITY guards treat >= 1.0 as "no transparency configured",
    // mirroring upstream macOS.
    double BackgroundOpacity() const noexcept {
        double opacity = 1.0;
        GetRaw("background-opacity", &opacity);
        return opacity;
    }

    // `background-blur` crosses the C API via cval() as an i16:
    // 0 = off, 20 = a bare `true`, positive = the radius, negative
    // = macOS glass sentinels ("imply some blur" per upstream, so
    // any non-zero value counts as enabled here).
    bool BackgroundBlurEnabled() const noexcept {
        short v = 0;
        if (!GetRaw("background-blur", &v)) return false;
        return v != 0;
    }

    // ----- notify-on-command-finish (v1.3.0+, default: never) -----
    // The whole feature is opt-in; every getter falls back to the
    // documented default when the key is unreadable.

    enum class NotifyOnCommandFinish { Never, Unfocused, Always };
    NotifyOnCommandFinish CommandFinishNotify() const noexcept {
        // Enum values arrive as their @tagName string — see the
        // pointer-width warning on WindowDecoratedByConfig below.
        const char* tag = nullptr;
        if (!GetRaw("notify-on-command-finish", &tag) || !tag) {
            return NotifyOnCommandFinish::Never;
        }
        if (std::strcmp(tag, "unfocused") == 0) return NotifyOnCommandFinish::Unfocused;
        if (std::strcmp(tag, "always") == 0) return NotifyOnCommandFinish::Always;
        return NotifyOnCommandFinish::Never;
    }

    // Minimum run time before a finished command qualifies.
    // ghostty's Duration crosses the C API via cval() as
    // MILLISECONDS (a usize) — NOT the nanoseconds the
    // COMMAND_FINISHED action payload uses. Convert here so
    // callers compare ns against ns; comparing the raw value made
    // a 1s threshold behave as 1µs (caught during #148 testing).
    uint64_t CommandFinishNotifyAfterNs() const noexcept {
        size_t ms = 0;
        if (!GetRaw("notify-on-command-finish-after", &ms)) {
            return 5'000'000'000ull;  // documented default: 5s
        }
        return static_cast<uint64_t>(ms) * 1'000'000ull;
    }

    // How to notify. The packed struct crosses the C API as its bit
    // representation in a c_uint: bit 0 = bell (field order in
    // ghostty's NotifyOnCommandFinishAction), bit 1 = notify.
    struct CommandFinishActions {
        bool bell;
        bool notify;
    };
    CommandFinishActions CommandFinishNotifyActions() const noexcept {
        unsigned bits = 0;
        if (!GetRaw("notify-on-command-finish-action", &bits)) {
            return { true, false };  // documented default: bell only
        }
        return { (bits & 1u) != 0, (bits & 2u) != 0 };
    }

    bool WindowDecoratedByConfig() const noexcept {
        // ghostty exposes enum config values as their @tagName — a
        // pointer to a null-terminated string — NOT as the
        // underlying integer (see ghostty src/config/c_get.zig
        // line 62-65). The output pointer must be a `const char*`,
        // not an int; treating it as an int silently truncates the
        // 64-bit pointer write to 4 bytes and corrupts the stack.
        // Possible tag names: "auto" / "client" / "server" / "none".
        const char* tag = nullptr;
        if (!GetRaw("window-decoration", &tag) || !tag) {
            return true;  // key unreadable; assume decorated default
        }
        return std::strcmp(tag, "none") != 0;
    }

    // Unfocused-split overlay opacity as ghostty defines it: the
    // visibility of the unfocused side itself (0.7 default = "still
    // mostly visible"). The host typically wants the overlay alpha,
    // which is (1 - this value); kept as the raw config-side number
    // so it round-trips with the docs.
    double UnfocusedSplitOpacity() const noexcept {
        double opacity = 0.7;
        GetRaw("unfocused-split-opacity", &opacity);
        return opacity;
    }

    // Fill colour the host overlays on unfocused splits. Honours
    // `unfocused-split-fill` when set; otherwise falls back to the
    // background colour — matches upstream behaviour where the
    // dimming overlay defaults to the terminal's own background.
    winrt::Windows::UI::Color UnfocusedSplitFill() const noexcept {
        ghostty_config_color_s c{};
        if (GetRaw("unfocused-split-fill", &c)) return ColorFrom(c);
        return ColorFrom(BackgroundRgb());
    }

    // ----- pure helpers exposed for testing -----
    // These have no dependency on ghostty_config_t and can be
    // exercised directly by unit tests; the live config-aware
    // accessors above ultimately delegate to them.

    // Linear darken by `factor` in [0, 1]. Applied per-channel; no
    // gamma correction because the upstream implementation doesn't
    // do any either (`darken(by:)` in OSColor) — staying byte-equal
    // with upstream is worth more here than colorimetric accuracy.
    static ghostty_config_color_s Darken(ghostty_config_color_s c, double factor) noexcept {
        const double k = 1.0 - factor;
        return {
            static_cast<std::uint8_t>(c.r * k),
            static_cast<std::uint8_t>(c.g * k),
            static_cast<std::uint8_t>(c.b * k),
        };
    }

    // True when the channel-average of `c` exceeds 128 — the
    // "light background" classifier upstream uses to pick the
    // 8 % vs 40 % darken factor for the split divider fallback.
    static bool IsLight(ghostty_config_color_s c) noexcept {
        return (static_cast<int>(c.r)
                + static_cast<int>(c.g)
                + static_cast<int>(c.b)) / 3 > 128;
    }

    // Compute the divider colour fallback from a background colour
    // alone. Pure function — the live SplitDividerColor() method
    // first probes `split-divider-color`, then falls back here.
    static winrt::Windows::UI::Color DeriveDividerFromBackground(ghostty_config_color_s bg) noexcept {
        const double factor = IsLight(bg) ? 0.08 : 0.40;
        return ColorFrom(Darken(bg, factor));
    }

    // Promote a raw RGB triple to an opaque WinUI ARGB colour. The
    // host applies its own translucency where needed (UnfocusedDim
    // Opacity, etc.), so every accessor returns alpha=255.
    static winrt::Windows::UI::Color ColorFrom(ghostty_config_color_s c) noexcept {
        return winrt::Windows::UI::Color{ 255, c.r, c.g, c.b };
    }

private:
    // Compile-time-correct key length lookup. The template parameter
    // N captures the string-literal array length (including the null
    // terminator), so `sizeof("key") - 1` becomes free of off-by-one
    // risk at every call site. The bool short-circuit also guards
    // against a null config handle so callers don't have to.
    template <std::size_t N>
    bool GetRaw(const char (&key)[N], void* out) const noexcept {
        return m_config != nullptr
            && ghostty_config_get(m_config, out, key, N - 1);
    }

    // Background as a raw RGB triple. Used as the source for derived
    // colours (divider darken, unfocused fill fallback). Black on
    // miss so the downstream maths still produces a usable colour.
    ghostty_config_color_s BackgroundRgb() const noexcept {
        ghostty_config_color_s bg{ 0, 0, 0 };
        GetRaw("background", &bg);
        return bg;
    }

    ghostty_config_t m_config;
};

}  // namespace core::ghostty
