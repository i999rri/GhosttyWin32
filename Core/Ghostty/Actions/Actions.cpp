#include "Actions.h"
#include "Tags/TriggerLabel.h"
#include "Interop/Encoding.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <utility>

namespace core::ghostty::actions {

// ===== terminal events =====

bool Actions::OnRingBell() {
    // Terminal sent BEL (\x07). MessageBeep plays the user's
    // configured "Default Beep" sound asynchronously and is
    // thread-safe, so we don't bounce through the UI dispatcher.
    // Honouring `bell-features` (audio / attention / title /
    // unread) is a follow-up.
    MessageBeep(MB_OK);
    return true;
}

bool Actions::OnShowChildExited(ghostty_surface_message_childexited_s ce) {
    // Shell process for a surface exited. With confirm-close-
    // surface=false (our default) the surface tears itself down
    // via close_surface_cb almost immediately, so this is a
    // breadcrumb. A proper in-terminal overlay is design work;
    // log and move on. Mirror to OutputDebugString so the line
    // survives stderr buffering through surface teardown.
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "[child_exited] exit_code=%u after_ms=%llu\n",
                  ce.exit_code,
                  static_cast<unsigned long long>(ce.timetime_ms));
    std::fputs(buf, stderr);
    std::fflush(stderr);
    OutputDebugStringA(buf);
    return true;
}

bool Actions::OnRendererHealth(ghostty_action_renderer_health_e health) {
    // UNHEALTHY means the generic renderer hit a problem (texture
    // allocation, shader compile, etc.) and dropped into a
    // degraded mode. One stderr line keeps the root cause findable
    // in the debugger output without committing to user-facing UX
    // (toast, status bar) yet.
    bool healthy = health == GHOSTTY_RENDERER_HEALTH_HEALTHY;
    std::fprintf(stderr, "[renderer_health] %s\n",
                 healthy ? "healthy" : "unhealthy");
    std::fflush(stderr);
    return true;
}

bool Actions::OnRender() {
    // ghostty wants a repaint outside the natural wakeup_cb ->
    // tick cadence. The dispatcher used by wakeup_cb already
    // serialises ticks on the UI thread, so we go through the
    // same path. ghostty_app_tick is idempotent — calling it
    // when nothing is dirty is a cheap no-op.
    DispatchToView([this]() {
        m_view.Tick();
    });
    return true;
}

// ===== shell-verb passthroughs =====

