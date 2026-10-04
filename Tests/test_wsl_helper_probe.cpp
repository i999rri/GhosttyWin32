#include "pch.h"
#include "../Core/Wsl/HelperProbe.h"

using namespace core::wsl;

namespace {

constexpr std::wstring_view kExe = L"C:\\Windows\\System32\\wsl.exe";

}  // namespace

TEST(WslHelperProbeTest, AsksTheDefaultDistributionWithoutNamingOne) {
    EXPECT_EQ(HelperProbeCommandLine(kExe, L""),
              L"\"C:\\Windows\\System32\\wsl.exe\""
              L" --exec /bin/sh -c \"command -v ghostty-wsl-bridge >/dev/null\"");
}

TEST(WslHelperProbeTest, WritesTheDistributionNameBare) {
    // Measured: `--distribution "NixOS"` answers WSL_E_DISTRO_NOT_FOUND
    // and the same name unquoted succeeds, so quoting it here made every
    // `wsl -d NAME` look like a distribution without the helper.
    const auto line = HelperProbeCommandLine(kExe, L"NixOS");
    EXPECT_NE(line.find(L" --distribution NixOS "), std::wstring::npos);
    EXPECT_EQ(line.find(L"\"NixOS\""), std::wstring::npos);
}

TEST(WslHelperProbeTest, KeepsTheShellCommandAsOneArgument) {
    // wsl.exe does take quotes off what follows --exec, and the command
    // has spaces in it, so these quotes have to stay.
    const auto line = HelperProbeCommandLine(kExe, L"NixOS");
    EXPECT_NE(line.find(L"-c \"command -v ghostty-wsl-bridge >/dev/null\""),
              std::wstring::npos);
}

TEST(WslHelperProbeTest, QuotesOnlyThePathThatCanHoldASpace) {
    // Program Files is not where wsl.exe lives, but the path comes from
    // GetSystemDirectoryW and nothing here may assume it is spaceless.
    const auto line = HelperProbeCommandLine(L"C:\\Program Files\\wsl.exe", L"");
    EXPECT_EQ(line.rfind(L"\"C:\\Program Files\\wsl.exe\"", 0), 0u);
}
