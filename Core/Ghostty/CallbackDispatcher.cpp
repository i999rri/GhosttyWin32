#include "CallbackDispatcher.h"

namespace core::ghostty {

std::unique_ptr<CallbackDispatcher>
CallbackDispatcher::Create(host::IWindow& view,
                           actions::Actions::AppHooks hooks) {
    // Default for `hooks` lives on the declaration; the
    // definition can't repeat it or the compiler flags a duplicate.
    return std::unique_ptr<CallbackDispatcher>(
        new CallbackDispatcher(view, std::move(hooks)));
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
        case GHOSTTY_ACTION_CLOSE_WINDOW:
            return m_actions.OnCloseWindow();
        case GHOSTTY_ACTION_CLOSE_ALL_WINDOWS:
            return m_actions.OnCloseAllWindows();
        case GHOSTTY_ACTION_QUIT:
            return m_actions.OnQuit();
        case GHOSTTY_ACTION_TOGGLE_VISIBILITY:
            return m_actions.OnToggleVisibility();
        case GHOSTTY_ACTION_TOGGLE_MAXIMIZE:
            return m_actions.OnToggleMaximize();
        case GHOSTTY_ACTION_PRESENT_TERMINAL:
            return m_actions.OnPresentTerminal();
        case GHOSTTY_ACTION_SHOW_ON_SCREEN_KEYBOARD:
            return m_actions.OnShowOnScreenKeyboard();
        case GHOSTTY_ACTION_OPEN_CONFIG:
            return m_actions.OnOpenConfig();

        // ----- sizing -----
        case GHOSTTY_ACTION_INITIAL_SIZE:
            return m_actions.OnInitialSize(action.action.initial_size);
        case GHOSTTY_ACTION_RESET_WINDOW_SIZE:
            return m_actions.OnResetWindowSize();

        // ----- tab lifecycle / navigation / title -----
        case GHOSTTY_ACTION_NEW_TAB:
            return m_actions.OnNewTab();
        case GHOSTTY_ACTION_NEW_WINDOW:
            return m_actions.OnNewWindow();
        case GHOSTTY_ACTION_CLOSE_TAB:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnCloseTab(target.target.surface);
            return false;
        case GHOSTTY_ACTION_GOTO_TAB:
            return m_actions.OnGotoTab(static_cast<int>(action.action.goto_tab));
        case GHOSTTY_ACTION_GOTO_WINDOW:
            return m_actions.OnGotoWindow(action.action.goto_window);
        case GHOSTTY_ACTION_MOVE_TAB:
            return m_actions.OnMoveTab(action.action.move_tab);
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
        case GHOSTTY_ACTION_MOUSE_OVER_LINK:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnMouseOverLink(target.target.surface,
                                                 action.action.mouse_over_link);
            return false;
        case GHOSTTY_ACTION_MOUSE_VISIBILITY:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnMouseVisibility(target.target.surface,
                                                   action.action.mouse_visibility);
            return false;
        // PROMPT_TITLE: rename-title prompt. The SURFACE/TAB payload
        // variants collapse in the handler (one title per tab, same
        // as SET_TITLE/SET_TAB_TITLE). App-targeted is a no-op
        // upstream — ack.
        case GHOSTTY_ACTION_PROMPT_TITLE:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnPromptTitle(target.target.surface);
            return true;
        // READONLY: toggle_readonly indicator (the blocking itself
        // is core-side). App-targeted is a no-op upstream — ack.
        case GHOSTTY_ACTION_READONLY:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnReadonly(target.target.surface,
                                            action.action.readonly);
            return true;
        // COMMAND_FINISHED: shell-integration command tracking. The
        // app-targeted variant is a no-op upstream — ack.
        case GHOSTTY_ACTION_COMMAND_FINISHED:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnCommandFinished(target.target.surface,
                                                   action.action.command_finished);
            return true;
        // PWD: shell-reported working directory. Upstream treats the
        // app-targeted variant as a no-op (logged warning) — ack.
        case GHOSTTY_ACTION_PWD:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnPwd(target.target.surface,
                                       action.action.pwd.pwd);
            return true;
        // KEY_SEQUENCE / KEY_TABLE: modal keyboard state indicators.
        // Upstream treats the app-targeted variants as no-ops (logged
        // warnings), so those ack rather than refuse.
        case GHOSTTY_ACTION_KEY_SEQUENCE:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnKeySequence(target.target.surface,
                                               action.action.key_sequence);
            return true;
        case GHOSTTY_ACTION_KEY_TABLE:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnKeyTable(target.target.surface,
                                            action.action.key_table);
            return true;
        case GHOSTTY_ACTION_SECURE_INPUT:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnSecureInput(target.target.surface,
                                               action.action.secure_input);
            // App-targeted SECURE_INPUT is macOS's
            // EnableSecureEventInput (OS-level keylogger protection);
            // Windows has no counterpart, so the app variant stays a
            // deliberate ack.
            return true;
        case GHOSTTY_ACTION_RELOAD_CONFIG:
            return m_actions.OnReloadConfig(action.action.reload_config.soft);
        case GHOSTTY_ACTION_CONFIG_CHANGE:
            return m_actions.OnConfigChange(action.action.config_change.config);
        case GHOSTTY_ACTION_DESKTOP_NOTIFICATION: {
            // ghostty sets target.surface to the pane that emitted the
            // notification; a null surface is legal (the toast still
            // shows, the click handler just doesn't retarget tabs).
            ghostty_surface_t s = target.tag == GHOSTTY_TARGET_SURFACE
                ? target.target.surface : nullptr;
            return m_actions.OnDesktopNotification(s, action.action.desktop_notification);
        }
        case GHOSTTY_ACTION_PROGRESS_REPORT:
            return m_actions.OnProgressReport(action.action.progress_report);

        // ----- window state -----
        case GHOSTTY_ACTION_SIZE_LIMIT:
            return m_actions.OnSizeLimit(action.action.size_limit);
        case GHOSTTY_ACTION_TOGGLE_FULLSCREEN:
            return m_actions.OnToggleFullscreen();
        case GHOSTTY_ACTION_TOGGLE_WINDOW_DECORATIONS:
            return m_actions.OnToggleWindowDecorations();
        case GHOSTTY_ACTION_FLOAT_WINDOW:
            return m_actions.OnFloatWindow(action.action.float_window);
        case GHOSTTY_ACTION_TOGGLE_BACKGROUND_OPACITY:
            return m_actions.OnToggleBackgroundOpacity();
        // Undo/redo of parked closes (#151). App-scoped: the undo
        // stack is per-window view state, not per-surface.
        case GHOSTTY_ACTION_UNDO:
            return m_actions.OnUndo();
        case GHOSTTY_ACTION_REDO:
            return m_actions.OnRedo();

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
        // Feature surfaces intentionally not on this port's plate
        // (search bar, ImGui inspector, tab overview, quick
        // terminal, command palette):
        case GHOSTTY_ACTION_START_SEARCH:
        case GHOSTTY_ACTION_END_SEARCH:
        case GHOSTTY_ACTION_SEARCH_TOTAL:
        case GHOSTTY_ACTION_SEARCH_SELECTED:
        case GHOSTTY_ACTION_INSPECTOR:
        case GHOSTTY_ACTION_RENDER_INSPECTOR:
        // GTK-only, will never fire on Windows. Acked so it doesn't
        // fall through to the default "unhandled action" return.
        case GHOSTTY_ACTION_SHOW_GTK_INSPECTOR:
        case GHOSTTY_ACTION_TOGGLE_TAB_OVERVIEW:
        case GHOSTTY_ACTION_TOGGLE_QUICK_TERMINAL:
        case GHOSTTY_ACTION_TOGGLE_COMMAND_PALETTE:
        // macOS-only quit countdown — Windows already quits on
        // last-HWND-gone via CLOSE_WINDOW:
        case GHOSTTY_ACTION_QUIT_TIMER:
            return true;

        // Scroll-position updates: feeds the pane's overlay
        // scrollbar (#154). Surface-targeted by construction.
        case GHOSTTY_ACTION_SCROLLBAR:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnScrollbar(target.target.surface,
                                             action.action.scrollbar);
            return true;

        // Cell metrics: feeds snap-to-cell window resizing (#155).
        case GHOSTTY_ACTION_CELL_SIZE:
            if (target.tag == GHOSTTY_TARGET_SURFACE)
                return m_actions.OnCellSize(target.target.surface,
                                            action.action.cell_size);
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
