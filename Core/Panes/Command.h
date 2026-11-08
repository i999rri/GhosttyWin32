#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace core::panes {

// What a pane's shell was started with, kept on the pane so a pane
// made from it -- a split, a new tab -- starts the same way, and a
// WSL pane begets WSL panes (#220). Empty stands for the configured
// default, which libghostty resolves, so this never spells it out.
class Command {
public:
    Command() = default;
    static Command Default() { return Command{}; }
    static Command Of(std::string text) { return Command{ std::move(text) }; }

    bool IsDefault() const noexcept { return m_text.empty(); }

    // Whether the program is wsl.exe itself, in any spelling a shell
    // takes for it. Such a pane is WSL already, so the in-place shim
    // has nothing to do there.
    bool StartsWsl() const {
        std::string_view program = std::string_view(m_text).substr(0, m_text.find_first_of(" \t"));
        return EqualsIgnoringCase(program, "wsl") || EqualsIgnoringCase(program, "wsl.exe");
    }

    // What libghostty is handed as the surface command; empty leaves
    // its config in charge.
    std::string const& Text() const noexcept { return m_text; }

    bool operator==(Command const&) const = default;

private:
    explicit Command(std::string text) : m_text(std::move(text)) {}

    static bool EqualsIgnoringCase(std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }

    std::string m_text;
};

}  // namespace core::panes
