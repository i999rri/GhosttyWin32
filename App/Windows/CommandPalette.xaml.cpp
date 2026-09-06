#include "pch.h"
#include "Windows/CommandPalette.xaml.h"
#include "Host/FuzzyMatch.h"
#include "Win32/DebugTrace.h"
#include <winrt/Windows.System.h>
#include <algorithm>
#if __has_include("CommandPalette.g.cpp")
#include "CommandPalette.g.cpp"
#endif

namespace winrt::GhosttyWin32::implementation
{
    namespace mux = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
    namespace muxi = winrt::Microsoft::UI::Xaml::Input;

    namespace {
        // Two-line row: the title, then the smaller dimmed
        // description (omitted when the entry has none).
        mux::UIElement MakeRow(PaletteEntry const& e)
        {
            muxc::StackPanel panel;
            panel.Padding(mux::Thickness{ 4, 4, 4, 4 });
            muxc::TextBlock title;
            title.Text(e.title);
            panel.Children().Append(title);
            if (!e.description.empty()) {
                muxc::TextBlock desc;
                desc.Text(e.description);
                desc.FontSize(12);
                desc.Opacity(0.7);
                desc.TextTrimming(mux::TextTrimming::CharacterEllipsis);
                panel.Children().Append(desc);
            }
            return panel;
        }
    }

    CommandPalette::CommandPalette()
    {
        InitializeComponent();
        auto weakSelf = get_weak();

        // Click-away on the scrim closes without executing.
        Scrim().Tapped([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) self->RequestClose();
        });

        Input().TextChanged([weakSelf](auto&&, auto&&) {
            if (auto self = weakSelf.get()) self->Refilter();
        });

        // The box keeps keyboard focus the whole time; list
        // navigation and execution are keys on the box, launcher
        // style.
        Input().KeyDown([weakSelf](auto&&, muxi::KeyRoutedEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self) return;
            switch (args.Key()) {
                case winrt::Windows::System::VirtualKey::Down:
                    self->MoveSelection(+1);
                    args.Handled(true);
                    break;
                case winrt::Windows::System::VirtualKey::Up:
                    self->MoveSelection(-1);
                    args.Handled(true);
                    break;
                case winrt::Windows::System::VirtualKey::Enter:
                    self->ExecuteSelected();
                    args.Handled(true);
                    break;
                case winrt::Windows::System::VirtualKey::Escape:
                    self->RequestClose();
                    args.Handled(true);
                    break;
                default:
                    break;
            }
        });

        Results().ItemClick([weakSelf](auto&&, muxc::ItemClickEventArgs const& args) {
            auto self = weakSelf.get();
            if (!self) return;
            uint32_t index = 0;
            if (self->Results().Items().IndexOf(args.ClickedItem(), index)) {
                self->Results().SelectedIndex(static_cast<int32_t>(index));
                self->ExecuteSelected();
            }
        });
    }

    void CommandPalette::SetEntries(std::vector<PaletteEntry> entries)
    {
        m_entries = std::move(entries);
        m_rows.assign(m_entries.size(), nullptr);
        m_listStale = true;

        // Pre-warm at idle priority. XAML forbids building UI off
        // the UI thread, so "in parallel" here means "after the
        // startup path's real work, before the user's first open" —
        // low priority keeps it out of everything that matters. If
        // the palette opens before this runs, Open's own stale
        // check wins and this becomes a no-op.
        auto weakSelf = get_weak();
        DispatcherQueue().TryEnqueue(
            winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
            [weakSelf]() {
                auto self = weakSelf.get();
                if (!self) return;
                if (!self->m_listStale || self->m_open) return;

                DEBUG_TRACE(L"Palette: idle pre-warm
");
                self->Refilter();
                self->m_listStale = false;
            });
    }

    void CommandPalette::Open()
    {
        const auto t0 = GetTickCount64();
        m_open = true;
        Visibility(mux::Visibility::Visible);
        auto input = Input();
        // Clearing a leftover query refilters through TextChanged;
        // when it was already empty no event fires, so a stale list
        // (SetEntries since the last build) refilters explicitly.
        const bool hadQuery = !input.Text().empty();
        const bool wasStale = m_listStale;
        input.Text(L"");
        if (m_listStale && !hadQuery) Refilter();
        m_listStale = false;
        input.Focus(mux::FocusState::Programmatic);
        DEBUG_TRACE(L"Palette: open %llums (was-stale=%d)
",
                    GetTickCount64() - t0, wasStale ? 1 : 0);
    }

    bool CommandPalette::Close()
    {
        const bool wasOpen = m_open;
        m_open = false;
        Visibility(mux::Visibility::Collapsed);
        return wasOpen;
    }

    void CommandPalette::Refilter()
    {
        const auto t0 = GetTickCount64();
        std::size_t built = 0;
        const std::wstring query{ std::wstring_view{ Input().Text() } };

        // Score against "title description" so a query can hit
        // either; FuzzyMatch decides the order (its header states
        // the rules).
        struct Scored { std::size_t index; int score; };
        std::vector<Scored> scored;
        scored.reserve(m_entries.size());
        for (std::size_t i = 0; i < m_entries.size(); ++i) {
            std::wstring hay{ std::wstring_view{ m_entries[i].title } };
            hay += L' ';
            hay.append(std::wstring_view{ m_entries[i].description });
            if (auto s = core::host::FuzzyMatch::Score(query, hay)) {
                scored.push_back({ i, *s });
            }
        }
        // stable: equal scores keep config order.
        std::stable_sort(scored.begin(), scored.end(),
                         [](Scored const& a, Scored const& b) { return a.score > b.score; });

        if (scored.size() > kMaxVisibleRows) scored.resize(kMaxVisibleRows);

        m_visible.clear();
        auto items = Results().Items();
        items.Clear();
        for (auto const& s : scored) {
            m_visible.push_back(s.index);
            auto& row = m_rows[s.index];
            if (!row) {
                row = MakeRow(m_entries[s.index]);
                ++built;
            }
            items.Append(row);
        }
        if (!m_visible.empty()) Results().SelectedIndex(0);

        DEBUG_TRACE(L"Palette: refilter %llums rows=%zu built=%zu
",
                    GetTickCount64() - t0, m_visible.size(), built);
    }

    void CommandPalette::MoveSelection(int delta)
    {
        const int count = static_cast<int>(m_visible.size());
        if (count == 0) return;

        const int next = std::clamp(Results().SelectedIndex() + delta, 0, count - 1);
        Results().SelectedIndex(next);
        Results().ScrollIntoView(Results().Items().GetAt(static_cast<uint32_t>(next)));
    }

    void CommandPalette::ExecuteSelected()
    {
        const int sel = Results().SelectedIndex();
        if (sel < 0 || static_cast<std::size_t>(sel) >= m_visible.size()) return;

        // Copy out before Close: the execute callback may replace
        // the entry set (e.g. an action that reloads the config).
        const std::string action =
            m_entries[m_visible[static_cast<std::size_t>(sel)]].actionUtf8;
        Close();
        if (m_onExecute) m_onExecute(action);
        if (m_onClosed) m_onClosed();
    }

    void CommandPalette::RequestClose()
    {
        if (!Close()) return;

        if (m_onClosed) m_onClosed();
    }
}
