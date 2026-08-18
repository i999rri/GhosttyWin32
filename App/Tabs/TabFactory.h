#pragma once

#include "Ghostty/App.h"
#include "Ghostty/Config.h"
#include "Tabs/Panes/Branch.h"
#include "Tabs/Panes/PaneId.h"
#include "Tabs/Panes/PaneIdAllocator.h"
#include "SplitPanel.h"
#include "Tabs/Tab.h"
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

namespace ghostty = core::ghostty;


// Builds Tabs. Holds the cross-cutting context (the ghostty::App
// wrapper for the app/config handles, the HWND for DPI/initial-size,
// the PaneIdAllocator that produces fresh per-leaf IDs, and an
// optional "leaf gained focus" callback that every TerminalControl
// this factory creates will fire) so callers don't have to thread
// those through every Make() call.
//
// The focus callback is the one piece of MainWindow-side context that
// needs to reach into each leaf without each leaf knowing about
// MainWindow directly. Wiring it here keeps the dependency direction
// host -> control (the inverse would be a layering violation).
//
// Config values are re-read from ghostty::App::ConfigHandle() on
// every Make/MakePane — never cached across calls. The handle is
// swapped and the OLD ghostty_config_t FREED on every config change
// (App::ReplaceConfig; fired by reload and by the system-theme
// follow), so a cached ghostty::Config would dangle and feed
// freed-memory garbage into e.g. the unfocused-split overlay color.
//
// Stateless beyond the injected references — no mutable state of its
// own. ID counter mutation lives in PaneIdAllocator; the factory only
// borrows it.
class TabFactory {
public:
    TabFactory(ghostty::App const& app, HWND hwnd,
               PaneIdAllocator& idAllocator,
               std::function<void(ghostty_surface_t)> onLeafFocused = {}) noexcept
        : m_ghostty(app), m_hwnd(hwnd), m_idAllocator(idAllocator),
          m_onLeafFocused(std::move(onLeafFocused)) {}

    TabFactory(const TabFactory&) = delete;
    TabFactory& operator=(const TabFactory&) = delete;
    TabFactory(TabFactory&&) = delete;
    TabFactory& operator=(TabFactory&&) = delete;

    // Build a fully-formed Tab. The caller created `item` and appended
    // it to the TabView; this factory creates the TerminalControl,
    // wraps it in a single-leaf Pane tree, and hosts it in a fresh
    // SplitPanel. The SplitPanel is handed back via Tab.Panel() — the
    // host is responsible for parenting it (today: under AppContent
    // alongside the other tabs' panels, with Visibility driven by
    // TabView selection). The factory deliberately doesn't touch
    // item.Content or any other host-side layout: keeps the factory
    // free of "where does the panel live" knowledge.
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
        auto branch = MakePane(initialWidth, initialHeight, std::move(onActivated));
        if (!branch) return nullptr;

        // Wrap the pane in a SplitPanel. With one pane the panel
        // collapses to "arrange the single child at the full rect".
        // The host parents the returned panel under AppContent — the
        // factory deliberately doesn't, so it stays unaware of the
        // host's chrome / content split.
        winrt::GhosttyWin32::SplitPanel splitPanel{};
        auto* splitPanelImpl = winrt::get_self<implementation::SplitPanel>(splitPanel);
        if (!splitPanelImpl) {
            OutputDebugStringA("TabFactory::Make: get_self<SplitPanel> FAILED\n");
            DetachSubtree(*branch);
            return nullptr;
        }
        // Apply the divider colour before any tree exists so the
        // first splitter Border built by SetRoot already paints
        // with the correct brush. Resolved fresh from the live config
        // handle — see the class comment for why nothing config-
        // derived may be cached across calls.
        splitPanelImpl->SetDividerColor(
            ghostty::Config(m_ghostty.ConfigHandle()).SplitDividerColor());
        splitPanelImpl->SetRoot(std::move(branch));

