#pragma once

#include "Pane.h"
#include "PaneId.h"
#include "PaneIdAllocator.h"
#include "SplitPanel.h"
#include "Tab.h"
#include "TerminalControl.xaml.h"
#include "ghostty.h"
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <dcomp.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <functional>
#include <memory>

#pragma comment(lib, "dcomp.lib")

namespace winrt::GhosttyWin32::implementation {

// Builds Tabs. Holds the cross-cutting context (ghostty app handle, the
// HWND for DPI/initial-size, the PaneIdAllocator that produces fresh
// per-leaf IDs, and an optional "leaf gained focus" callback that
// every TerminalControl this factory creates will fire) so callers
// don't have to thread those through every Make() call.
//
// The focus callback is the one piece of MainWindow-side context that
// needs to reach into each leaf without each leaf knowing about
// MainWindow directly. Wiring it here keeps the dependency direction
// host -> control (the inverse would be a layering violation).
//
// Stateless beyond the injected references — no mutable state of its
// own. ID counter mutation lives in PaneIdAllocator; the factory only
// borrows it.
class TabFactory {
public:
    TabFactory(ghostty_app_t app, ghostty_config_t config, HWND hwnd,
               PaneIdAllocator& idAllocator,
               std::function<void(ghostty_surface_t)> onLeafFocused = {}) noexcept
        : m_app(app), m_config(config), m_hwnd(hwnd), m_idAllocator(idAllocator),
          m_onLeafFocused(std::move(onLeafFocused)),
          m_dividerColor(ResolveDividerColor(config)) {}

    TabFactory(const TabFactory&) = delete;
    TabFactory& operator=(const TabFactory&) = delete;
    TabFactory(TabFactory&&) = delete;
    TabFactory& operator=(TabFactory&&) = delete;

    // Build a fully-formed Tab. The caller created `item` and appended
    // it to the TabView, but did NOT set `item.Content` — this factory
    // creates the TerminalControl, wraps it in a single-leaf Pane
    // tree, hosts it in a SplitPanel, and assigns the SplitPanel as
    // the item's content. That keeps the pane-tree ownership invariant
    // ("SplitPanel owns the tree, Tab borrows it") in one place.
    //
    // Returns nullptr on failure (after cleaning up any partially-
    // acquired resources). Call on the UI thread; neither the inner
    // SwapChainPanel nor the SplitPanel need to be in the visual tree
    // yet — see issue #22, where making the panel visible before it
    // had displayable content produced a flicker. The optional
    // onActivated callback runs on the UI thread once ghostty has
    // presented its first frame and we've bound the swap chain to the
    // panel; the host uses it to switch the TabView so the panel
    // becomes visible only with real content.
    //
    // Ordering: the DComp surface handle is bound to the panel only
    // AFTER ghostty's renderer thread has presented at least one real
    // frame — see TerminalControl::OnSwapChainReady. ghostty fires the
    // swap-chain-ready callback from drawFrameEnd (post first present),
    // not from swap-chain creation, so the back buffer is guaranteed to
    // have displayable content by the time we attach.
    std::unique_ptr<Tab> Make(
        Microsoft::UI::Xaml::Controls::TabViewItem item,
        std::function<void()> onActivated = {},
        uint32_t initialWidth = 0,
        uint32_t initialHeight = 0)
    {
        auto leaf = MakeLeaf(initialWidth, initialHeight, std::move(onActivated));
        if (!leaf) return nullptr;

        // Wrap the leaf in a SplitPanel and assign as item.Content.
        // With one leaf SplitPanel collapses to "arrange the single
        // child at the full rect", matching the previous behaviour of
        // placing the control directly under TabViewItem.
        winrt::GhosttyWin32::SplitPanel splitPanel{};
        auto* splitPanelImpl = winrt::get_self<implementation::SplitPanel>(splitPanel);
        if (!splitPanelImpl) {
            OutputDebugStringA("TabFactory::Make: get_self<SplitPanel> FAILED\n");
            DetachLeaf(*leaf);
            return nullptr;
        }
        // Apply the divider colour before any tree exists so the
        // first splitter Border built by SetRoot already paints
        // with the correct brush.
        splitPanelImpl->SetDividerColor(m_dividerColor);
        splitPanelImpl->SetRoot(std::move(leaf));
        item.Content(splitPanel);

        try {
            return std::make_unique<Tab>(std::move(splitPanel), std::move(item));
        } catch (winrt::hresult_error const&) {
            // Tab construction validation failed. Detach synchronously
            // so the surface/handle don't leak. The splitPanel /
            // tree own the leaf at this point.
            if (auto* root = splitPanelImpl->Root()) DetachLeaf(*root);
            return nullptr;
        }
    }

