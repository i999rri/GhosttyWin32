#include "pch.h"
#include "GhosttyActions.h"
#include "Encoding.h"
#include <shellapi.h>
#include <cstdio>
#include <string>

namespace winrt::GhosttyWin32::implementation {

// ===== terminal events =====

bool GhosttyActions::OnRingBell() {
    // Terminal sent BEL (\x07). MessageBeep plays the user's
    // configured "Default Beep" sound asynchronously and is
    // thread-safe, so we don't bounce through the UI dispatcher.
    // Honouring `bell-features` (audio / attention / title /
    // unread) is a follow-up.
    MessageBeep(MB_OK);
    return true;
}

bool GhosttyActions::OnShowChildExited(ghostty_action_child_exited_s ce) {
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

bool GhosttyActions::OnRendererHealth(ghostty_action_renderer_health_e health) {
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

bool GhosttyActions::OnRender() {
    // ghostty wants a repaint outside the natural wakeup_cb ->
    // tick cadence. The dispatcher used by wakeup_cb already
    // serialises ticks on the UI thread, so we go through the
    // same path. ghostty_app_tick is idempotent — calling it
    // when nothing is dirty is a cheap no-op.
    m_view.Dispatcher().TryEnqueue([this]() {
        m_view.Tick();
    });
    return true;
}

// ===== shell-verb passthroughs =====

bool GhosttyActions::OnCheckForUpdates() {
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

bool GhosttyActions::OnOpenUrl(ghostty_action_open_url_s ou) {
    // Ctrl+click on a URL in the terminal. Hand off to the shell
    // verb opener so the user's default browser / mail client /
    // etc. handles it.
    if (ou.url && ou.len > 0) {
        std::wstring wurl = Encoding::toUtf16(ou.url, static_cast<int>(ou.len));
        if (!wurl.empty()) {
            ShellExecuteW(m_view.Hwnd(), L"open", wurl.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    return true;
}

// ===== window lifecycle =====

bool GhosttyActions::OnCloseWindow() {
    // Single-window builds collapse CLOSE_WINDOW, QUIT, and
    // CLOSE_ALL_WINDOWS into the same effect — close the one
    // window we have, which terminates the app. Multi-window
    // (#55) will need to give these three distinct behaviours.
    m_view.Dispatcher().TryEnqueue([this]() {
        m_view.RequestClose();
    });
    return true;
}

bool GhosttyActions::OnToggleVisibility() {
    // SW_MINIMIZE / SW_RESTORE instead of SW_HIDE because hiding
    // from the taskbar leaves Windows users with no discoverable
    // way back — ghostty's `global:` keybind qualifier isn't
    // wired to RegisterHotKey on this port yet, so a SW_HIDE'd
    // window with no taskbar entry can only be recovered by
    // relaunching. Minimizing keeps the window reachable via
    // taskbar click / alt-tab.
    m_view.Dispatcher().TryEnqueue([this]() {
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

bool GhosttyActions::OnToggleMaximize() {
    // WM_SYSCOMMAND path used by the caption-button click — keeps
    // the NVIDIA presenter AV from issue #26 out of the picture.
    // SendMessage runs on the UI thread; dispatch through
    // Dispatcher() because action_cb fires from the renderer
    // thread.
    m_view.Dispatcher().TryEnqueue([this]() {
        HWND hwnd = m_view.Hwnd();
        if (!hwnd) return;
        SendMessageW(hwnd, WM_SYSCOMMAND,
                     IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0);
    });
    return true;
}

bool GhosttyActions::OnOpenConfig() {
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

bool GhosttyActions::OnInitialSize(ghostty_action_initial_size_s size) {
    // Record the desired startup window dimensions ghostty
    // computes from config (`window-width` × `cell-width-px`,
    // etc.). Stored as physical pixels — ghostty already did the
    // cell-to-pixel math, so OnResetWindowSize can hand the value
    // to SetWindowPos directly without re-scaling.
    m_initialWidth = size.width;
    m_initialHeight = size.height;
    return true;
}

bool GhosttyActions::OnResetWindowSize() {
    // Restore the window to its startup footprint. Prefer the
    // size OnInitialSize recorded (honoring the user's config);
    // if INITIAL_SIZE never fired, fall back to 1280×720 DIPs —
    // matches the WinUI 3 fresh-window default and lands an
    // 80×24-ish terminal at common font sizes.
    uint32_t w = m_initialWidth;
    uint32_t h = m_initialHeight;
    m_view.Dispatcher().TryEnqueue([this, w, h]() {
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

// ===== split-pane =====
// All five surface-target split operations share the same shape:
// bounce through the UI dispatcher, then hand off to the view
// which owns the pane tree.

bool GhosttyActions::OnNewSplit(ghostty_surface_t surface,
                                ghostty_action_split_direction_e direction) {
    if (!surface) return true;
    m_view.Dispatcher().TryEnqueue([this, surface, direction]() {
        m_view.SplitActivePane(surface, direction);
    });
    return true;
}

bool GhosttyActions::OnResizeSplit(ghostty_surface_t surface,
                                   ghostty_action_resize_split_s resize) {
    if (!surface) return true;
    m_view.Dispatcher().TryEnqueue([this, surface, resize]() {
        m_view.ResizeSplitFromAction(surface, resize);
    });
    return true;
}

bool GhosttyActions::OnGotoSplit(ghostty_surface_t surface,
                                 ghostty_action_goto_split_e direction) {
    if (!surface) return true;
    m_view.Dispatcher().TryEnqueue([this, surface, direction]() {
        m_view.GotoSplitFromAction(surface, direction);
    });
    return true;
}

bool GhosttyActions::OnEqualizeSplits(ghostty_surface_t surface) {
    if (!surface) return true;
    m_view.Dispatcher().TryEnqueue([this, surface]() {
        m_view.EqualizeSplitsForSurface(surface);
    });
    return true;
}

bool GhosttyActions::OnToggleSplitZoom(ghostty_surface_t surface) {
    if (!surface) return true;
    m_view.Dispatcher().TryEnqueue([this, surface]() {
        m_view.ToggleSplitZoomForSurface(surface);
    });
    return true;
}

}  // namespace winrt::GhosttyWin32::implementation
