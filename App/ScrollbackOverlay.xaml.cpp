#include "pch.h"
#include "ScrollbackOverlay.xaml.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#if __has_include("ScrollbackOverlay.g.cpp")
#include "ScrollbackOverlay.g.cpp"
#endif

namespace winrt::GhosttyWin32::implementation
{
    namespace mux = winrt::Microsoft::UI::Xaml;

    ScrollbackOverlay::ScrollbackOverlay()
    {
        InitializeComponent();
        auto bar = Bar();
        if (!bar) return;
        auto weakSelf = get_weak();

        // Thumb drag / track click → absolute scroll. Echoes from
        // SetScrollbar are filtered by m_syncing.
        bar.ValueChanged([weakSelf](auto&&,
                Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self || self->m_syncing) return;
            if (self->m_onScrollToRow) {
                self->m_onScrollToRow(static_cast<uint64_t>(std::llround(args.NewValue())));
            }
            self->Reveal();
        });

        // Hover keeps the bar visible; leaving restarts the idle fade.
        bar.PointerEntered([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) {
                self->m_hovered = true;
                self->Reveal();
                if (self->m_onHoverChanged) self->m_onHoverChanged(true);
            }
        });
        bar.PointerExited([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) {
                self->m_hovered = false;
                self->FadeIfIdle();
                if (self->m_onHoverChanged) self->m_onHoverChanged(false);
            }
        });

        // Wheel over the bar itself: hand the delta to the composite
        // instead of letting the ScrollBar consume it.
        bar.PointerWheelChanged([weakSelf](auto&&,
                winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self) return;
            if (self->m_onWheel) {
                self->m_onWheel(args.GetCurrentPoint(nullptr).Properties().MouseWheelDelta());
            }
            args.Handled(true);
        });

        m_fadeTimer = DispatcherQueue().CreateTimer();
        m_fadeTimer.Interval(std::chrono::milliseconds{ 1200 });
        m_fadeTimer.IsRepeating(false);
        m_fadeTimer.Tick([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) self->FadeIfIdle();
        });
    }

    void ScrollbackOverlay::SetScrollbar(ghostty_action_scrollbar_s bar)
    {
        auto sb = Bar();
        if (!sb) return;
        // Nothing to scroll: the whole screen fits the viewport.
        if (bar.total <= bar.len) {
            sb.Visibility(mux::Visibility::Collapsed);
            return;
        }
        // Range mapping: Value = first visible row, Maximum = the
        // last first-visible-row (total - len), ViewportSize = len
        // so the thumb length reflects the visible fraction.
        const double maximum = static_cast<double>(bar.total - bar.len);
        const double value = std::min(static_cast<double>(bar.offset), maximum);
        m_syncing = true;
        sb.Maximum(maximum);
        sb.ViewportSize(static_cast<double>(bar.len));
        sb.LargeChange(static_cast<double>(bar.len));
        sb.Value(value);
        m_syncing = false;
        sb.Visibility(mux::Visibility::Visible);
        Reveal();
    }

    void ScrollbackOverlay::Stop()
    {
        if (m_fadeTimer) m_fadeTimer.Stop();
    }

    void ScrollbackOverlay::Reveal()
    {
        auto sb = Bar();
        if (!sb) return;
        sb.Opacity(1.0);
        if (m_fadeTimer) {
            m_fadeTimer.Stop();
            m_fadeTimer.Start();
        }
    }

    void ScrollbackOverlay::FadeIfIdle()
    {
        if (m_hovered) return;
        if (auto sb = Bar()) sb.Opacity(0.0);
    }
}