    // Build a new pane: TerminalControl + DComp surface handle +
    // ghostty surface + freshly-allocated PaneId, returned as a Pane
    // leaf. Shared between Make() (the leaf becomes the only pane in
    // a brand-new tab) and the NEW_SPLIT action handler (the leaf is
    // inserted into an existing tab's tree alongside its source pane).
    //
    // Returns nullptr on any failure. Resources acquired before the
    // failure point are released before the return — caller doesn't
    // need to clean up after a null result.
    std::unique_ptr<Pane> MakeLeaf(
        uint32_t initialWidth,
        uint32_t initialHeight,
        std::function<void()> onActivated = {})
    {
        constexpr DWORD COMPOSITIONSURFACE_ALL_ACCESS = 0x0003L;

        auto control = winrt::GhosttyWin32::TerminalControl();
        auto* controlImpl = winrt::get_self<implementation::TerminalControl>(control);
        if (!controlImpl) {
            OutputDebugStringA("TabFactory::MakeLeaf: get_self<TerminalControl> FAILED\n");
            return nullptr;
        }
        auto panel = controlImpl->InnerPanel();
        if (!panel) {
            OutputDebugStringA("TabFactory::MakeLeaf: TerminalControl has no inner panel\n");
            return nullptr;
        }

        // Allocate the PaneId for this leaf up-front — it's used both
        // as the close_surface_cb routing key (cfg.userdata) and as
        // the leaf's stable identifier inside the tree. Allocated
        // before surface_new so cfg.userdata is set; the value is
        // opaque to ghostty and travels back to us through
        // close_surface_cb.
        PaneId paneId = m_idAllocator.Allocate();

        HANDLE handle = nullptr;
        if (FAILED(DCompositionCreateSurfaceHandle(COMPOSITIONSURFACE_ALL_ACCESS, nullptr, &handle))) {
            OutputDebugStringA("TabFactory::MakeLeaf: DCompositionCreateSurfaceHandle FAILED\n");
            return nullptr;
        }

        auto attach = std::make_shared<SwapChainAttachRequest>();
        attach->handle = handle;
        attach->panel = panel;
        attach->dispatcher = panel.DispatcherQueue();
        attach->onActivated = std::move(onActivated);
        // Heap-allocated owning shared_ptr handed to ghostty; it'll
        // come back through OnSwapChainReady (or be deleted here if
        // surface_new fails).
        auto* attachOwned = new std::shared_ptr<SwapChainAttachRequest>(attach);

        ghostty_surface_config_s cfg = ghostty_surface_config_new();
        cfg.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
        cfg.platform.windows.hwnd = m_hwnd;
        cfg.platform.windows.composition_surface_handle = handle;
        cfg.platform.windows.swap_chain_ready_cb = &TerminalControl::OnSwapChainReady;
        cfg.platform.windows.swap_chain_ready_userdata = attachOwned;
        cfg.userdata = paneId.ToUserdata();
        // Initial swap chain size: prefer the host's caller-supplied
        // estimate (typically the active tab/pane's panel size, since
        // the new panel will land in the same content area), then
        // fall back to the panel's own ActualWidth/Height. With
        // deferred SelectedItem (issue #22) the panel isn't in the
        // visual tree yet so its ActualWidth is 0 — without the host
        // hint, ghostty would fall back further to the main window's
        // full client rect, which is taller than the actual panel area
        // by the tab strip height. That mismatch causes a visible
        // "stretch then resize" when the panel becomes visible and
        // SizeChanged fires.
        uint32_t initW = initialWidth ? initialWidth
                                      : static_cast<uint32_t>(panel.ActualWidth());
        uint32_t initH = initialHeight ? initialHeight
                                       : static_cast<uint32_t>(panel.ActualHeight());
        cfg.platform.windows.initial_width = initW;
        cfg.platform.windows.initial_height = initH;
        UINT dpi = GetDpiForWindow(m_hwnd);
        cfg.scale_factor = static_cast<double>(dpi) / 96.0;

        ghostty_surface_t surface = ghostty_surface_new(m_app, &cfg);
        if (!surface) {
            OutputDebugStringA("TabFactory::MakeLeaf: ghostty_surface_new FAILED\n");
            // Callback won't fire — release the renderer's owning handle.
            delete attachOwned;
            CloseHandle(handle);
            return nullptr;
        }

        // Hand surface ownership to the control. From here on the
        // control's Detach() is responsible for freeing the surface
        // and closing the handle.
        controlImpl->Attach(m_app, surface, handle, m_hwnd, attach);

        // Wire the host-supplied focus callback so the control can
        // notify MainWindow whenever it gains keyboard focus without
        // taking a hard dependency on MainWindow.
        if (m_onLeafFocused) controlImpl->SetOnFocused(m_onLeafFocused);

        // Resolve the unfocused-split appearance from ghostty config
        // and stamp it onto the control. unfocused-split-opacity is
        // ghostty's "how visible the unfocused side should be" (0.7
        // by default), so the overlay alpha is 1 - that. fill falls
        // back to the terminal background, matching upstream.
        //
        // ghostty_config_get's 4th parameter is the LENGTH OF THE
        // KEY STRING, not the size of the output buffer (see upstream
        // Ghostty.Config.swift which passes key.lengthOfBytes). We
        // were passing sizeof(out) which made every lookup miss on a
        // 2-3 char prefix of the key. The constexpr-string-literal
        // sizeof minus the null terminator is the smallest way to
        // compute the key length without a runtime strlen.
        double opacity = 0.7;
        ghostty_config_color_s fill{};
        bool gotFill = false;
        if (m_config) {
            ghostty_config_get(m_config, &opacity,
                               "unfocused-split-opacity",
                               sizeof("unfocused-split-opacity") - 1);
            gotFill = ghostty_config_get(m_config, &fill,
                                         "unfocused-split-fill",
                                         sizeof("unfocused-split-fill") - 1);
            if (!gotFill) {
                gotFill = ghostty_config_get(m_config, &fill,
                                             "background",
                                             sizeof("background") - 1);
            }
        }
        if (!gotFill) { fill.r = 0; fill.g = 0; fill.b = 0; }
        winrt::Windows::UI::Color overlayFill{ 255, fill.r, fill.g, fill.b };
        controlImpl->SetUnfocusedAppearance(1.0 - opacity, overlayFill);

        return Pane::MakeLeaf(control, paneId);
    }

private:
    // Resolve split-divider-color from config the way upstream does
    // (macOS Ghostty.Config.swift `splitDividerColor`): prefer the
    // user-set `split-divider-color`; otherwise derive from the
    // background by darkening it — 8% if the background is light,
    // 40% if it's dark. Falls back to a neutral mid-grey if config
    // is null or even `background` isn't readable (shouldn't happen
    // in normal startup but keeps the brush valid).
    static winrt::Windows::UI::Color ResolveDividerColor(ghostty_config_t config) noexcept {
        // ghostty_config_get's 4th argument is the length of the key
        // string (see upstream Ghostty.Config.swift), not the size of
        // the output buffer. Passing sizeof(out) silently truncates
        // the key and the lookup fails for everything longer than
        // a couple of characters.
        ghostty_config_color_s c{ 128, 128, 128 };
        bool resolved = false;
        if (config) {
            resolved = ghostty_config_get(config, &c,
                                          "split-divider-color",
                                          sizeof("split-divider-color") - 1);
            if (!resolved) {
                ghostty_config_color_s bg{};
                if (ghostty_config_get(config, &bg,
                                       "background",
                                       sizeof("background") - 1)) {
                    const bool light = (static_cast<int>(bg.r)
                                        + static_cast<int>(bg.g)
                                        + static_cast<int>(bg.b)) / 3 > 128;
                    const double factor = light ? 0.08 : 0.40;
                    c.r = static_cast<uint8_t>(bg.r * (1.0 - factor));
                    c.g = static_cast<uint8_t>(bg.g * (1.0 - factor));
                    c.b = static_cast<uint8_t>(bg.b * (1.0 - factor));
                    resolved = true;
                }
            }
        }
        // Use alpha=255: the divider is a 1 DIP opaque hairline.
        // Letting the brush be translucent would let the colour
        // beneath bleed through, which on a dark background reads
        // as "blurry edge" rather than "clean separator".
        return winrt::Windows::UI::Color{ 255, c.r, c.g, c.b };
    }

