#pragma once

#include "ghostty.h"

#include <cstdint>
#include <utility>

namespace core::ghostty {

// RAII wrapper around a single ghostty_surface_t.
//
// Owns the handle: dtor (and Reset) call ghostty_surface_free. The
// destructor is the safety net — TerminalControl::Detach drives the
// real teardown order (stop SizeChanged / swap-chain-changed callbacks
// → call Reset() → release the shared_ptr the renderer thread watches),
// because ghostty_surface_free joins the renderer thread and must
// happen at exactly that point. The wrapper just makes "free the
// handle" expressible as `m_surface.reset()` instead of a raw call.
//
// Every typed method is a no-op when the wrapper is empty, so call
// sites don't need to null-check the handle themselves.
class Surface {
public:
    Surface() noexcept = default;
    explicit Surface(ghostty_surface_t handle) noexcept : m_handle(handle) {}

    Surface(Surface const&) = delete;
    Surface& operator=(Surface const&) = delete;

    Surface(Surface&& other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr)) {}

    Surface& operator=(Surface&& other) noexcept {
        if (this != &other) {
            Reset();
            m_handle = std::exchange(other.m_handle, nullptr);
        }
        return *this;
    }

    ~Surface() { Reset(); }

    void Reset() noexcept {
        if (m_handle) {
            ghostty_surface_free(m_handle);
            m_handle = nullptr;
        }
    }

    // Raw handle escape hatch — for libghostty APIs we haven't wrapped
    // yet, and for C action callbacks that receive the handle from
    // ghostty itself and need to compare it against ours.
    ghostty_surface_t Handle() const noexcept { return m_handle; }

    explicit operator bool() const noexcept { return m_handle != nullptr; }

    // True when this wrapper owns the given raw ghostty_surface_t.
    // Identity is the underlying pointer — libghostty action callbacks
    // hand the host a raw handle and the host walks its windows /
    // tabs / leaves asking "which Surface owns this?" to route the
    // action to the right TerminalControl. Spelled out as a method
    // rather than operator== so the call site reads as ownership
    // ("is this the surface that owns that handle?") instead of an
    // unexplained pointer comparison.
    bool Owns(ghostty_surface_t handle) const noexcept {
        return m_handle == handle;
    }

    // ---- input ----
    bool Key(ghostty_input_key_s ev) noexcept {
        return m_handle && ghostty_surface_key(m_handle, ev);
    }
    void Text(char const* text, uintptr_t len) noexcept {
        if (m_handle) ghostty_surface_text(m_handle, text, len);
    }
    void Preedit(char const* text, uintptr_t len) noexcept {
        if (m_handle) ghostty_surface_preedit(m_handle, text, len);
    }

    // ---- mouse ----
    bool MouseButton(ghostty_input_mouse_state_e state,
                     ghostty_input_mouse_button_e button,
                     ghostty_input_mods_e mods) noexcept {
        return m_handle &&
            ghostty_surface_mouse_button(m_handle, state, button, mods);
    }
    void MousePos(double x, double y, ghostty_input_mods_e mods) noexcept {
        if (m_handle) ghostty_surface_mouse_pos(m_handle, x, y, mods);
    }
    void MouseScroll(double dx, double dy,
                     ghostty_input_scroll_mods_t mods) noexcept {
        if (m_handle) ghostty_surface_mouse_scroll(m_handle, dx, dy, mods);
    }

    // ---- render / lifecycle ----
    void Refresh() noexcept {
        if (m_handle) ghostty_surface_refresh(m_handle);
    }
    // Renderer-side focus state. Drives the renderer thread's cadence
    // (focused surfaces poll faster) and cursor-blink gating; ghostty
    // defaults every surface to focused, so without these calls every
    // window keeps blink-presenting forever.
    void SetFocus(bool focused) noexcept {
        if (m_handle) ghostty_surface_set_focus(m_handle, focused);
    }
    // Renderer-side visibility. While false the renderer thread skips
    // draws entirely and parks the surface's DComp visual; the frame
    // shown at the time of hiding is redrawn on the next show.
    void SetOcclusion(bool visible) noexcept {
        if (m_handle) ghostty_surface_set_occlusion(m_handle, visible);
    }
    void SetSize(uint32_t w, uint32_t h) noexcept {
        if (m_handle) ghostty_surface_set_size(m_handle, w, h);
    }
    void SetContentScale(double x, double y) noexcept {
        if (m_handle) ghostty_surface_set_content_scale(m_handle, x, y);
    }
    // Per-surface light/dark override. ghostty_app_set_color_scheme
    // updates only the app-level conditional state, so a soft reload
    // triggered by that path re-derives each surface's config against
    // the surface's OLD scheme and the theme fails to switch until the
    // surface is recreated. Calling this per surface after the app-level
    // push forces each one to pick up the new scheme immediately.
    void SetColorScheme(ghostty_color_scheme_e scheme) noexcept {
        if (m_handle) ghostty_surface_set_color_scheme(m_handle, scheme);
    }

    // PID of the current foreground process in this surface's PTY —
    // not necessarily the shell (that's the ancestor). Used by the
    // tab-title poll to show the running command's name (`vim`,
    // `ssh host`) instead of just the shell name. Returns 0 when
    // no surface, no PTY yet, or no foreground process.
    uint32_t ForegroundPid() const noexcept {
        return m_handle ? ghostty_surface_foreground_pid(m_handle) : 0;
    }

    // ---- selection ----
    bool HasSelection() const noexcept {
        return m_handle && ghostty_surface_has_selection(m_handle);
    }
    bool ReadSelection(ghostty_text_s* out) const noexcept {
        return m_handle && ghostty_surface_read_selection(m_handle, out);
    }
    void FreeText(ghostty_text_s* text) noexcept {
        if (m_handle) ghostty_surface_free_text(m_handle, text);
    }

    // ---- IME ----
    void ImePoint(double* x, double* y, double* w, double* h) noexcept {
        if (m_handle) ghostty_surface_ime_point(m_handle, x, y, w, h);
    }

    // ---- clipboard callback completion ----
    // Called from the read-clipboard / confirm-read-clipboard runtime
    // callbacks to hand the clipboard content back to ghostty for
    // whichever surface requested it.
    void CompleteClipboardRequest(char const* content, void* state,
                                  bool confirmed) noexcept {
        if (m_handle) {
            ghostty_surface_complete_clipboard_request(
                m_handle, content, state, confirmed);
        }
    }

private:
    ghostty_surface_t m_handle{ nullptr };
};

}  // namespace core::ghostty
