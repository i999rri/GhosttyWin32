#pragma once

// A `wsl [args]` command line, read for what it asks of this host and
// written back out for libghostty.
//
// The same line is read twice: here, to decide whether the shim takes
// the pane over, and again by the fork's wsl.Invocation once the host
// has rebuilt it. The two have to agree about what an option means, so
// this names the options it understands and leaves every other line to
// the real wsl.exe.
//
// Reading and writing share one table, so an option added to it is
// understood and emitted together. That matters because the line the
// host builds is split on whitespace and shell-expanded downstream:
// values are validated rather than quoted (see ShimProtocol.h), and an
// option whose value is written without its rule would be the hole the
// rules exist to close.

#include "Wsl/ShimProtocol.h"

#include <string>
#include <string_view>

namespace core::wsl {

class Invocation {
public:
    // From the shim's argv. Everything this host does not understand
    // leaves TakesOver() false, and the line belongs to wsl.exe.
    static Invocation Parse(int argc, wchar_t const* const* argv) noexcept;

    // From the fields the shim sent. Values that fail their rule are
    // dropped rather than trusted, so what this holds can always be
    // written out.
    static Invocation FromRequest(std::wstring_view distribution,
                                  std::wstring_view directory,
                                  std::wstring_view user) noexcept;

    // Whether the shim should ask the host for an in-place session
    // instead of running wsl.exe.
    bool TakesOver() const noexcept { return m_takesOver; }

    // Empty means wsl.exe's own default: the default distribution, the
    // working directory it inherits, the distribution's default user.
    std::wstring const& Distribution() const noexcept { return m_distribution; }
    std::wstring const& Directory() const noexcept { return m_directory; }
    std::wstring const& User() const noexcept { return m_user; }

    // The line for libghostty, e.g. `wsl -d NixOS --cd ~ --user root`.
    // Only validated values reach it, and the options keep the order of
    // the table so the result reads the same way every time.
    std::string ToCommandLine() const;

private:
    // The options that keep the pane, each with the rule its value has
    // to meet. An option absent from this table hands the line to
    // wsl.exe whole, which is why `--exec` and the management commands
    // need no mention: they are not lines that open a shell in a pane.
    //
    // Filled in below the class: a pointer to a member needs the class
    // to be complete, which it is not while its own body is being read.
    struct Option {
        std::wstring_view longName;
        std::wstring_view shortName;  // empty when the option has none
        bool (*forwardable)(std::wstring_view) noexcept;
        std::wstring Invocation::*field;
    };

    static const Option kOptions[3];

    std::wstring m_distribution;
    std::wstring m_directory;
    std::wstring m_user;
    bool m_takesOver{ false };
};

inline constexpr Invocation::Option Invocation::kOptions[3] = {
    { L"--distribution", L"-d", &IsForwardableDistro, &Invocation::m_distribution },
    { L"--cd", L"", &IsForwardableDirectory, &Invocation::m_directory },
    { L"--user", L"-u", &IsForwardableUser, &Invocation::m_user },
};

inline Invocation Invocation::Parse(int argc, wchar_t const* const* argv) noexcept {
    Invocation result;
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg = argv[i];

        // wsl.exe reads a lone `~` as `--cd ~`.
        if (arg == L"~") {
            result.m_directory = L"~";
            continue;
        }

        Option const* option = nullptr;
        for (Option const& candidate : kOptions) {
            if (arg == candidate.longName ||
                (!candidate.shortName.empty() && arg == candidate.shortName))
            {
                option = &candidate;
                break;
            }
        }

        // Unknown to this host, or given without the value it takes.
        if (option == nullptr || i + 1 >= argc) return result;

        std::wstring_view value = argv[++i];
        if (!option->forwardable(value)) return result;
        // Given twice: wsl.exe has its own answer for that, and this is
        // not the place to guess at it.
        if (!(result.*(option->field)).empty()) return result;
        result.*(option->field) = value;
    }

    result.m_takesOver = true;
    return result;
}

inline Invocation Invocation::FromRequest(std::wstring_view distribution,
                                          std::wstring_view directory,
                                          std::wstring_view user) noexcept {
    // Named one by one rather than walked with the table: the caller
    // hands these over as three separate fields, and pairing them by
    // position would turn a reordering of the table into a silently
    // wrong command line. Each rule still appears beside its field.
    Invocation result;
    if (IsForwardableDistro(distribution)) result.m_distribution = distribution;
    if (IsForwardableDirectory(directory)) result.m_directory = directory;
    if (IsForwardableUser(user)) result.m_user = user;
    result.m_takesOver = true;
    return result;
}

inline std::string Invocation::ToCommandLine() const {
    std::string line = "wsl";
    for (Option const& option : kOptions) {
        std::wstring const& value = this->*(option.field);
        if (value.empty()) continue;
        line += " " + ToUtf8(option.longName) + " " + ToUtf8(value);
    }
    return line;
}

}  // namespace core::wsl
