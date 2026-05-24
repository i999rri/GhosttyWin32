#include "CallbackDispatcher.h"

namespace core::ghostty {

std::unique_ptr<CallbackDispatcher>
CallbackDispatcher::Create(host::IWindow& view) {
    return std::unique_ptr<CallbackDispatcher>(new CallbackDispatcher(view));
}

bool CallbackDispatcher::DispatchAction(ghostty_target_s target, ghostty_action_s action) {
    switch (action.tag) {
        // ----- terminal events -----
        case GHOSTTY_ACTION_RING_BELL:
            return m_actions.OnRingBell();
        case GHOSTTY_ACTION_SHOW_CHILD_EXITED:
            return m_actions.OnShowChildExited(action.action.child_exited);
        case GHOSTTY_ACTION_RENDERER_HEALTH:
            return m_actions.OnRendererHealth(action.action.renderer_health);
        case GHOSTTY_ACTION_RENDER:
            return m_actions.OnRender();

        // ----- shell-verb passthroughs -----
        case GHOSTTY_ACTION_CHECK_FOR_UPDATES:
            return m_actions.OnCheckForUpdates();
        case GHOSTTY_ACTION_OPEN_URL:
            return m_actions.OnOpenUrl(action.action.open_url);

        // ----- window lifecycle -----
        // CLOSE_WINDOW / CLOSE_ALL_WINDOWS / QUIT all collapse to
        // OnCloseWindow on the single-window build; multi-window
        // (#55) will need to give these three distinct handlers.
        case GHOSTTY_ACTION_CLOSE_WINDOW:
        case GHOSTTY_ACTION_CLOSE_ALL_WINDOWS:
        case GHOSTTY_ACTION_QUIT:
            return m_actions.OnCloseWindow();
        case GHOSTTY_ACTION_TOGGLE_VISIBILITY:
            return m_actions.OnToggleVisibility();
        case GHOSTTY_ACTION_TOGGLE_MAXIMIZE:
            return m_actions.OnToggleMaximize();
        case GHOSTTY_ACTION_OPEN_CONFIG:
            return m_actions.OnOpenConfig();

        // ----- sizing -----
        case GHOSTTY_ACTION_INITIAL_SIZE:
            return m_actions.OnInitialSize(action.action.initial_size);
        case GHOSTTY_ACTION_RESET_WINDOW_SIZE:
            return m_actions.OnResetWindowSize();

        // ----- tab lifecycle / navigation / title -----
        // NEW_WINDOW collapses to NEW_TAB on the single-window
        // build; multi-window (#55) will split them again.
        case GHOSTTY_ACTION_NEW_TAB:
        case GHOSTTY_ACTION_NEW_WINDOW:
            return m_actions.OnNewTab();
        case GHOSTTY_ACTION_CLOSE_TAB:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnCloseTab(target.target.surface);
            return false;
        case GHOSTTY_ACTION_GOTO_TAB:
            return m_actions.OnGotoTab(static_cast<int>(action.action.goto_tab));
        // SET_TITLE / SET_TAB_TITLE: this port treats them
        // identically (one title surface per tab); the action union
        // happens to share `set_title` for both tags.
        case GHOSTTY_ACTION_SET_TITLE:
        case GHOSTTY_ACTION_SET_TAB_TITLE:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnSetTitle(target.target.surface,
                                            action.action.set_title.title);
            return false;
        case GHOSTTY_ACTION_COPY_TITLE_TO_CLIPBOARD:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnCopyTitleToClipboard(target.target.surface);
            return false;

        // ----- terminal-driven appearance + lifecycle -----
        case GHOSTTY_ACTION_COLOR_CHANGE:
            return m_actions.OnColorChange(action.action.color_change);
        case GHOSTTY_ACTION_MOUSE_SHAPE:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnMouseShape(target.target.surface,
                                              action.action.mouse_shape);
            return false;
        case GHOSTTY_ACTION_RELOAD_CONFIG:
            return m_actions.OnReloadConfig(action.action.reload_config.soft);
        case GHOSTTY_ACTION_CONFIG_CHANGE:
            return m_actions.OnConfigChange(action.action.config_change.config);
        case GHOSTTY_ACTION_DESKTOP_NOTIFICATION:
            return m_actions.OnDesktopNotification(action.action.desktop_notification);
        case GHOSTTY_ACTION_PROGRESS_REPORT:
            return m_actions.OnProgressReport(action.action.progress_report);

        // ----- window state -----
        case GHOSTTY_ACTION_SIZE_LIMIT:
            return m_actions.OnSizeLimit(action.action.size_limit);
        case GHOSTTY_ACTION_TOGGLE_FULLSCREEN:
            return m_actions.OnToggleFullscreen();

        // ----- split-pane (surface-targeted) -----
        case GHOSTTY_ACTION_NEW_SPLIT:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnNewSplit(target.target.surface,
                                            action.action.new_split);
            return false;
        case GHOSTTY_ACTION_RESIZE_SPLIT:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnResizeSplit(target.target.surface,
                                               action.action.resize_split);
            return false;
        case GHOSTTY_ACTION_GOTO_SPLIT:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnGotoSplit(target.target.surface,
                                             action.action.goto_split);
            return false;
        case GHOSTTY_ACTION_EQUALIZE_SPLITS:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnEqualizeSplits(target.target.surface);
            return false;
        case GHOSTTY_ACTION_TOGGLE_SPLIT_ZOOM:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnToggleSplitZoom(target.target.surface);
            return false;

        // ----- intentionally acked no-ops -----
        // Routing decisions, not implementation. Returning true
        // here keeps libghostty's future "unhandled action" audit
        // quiet without inventing empty GhosttyActions methods.
        //
        // Informational actions whose UI surfaces this port
        // doesn't have yet (read-only banner, secure-input
        // padlock, pending chord, modal-key-table label, shell-
        // supplied title source flag, PWD breadcrumb, post-
        // command summary):
        case GHOSTTY_ACTION_READONLY:
        case GHOSTTY_ACTION_SECURE_INPUT:
        case GHOSTTY_ACTION_KEY_SEQUENCE:
        case GHOSTTY_ACTION_KEY_TABLE:
        case GHOSTTY_ACTION_PROMPT_TITLE:
        case GHOSTTY_ACTION_PWD:
        case GHOSTTY_ACTION_COMMAND_FINISHED:
        // Feature surfaces intentionally not on this port's plate
        // (search bar, ImGui inspector, tab overview, quick
        // terminal, command palette, terminal-level undo/redo):
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
        // Scroll-position updates — no scrollbar UI to feed:
        case GHOSTTY_ACTION_SCROLLBAR:
        // macOS-only quit countdown — Windows already quits on
        // last-HWND-gone via CLOSE_WINDOW:
        case GHOSTTY_ACTION_QUIT_TIMER:
        // Cell metrics — no host-side consumer today:
        case GHOSTTY_ACTION_CELL_SIZE:
        // MOUSE_OVER_LINK is acked while the TOOLTIPS popup is
        // disabled (issue #61: a TTM_TRACKACTIVATE / SetWindowPos
        // interaction with the DComp surface crashed the process
        // on URL click); the URL click path itself still works.
        case GHOSTTY_ACTION_MOUSE_OVER_LINK:
        // FLOAT_WINDOW: no keybind reaches us yet; ghostty's
        // dispatch path for it isn't understood on this port.
        // Acking avoids "unhandled action" noise once that's
        // fixed and the keybind starts firing.
        case GHOSTTY_ACTION_FLOAT_WINDOW:
        // MOUSE_VISIBILITY disabled pending #60 (the renderer-
        // side ghostty_surface_key call doesn't populate the
        // event fields ghostty checks before firing this).
        case GHOSTTY_ACTION_MOUSE_VISIBILITY:
            return true;

        default:
            // Not yet migrated; let MainWindow::action_cb's
            // existing if/else chain handle it. Once every
            // handler has moved across, this falls through to the
            // dispatcher's own "unhandled action" return-false.
            return false;
    }
}

}  // namespace core::ghostty
