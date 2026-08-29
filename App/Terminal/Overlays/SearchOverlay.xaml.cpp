#include "pch.h"
#include "Terminal/Overlays/SearchOverlay.xaml.h"
#include <winrt/Windows.System.h>
#include <chrono>
#if __has_include("SearchOverlay.g.cpp")
#include "SearchOverlay.g.cpp"
#endif

namespace winrt::GhosttyWin32::implementation
{
    namespace mux = winrt::Microsoft::UI::Xaml;
    namespace muxi = winrt::Microsoft::UI::Xaml::Input;

    SearchOverlay::SearchOverlay()
    {
        InitializeComponent();
        // The bar takes pointer input, so it declares its own cursor:
        // an Arrow over the buttons instead of the terminal's I-beam
        // the composite sets on itself (#171 review). ProtectedCursor
        // resolves from the element under the pointer upwards, so
        // this overrides the parent's for this subtree only.
        ProtectedCursor(winrt::Microsoft::UI::Input::InputSystemCursor::Create(
            winrt::Microsoft::UI::Input::InputSystemCursorShape::Arrow));
        auto input = Input();
        if (!input) return;
        auto weakSelf = get_weak();

        m_debounce = DispatcherQueue().CreateTimer();
        m_debounce.Interval(std::chrono::milliseconds{ 300 });
        m_debounce.IsRepeating(false);
        m_debounce.Tick([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) self->SendNeedle();
        });

        input.TextChanged([weakSelf](auto&&, auto&&) {
            auto self = weakSelf.get();
            if (!self || self->m_syncing || !self->m_open) return;
            auto text = self->Input().Text();
            const bool immediate = text.empty() || text.size() >= 3;
            self->m_debounce.Stop();
            if (immediate) self->SendNeedle();
            else self->m_debounce.Start();
        });

        // Enter = next, Shift+Enter = previous, Esc = close. Handled
        // so the keys never bubble into the terminal's own KeyDown.
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
                if (self->m_debounce.IsRunning()) {
                    self->m_debounce.Stop();
                    self->SendNeedle();
                }
                if (self->m_onNavigate) self->m_onNavigate(!shift);
                args.Handled(true);
            } else if (key == VirtualKey::Escape) {
                if (self->m_onCloseRequested) self->m_onCloseRequested();
                args.Handled(true);
            }
            // Everything else is the TextBox's own editing; the
            // composite's input gate keeps it out of the pty.
        });
        // Belt and braces for KeyUp: the release of a key typed in
        // the box must not bubble into the terminal's KeyUp.
        input.KeyUp([weakSelf](auto&&, muxi::KeyRoutedEventArgs const& args) {
            if (auto self = weakSelf.get(); self && self->HoldsKeyboardFocus())
                args.Handled(true);
        });

        NextButton().Click([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get(); self && self->m_onNavigate) self->m_onNavigate(true);
        });
        PrevButton().Click([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get(); self && self->m_onNavigate) self->m_onNavigate(false);
        });
        CloseButton().Click([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get(); self && self->m_onCloseRequested) self->m_onCloseRequested();
        });
    }

    void SearchOverlay::Open(winrt::hstring const& needle)
    {
        auto input = Input();
        if (!input) return;
        m_open = true;
        m_total = -1;
        m_selected = -1;
        UpdateCount();
        Visibility(mux::Visibility::Visible);
        if (!needle.empty()) {
            m_syncing = true;
            input.Text(needle);
            m_syncing = false;
        }
        input.Focus(mux::FocusState::Programmatic);
        input.SelectAll();
    }

    bool SearchOverlay::Close()
    {
        if (m_debounce) m_debounce.Stop();
        const bool wasOpen = m_open;
        m_open = false;
        Visibility(mux::Visibility::Collapsed);
        return wasOpen;
    }

    bool SearchOverlay::HoldsKeyboardFocus()
    {
        if (!m_open) return false;
        auto input = Input();
        if (!input) return false;
        return input.FocusState() != mux::FocusState::Unfocused;
    }

    bool SearchOverlay::FocusInput()
    {
        if (!m_open) return false;
        auto input = Input();
        if (!input) return false;
        input.Focus(mux::FocusState::Programmatic);
        return true;
    }

    void SearchOverlay::SetTotal(ptrdiff_t total)
    {
        m_total = total;
        UpdateCount();
    }

    void SearchOverlay::SetSelected(ptrdiff_t selected)
    {
        m_selected = selected;
        UpdateCount();
    }

    void SearchOverlay::Stop()
    {
        if (m_debounce) m_debounce.Stop();
    }

    void SearchOverlay::SendNeedle()
    {
        if (!m_open || !m_onNeedle) return;
        m_onNeedle(Input().Text());
    }

    void SearchOverlay::UpdateCount()
    {
        auto count = Count();
        if (!count) return;
        wchar_t buf[48];
        if (m_total < 0) {
            buf[0] = L'\0';
        } else if (m_total == 0) {
            wcscpy_s(buf, L"0 / 0");
        } else if (m_selected < 1) {
            swprintf_s(buf, L"– / %lld", static_cast<long long>(m_total));
        } else {
            swprintf_s(buf, L"%lld / %lld",
                       static_cast<long long>(m_selected),
                       static_cast<long long>(m_total));
        }
        count.Text(buf);
    }
}
