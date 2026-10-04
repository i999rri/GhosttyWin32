#pragma once

// How the shim asks a distribution whether the bridge's in-distro half
// is installed (#229). The command line is built here, away from the
// CreateProcessW call that runs it, because its quoting is the part that
// is easy to get wrong and impossible to see going wrong: a probe that
// cannot reach the distribution answers the same way as one that
// reaches it and finds nothing.

#include <string>
#include <string_view>

namespace core::wsl {

// The helper's name, as installed on the distribution's PATH.
inline constexpr std::wstring_view kHelperName = L"ghostty-wsl-bridge";

// `exe` is the real wsl.exe by absolute path, since the shim is also
// called wsl.exe; an empty `distro` asks the default distribution.
//
// The distribution name goes in bare. wsl.exe does not take quotes off
// this value: `--distribution "NixOS"` answers WSL_E_DISTRO_NOT_FOUND
// where the same name unquoted succeeds. Bare is safe because
// IsForwardableDistro has already refused anything with a space or a
// quote in it. What follows `--exec` does have its quotes taken off, so
// the shell command stays quoted and arrives as one argument.
inline std::wstring HelperProbeCommandLine(std::wstring_view exe,
                                           std::wstring_view distro) {
    std::wstring line;
    line += L'"';
    line += exe;
    line += L'"';
    if (!distro.empty()) {
        line += L" --distribution ";
        line += distro;
    }
    line += L" --exec /bin/sh -c \"command -v ";
    line += kHelperName;
    line += L" >/dev/null\"";
    return line;
}

}  // namespace core::wsl
