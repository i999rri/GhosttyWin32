#pragma once

#include "Host/IWindow.h"
#include "ghostty.h"
#include <cstdint>
#include <functional>
#include <utility>

namespace core::ghostty::actions {

// Host-side implementations of every ghostty action this port
// handles. GhosttyCallbackDispatcher routes incoming
// `ghostty_action_s` values here; the routing layer stays a pure
// switch / lookup,
// and the handlers themselves live in one place that can be
// unit-tested against a mock IMainWindowView without ghostty
// running.
//
// Per-handler state (e.g. the last INITIAL_SIZE width/height
// that RESET_WINDOW_SIZE consults) lives on this class because
// it's only meaningful inside the action lifecycle — keeping it
// off the view stops MainWindow from growing fields that nothing
// outside this file ever touches.
//
// Each method returns true iff the action was handled, matching
// the ghostty action_cb contract.
class Actions {
public:
    // App-scope operations injected at construction time. These are
    // the actions that don't belong on IWindow — the per-window view
    // doesn't own "add another window", "close every window", or
    // "quit the app" — so App supplies them as callables. Same shape
    // as MainWindowRuntime's Host bundle for other cross-scope hooks.
    //
    // Every slot defaults to empty so unit tests that never exercise
    // these actions keep the bare one-arg construction; each handler
    // degrades sensibly when its slot is missing (OnNewWindow bails,
    // the close-scope handlers fall back to closing the one window
    // they can reach — exactly the single-window behaviour these
    // actions had before the split).
    struct AppHooks {
        std::function<void()> newWindow;
        std::function<void()> closeAllWindows;
        std::function<void()> quit;
        // GOTO_WINDOW: bring another top-level window forward. App
        // owns the window vector, so the traversal (previous / next
        // relative to the currently-foreground window) lives there.
        std::function<void(ghostty_action_goto_window_e)> gotoWindow;
    };

    Actions(host::IWindow& view, AppHooks hooks = {}) noexcept
        : m_view(view)
        , m_hooks(std::move(hooks)) {}

    // ----- terminal events -----
    bool OnRingBell();
    bool OnShowChildExited(ghostty_surface_message_childexited_s child);
    bool OnRendererHealth(ghostty_action_renderer_health_e health);
    bool OnRender();

    // ----- shell-verb passthroughs -----
    bool OnCheckForUpdates();
    bool OnOpenUrl(ghostty_action_open_url_s url);

    // ----- window lifecycle -----
    // CLOSE_WINDOW: close the window that owns the action's target.
    // This Actions instance is already per-window (the runtime
    // routes actions by target surface), so closing m_view is the
    // right scope — sibling windows survive.
    bool OnCloseWindow();
    // CLOSE_ALL_WINDOWS / QUIT: app-scope teardown via the injected
    // hooks. Distinct handlers because their scope differs from
    // CLOSE_WINDOW in a multi-window session, even though on Windows
    // (process lifetime == live windows) both currently resolve to
    // "close every window".
    bool OnCloseAllWindows();
    bool OnQuit();
    bool OnToggleVisibility();
    bool OnToggleMaximize();
    bool OnPresentTerminal();
    bool OnShowOnScreenKeyboard();
    bool OnOpenConfig();
    // FLOAT_WINDOW: toggle always-on-top for this window. macOS's
    // NSWindowLevelFloating maps to Win32 HWND_TOPMOST via
    // SetWindowPos; the actual call lives on the view (state tracking
    // + HWND access), this handler just bounces there.
    bool OnFloatWindow(ghostty_action_float_window_e mode);

    // ----- sizing -----
    bool OnInitialSize(ghostty_action_initial_size_s size);
    bool OnResetWindowSize();

    // ----- tab lifecycle / navigation / title -----
    bool OnNewTab();
    // NEW_WINDOW: spawn a fresh top-level MainWindow. Bounces
    // through the injected newWindow hook (which App fills with
    // `App::g_app->CreateNewWindow()`); Actions doesn't know or
    // care what a "window" is at that layer.
    bool OnNewWindow();
    bool OnCloseTab(ghostty_surface_t surface);
    bool OnGotoTab(int requested);
    // GOTO_WINDOW: navigate to the previous / next top-level
    // MainWindow. Bounces through the injected gotoWindow hook (App
    // owns the window vector, so the traversal lives there).
    bool OnGotoWindow(ghostty_action_goto_window_e direction);
    bool OnMoveTab(ghostty_action_move_tab_s move);
    // SET_TITLE and SET_TAB_TITLE collapse to the same handler —
    // this port has one title surface per tab.
    bool OnSetTitle(ghostty_surface_t surface, const char* utf8Title);
    bool OnCopyTitleToClipboard(ghostty_surface_t surface);

    // ----- terminal-driven appearance + lifecycle -----
    bool OnColorChange(ghostty_action_color_change_s cc);
    bool OnMouseShape(ghostty_surface_t surface,
                      ghostty_action_mouse_shape_e shape);
    bool OnReloadConfig(bool soft);
    bool OnConfigChange(ghostty_config_t newCfg);
    bool OnDesktopNotification(ghostty_surface_t surface,
                               ghostty_action_desktop_notification_s dn);
    bool OnProgressReport(ghostty_action_progress_report_s pr);

    // ----- window state -----
    // Both delegate the actual mutation to a dedicated state owner
    // on the view (SizeLimit / Fullscreen); the action
    // method just bounces through the UI dispatcher.
    bool OnSizeLimit(ghostty_action_size_limit_s limit);
    bool OnToggleFullscreen();
    bool OnToggleWindowDecorations();

    // ----- split-pane -----
    bool OnNewSplit(ghostty_surface_t surface,
                    ghostty_action_split_direction_e direction);
    bool OnResizeSplit(ghostty_surface_t surface,
                       ghostty_action_resize_split_s resize);
    bool OnGotoSplit(ghostty_surface_t surface,
                     ghostty_action_goto_split_e direction);
    bool OnEqualizeSplits(ghostty_surface_t surface);
    bool OnToggleSplitZoom(ghostty_surface_t surface);

private:
    host::IWindow& m_view;
    AppHooks       m_hooks;

    // Initial window size from GHOSTTY_ACTION_INITIAL_SIZE
    // (physical pixels). Zero means "not yet received" —
    // OnResetWindowSize falls back to a DPI-scaled 1280×720 in
    // that case.
    uint32_t m_initialWidth = 0;
    uint32_t m_initialHeight = 0;
};

} // namespace core::ghostty::actions
