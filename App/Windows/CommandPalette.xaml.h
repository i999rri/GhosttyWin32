#pragma once

#include "CommandPalette.g.h"
#include <functional>
#include <string>
#include <vector>

namespace winrt::GhosttyWin32::implementation
{
    // One palette entry as the host consumes it: display strings
    // already UTF-16, the action string kept UTF-8 because that is
    // what ghostty_surface_binding_action takes back.
    struct PaletteEntry
    {
        winrt::hstring title;
        winrt::hstring description;
        std::string actionUtf8;
    };

    // The window's command palette (TOGGLE_COMMAND_PALETTE, #205).
    // Owns the input box, the filtered list and the open state;
    // knows nothing about ghostty. Filtering is FuzzyMatch over
    // "title description"; user intent leaves through two
    // callbacks: an entry to execute (its action string) and
    // "closed" (the window returns focus to the terminal).
    // UI thread only.
    struct CommandPalette : CommandPaletteT<CommandPalette>
    {
        CommandPalette();

        // Show with a fresh entry set (re-read from config on every
        // open — a reload may have changed it), clear the query,
        // focus the box.
        void Open(std::vector<PaletteEntry> entries);

        // Hide. Returns whether it was open, so the caller hands
        // focus back to the terminal only in that case.
        bool Close();

        bool IsOpen() const noexcept { return m_open; }

        // The selected entry's action string, fired before the
        // palette closes itself.
        void SetOnExecute(std::function<void(std::string const&)> cb) noexcept {
            m_onExecute = std::move(cb);
        }
        // Fired after any close (execute, Esc, click-away).
        void SetOnClosed(std::function<void()> cb) noexcept {
            m_onClosed = std::move(cb);
        }

    private:
        // Re-score every entry against the current query and rebuild
        // the list: score descending, config order as the tiebreak,
        // first row selected.
        void Refilter();
        void MoveSelection(int delta);
        void ExecuteSelected();
        void RequestClose();

        std::vector<PaletteEntry> m_entries;
        // Results row -> m_entries index, rebuilt by Refilter.
        std::vector<std::size_t> m_visible;
        std::function<void(std::string const&)> m_onExecute;
        std::function<void()> m_onClosed;
        bool m_open{ false };
    };
}

namespace winrt::GhosttyWin32::factory_implementation
{
    struct CommandPalette : CommandPaletteT<CommandPalette, implementation::CommandPalette>
    {
    };
}
