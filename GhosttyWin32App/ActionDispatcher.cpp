#include "pch.h"
#include "ActionDispatcher.h"
#include "Encoding.h"
#include <shellapi.h>
#include <cstdio>
#include <string>

namespace winrt::GhosttyWin32::implementation {

std::unique_ptr<ActionDispatcher> ActionDispatcher::Create(IMainWindowView& view) {
    return std::unique_ptr<ActionDispatcher>(new ActionDispatcher(view));
}

bool ActionDispatcher::Dispatch(ghostty_target_s target, ghostty_action_s action) {
    (void)target;  // unused until tab / surface targeted handlers move across
    switch (action.tag) {
        // ----- terminal events -----

        // Terminal sent BEL (\x07). MessageBeep plays the user's
        // configured "Default Beep" sound asynchronously and is
        // thread-safe, so we don't bounce through the UI dispatcher.
        // Honouring `bell-features` (audio / attention / title /
        // unread) is a follow-up.
        case GHOSTTY_ACTION_RING_BELL:
            MessageBeep(MB_OK);
            return true;

        // ----- intentionally acked no-ops -----

        // Informational actions whose UI surfaces this port doesn't
        // have yet (read-only banner, secure-input padlock, pending
        // chord indicator, modal-key-table label, shell-supplied
        // title source flag, PWD breadcrumb, post-command summary).
        // Acking keeps libghostty's future "unhandled" audit quiet;
        // the real UI work is tracked in #57.
        case GHOSTTY_ACTION_READONLY:
        case GHOSTTY_ACTION_SECURE_INPUT:
        case GHOSTTY_ACTION_KEY_SEQUENCE:
        case GHOSTTY_ACTION_KEY_TABLE:
        case GHOSTTY_ACTION_PROMPT_TITLE:
        case GHOSTTY_ACTION_PWD:
        case GHOSTTY_ACTION_COMMAND_FINISHED:

        // Feature surfaces intentionally not on this port's plate
        // (search bar, ImGui inspector, tab overview, quick terminal,
        // command palette, terminal-level undo/redo). Same "ack and
        // move on" treatment — proper feature work tracked in #57.
        case GHOSTTY_ACTION_UNDO:
        case GHOSTTY_ACTION_REDO:
        case GHOSTTY_ACTION_START_SEARCH:
        case GHOSTTY_ACTION_END_SEARCH:
        case GHOSTTY_ACTION_SEARCH_TOTAL:
        case GHOSTTY_ACTION_SEARCH_SELECTED:
        case GHOSTTY_ACTION_INSPECTOR:
        case GHOSTTY_ACTION_RENDER_INSPECTOR:
        case GHOSTTY_ACTION_TOGGLE_TAB_OVERVIEW:
        case GHOSTTY_ACTION_TOGGLE_QUICK_TERMINAL:
        case GHOSTTY_ACTION_TOGGLE_COMMAND_PALETTE:

        // Scroll-position updates from ghostty — we don't render a
        // scrollbar (the terminal surface fills the available area
        // without one) so the data has nowhere to go.
        case GHOSTTY_ACTION_SCROLLBAR:

        // macOS apprts use QUIT_TIMER to manage the "wait N seconds
        // after the last window closes before actually quitting"
        // countdown. Windows quits as soon as the last top-level
        // HWND goes away (CLOSE_WINDOW already does that), so
        // neither START nor STOP needs wiring.
        case GHOSTTY_ACTION_QUIT_TIMER:
            return true;

        // ----- diagnostic-only -----

        // Shell process for a surface exited. With confirm-close-
        // surface=false (our default) the surface tears itself down
        // via close_surface_cb almost immediately, so this is a
        // breadcrumb. A proper in-terminal overlay is design work;
        // log and move on. Mirror to OutputDebugString so the line
        // survives stderr buffering through surface teardown.
        case GHOSTTY_ACTION_SHOW_CHILD_EXITED: {
            auto ce = action.action.child_exited;
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

        // Renderer status. UNHEALTHY means the generic renderer hit
        // a problem (texture allocation, shader compile, etc.) and
        // dropped into a degraded mode. One stderr line keeps the
        // root cause findable in the debugger output without us
        // committing to user-facing UX (toast, status bar) yet.
        case GHOSTTY_ACTION_RENDERER_HEALTH: {
            bool healthy = action.action.renderer_health == GHOSTTY_RENDERER_HEALTH_HEALTHY;
            std::fprintf(stderr, "[renderer_health] %s\n",
                         healthy ? "healthy" : "unhealthy");
            std::fflush(stderr);
            return true;
        }

        // ----- explicit repaint -----

        // ghostty wants a repaint outside the natural wakeup_cb ->
        // tick cadence. The dispatcher used by wakeup_cb already
        // serialises ticks on the UI thread, so we go through the
        // same path. ghostty_app_tick is idempotent — calling it
        // when nothing is dirty is a cheap no-op.
        case GHOSTTY_ACTION_RENDER:
            m_view.Dispatcher().TryEnqueue([this]() {
                m_view.Tick();
            });
            return true;

        // ----- shell-verb passthroughs -----

        // No built-in updater on Windows; hand the user off to the
        // GitHub releases page. ShellExecuteW dispatches via the
        // default browser without the rundll32 child-process flash
        // libghostty's std.process.Child fallback would otherwise
        // produce.
        case GHOSTTY_ACTION_CHECK_FOR_UPDATES:
            ShellExecuteW(m_view.Hwnd(), L"open",
                          L"https://github.com/i999rri/GhosttyWin32/releases",
                          nullptr, nullptr, SW_SHOWNORMAL);
            return true;

        // Ctrl+click on a URL in the terminal. Hand off to the
        // shell verb opener so the user's default browser / mail
        // client / etc. handles it.
        case GHOSTTY_ACTION_OPEN_URL: {
            auto& ou = action.action.open_url;
            if (ou.url && ou.len > 0) {
                std::wstring wurl = Encoding::toUtf16(ou.url, static_cast<int>(ou.len));
                if (!wurl.empty()) {
                    ShellExecuteW(m_view.Hwnd(), L"open", wurl.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            return true;
        }

        default:
            // Not yet migrated; let MainWindow::action_cb's existing
            // if/else chain handle it. Once every handler has moved
            // across, this fallthrough turns into the dispatcher's
            // own "unhandled action" return-false branch.
            return false;
    }
}

}  // namespace winrt::GhosttyWin32::implementation
