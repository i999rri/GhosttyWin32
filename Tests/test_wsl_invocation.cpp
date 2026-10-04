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

TEST(WslInvocationTest, LeavesATildeThatIsNotFirstToWslExe) {
    // Only the first argument reads as `--cd ~`. Further along, wsl.exe
    // starts the in-distro command with it: `wsl --cd /tmp ~` has the
    // login shell try to run /home/<me>, which is not a pane this host
    // opens.
    EXPECT_FALSE(Parse({ L"wsl", L"--cd", L"/tmp", L"~" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"-d", L"NixOS", L"~" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"~", L"~" }).TakesOver());
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

TEST(WslInvocationTest, KeepsTheLastOfAnOptionGivenTwice) {
    // What wsl.exe does: `--cd /tmp --cd /var` starts in /var, and
    // `-u root -u me` looks up me. A PowerShell profile that wraps `wsl`
    // with its own `--cd` is the common way to get two, and the one the
    // user typed comes second.
    auto dir = Parse({ L"wsl", L"--cd", L"/srv", L"--cd", L"/tmp" });
    EXPECT_TRUE(dir.TakesOver());
    EXPECT_EQ(dir.Directory(), L"/tmp");

    auto distro = Parse({ L"wsl", L"-d", L"NixOS", L"-d", L"Ubuntu" });
    EXPECT_TRUE(distro.TakesOver());
    EXPECT_EQ(distro.Distribution(), L"Ubuntu");

    auto user = Parse({ L"wsl", L"-u", L"root", L"--user", L"me" });
    EXPECT_TRUE(user.TakesOver());
    EXPECT_EQ(user.User(), L"me");
}

TEST(WslInvocationTest, TakesAWindowsDirectoryAsWhereThePaneStarts) {
    // wsl.exe accepts a Windows `--cd` and translates it under /mnt,
    // which is also what it does with the directory it inherits. So the
    // value is handed over as the pane's working directory and nothing
    // is written for it: a backslash or a space could not survive the
    // command line, and here it does not have to.
    auto inv = Parse({ L"wsl", L"--cd", L"C:\\Program Files" });
    EXPECT_TRUE(inv.TakesOver());
    EXPECT_EQ(inv.WorkingDirectory(), L"C:\\Program Files");
    EXPECT_TRUE(inv.Directory().empty());
    EXPECT_EQ(inv.ToCommandLine(), "wsl");

    // The shape pwsh produces: it expands `~` itself, so what reaches
    // the shim is a Windows path with the separators mixed.
    auto expanded = Parse({ L"wsl", L"--cd", L"C:\\Users\\me/" });
    EXPECT_TRUE(expanded.TakesOver());
    EXPECT_EQ(expanded.WorkingDirectory(), L"C:\\Users\\me/");
}

TEST(WslInvocationTest, KeepsBothFormsOfTheDirectorySoTheLinuxOneCanWin) {
    // wsl.exe keeps the two forms apart: a Linux `--cd` wins over a
    // Windows one whichever came first. Measured both ways, and this is
    // the line a wrapped `wsl --cd ~` arrives as, since pwsh turns the
    // user's `~` into a Windows path while the profile's stays literal.
    for (auto argv : { std::vector<wchar_t const*>{ L"wsl", L"--cd", L"~", L"--cd", L"C:\\src" },
                       std::vector<wchar_t const*>{ L"wsl", L"--cd", L"C:\\src", L"--cd", L"~" } }) {
        auto inv = Parse(argv);
        EXPECT_TRUE(inv.TakesOver());
        EXPECT_EQ(inv.Directory(), L"~");
        EXPECT_EQ(inv.WorkingDirectory(), L"C:\\src");
        // The host starts the pane where the command line says, and
        // only falls back to the working directory without one.
        EXPECT_EQ(inv.ToCommandLine(), "wsl --cd ~");
    }
}

TEST(WslInvocationTest, KeepsTheLastDirectoryOfTheSameForm) {
    // Within one form it is the last that counts, as it is for wsl.exe.
    auto linux = Parse({ L"wsl", L"--cd", L"~", L"--cd", L"/tmp" });
    EXPECT_EQ(linux.Directory(), L"/tmp");
    EXPECT_TRUE(linux.WorkingDirectory().empty());

    auto windows = Parse({ L"wsl", L"--cd", L"C:\\Windows", L"--cd", L"C:\\" });
    EXPECT_EQ(windows.WorkingDirectory(), L"C:\\");
    EXPECT_TRUE(windows.Directory().empty());

    // A leading `~` is the same form, so a later --cd replaces it.
    EXPECT_EQ(Parse({ L"wsl", L"~", L"--cd", L"/tmp" }).Directory(), L"/tmp");
}

TEST(WslInvocationTest, LeavesADirectoryOfNeitherFormToWslExe) {
    // Relative, and the forms that could reach the network or a device
    // namespace: the host would have to wait on them.
    EXPECT_FALSE(Parse({ L"wsl", L"--cd", L"C:relative" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"--cd", L"\\\\server\\share" }).TakesOver());
    EXPECT_FALSE(Parse({ L"wsl", L"--cd", L"\\\\wsl.localhost\\NixOS\\home" }).TakesOver());
}
TEST(WslInvocationTest, ReadsTheLineTheShimSent) {
    // The shim sends the arguments as the shell wrote them, so the
    // split is this side's job now. Quoting has to survive it: a
    // directory with a space in it is one wsl.exe would take.
    auto inv = Invocation::ParseLine(L"-d NixOS --cd /srv -u root");
    EXPECT_TRUE(inv.TakesOver());
    EXPECT_EQ(inv.Distribution(), L"NixOS");
    EXPECT_EQ(inv.Directory(), L"/srv");
    EXPECT_EQ(inv.User(), L"root");

    auto quoted = Invocation::ParseLine(L"--cd \"C:\\Program Files\"");
    EXPECT_TRUE(quoted.TakesOver());
    EXPECT_EQ(quoted.WorkingDirectory(), L"C:\\Program Files");
    EXPECT_EQ(quoted.ToCommandLine(), "wsl");
}

TEST(WslInvocationTest, AnEmptyLineIsABareWsl) {
    auto inv = Invocation::ParseLine(L"");
    EXPECT_TRUE(inv.TakesOver());
    EXPECT_TRUE(inv.Distribution().empty());
    EXPECT_TRUE(inv.Directory().empty());
    EXPECT_TRUE(inv.User().empty());
    EXPECT_EQ(inv.ToCommandLine(), "wsl");
}

TEST(WslInvocationTest, LeavesALineItDoesNotUnderstand) {
    // Same answers as the argv form, now that the line is where they
    // come from: this is the whole of what the shim used to decide.
    EXPECT_FALSE(Invocation::ParseLine(L"-l -v").TakesOver());
    EXPECT_FALSE(Invocation::ParseLine(L"echo hi").TakesOver());
    EXPECT_FALSE(Invocation::ParseLine(L"--system").TakesOver());
    EXPECT_FALSE(Invocation::ParseLine(L"--cd relative").TakesOver());
    EXPECT_FALSE(Invocation::ParseLine(L"-- htop").TakesOver());
}
