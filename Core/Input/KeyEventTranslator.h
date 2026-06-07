#pragma once

#include "Host/KeyModifiers.h"
#include "ghostty.h"
#include <cstdint>

namespace core::input {

// Primitive snapshot of one keyboard event, decoupled from WinUI's
// KeyRoutedEventArgs and from the live Win32 modifier-state query
// the host does before calling Translate. The translator only sees
// these fields, so unit tests can drive every input shape from a
// struct literal — no XAML runtime, no live keyboard.
//
// Press and release are separate types on purpose. Both rest on the
// same OS primitives (vk code, scan code, mods), but a key release
// doesn't carry text or an unshifted codepoint — there's no binding
// or text encoder that consumes those on the release path. Splitting
// keeps the release struct lean and lets the compiler pick the right
// Translate overload from the argument's static type — `FromKeyDown`
// can only produce a press, `FromKeyUp` can only produce a release,
// and the call site can't accidentally feed one to the other. Both
// structs stay free of ghostty types; the conversion is the job of
// the Translate overloads below.
//
// The common fields (vk_code, scan_code, is_extended, shift/ctrl/alt)
// are deliberately duplicated between the two structs instead of
// factored into a shared base. C++20 designated initializers don't
// reach inherited members directly — the syntax is `Derived{{.base
// = ...}, .derived = ...}`, which is more awkward than the flat
// repetition and makes test data harder to read. Six fields of
// duplication is the cheaper trade.
struct RawKeyPress {
    // Win32 virtual-key code (VK_*).
    uint32_t vk_code = 0;
    // Hardware scan code from KeyStatus.ScanCode. ghostty's
    // src/input/keycodes.zig table is keyed off this (Win column) —
    // it's how `physical_key` is resolved on the ghostty side.
    uint32_t scan_code = 0;
    // KeyStatus.IsExtendedKey. When true the host ORs 0xE000 into
    // `keycode` so the extended-key entries (arrow keys, the right-
    // hand modifiers, etc.) match the keycodes table.
    bool is_extended = false;
    // Live modifier state at event time. The host reads these from
    // GetKeyState because KeyRoutedEventArgs only reports the
    // modifier of the key being pressed itself, not the live combo.
    bool shift = false;
    bool ctrl  = false;
    bool alt   = false;
    // True while IME composition is in flight. ghostty's encoder
    // emits nothing for non-modifier keys while composing.
    bool composing = false;
    // OS-translated UTF-8 for printable input (`ToUnicode` with live
    // modifiers) — what ghostty's `event.utf8` corresponds to. Must
    // be null for control characters (< 0x20) because ghostty's
    // ctrlSeq / pcStyleFunctionKey re-encode those itself; setting
    // `text` for them would double-write. The pointer must outlive
    // the Translate call. nullptr when the key isn't printable.
    const char* text = nullptr;
    // The codepoint the key would produce with no modifiers held —
    // what `ToUnicode(vk, sc, zeroed_state, ...)` returns. ghostty
    // uses this to match unicode-keyed bindings (`ctrl+shift+t = new_tab`
    // matches against 't', not against the physical-key enum). 0
    // when the key has no printable form.
    uint32_t unshifted_codepoint = 0;
};

struct RawKeyRelease {
    uint32_t vk_code = 0;
    uint32_t scan_code = 0;
    bool is_extended = false;
    bool shift = false;
    bool ctrl  = false;
    bool alt   = false;
};

// Press overload. `keycode = scan_code | (extended ? 0xE000 : 0)`.
// The Zig side looks up the Win column in keycodes.zig to derive
// `physical_key`; if we hand it anything other than the raw hardware
// scan code, the lookup misses and every physical-key binding falls
// through to UNIDENTIFIED. `text` carries the OS-translated UTF-8
// through verbatim — ghostty's encoder writes event.utf8 to the pty
// when no binding matches, so populating `text` is how printable
// input reaches the terminal.
//
// `consumed_mods` stays 0. Setting it to the live modifier value
// claims every held modifier was consumed translating the key, which
// silently suppresses every modifier-based binding (ctrl+c never
// sends SIGINT, ctrl+shift+t never opens a new tab). The Win32
// ToUnicode call doesn't surface a "modifiers actually consumed"
// value, so leave it 0 and let ghostty compute the effective mods
// from `mods` directly.
inline ghostty_input_key_s Translate(RawKeyPress const& raw) noexcept
{
    return ghostty_input_key_s{
        .action              = GHOSTTY_ACTION_PRESS,
        .mods                = core::host::buildMods(raw.shift, raw.ctrl, raw.alt),
        .consumed_mods       = static_cast<ghostty_input_mods_e>(0),
        .keycode             = raw.scan_code | (raw.is_extended ? 0xE000u : 0u),
        .text                = raw.text,
        .unshifted_codepoint = raw.unshifted_codepoint,
        .composing           = raw.composing,
    };
}

// Release overload. No text / unshifted / composing — they aren't
// part of a release event.
inline ghostty_input_key_s Translate(RawKeyRelease const& raw) noexcept
{
    return ghostty_input_key_s{
        .action              = GHOSTTY_ACTION_RELEASE,
        .mods                = core::host::buildMods(raw.shift, raw.ctrl, raw.alt),
        .consumed_mods       = static_cast<ghostty_input_mods_e>(0),
        .keycode             = raw.scan_code | (raw.is_extended ? 0xE000u : 0u),
        .text                = nullptr,
        .unshifted_codepoint = 0,
        .composing           = false,
    };
}

}  // namespace core::input
