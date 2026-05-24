#pragma once

#include "Host/IWindow.h"
#include "ghostty.h"
#include <cstdint>

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
    explicit Actions(host::IWindow& view) noexcept : m_view(view) {}

    // ----- terminal events -----
    bool OnRingBell();
    bool OnShowChildExited(ghostty_surface_message_childexited_s child);
    bool OnRendererHealth(ghostty_action_renderer_health_e health);
    bool OnRender();

    // ----- shell-verb passthroughs -----
    bool OnCheckForUpdates();
    bool OnOpenUrl(ghostty_action_open_url_s url);

    // ----- window lifecycle -----
    // Used for CLOSE_WINDOW / CLOSE_ALL_WINDOWS / QUIT — the
    // single-window build collapses all three to the same effect.
    bool OnCloseWindow();
    bool OnToggleVisibility();
    bool OnToggleMaximize();
    bool OnPresentTerminal();
    bool OnShowOnScreenKeyboard();
    bool OnOpenConfig();

    // ----- sizing -----
    bool OnInitialSize(ghostty_action_initial_size_s size);
    bool OnResetWindowSize();

    // ----- tab lifecycle / navigation / title -----
    // NEW_TAB and NEW_WINDOW both land here in the single-window
    // build; multi-window (#55) will route NEW_WINDOW elsewhere.
    bool OnNewTab();
    bool OnCloseTab(ghostty_surface_t surface);
    bool OnGotoTab(int requested);
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
    bool OnDesktopNotification(ghostty_action_desktop_notification_s dn);
    bool OnProgressReport(ghostty_action_progress_report_s pr);

    // ----- window state -----
    // Both delegate the actual mutation to a dedicated state owner
    // on the view (SizeLimit / Fullscreen); the action
    // method just bounces through the UI dispatcher.
    bool OnSizeLimit(ghostty_action_size_limit_s limit);
    bool OnToggleFullscreen();

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

    // Initial window size from GHOSTTY_ACTION_INITIAL_SIZE
    // (physical pixels). Zero means "not yet received" —
    // OnResetWindowSize falls back to a DPI-scaled 1280×720 in
    // that case.
    uint32_t m_initialWidth = 0;
    uint32_t m_initialHeight = 0;
};

}  // namespace core::ghostty::actions
