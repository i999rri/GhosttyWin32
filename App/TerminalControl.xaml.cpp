#include "pch.h"
#include "TerminalControl.xaml.h"
#include "resource.h"
#include "Interop/Encoding.h"
#include <winrt/Windows.System.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#if __has_include("TerminalControl.g.cpp")
#include "TerminalControl.g.cpp"
#endif


namespace winrt::GhosttyWin32::implementation
{
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
        // and the host it owns is alive for as long as that lock is.
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
            self->Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
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
            self->Focus(Microsoft::UI::Xaml::FocusState::Pointer);
            self->m_host->OnPointerPressed(args);
        });
        PointerReleased([weakSelf](auto&&, muxi::PointerRoutedEventArgs const& args) {
            if (auto self = weakSelf.get()) self->m_host->OnPointerReleased(args);
        });
        PointerWheelChanged([weakSelf](auto&&, muxi::PointerRoutedEventArgs const& args) {
            if (auto self = weakSelf.get()) self->m_host->OnPointerWheelChanged(args);
        });

        SetupScrollbar();
        SetupSearchBar();

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

        // Terminals default to a text-input cursor; ghostty issues a
        // MOUSE_SHAPE = TEXT request once the surface is wired up, but
        // setting it here too avoids a flash of arrow cursor between
        // window show and the first ghostty tick.
        SetCursorShape(GHOSTTY_MOUSE_SHAPE_TEXT);
    }

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

    void TerminalControl::ApplyCursor()
    {
        // Single writer for ProtectedCursor so the three inputs
        // compose in one place: hovering an interactive overlay
        // (scrollbar, search bar) wins with an Arrow (a text I-beam
        // over a draggable thumb or a button reads wrong — #170 /
        // #171 review), hidden state wins next, else the shape
        // ghostty last asked for.
        if (m_scrollbarHovered || m_overlayHovered) {
            static const auto arrow =
                winrt::Microsoft::UI::Input::InputSystemCursor::Create(
                    winrt::Microsoft::UI::Input::InputSystemCursorShape::Arrow);
            ProtectedCursor(arrow);
            return;
        }
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

    void TerminalControl::AppendKeySequence(winrt::hstring const& label)
    {
        m_keySequence.push_back(label);
        UpdateKeyStateBadge();
    }

    void TerminalControl::ClearKeySequence()
    {
        if (m_keySequence.empty()) return;
        m_keySequence.clear();
        UpdateKeyStateBadge();
    }

    void TerminalControl::PushKeyTable(winrt::hstring const& name)
    {
        m_keyTables.push_back(name);
        UpdateKeyStateBadge();
    }

    void TerminalControl::PopKeyTable(bool all)
    {
        if (m_keyTables.empty()) return;
        if (all) {
            m_keyTables.clear();
        } else {
            m_keyTables.pop_back();
        }
        UpdateKeyStateBadge();
    }

    void TerminalControl::UpdateKeyStateBadge()
    {
        // Table stack first ("resize"), pending chord second
        // ("ctrl+a …"), separated when both are live. Mirrors the
        // information upstream's KeyStateIndicator carries, minus
        // its popover chrome.
        std::wstring text;
        for (auto const& name : m_keyTables) {
            if (!text.empty()) text += L" · ";
            text += name;
        }
        if (!m_keySequence.empty()) {
            if (!text.empty()) text += L" · ";
            for (auto const& label : m_keySequence) {
                text += label;
                text += L' ';
            }
            text += L'…';
        }
        if (text.empty()) {
            KeyStateBadge().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            return;
        }
        KeyStateBadgeText().Text(text);
        KeyStateBadge().Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
    }

    void TerminalControl::SetOpaqueBackground(bool opaque, winrt::Windows::UI::Color bg)
    {
        auto underlay = OpaqueUnderlay();
        if (!underlay) return;
        if (opaque) {
            underlay.Fill(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(bg));
            underlay.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        } else {
            underlay.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        }
    }

    void TerminalControl::SetReadonly(bool readonly)
    {
        ReadonlyBadge().Visibility(readonly
            ? winrt::Microsoft::UI::Xaml::Visibility::Visible
            : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    }

    void TerminalControl::SetSecureInput(ghostty_action_secure_input_e mode)
    {
        const bool on = mode == GHOSTTY_SECURE_INPUT_ON    ? true
                      : mode == GHOSTTY_SECURE_INPUT_OFF   ? false
                                                           : !m_secureInput;
        if (on == m_secureInput) return;
        m_secureInput = on;
        SecureInputBadge().Visibility(on
            ? winrt::Microsoft::UI::Xaml::Visibility::Visible
            : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    }

    void TerminalControl::SetMouseVisibility(bool visible)
    {
        if (m_cursorHidden == !visible) return;
        m_cursorHidden = !visible;
        ApplyCursor();
    }

    void TerminalControl::SetUnfocusedAppearance(double overlayOpacity,
                                                  winrt::Windows::UI::Color overlayFill) noexcept
    {
        m_unfocusedOpacity = overlayOpacity;
        // Build the brush eagerly so ApplyFocusVisual is allocation-
        // free on every focus toggle. Recreate (rather than mutate
        // the existing brush's Color) so config reloads pick up the
        // new value with a single assignment.
        m_unfocusedFillBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(overlayFill);
        // If we're currently unfocused, refresh the live overlay so a
        // reload doesn't wait until the next focus toggle to take
        // effect.
        if (auto dim = UnfocusedDim();
            dim && dim.Visibility() == winrt::Microsoft::UI::Xaml::Visibility::Visible)
        {
            dim.Fill(m_unfocusedFillBrush);
            dim.Opacity(m_unfocusedOpacity);
        }
    }

    void TerminalControl::SetHoveredLink(winrt::hstring const& url)
    {
        auto banner = LinkBanner();
        if (!banner) return;
        if (url.empty()) {
            banner.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            return;
        }
        LinkBannerText().Text(url);
        banner.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
    }

    // ----- scrollback scrollbar (#154) -----

    void TerminalControl::SetupScrollbar()
    {
        auto bar = ScrollbackBar();
        if (!bar) return;
        auto weakSelf = get_weak();

        // Thumb drag / track click → absolute scroll. Echoes from
        // SetScrollbar are filtered by m_scrollbarSyncing.
        bar.ValueChanged([weakSelf](auto&&,
                Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || self->m_scrollbarSyncing || !self->Surface()) return;
            self->Surface().ScrollToRow(
                static_cast<uint64_t>(std::llround(args.NewValue())));
            self->RevealScrollbar();
        });

        // Hover keeps the bar visible; leaving restarts the idle fade.
        bar.PointerEntered([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) {
                self->m_scrollbarHovered = true;
                self->RevealScrollbar();
                self->ApplyCursor();
            }
        });
        bar.PointerExited([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) {
                self->m_scrollbarHovered = false;
                self->FadeScrollbarIfIdle();
                self->ApplyCursor();
            }
        });

        // Wheel over the bar itself: forward to the terminal instead
        // of letting the ScrollBar consume it (its default wheel
        // handling would scroll by SmallChange and echo through
        // ValueChanged — correct but jerky).
        bar.PointerWheelChanged([weakSelf](auto&&,
                winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self) return;
            self->m_host->ScrollByWheel(
                args.GetCurrentPoint(self->Panel()).Properties().MouseWheelDelta());
            args.Handled(true);
        });

        m_scrollbarFadeTimer = DispatcherQueue().CreateTimer();
        m_scrollbarFadeTimer.Interval(std::chrono::milliseconds{ 1200 });
        m_scrollbarFadeTimer.IsRepeating(false);
        m_scrollbarFadeTimer.Tick([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) self->FadeScrollbarIfIdle();
        });
    }

    void TerminalControl::SetScrollbar(ghostty_action_scrollbar_s bar)
    {
        auto sb = ScrollbackBar();
        if (!sb) return;
        // Nothing to scroll: the whole screen fits the viewport.
        if (bar.total <= bar.len) {
            sb.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            return;
        }
        // Range mapping: Value = first visible row, Maximum = the
        // last first-visible-row (total - len), ViewportSize = len
        // so the thumb length reflects the visible fraction.
        const double maximum = static_cast<double>(bar.total - bar.len);
        const double value = std::min(static_cast<double>(bar.offset), maximum);
        m_scrollbarSyncing = true;
        sb.Maximum(maximum);
        sb.ViewportSize(static_cast<double>(bar.len));
        sb.LargeChange(static_cast<double>(bar.len));
        sb.Value(value);
        m_scrollbarSyncing = false;
        sb.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        RevealScrollbar();
    }

    void TerminalControl::RevealScrollbar()
    {
        auto sb = ScrollbackBar();
        if (!sb) return;
        sb.Opacity(1.0);
        if (m_scrollbarFadeTimer) {
            m_scrollbarFadeTimer.Stop();
            m_scrollbarFadeTimer.Start();
        }
    }

    void TerminalControl::FadeScrollbarIfIdle()
    {
        if (m_scrollbarHovered) return;
        if (auto sb = ScrollbackBar()) sb.Opacity(0.0);
    }

    // ----- search bar -----

    void TerminalControl::SetupSearchBar()
    {
        namespace muxi = winrt::Microsoft::UI::Xaml::Input;
        auto input = SearchInput();
        if (!input) return;
        auto weakSelf = get_weak();

        // Debounce timer (macOS SurfaceView rule: <3 chars wait
        // 300ms, otherwise immediate — see the header comment).
        m_searchDebounce = DispatcherQueue().CreateTimer();
        m_searchDebounce.Interval(std::chrono::milliseconds{ 300 });
        m_searchDebounce.IsRepeating(false);
        m_searchDebounce.Tick([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) self->SendSearchNeedle();
        });

        input.TextChanged([weakSelf](auto&&, auto&&) {
            auto self = weakSelf.get();
            if (!self || self->m_searchSyncing || !self->m_searchOpen) return;
            auto text = self->SearchInput().Text();
            const bool immediate = text.empty() || text.size() >= 3;
            self->m_searchDebounce.Stop();
            if (immediate) self->SendSearchNeedle();
            else self->m_searchDebounce.Start();
        });

        // Enter = next, Shift+Enter = previous, Esc = close. Handled
        // so the keys never bubble into the terminal's own KeyDown
        // (which would type them into the pty).
        input.KeyDown([weakSelf](auto&&, muxi::KeyRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self) return;
            using winrt::Windows::System::VirtualKey;
            const auto key = args.Key();
            if (key == VirtualKey::Enter) {
                const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                // Make sure a pending debounced needle lands before
                // navigating, else Enter on a fresh 1-2 char needle
                // navigates the previous search.
                if (self->m_searchDebounce.IsRunning()) {
                    self->m_searchDebounce.Stop();
                    self->SendSearchNeedle();
                }
                self->Surface().NavigateSearch(!shift);
                args.Handled(true);
            } else if (key == VirtualKey::Escape) {
                self->CloseSearchFromUi();
                args.Handled(true);
            }
            // Everything else is the TextBox's own editing; the
            // host's input gate keeps it out of the pty either way.
        });
        // Belt and braces for KeyUp: the release of a key typed in
        // the box must not bubble into the terminal's KeyUp.
        input.KeyUp([weakSelf](auto&&, muxi::KeyRoutedEventArgs const& args) {
            if (auto self = weakSelf.get(); self && self->SearchBoxHasFocus())
                args.Handled(true);
        });

        // The control-wide ProtectedCursor (ghostty's TEXT shape) is
        // inherited by the bar; show an Arrow over it like over the
        // scrollbar — same ApplyCursor composition.
        auto bar = SearchBar();
        bar.PointerEntered([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) {
                self->m_overlayHovered = true;
                self->ApplyCursor();
            }
        });
        bar.PointerExited([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) {
                self->m_overlayHovered = false;
                self->ApplyCursor();
            }
        });

        SearchNext().Click([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) self->Surface().NavigateSearch(true);
        });
        SearchPrev().Click([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) self->Surface().NavigateSearch(false);
        });
        SearchClose().Click([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) self->CloseSearchFromUi();
        });
    }

    void TerminalControl::StartSearch(std::wstring needle)
    {
        auto bar = SearchBar();
        auto input = SearchInput();
        if (!bar || !input) return;
        m_searchOpen = true;
        m_searchTotal = -1;
        m_searchSelected = -1;
        UpdateSearchCount();
        bar.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        // A non-empty needle came from search_selection: ghostty has
        // already started that search, so pre-fill without echoing
        // it back. An empty needle (bare start_search) leaves the
        // box as-is — re-opening keeps the last query, which matches
        // every editor's find bar.
        if (!needle.empty()) {
            m_searchSyncing = true;
            input.Text(needle);
            m_searchSyncing = false;
        }
        input.Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
        input.SelectAll();
    }

    void TerminalControl::EndSearch()
    {
        auto bar = SearchBar();
        if (!bar) return;
        if (m_searchDebounce) m_searchDebounce.Stop();
        const bool wasOpen = m_searchOpen;
        m_searchOpen = false;
        bar.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        // Hand focus back to the terminal only if the bar had it;
        // ghostty can END_SEARCH while focus is elsewhere (another
        // pane), and stealing it then would be a surprise.
        if (wasOpen) {
            Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
        }
    }

    bool TerminalControl::SearchBoxHasFocus()
    {
        if (!m_searchOpen) return false;
        auto input = SearchInput();
        if (!input) return false;
        return input.FocusState() != winrt::Microsoft::UI::Xaml::FocusState::Unfocused;
    }

    bool TerminalControl::FocusSearchIfOpen()
    {
        if (!m_searchOpen) return false;
        auto input = SearchInput();
        if (!input) return false;
        input.Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
        return true;
    }

    void TerminalControl::CloseSearchFromUi()
    {
        // The UI closes by asking ghostty to end the search; the
        // resulting END_SEARCH action performs the actual hide, so
        // core and host never disagree about whether a search is on.
        if (Surface()) Surface().EndSearch();
        else EndSearch();
    }

    void TerminalControl::SendSearchNeedle()
    {
        if (!Surface() || !m_searchOpen) return;
        std::wstring text{ SearchInput().Text() };
        Surface().Search(interop::Encoding::toUtf8(text));
    }

    void TerminalControl::SetSearchTotal(ptrdiff_t total)
    {
        m_searchTotal = total;
        UpdateSearchCount();
    }

    void TerminalControl::SetSearchSelected(ptrdiff_t selected)
    {
        m_searchSelected = selected;
        UpdateSearchCount();
    }

    void TerminalControl::UpdateSearchCount()
    {
        auto count = SearchCount();
        if (!count) return;
        wchar_t buf[48];
        if (m_searchTotal < 0) {
            buf[0] = L'\0';
        } else if (m_searchTotal == 0) {
            wcscpy_s(buf, L"0 / 0");
        } else if (m_searchSelected < 1) {
            swprintf_s(buf, L"– / %lld", static_cast<long long>(m_searchTotal));
        } else {
            swprintf_s(buf, L"%lld / %lld",
                       static_cast<long long>(m_searchSelected),
                       static_cast<long long>(m_searchTotal));
        }
        count.Text(buf);
    }

    void TerminalControl::ApplyFocusVisual(bool focused)
    {
        auto dim = UnfocusedDim();
        if (!dim) return;
        if (focused) {
            dim.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            return;
        }
        // Lazy fall-back: if the factory never called
        // SetUnfocusedAppearance (e.g. config-less unit test paths)
        // we still want something visible. Black @ 30 % matches the
        // upstream default of 0.7 opacity over a dark background.
        if (!m_unfocusedFillBrush) {
            winrt::Windows::UI::Color fallback{ 255, 0, 0, 0 };
            m_unfocusedFillBrush = winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(fallback);
        }
        dim.Fill(m_unfocusedFillBrush);
        dim.Opacity(m_unfocusedOpacity);
        dim.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
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
        if (m_scrollbarFadeTimer) m_scrollbarFadeTimer.Stop();
        if (m_searchDebounce) m_searchDebounce.Stop();
        // Null only if InitializeComponent threw before the host was
        // created; the destructor still runs Detach in that case.
        if (m_host) m_host->Detach();
    }
}