bool Actions::OnCheckForUpdates() {
    // No built-in updater on Windows; hand the user off to the
    // GitHub releases page. ShellExecuteW dispatches via the
    // default browser without the rundll32 child-process flash
    // libghostty's std.process.Child fallback would otherwise
    // produce.
    ShellExecuteW(m_view.Hwnd(), L"open",
                  L"https://github.com/i999rri/GhosttyWin32/releases",
                  nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

bool Actions::OnOpenUrl(ghostty_action_open_url_s ou) {
    // Ctrl+click on a URL in the terminal. Hand off to the shell
    // verb opener so the user's default browser / mail client /
    // etc. handles it.
    if (ou.url && ou.len > 0) {
        std::wstring wurl = interop::Encoding::toUtf16(ou.url, static_cast<int>(ou.len));
        if (!wurl.empty()) {
            ShellExecuteW(m_view.Hwnd(), L"open", wurl.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    return true;
}

// ===== window lifecycle =====

bool Actions::OnCloseWindow() {
    // Close the window that owns the action's target and nothing
    // else. Same intent as the X button — go through TryClose so
    // needs_confirm_quit prompts the user before we tear anything
    // down. Sibling windows survive either way.
    DispatchToView([this]() {
        m_view.TryClose();
    });
    return true;
}

bool Actions::OnCloseAllWindows() {
    // Fixtures without the app hook degrade to closing the one
    // window they can reach — the pre-split behaviour.
    if (!m_hooks.closeAllWindows) return OnCloseWindow();
    // UI-thread hop for the same reason as OnNewWindow: window
    // teardown is XAML work, and every window shares this thread.
    DispatchToView([this]() {
        m_hooks.closeAllWindows();
    });
    return true;
}

bool Actions::OnQuit() {
    if (!m_hooks.quit) return OnCloseWindow();
    DispatchToView([this]() {
        m_hooks.quit();
    });
    return true;
}

bool Actions::OnToggleVisibility() {
    // SW_MINIMIZE / SW_RESTORE instead of SW_HIDE because hiding
    // from the taskbar leaves Windows users with no discoverable
    // way back — ghostty's `global:` keybind qualifier isn't
    // wired to RegisterHotKey on this port yet, so a SW_HIDE'd
    // window with no taskbar entry can only be recovered by
    // relaunching. Minimizing keeps the window reachable via
    // taskbar click / alt-tab.
    DispatchToView([this]() {
        HWND hwnd = m_view.Hwnd();
        if (!hwnd) return;
        if (IsIconic(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else {
            ShowWindow(hwnd, SW_MINIMIZE);
        }
    });
    return true;
}

bool Actions::OnToggleMaximize() {
    // WM_SYSCOMMAND path used by the caption-button click — keeps
    // the NVIDIA presenter AV from issue #26 out of the picture.
    // SendMessage runs on the UI thread; dispatch through
    // Dispatcher() because action_cb fires from the renderer
    // thread.
    DispatchToView([this]() {
        HWND hwnd = m_view.Hwnd();
        if (!hwnd) return;
        SendMessageW(hwnd, WM_SYSCOMMAND,
                     IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
    });
    return true;
}

bool Actions::OnPresentTerminal() {
    // Used by external notification "click-to-focus" paths. The
    // view restores minimized state and grabs foreground; raw
    // SetForegroundWindow alone would silently fail when our
    // window isn't already in the allowed-set per Win32's
    // foreground rules.
    DispatchToView([this]() {
        m_view.PresentTerminal();
    });
    return true;
}

bool Actions::OnFloatWindow(ghostty_action_float_window_e mode) {
    // Route through the view: the view owns the HWND and remembers
    // the current topmost state so TOGGLE can flip without the
    // dispatcher tracking it.
    DispatchToView([this, mode]() {
        m_view.SetFloatOnTop(mode);
    });
    return true;
}

bool Actions::OnShowOnScreenKeyboard() {
    // Touch / pen users without a physical keyboard. The OSK is
    // its own top-level window; we just launch it and let the user
    // dismiss it normally.
    DispatchToView([this]() {
        m_view.ShowOnScreenKeyboard();
    });
    return true;
}

bool Actions::OnOpenConfig() {
    // Open the user's ghostty config in their default editor.
    // The Windows config path is %LOCALAPPDATA%\ghostty\config
    // (no extension); without an association Windows shows the
    // "Open With" dialog, which is the right OS-native behaviour
    // for first run. GetEnvironmentVariableW over _wgetenv: the
    // CRT helper is marked deprecated under MSVC /W4, the Win32
    // API is the documented modern path with a caller-supplied
    // buffer.
    wchar_t appdata[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", appdata,
                                        static_cast<DWORD>(std::size(appdata)));
    if (len > 0 && len < std::size(appdata)) {
        std::wstring path = std::wstring(appdata) + L"\\ghostty\\config";
        ShellExecuteW(nullptr, L"open", path.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
    return true;
}

// ===== sizing =====

bool Actions::OnInitialSize(ghostty_action_initial_size_s size) {
    // Record the desired startup window dimensions ghostty
    // computes from config (`window-width` × `cell-width-px`,
    // etc.). Stored as physical pixels — ghostty already did the
    // cell-to-pixel math, so OnResetWindowSize can hand the value
    // to SetWindowPos directly without re-scaling.
    m_initialWidth = size.width;
    m_initialHeight = size.height;
    return true;
}

bool Actions::OnResetWindowSize() {
    // Restore the window to its startup footprint. Prefer the
    // size OnInitialSize recorded (honoring the user's config);
    // if INITIAL_SIZE never fired, fall back to 1280×720 DIPs —
    // matches the WinUI 3 fresh-window default and lands an
    // 80×24-ish terminal at common font sizes.
    uint32_t w = m_initialWidth;
    uint32_t h = m_initialHeight;
    DispatchToView([this, w, h]() {
        HWND hwnd = m_view.Hwnd();
        if (!hwnd) return;
        int width, height;
        if (w && h) {
            width = static_cast<int>(w);
            height = static_cast<int>(h);
        } else {
            UINT dpi = GetDpiForWindow(hwnd);
            width = MulDiv(1280, dpi, 96);
            height = MulDiv(720, dpi, 96);
        }
        SetWindowPos(hwnd, nullptr, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    });
    return true;
}

// ===== tab lifecycle / navigation / title =====

bool Actions::OnNewTab() {
    DispatchToView([this]() {
        m_view.CreateTab();
    });
    return true;
}

bool Actions::OnNewWindow() {
    if (!m_hooks.newWindow) return false;
    // Hop to the UI thread — CreateNewWindow builds a Xaml Window
    // and every Xaml touch has to happen there. The `m_view`
    // dispatch is a UI-thread hop that any live window can provide;
    // we don't need our own dispatcher for this to arrive there.
    DispatchToView([this]() {
        m_hooks.newWindow();
    });
    return true;
}

bool Actions::OnGotoWindow(ghostty_action_goto_window_e direction) {
    if (!m_hooks.gotoWindow) return false;
    // Same UI-thread hop rationale as OnNewWindow — the hook calls
    // Xaml Window::Activate under the hood.
    DispatchToView([this, direction]() {
        m_hooks.gotoWindow(direction);
    });
    return true;
}

bool Actions::OnCloseTab(ghostty_surface_t surface) {
    if (!surface) return true;
    DispatchToView([this, surface]() {
        m_view.CloseTabBySurface(surface);
    });
    return true;
}

bool Actions::OnGotoTab(int requested) {
    DispatchToView([this, requested]() {
        m_view.GoToTab(requested);
    });
    return true;
}

bool Actions::OnMoveTab(ghostty_action_move_tab_s move) {
    // ghostty hands us a signed offset; semantics are "shift the
    // currently selected tab by N positions, clamped to the
    // TabView's bounds" (no wrap-around — matches upstream).
    DispatchToView([this, move]() {
        m_view.MoveActiveTabBy(move.amount);
    });
    return true;
}

bool Actions::OnSetTitle(ghostty_surface_t surface, const char* utf8Title) {
    // Convert on the renderer thread so the UTF-16 string is the
    // only thing the captured lambda carries. The lambda copy then
    // makes a single hstring on the UI side.
    if (!surface || !utf8Title) return true;
    std::wstring wide = interop::Encoding::toUtf16(utf8Title);
    if (wide.empty()) return true;
    DispatchToView([this, surface, wide = std::move(wide)]() mutable {
        m_view.SetTabTitleForSurface(surface, std::move(wide));
    });
    return true;
}

bool Actions::OnCopyTitleToClipboard(ghostty_surface_t surface) {
    if (!surface) return true;
    DispatchToView([this, surface]() {
        m_view.CopyTabTitleForSurface(surface);
    });
    return true;
}

// ===== terminal-driven appearance + lifecycle =====

bool Actions::OnColorChange(ghostty_action_color_change_s cc) {
    // Only background colour drives our title/XAML chrome; the
    // other kinds (cursor, palette indices, etc.) are surface-
    // visible only and ghostty handles them internally.
    if (cc.kind != GHOSTTY_ACTION_COLOR_KIND_BACKGROUND) return true;
    uint8_t r = cc.r, g = cc.g, b = cc.b;
    DispatchToView([this, r, g, b]() {
        m_view.ApplyBackgroundColor(r, g, b);
    });
    return true;
}

bool Actions::OnMouseShape(ghostty_surface_t surface,
                                  ghostty_action_mouse_shape_e shape) {
    if (!surface) return true;
    DispatchToSurface(surface, [shape](host::ISurfaceView& v) {
        v.SetCursorShape(shape);
    });
    return true;
}

bool Actions::OnMouseVisibility(ghostty_surface_t surface,
                                ghostty_action_mouse_visibility_e visibility) {
    if (!surface) return true;
    const bool visible = visibility == GHOSTTY_MOUSE_VISIBLE;
    DispatchToSurface(surface, [visible](host::ISurfaceView& v) {
        v.SetMouseVisibility(visible);
    });
    return true;
}

bool Actions::OnSecureInput(ghostty_surface_t surface,
                            ghostty_action_secure_input_e mode) {
    if (!surface) return true;
    // TOGGLE resolution happens in the view: the indicator is
    // per-pane visual state, so the pane owns the current value.
    DispatchToSurface(surface, [mode](host::ISurfaceView& v) {
        v.SetSecureInput(mode);
    });
    return true;
}

bool Actions::OnPwd(ghostty_surface_t surface, const char* utf8Pwd) {
    if (!surface) return true;
    // Unlike most string payloads, pwd IS NUL-terminated (matches
    // the macOS apprt's String(cString:) usage). An empty string is
    // dispatched through so the view can clear the tooltip.
    std::wstring wide = utf8Pwd ? interop::Encoding::toUtf16(utf8Pwd)
                                : std::wstring{};
    DispatchToView([this, surface, wide = std::move(wide)]() mutable {
        m_view.SetPwdForSurface(surface, std::move(wide));
    });
    return true;
}

bool Actions::OnPromptTitle(ghostty_surface_t surface) {
    if (!surface) return true;
    DispatchToView([this, surface]() {
        m_view.PromptTitleForSurface(surface);
    });
    return true;
}

bool Actions::OnReadonly(ghostty_surface_t surface,
                         ghostty_action_readonly_e readonly) {
    if (!surface) return true;
    const bool on = readonly == GHOSTTY_READONLY_ON;
    DispatchToSurface(surface, [on](host::ISurfaceView& v) {
        v.SetReadonly(on);
    });
    return true;
}

bool Actions::OnCommandFinished(ghostty_surface_t surface,
                                ghostty_action_command_finished_s cf) {
    if (!surface) return true;
    const int exitCode = cf.exit_code;   // -1 = not reported
    const uint64_t durationNs = cf.duration;
    DispatchToView([this, surface, exitCode, durationNs]() {
        m_view.NotifyCommandFinishedForSurface(surface, exitCode, durationNs);
    });
    return true;
}

bool Actions::OnKeySequence(ghostty_surface_t surface,
                            ghostty_action_key_sequence_s seq) {
    if (!surface) return true;
    if (!seq.active) {
        DispatchToSurface(surface, [](host::ISurfaceView& v) {
            v.ClearKeySequence();
        });
        return true;
    }
    std::wstring label = TriggerLabel(seq.trigger);
    DispatchToSurface(surface, [label = std::move(label)](host::ISurfaceView& v) mutable {
        v.AppendKeySequence(std::move(label));
    });
    return true;
}

bool Actions::OnKeyTable(ghostty_surface_t surface,
                         ghostty_action_key_table_s table) {
    if (!surface) return true;
    switch (table.tag) {
        case GHOSTTY_KEY_TABLE_ACTIVATE: {
            // The name is length-bounded, not NUL-terminated.
            std::wstring name;
            if (table.value.activate.name && table.value.activate.len > 0) {
                name = interop::Encoding::toUtf16(
                    table.value.activate.name,
                    static_cast<int>(table.value.activate.len));
            }
            DispatchToSurface(surface, [name = std::move(name)](host::ISurfaceView& v) mutable {
                v.PushKeyTable(std::move(name));
            });
            return true;
        }
        case GHOSTTY_KEY_TABLE_DEACTIVATE:
        case GHOSTTY_KEY_TABLE_DEACTIVATE_ALL: {
            const bool all = table.tag == GHOSTTY_KEY_TABLE_DEACTIVATE_ALL;
            DispatchToSurface(surface, [all](host::ISurfaceView& v) {
                v.PopKeyTable(all);
            });
            return true;
        }
    }
    return true;
}

bool Actions::OnMouseOverLink(ghostty_surface_t surface,
                              ghostty_action_mouse_over_link_s link) {
    if (!surface) return true;
    // The url field is NOT NUL-terminated — len bounds it. len == 0
    // means the pointer left the link; the empty string flows through
    // so the view hides the banner.
    std::wstring wide;
    if (link.url && link.len > 0)
        wide = interop::Encoding::toUtf16(link.url, static_cast<int>(link.len));
    DispatchToSurface(surface, [wide = std::move(wide)](host::ISurfaceView& v) mutable {
        v.SetHoveredLink(std::move(wide));
    });
    return true;
}

bool Actions::OnToggleBackgroundOpacity() {
    // Guards (opacity >= 1.0, fullscreen) live in the view — it
    // owns config access and the fullscreen state.
    DispatchToView([this]() {
        m_view.ToggleBackgroundOpacity();
    });
    return true;
}

bool Actions::OnUndo() {
    // Empty-stack handling lives in the view (the stack is view
    // state); the action is acked as delivered either way.
    DispatchToView([this]() {
        m_view.Undo();
    });
    return true;
}

bool Actions::OnRedo() {
    DispatchToView([this]() {
        m_view.Redo();
    });
    return true;
}

bool Actions::OnCellSize(ghostty_surface_t surface,
                         ghostty_action_cell_size_s cell) {
    if (!surface) return true;
    DispatchToView([this, surface, cell]() {
        m_view.ApplyCellSizeForSurface(surface, cell);
    });
    return true;
}

bool Actions::OnScrollbar(ghostty_surface_t surface,
                          ghostty_action_scrollbar_s bar) {
    if (!surface) return true;
    DispatchToSurface(surface, [bar](host::ISurfaceView& v) {
        v.SetScrollbar(bar);
    });
    return true;
}

bool Actions::OnStartSearch(ghostty_surface_t surface,
                            ghostty_action_start_search_s search) {
    if (!surface) return true;
    // The needle is NUL-terminated (StartSearch.C uses [*:0]) and
    // empty for the bare start_search keybind; search_selection
    // pre-fills it with the selected text. Convert before crossing
    // threads — ghostty owns the pointer only for this call.
    std::wstring needle;
    if (search.needle && *search.needle)
        needle = interop::Encoding::toUtf16(search.needle);
    DispatchToSurface(surface, [needle = std::move(needle)](host::ISurfaceView& v) mutable {
        v.StartSearch(std::move(needle));
    });
    return true;
}

bool Actions::OnEndSearch(ghostty_surface_t surface) {
    if (!surface) return true;
    DispatchToSurface(surface, [](host::ISurfaceView& v) {
        v.EndSearch();
    });
    return true;
}

bool Actions::OnSearchTotal(ghostty_surface_t surface,
                            ghostty_action_search_total_s total) {
    if (!surface) return true;
    DispatchToSurface(surface, [total](host::ISurfaceView& v) {
        v.SetSearchTotal(total.total);
    });
    return true;
}

bool Actions::OnSearchSelected(ghostty_surface_t surface,
                               ghostty_action_search_selected_s selected) {
    if (!surface) return true;
    DispatchToSurface(surface, [selected](host::ISurfaceView& v) {
        v.SetSearchSelected(selected.selected);
    });
    return true;
}

bool Actions::OnReloadConfig(bool soft) {
    // The view-side implementation handles thread placement
    // (soft re-uses UI thread, hard spins a 4MB-stack worker
    // because ghostty's config parser blows the default 1MB).
    m_view.ReloadConfig(soft);
    return true;
}

bool Actions::OnConfigChange(ghostty_config_t newCfg) {
    // Clone here because ghostty owns the incoming pointer.
    // The view takes ownership of the clone and either swaps it
    // in or frees it on the UI thread.
    if (!newCfg) return true;
    auto cloned = ghostty_config_clone(newCfg);
    if (!cloned) return true;
    DispatchToView([this, cloned]() {
        m_view.ReplaceConfig(cloned);
    });
    return true;
}

bool Actions::OnDesktopNotification(ghostty_surface_t surface,
                                    ghostty_action_desktop_notification_s dn) {
    // Convert UTF-8 to UTF-16 on the renderer thread so the
    // captured lambda carries native strings.
    std::wstring title = (dn.title && dn.title[0]) ? interop::Encoding::toUtf16(dn.title) : L"";
    std::wstring body  = (dn.body  && dn.body[0])  ? interop::Encoding::toUtf16(dn.body)  : L"";
    if (title.empty() && body.empty()) return true;
    DispatchToView([this, surface, title = std::move(title),
                                    body = std::move(body)]() mutable {
        m_view.ShowDesktopNotification(surface, std::move(title), std::move(body));
    });
    return true;
}

bool Actions::OnProgressReport(ghostty_action_progress_report_s pr) {
    DispatchToView([this, pr]() {
        m_view.ReportProgress(pr);
    });
    return true;
}

// ===== window state =====

bool Actions::OnSizeLimit(ghostty_action_size_limit_s limit) {
    DispatchToView([this, limit]() {
        m_view.ApplySizeLimit(limit);
    });
    return true;
}

bool Actions::OnToggleFullscreen() {
    // The ghostty enum carries NATIVE + three macOS-specific
    // NON_NATIVE variants; on Windows they all collapse to the
    // same borderless-fullscreen behaviour, so the value is
    // ignored here. The Fullscreen value on the view side
    // owns the placement/style snapshot for restore.
    DispatchToView([this]() {
        m_view.ToggleFullscreen();
    });
    return true;
}

bool Actions::OnToggleWindowDecorations() {
    // The override state + the XAML application (show / hide caption
    // buttons + drag region) both live on the view; this method just
    // bounces the trigger through the UI dispatcher. action_cb fires
    // from the renderer thread, so the dispatcher hop is required
    // before touching XAML.
    DispatchToView([this]() {
        m_view.ToggleWindowDecorations();
    });
    return true;
}

// ===== split-pane =====
// All five surface-target split operations share the same shape:
// bounce through the UI dispatcher, then hand off to the view
// which owns the pane tree.

bool Actions::OnNewSplit(ghostty_surface_t surface,
                                ghostty_action_split_direction_e direction) {
    if (!surface) return true;
    DispatchToView([this, surface, direction]() {
        m_view.SplitActivePane(surface, direction);
    });
    return true;
}

bool Actions::OnResizeSplit(ghostty_surface_t surface,
                                   ghostty_action_resize_split_s resize) {
    if (!surface) return true;
    DispatchToView([this, surface, resize]() {
        m_view.ResizeSplitFromAction(surface, resize);
    });
    return true;
}

bool Actions::OnGotoSplit(ghostty_surface_t surface,
                                 ghostty_action_goto_split_e direction) {
    if (!surface) return true;
    DispatchToView([this, surface, direction]() {
        m_view.GotoSplitFromAction(surface, direction);
    });
    return true;
}

bool Actions::OnEqualizeSplits(ghostty_surface_t surface) {
    if (!surface) return true;
    DispatchToView([this, surface]() {
        m_view.EqualizeSplitsForSurface(surface);
    });
    return true;
}

bool Actions::OnToggleSplitZoom(ghostty_surface_t surface) {
    if (!surface) return true;
    DispatchToView([this, surface]() {
        m_view.ToggleSplitZoomForSurface(surface);
    });
    return true;
}

}  // namespace core::ghostty::actions
