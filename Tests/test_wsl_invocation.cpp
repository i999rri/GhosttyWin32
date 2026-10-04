#include "pch.h"
#include "../Core/Wsl/Invocation.h"

#include <vector>

using core::wsl::Invocation;

namespace {

// The shim is handed argv, so the tests are too. argv[0] is wsl itself.
Invocation Parse(std::vector<wchar_t const*> argv) {
    return Invocation::Parse(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST(WslInvocationTest, BareWslTakesOver) {
    auto inv = Parse({ L"wsl" });
    EXPECT_TRUE(inv.TakesOver());
    EXPECT_TRUE(inv.Distribution().empty());
    EXPECT_TRUE(inv.Directory().empty());
    EXPECT_TRUE(inv.User().empty());
    EXPECT_EQ(inv.ToCommandLine(), "wsl");
}

TEST(WslInvocationTest, TakesTheOptionsThatOpenAShell) {
    auto inv = Parse({ L"wsl", L"-d", L"NixOS", L"--cd", L"/srv", L"-u", L"root" });
    EXPECT_TRUE(inv.TakesOver());
    EXPECT_EQ(inv.Distribution(), L"NixOS");
    EXPECT_EQ(inv.Directory(), L"/srv");
    EXPECT_EQ(inv.User(), L"root");
}

TEST(WslInvocationTest, AcceptsBothSpellings) {
    auto shortForm = Parse({ L"wsl", L"-d", L"NixOS", L"-u", L"root" });
    auto longForm = Parse({ L"wsl", L"--distribution", L"NixOS", L"--user", L"root" });
    EXPECT_EQ(shortForm.ToCommandLine(), longForm.ToCommandLine());
}

TEST(WslInvocationTest, ALoneTildeMeansTheLinuxHome) {
    // wsl.exe reads `wsl ~` as `wsl --cd ~`, so this host has to as well:
    // dropping it would start the session under /mnt/c instead.
    auto inv = Parse({ L"wsl", L"~" });
    EXPECT_TRUE(inv.TakesOver());
    EXPECT_EQ(inv.Directory(), L"~");
    EXPECT_EQ(inv.ToCommandLine(), "wsl --cd ~");
}

TEST(WslInvocationTest, WritesTheLineInTheOrderOfTheTable) {
    auto inv = Parse({ L"wsl", L"-u", L"root", L"--cd", L"~", L"-d", L"NixOS" });
    EXPECT_EQ(inv.ToCommandLine(), "wsl --distribution NixOS --cd ~ --user root");
}

TEST(WslInvocationTest, LeavesEveryOtherLineToWslExe) {
    // An option this host does not understand, whatever it means.
    EXPECT_FALSE(Parse({ L"wsl", L"--system" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"--shell-type", L"login" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"--distribution-id", L"{guid}" }).TakesOver());
    // A command to run is not a shell to swap a pane for.
    EXPECT_FALSE(Parse({ L"wsl", L"htop" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"-e", L"htop" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"--", L"htop" }).TakesOver());
    // Management commands have no pane to take over.
    EXPECT_FALSE(Parse({ L"wsl", L"-l", L"-v" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"--shutdown" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"--status" }).TakesOver());
}

TEST(WslInvocationTest, LeavesALineWhoseValueCouldCarryArguments) {
    // The rules live in ShimProtocol.h; this is that they are consulted.
    EXPECT_FALSE(Parse({ L"wsl", L"-d", L"Ubuntu --exec calc" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"--cd", L"/srv/my app" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"--cd", L"/tmp;calc" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"-u", L"ro ot" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"-u", L"1root" }).TakesOver());
}

TEST(WslInvocationTest, LeavesAnOptionWithNothingToTake) {
    // The value is the next argument, and there is none.
    EXPECT_FALSE(Parse({ L"wsl", L"-d" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"--cd" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"-u" }).TakesOver());
}

TEST(WslInvocationTest, LeavesAnOptionGivenTwice) {
    // wsl.exe has its own answer for a repeat; guessing at it here would
    // mean the line does something different with the bridge on.
    EXPECT_FALSE(Parse({ L"wsl", L"-d", L"NixOS", L"-d", L"Ubuntu" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"--cd", L"/srv", L"--cd", L"/tmp" }).TakesOver());
}

TEST(WslInvocationTest, FromRequestDropsWhatItCannotWrite) {
    // The pipe checks these too, so this is the second line of defence:
    // a value that got through still never reaches a command line.
    auto inv = Invocation::FromRequest(L"NixOS", L"/srv/my app", L"root");
    EXPECT_EQ(inv.Distribution(), L"NixOS");
    EXPECT_TRUE(inv.Directory().empty());
    EXPECT_EQ(inv.User(), L"root");
    EXPECT_EQ(inv.ToCommandLine(), "wsl --distribution NixOS --user root");
}

TEST(WslInvocationTest, FromRequestKeepsWhatTheShimSent) {
    auto inv = Invocation::FromRequest(L"NixOS", L"~", L"root");
    EXPECT_EQ(inv.ToCommandLine(), "wsl --distribution NixOS --cd ~ --user root");
}