    // Synchronously detach every TerminalControl under `node` so the
    // surface / DComp handle don't leak when an error path discards a
    // partially-constructed tree. Mirrors Tab::DetachAllLeaves but is
    // factored locally so the error paths above can call it without
    // depending on Tab.
    static void DetachLeaf(Pane const& node) {
        if (node.IsLeaf()) {
            if (auto* tc = Tab::LeafToTerminalControl(node)) {
                tc->Detach();
            }
            return;
        }
        if (auto* f = node.First()) DetachLeaf(*f);
        if (auto* s = node.Second()) DetachLeaf(*s);
    }

    ghostty_app_t m_app;
    ghostty_config_t m_config;
    HWND m_hwnd;
    PaneIdAllocator& m_idAllocator;
    // Optional. Fires with the leaf's ghostty_surface_t whenever the
    // leaf's TerminalControl receives keyboard focus.
    std::function<void(ghostty_surface_t)> m_onLeafFocused;
    // Resolved once in the ctor (config is read-only-here) and
    // stamped onto every SplitPanel built by Make. Reload-time
    // updates would require re-running ResolveDividerColor and
    // pushing the new colour into existing SplitPanels via
    // SetDividerColor — wired but not yet triggered by anyone.
    winrt::Windows::UI::Color m_dividerColor;
};

}  // namespace winrt::GhosttyWin32::implementation
