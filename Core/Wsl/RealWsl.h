#pragma once

// Where the real wsl.exe is.
//
// By absolute path, because the shim is also called wsl.exe and comes
// first on the PATH of the shells that run it: anything resolved by
// name there would be itself.
//
// Shared by the shim and the host, unlike the rules about what a `wsl`
// line may ask for. Those live with the host and change as options are
// added; this does not change at all, which is what makes it safe for
// both to hold -- a shim and a host from different builds still agree
// about it. See Core/Wsl/InstalledShim.h for why that matters.

#include <windows.h>
#include <string>

namespace core::wsl {

// Empty when the system directory cannot be read, which leaves the
// caller to decide what to do without one.
inline std::wstring RealWslPath() {
    wchar_t system32[MAX_PATH];
    UINT len = GetSystemDirectoryW(system32, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::wstring(system32, len) + L"\\wsl.exe";
}

}  // namespace core::wsl
