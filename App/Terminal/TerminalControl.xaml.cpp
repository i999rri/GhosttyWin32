#include "pch.h"
#include "Terminal/TerminalControl.xaml.h"
#include "resource.h"
#include "Interop/Encoding.h"
#if __has_include("Terminal/TerminalControl.g.cpp")
#include "Terminal/TerminalControl.g.cpp"
#elif __has_include("TerminalControl.g.cpp")
#include "TerminalControl.g.cpp"
#endif

namespace winrt::GhosttyWin32::implementation
{
    namespace mux = winrt::Microsoft::UI::Xaml;

    TerminalControl::TerminalControl()
    {
        InitializeComponent();
        m_host = std::make_shared<SurfaceHost>(Panel());

        // Every handler below captures a weak_ref instead of `this`.
        // XAML can route a final pointer event during window/control
        // teardown after the impl has started destructing — a raw
        // `this` capture would dereference a dangling pointer (the AV
        // symptom we hit: microsoft.ui.xaml.dll reading near-null at a
        // member offset). The weak_ref short-circuits cleanly when the
        // impl is gone; weakSelf.get() returns a strong impl com_ptr,
        // and the host and overlays it owns are alive for as long as
        // that lock is.
        namespace muxi = winrt::Microsoft::UI::Xaml::Input;
        auto weakSelf = get_weak();

        // The host asks this before letting keystrokes, IME engagement
        // or IME commits through to the pty. The search box is a
        // child of this control, so its input bubbles up here too;
        // while it holds focus it owns the keyboard (#171 review:
        // typing in the box also typed into the shell).
        m_host->SetInputGate([weakSelf]() {
            auto self = weakSelf.get();
            return self && !self->SearchBoxHasFocus();
        });

        // Set up IME + self-focus on Loaded. Three reasons this all
        // happens here rather than in Attach or the ctor:
        //
        //   * SelectedItem-driven focus from the outside (MainWindow's
        //     SelectionChanged handler) fires while TabView's content
        //     presenter is still swapping us in, and Focus() returns
        //     false before layout completes. Loaded fires only once
        //     the control is actually in the live visual tree and
        //     measured — at that point Focus succeeds without retry.
        //
        //   * CoreTextEditContext registration with the OS-side text-
        //     services manager only takes effect when the EditContext
        //     is created against an element that's in the live visual
        //     tree. Creating it earlier (in Attach, before
        //     TabView.SelectedItem realises us) silently fails to
        //     register, so NotifyFocusEnter doesn't engage IME — the
        //     symptom was "first tab can't toggle 半角/全角 until a
        //     second tab is created."
        //
        //   * Loaded fires once per control, after both of the above
        //     conditions are true, so the setup is naturally a single
        //     idempotent step.
        Loaded([weakSelf](auto&&, auto&&) {
            auto self = weakSelf.get();
            if (!self) return;
            self->m_host->EnsureImeContext();
            self->Focus(mux::FocusState::Programmatic);
        });

        // Mirror keyboard-focus state into the host (EditContext
        // engagement, the window's focused-surface notification, and
        // ghostty's own surface focus). Tab switches inside the same
        // window trip these; window-level activation crosses the
        // boundary without firing them, which MainWindow's Activated
        // handler covers through NotifyImeFocusEnter/Leave.
        //
        // The UnfocusedDim overlay is deliberately NOT driven from
        // here. The dim represents "this leaf is the active split in
        // its tab", which has nothing to do with XAML keyboard focus;
        // hooking it on GotFocus / LostFocus produced a dim flash
        // whenever focus migrated briefly (tab switch, new-tab
        // creation, alt-tab away). Tab.SetActivePane owns it, and a
        // real focus shift still reaches it through the host's
        // onFocused → NotifySurfaceFocused → SetActivePane chain.
        GotFocus([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) self->m_host->OnFocusGained();
        });
        LostFocus([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) self->m_host->OnFocusLost();
        });

        // Pointer routing straight to the host; it early-returns until
        // a surface is attached, so registering here (before
        // TabFactory calls Attach) is safe.
        PointerMoved([weakSelf](auto&&, muxi::PointerRoutedEventArgs const& args) {
            if (auto self = weakSelf.get()) self->m_host->OnPointerMoved(args);
        });
        PointerPressed([weakSelf](auto&&, muxi::PointerRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self) return;
            // Claim focus here; the host marks the event Handled so
            // the bubble path can't move it off us again.
            self->Focus(mux::FocusState::Pointer);
            self->m_host->OnPointerPressed(args);
        });
        PointerReleased([weakSelf](auto&&, muxi::PointerRoutedEventArgs const& args) {
            if (auto self = weakSelf.get()) self->m_host->OnPointerReleased(args);
        });
        PointerWheelChanged([weakSelf](auto&&, muxi::PointerRoutedEventArgs const& args) {
            if (auto self = weakSelf.get()) self->m_host->OnPointerWheelChanged(args);
        });

        // KeyDown / KeyUp on the control itself: the events fire only
        // while focus is inside us, so they feed the host directly
        // without an ActiveControl() lookup. The host marks handled
        // keys, which in particular prevents the TabView's built-in
        // keybindings from also acting on the already-routed key.
        KeyDown([weakSelf](auto&&, muxi::KeyRoutedEventArgs const& args) {
            if (auto self = weakSelf.get()) self->m_host->OnKeyDown(args);
        });
        KeyUp([weakSelf](auto&&, muxi::KeyRoutedEventArgs const& args) {
            if (auto self = weakSelf.get()) self->m_host->OnKeyUp(args);
        });

        // Overlay → surface wiring. The overlays only know "the user
        // wants X"; mapping X onto a binding action is the
        // composite's job, and the Surface wrapper is a no-op while
        // empty so the callbacks need no attach state of their own.
        if (auto* sb = ScrollbackImpl()) {
            sb->SetOnScrollToRow([weakSelf](uint64_t row) {
                if (auto self = weakSelf.get()) self->Surface().ScrollToRow(row);
            });
            sb->SetOnWheel([weakSelf](int delta) {
                if (auto self = weakSelf.get()) self->m_host->ScrollByWheel(delta);
            });
        }
        if (auto* se = SearchImpl()) {
            se->SetOnNeedle([weakSelf](winrt::hstring const& needle) {
                if (auto self = weakSelf.get()) {
                    self->Surface().Search(interop::Encoding::toUtf8(std::wstring{ needle }));
                }
            });
            se->SetOnNavigate([weakSelf](bool next) {
                if (auto self = weakSelf.get()) self->Surface().NavigateSearch(next);
            });
            se->SetOnCloseRequested([weakSelf]() {
                auto self = weakSelf.get();
                if (!self) return;
                // Ask ghostty to end the search; the resulting
                // END_SEARCH performs the actual hide, so core and
                // host never disagree about whether a search is on.
                // Without a surface (nothing to ask) close locally.
                if (self->Surface()) self->Surface().EndSearch();
                else self->EndSearch();
            });
        }

        // Terminals default to a text-input cursor; ghostty issues a
        // MOUSE_SHAPE = TEXT request once the surface is wired up, but
        // setting it here too avoids a flash of arrow cursor between
        // window show and the first ghostty tick.
        SetCursorShape(GHOSTTY_MOUSE_SHAPE_TEXT);
    }

    TerminalControl::~TerminalControl()
    {
        // Belt-and-suspenders: Tab's destructor normally calls Detach
        // first, but if construction failed mid-way we still want the
        // surface/handle to be freed. Detach is idempotent.
        Detach();
    }

    void TerminalControl::Detach()
    {
        // The overlay timers hold only weak refs, but stop them anyway
        // so no tick lands while the control tears down.
        if (auto* sb = ScrollbackImpl()) sb->Stop();
        if (auto* se = SearchImpl()) se->Stop();
        // Null only if InitializeComponent threw before the host was
        // created; the destructor still runs Detach in that case.
        if (m_host) m_host->Detach();
    }

    PaneStatusOverlay* TerminalControl::StatusImpl()
    {
        auto status = Status();
        return status ? winrt::get_self<PaneStatusOverlay>(status) : nullptr;
    }

    ScrollbackOverlay* TerminalControl::ScrollbackImpl()
    {
        auto scrollback = Scrollback();
        return scrollback ? winrt::get_self<ScrollbackOverlay>(scrollback) : nullptr;
    }

    SearchOverlay* TerminalControl::SearchImpl()
    {
        auto search = Search();
        return search ? winrt::get_self<SearchOverlay>(search) : nullptr;
    }

    // ----- cursor -----

    void TerminalControl::SetCursorShape(ghostty_action_mouse_shape_e shape)
    {
        using muxi = winrt::Microsoft::UI::Input::InputSystemCursorShape;
        muxi mapped;
        switch (shape) {
            case GHOSTTY_MOUSE_SHAPE_TEXT:
            case GHOSTTY_MOUSE_SHAPE_VERTICAL_TEXT:    mapped = muxi::IBeam; break;
            case GHOSTTY_MOUSE_SHAPE_POINTER:          mapped = muxi::Hand; break;
            case GHOSTTY_MOUSE_SHAPE_HELP:             mapped = muxi::Help; break;
            case GHOSTTY_MOUSE_SHAPE_WAIT:             mapped = muxi::Wait; break;
            case GHOSTTY_MOUSE_SHAPE_PROGRESS:         mapped = muxi::AppStarting; break;
            case GHOSTTY_MOUSE_SHAPE_CROSSHAIR:        mapped = muxi::Cross; break;
            case GHOSTTY_MOUSE_SHAPE_NOT_ALLOWED:
            case GHOSTTY_MOUSE_SHAPE_NO_DROP:          mapped = muxi::UniversalNo; break;
            case GHOSTTY_MOUSE_SHAPE_ALL_SCROLL:       mapped = muxi::SizeAll; break;
            case GHOSTTY_MOUSE_SHAPE_N_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_S_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_NS_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_ROW_RESIZE:       mapped = muxi::SizeNorthSouth; break;
            case GHOSTTY_MOUSE_SHAPE_E_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_W_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_EW_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_COL_RESIZE:       mapped = muxi::SizeWestEast; break;
            case GHOSTTY_MOUSE_SHAPE_NE_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_SW_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_NESW_RESIZE:      mapped = muxi::SizeNortheastSouthwest; break;
            case GHOSTTY_MOUSE_SHAPE_NW_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_SE_RESIZE:
            case GHOSTTY_MOUSE_SHAPE_NWSE_RESIZE:      mapped = muxi::SizeNorthwestSoutheast; break;
            default:                                   mapped = muxi::Arrow; break;
        }
        // Cache the shape instead of applying it unconditionally:
        // while MOUSE_VISIBILITY has the cursor hidden, a shape
        // update (ghostty re-sends TEXT/POINTER as the pointer
        // moves over cells) must not resurrect the cursor — the
        // cached value is applied when visibility comes back.
        m_visibleCursor = winrt::Microsoft::UI::Input::InputSystemCursor::Create(mapped);
        ApplyCursor();
    }

    void TerminalControl::SetMouseVisibility(bool visible)
    {
        if (m_cursorHidden == !visible) return;
        m_cursorHidden = !visible;
        ApplyCursor();
    }

    void TerminalControl::ApplyCursor()
    {
        // Single writer for ProtectedCursor: hidden state wins, else
        // the shape ghostty last asked for. Overlays that take input
        // (scrollbar, search bar) set their own Arrow on themselves,
        // so hover never has to be tracked here.
        if (m_cursorHidden) {
            // WinUI 3 has no "hide" on ProtectedCursor: null means
            // "inherit the parent's cursor" and renders as Arrow
            // (verified during #60). Hiding is therefore expressed as
            // showing a fully-transparent cursor embedded as a Win32
            // resource in the EXE. Created once; UI thread only, so
            // the local static needs no synchronization.
            static const auto blank = []() -> winrt::Microsoft::UI::Input::InputCursor {
                try {
                    return winrt::Microsoft::UI::Input::InputDesktopResourceCursor::CreateFromModule(
                        L"GhosttyWin32.exe", IDC_GHOSTTY_BLANK_CURSOR);
                } catch (winrt::hresult_error const&) {
                    // Resource lookup failed — degrade to the inherited
                    // Arrow rather than crashing over a cosmetic feature.
                    return nullptr;
                }
            }();
            ProtectedCursor(blank);
            return;
        }
        ProtectedCursor(m_visibleCursor);
    }

    // ----- ISurfaceView → overlays -----

    void TerminalControl::SetHoveredLink(std::wstring url)
    {
        if (auto* st = StatusImpl()) st->SetHoveredLink(winrt::hstring{ url });
    }

    void TerminalControl::SetReadonly(bool readonly)
    {
        if (auto* st = StatusImpl()) st->SetReadonly(readonly);
    }

    void TerminalControl::SetSecureInput(ghostty_action_secure_input_e mode)
    {
        if (auto* st = StatusImpl()) st->SetSecureInput(mode);
    }

    void TerminalControl::AppendKeySequence(std::wstring label)
    {
        if (auto* st = StatusImpl()) st->AppendKeySequence(winrt::hstring{ label });
    }

    void TerminalControl::ClearKeySequence()
    {
        if (auto* st = StatusImpl()) st->ClearKeySequence();
    }

    void TerminalControl::PushKeyTable(std::wstring name)
    {
        if (auto* st = StatusImpl()) st->PushKeyTable(winrt::hstring{ name });
    }

    void TerminalControl::PopKeyTable(bool all)
    {
        if (auto* st = StatusImpl()) st->PopKeyTable(all);
    }

    void TerminalControl::SetScrollbar(ghostty_action_scrollbar_s bar)
    {
        if (auto* sb = ScrollbackImpl()) sb->SetScrollbar(bar);
    }

    void TerminalControl::StartSearch(std::wstring needle)
    {
        if (auto* se = SearchImpl()) se->Open(winrt::hstring{ needle });
    }

    void TerminalControl::EndSearch()
    {
        auto* se = SearchImpl();
        if (!se) return;
        // Hand focus back to the terminal only if the bar had it;
        // ghostty can END_SEARCH while focus is elsewhere (another
        // pane), and stealing it then would be a surprise.
        if (se->Close()) {
            Focus(mux::FocusState::Programmatic);
        }
    }

    void TerminalControl::SetSearchTotal(ptrdiff_t total)
    {
        if (auto* se = SearchImpl()) se->SetTotal(total);
    }

    void TerminalControl::SetSearchSelected(ptrdiff_t selected)
    {
        if (auto* se = SearchImpl()) se->SetSelected(selected);
    }

    bool TerminalControl::SearchBoxHasFocus()
    {
        auto* se = SearchImpl();
        return se && se->BoxHasFocus();
    }

    bool TerminalControl::FocusSearchIfOpen()
    {
        auto* se = SearchImpl();
        return se && se->FocusInput();
    }

    // ----- pane skin -----

    void TerminalControl::SetOpaqueBackground(bool opaque, winrt::Windows::UI::Color bg)
    {
        auto underlay = OpaqueUnderlay();
        if (!underlay) return;
        if (opaque) {
            underlay.Fill(mux::Media::SolidColorBrush(bg));
            underlay.Visibility(mux::Visibility::Visible);
        } else {
            underlay.Visibility(mux::Visibility::Collapsed);
        }
    }

    void TerminalControl::SetUnfocusedAppearance(double overlayOpacity,
                                                  winrt::Windows::UI::Color overlayFill) noexcept
    {
        m_unfocusedOpacity = overlayOpacity;
        // Build the brush eagerly so ApplyFocusVisual is allocation-
        // free on every focus toggle. Recreate (rather than mutate
        // the existing brush's Color) so config reloads pick up the
        // new value with a single assignment.
        m_unfocusedFillBrush = mux::Media::SolidColorBrush(overlayFill);
        // If we're currently unfocused, refresh the live overlay so a
        // reload doesn't wait until the next focus toggle to take
        // effect.
        if (auto dim = UnfocusedDim();
            dim && dim.Visibility() == mux::Visibility::Visible)
        {
            dim.Fill(m_unfocusedFillBrush);
            dim.Opacity(m_unfocusedOpacity);
        }
    }

    void TerminalControl::ApplyFocusVisual(bool focused)
    {
        auto dim = UnfocusedDim();
        if (!dim) return;
        if (focused) {
            dim.Visibility(mux::Visibility::Collapsed);
            return;
        }
        // Lazy fall-back: if the factory never called
        // SetUnfocusedAppearance (e.g. config-less unit test paths)
        // we still want something visible. Black @ 30 % matches the
        // upstream default of 0.7 opacity over a dark background.
        if (!m_unfocusedFillBrush) {
            winrt::Windows::UI::Color fallback{ 255, 0, 0, 0 };
            m_unfocusedFillBrush = mux::Media::SolidColorBrush(fallback);
        }
        dim.Fill(m_unfocusedFillBrush);
        dim.Opacity(m_unfocusedOpacity);
        dim.Visibility(mux::Visibility::Visible);
    }
}
