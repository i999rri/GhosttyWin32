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

    // Where the pane itself starts, set when `--cd` named a Windows
    // directory. wsl.exe translates such a value the same way it
    // translates the directory it inherits, so handing it over as the
    // pane's working directory asks for the same thing. Empty when the
    // line said nothing, or said it in Linux form.
    std::wstring const& WorkingDirectory() const noexcept { return m_workingDirectory; }

    // The line for libghostty, e.g. `wsl -d NixOS --cd ~ --user root`.
    // Only validated values reach it, and the options keep the order of
    // the table so the result reads the same way every time.
    std::string ToCommandLine() const;

private:
    // The options that keep the pane, each with what it accepts and
    // what it writes. An option absent from this table hands the line
    // to wsl.exe whole, which is why `--exec` and the management
    // commands need no mention: they are not lines that open a shell
    // in a pane.
    //
    // Filled in below the class: a pointer to a member needs the class
    // to be complete, which it is not while its own body is being read.
    struct Option {
        std::wstring_view longName;
        std::wstring_view shortName;  // empty when the option has none
        // Reading: checks the value against this option's own rule and
        // keeps it, or says the line is not one this host takes. `--cd`
        // has two accepted forms and keeps them apart, so the value does
        // not always land in `field`.
        bool (*take)(Invocation&, std::wstring_view) noexcept;
        // Writing: what ToCommandLine emits the option with, left empty
        // when the value went somewhere the command line cannot carry.
        std::wstring Invocation::*field;
    };

    static const Option kOptions[3];

    static bool TakeDistribution(Invocation& out, std::wstring_view value) noexcept;
    static bool TakeDirectory(Invocation& out, std::wstring_view value) noexcept;
    static bool TakeUser(Invocation& out, std::wstring_view value) noexcept;

    std::wstring m_distribution;
    std::wstring m_directory;
    std::wstring m_user;
    std::wstring m_workingDirectory;
    bool m_takesOver{ false };
};

inline constexpr Invocation::Option Invocation::kOptions[3] = {
    { L"--distribution", L"-d", &Invocation::TakeDistribution, &Invocation::m_distribution },
    { L"--cd", L"", &Invocation::TakeDirectory, &Invocation::m_directory },
    { L"--user", L"-u", &Invocation::TakeUser, &Invocation::m_user },
};

inline bool Invocation::TakeDistribution(Invocation& out, std::wstring_view value) noexcept {
    if (!IsForwardableDistro(value)) return false;
    out.m_distribution = value;
    return true;
}

inline bool Invocation::TakeDirectory(Invocation& out, std::wstring_view value) noexcept {
    // The two forms are kept apart because wsl.exe keeps them apart: a
    // Linux `--cd` wins over a Windows one whichever came first, and
    // within one form the last wins. Measured, both orders: `--cd ~
    // --cd C:\Users\me` and the reverse both start in the home, and
    // `--cd /tmp --cd C:\Windows` and the reverse both start in /tmp.
    //
    // A Linux path goes on the command line as `--cd`; a Windows one is
    // handed over as the directory the pane starts in, which crosses as
    // a field rather than as text, so a space or a backslash in it is
    // no trouble. The host prefers the former, which is what makes the
    // precedence above come out right.
    if (IsForwardableDirectory(value)) {
        out.m_directory = value;
        return true;
    }
    if (IsDriveAbsolutePath(value)) {
        out.m_workingDirectory = value;
        return true;
    }
    return false;
}

inline bool Invocation::TakeUser(Invocation& out, std::wstring_view value) noexcept {
    if (!IsForwardableUser(value)) return false;
    out.m_user = value;
    return true;
}

inline Invocation Invocation::Parse(int argc, wchar_t const* const* argv) noexcept {
    Invocation result;
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg = argv[i];

        // wsl.exe reads a leading `~` as `--cd ~`. Further along the
        // line it is the start of the in-distro command instead, which
        // this host does not open a pane for.
        if (arg == L"~") {
            if (i != 1) return result;
            TakeDirectory(result, L"~");
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

        // Given twice, the last one wins, as it does for wsl.exe. A
        // PowerShell profile that wraps `wsl` with its own `--cd` puts
        // the user's own in second, and the line has to mean there.
        if (!option->take(result, argv[++i])) return result;
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