        try {
            return std::make_unique<Tab>(std::move(splitPanel), std::move(item));
        } catch (winrt::hresult_error const&) {
            // Tab construction validation failed. Detach synchronously
            // so the surface/handle don't leak. The splitPanel / tree
            // own the pane at this point.
            if (auto* root = splitPanelImpl->Tree().Root()) DetachSubtree(*root);
            return nullptr;
        }
    }

    // Build a new pane: TerminalControl + DComp surface handle +
    // ghostty surface + freshly-allocated PaneId, wrapped in a
    // Branch (Pane variant). Shared between Make() (the branch
    // becomes the only pane in a brand-new tab) and the NEW_SPLIT
    // action handler (the branch is inserted into an existing tab's
    // tree alongside its source pane).
    //
    // Returns nullptr on any failure. Resources acquired before the
    // failure point are released before the return — caller doesn't
    // need to clean up after a null result.
    std::unique_ptr<Branch> MakePane(
        uint32_t initialWidth,
        uint32_t initialHeight,
        std::function<void()> onActivated = {})
    {
        constexpr DWORD COMPOSITIONSURFACE_ALL_ACCESS = 0x0003L;

        auto control = winrt::GhosttyWin32::TerminalControl();
        auto* controlImpl = winrt::get_self<implementation::TerminalControl>(control);
        if (!controlImpl) {
            OutputDebugStringA("TabFactory::MakePane: get_self<TerminalControl> FAILED\n");
            return nullptr;
        }
        auto panel = controlImpl->InnerPanel();
        if (!panel) {
            OutputDebugStringA("TabFactory::MakePane: TerminalControl has no inner panel\n");
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
            OutputDebugStringA("TabFactory::MakePane: DCompositionCreateSurfaceHandle FAILED\n");
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

        // SwapChainChangedContext: receives swap_chain_changed_cb on the
        // renderer thread so the host can install an
        // IDXGISwapChain2::SetMatrixTransform that cancels XAML
        // SwapChainPanel's implicit upscale. Initial scale is taken
        // from panel.CompositionScaleX (initial value used in
        // cfg.scale_factor below; CompositionScaleChanged later
        // publishes updates atomically). Raw pointer is handed to
        // libghostty as userdata; TerminalControl owns the shared_ptr
        // and releases it on Detach (after surface_free has joined the
        // renderer thread).
        auto swapChainChanged = std::make_shared<
            winrt::GhosttyWin32::implementation::SwapChainChangedContext>();

        ghostty_surface_config_s cfg = ghostty_surface_config_new();
        cfg.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
        cfg.platform.windows.hwnd = m_hwnd;
        cfg.platform.windows.composition_surface_handle = handle;
        cfg.platform.windows.swap_chain_ready_cb = &TerminalControl::OnSwapChainReady;
        cfg.platform.windows.swap_chain_ready_userdata = attachOwned;
        cfg.platform.windows.swap_chain_changed_cb = &TerminalControl::OnSwapChainChanged;
        cfg.platform.windows.swap_chain_changed_userdata = swapChainChanged.get();
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
        // Use the SwapChainPanel's own composition scale rather than
        // GetDpiForWindow(hwnd): the panel is what actually composites
        // the swap chain, so its scale is the value we have to match
        // to avoid a "rendered at 2x, composited at 1x" stretch. On
        // RDP the panel may not have settled yet at this point and
        // can report 1.0 even when the window is at 192 DPI; that's
        // fine — TerminalControl subscribes to CompositionScaleChanged
        // and re-issues ghostty_surface_set_content_scale once the
        // real value arrives. Fall back to GetDpiForWindow only when
        // the panel reports a non-positive value (composition pipeline
        // hasn't run at all yet); using a sane positive scale at
        // surface_new time prevents libghostty from starting at 0/NaN.
        double scaleX = panel.CompositionScaleX();
        double scaleY = panel.CompositionScaleY();
        if (scaleX <= 0.0 || scaleY <= 0.0) {
            UINT dpi = GetDpiForWindow(m_hwnd);
            scaleX = scaleY = static_cast<double>(dpi) / 96.0;
        }
        cfg.scale_factor = scaleX;
        // Publish the same initial scale to the renderer-thread context
        // so the very first swap_chain_changed_cb fire (during
        // threadEnter) installs the correct matrix instead of the 1.0
        // default. CompositionScaleChanged later publishes updates if
        // the panel's settled scale changes.
        swapChainChanged->compositionScale.store(scaleX, std::memory_order_release);

        ghostty_surface_t surface = ghostty_surface_new(m_ghostty.Handle(), &cfg);
        if (!surface) {
            OutputDebugStringA("TabFactory::MakePane: ghostty_surface_new FAILED\n");
            // Callback won't fire — release the renderer's owning handle.
            delete attachOwned;
            CloseHandle(handle);
            return nullptr;
        }

        // Hand surface ownership to the control. From here on the
        // control's Detach() is responsible for freeing the surface
        // and closing the handle.
        controlImpl->Attach(m_ghostty.Handle(), surface, handle, m_hwnd, attach, std::move(swapChainChanged));

        // Wire the host-supplied focus callback so the control can
        // notify MainWindow whenever it gains keyboard focus without
        // taking a hard dependency on MainWindow.
        if (m_onLeafFocused) controlImpl->SetOnFocused(m_onLeafFocused);

        // Stamp the unfocused-split appearance onto the control.
        // GhosttyConfig owns the key-length convention and the
        // background fallback so this call site just asks for what
        // it wants. ghostty's `unfocused-split-opacity` measures
        // "how visible the unfocused side should be", so the
        // overlay alpha is (1 - that). Wrap the live handle fresh —
        // a wrapper cached at construction would read a config that
        // ReplaceConfig has already freed (the "inactive pane turns
        // black/pink after a split" garbage-color bug).
        ghostty::Config liveCfg(m_ghostty.ConfigHandle());
        controlImpl->SetUnfocusedAppearance(
            1.0 - liveCfg.UnfocusedSplitOpacity(),
            liveCfg.UnfocusedSplitFill());

        return MakePaneBranch(control, paneId);
    }

private:
    // Synchronously detach every TerminalControl under `branch` so
    // the surface / DComp handle don't leak when an error path
    // discards a partially-constructed subtree. Mirrors Tab::DetachAll
    // but is factored locally so the error paths above can call it
    // without depending on Tab.
    static void DetachSubtree(Branch& branch) {
        branch.ForEachPane([](Pane& p) {
            if (auto* tc = Tab::PaneToTerminalControl(p)) {
                tc->Detach();
            }
        });
    }

    // App-scope wrapper; outlives every MainWindow (and therefore
    // this factory). Provides the app handle and, crucially, the
    // CURRENT config handle — the factory reads config through this
    // on every call instead of caching, because ReplaceConfig frees
    // the old handle on every config change.
    ghostty::App const& m_ghostty;
    HWND m_hwnd;
    PaneIdAllocator& m_idAllocator;
    // Optional. Fires with the leaf's ghostty_surface_t whenever the
    // leaf's TerminalControl receives keyboard focus.
    std::function<void(ghostty_surface_t)> m_onLeafFocused;
};

}  // namespace winrt::GhosttyWin32::implementation
