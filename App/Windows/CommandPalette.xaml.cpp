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

        auto weakSelf = get_weak();
        DispatcherQueue().TryEnqueue(
            winrt::Microsoft::UI::Dispatching::DispatcherQueuePriority::Low,
            [weakSelf]() {
                auto self = weakSelf.get();
                if (!self) return;
                if (!self->m_listStale || self->m_open) return;

                DEBUG_TRACE(L"Palette: idle pre-warm\n");
                self->Refilter();
                self->m_listStale = false;
                self->WarmUpLayout();
            });
    }

    void CommandPalette::WarmUpLayout()
    {
        const auto t0 = GetTickCount64();

        // Collapsed skips layout entirely, so paying the ListView's
        // first-realization cost needs one Visible pass. Opacity 0
        // plus the synchronous collapse below keep any frame of it
        // from reaching the compositor — laid out once, never shown.
        Opacity(0.0);
        IsHitTestVisible(false);
        Visibility(mux::Visibility::Visible);
        UpdateLayout();
        // Undo the disguise, or every real Open would inherit an invisible, click-through palette.
        Visibility(mux::Visibility::Collapsed);
        IsHitTestVisible(true);
        Opacity(1.0);
        DEBUG_TRACE(L"Palette: warm-up layout %llums\n", GetTickCount64() - t0);
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
        DEBUG_TRACE(L"Palette: open %llums (was-stale=%d)\n",
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

        DEBUG_TRACE(L"Palette: refilter %llums rows=%zu built=%zu\n",
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

        // A copy, not a reference: the execute callback can replace
        // m_entries under us (a reload_config action does).
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
