#include "pch.h"
#include "../Core/Wsl/ShimProtocol.h"

using namespace core::wsl;

TEST(ShimProtocolTest, OpenRoundTripsEveryField) {
    auto line = EncodeOpen(42, L"C:\\Users\\日本語\\src", L"NixOS", L"~", L"root");
    auto req = ParseOpen(line);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->paneId, 42u);
    EXPECT_EQ(req->cwd, L"C:\\Users\\日本語\\src");
    EXPECT_EQ(req->distro, L"NixOS");
    EXPECT_EQ(req->directory, L"~");
    EXPECT_EQ(req->user, L"root");
}

TEST(ShimProtocolTest, OpenLeavesUnaskedFieldsEmpty) {
    auto req = ParseOpen(EncodeOpen(7, L"D:\\", L"", L"", L""));
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->paneId, 7u);
    EXPECT_EQ(req->cwd, L"D:\\");
    EXPECT_TRUE(req->distro.empty());
    EXPECT_TRUE(req->directory.empty());
    EXPECT_TRUE(req->user.empty());
}

TEST(ShimProtocolTest, OpenRejectsMalformedLines) {
    // Each of these is a well-formed line but for the one thing named.
    EXPECT_FALSE(ParseOpen("open\t1\tC:\\\tNixOS\t\t").has_value());        // no newline
    EXPECT_FALSE(ParseOpen("open\t1\tC:\\\n").has_value());                 // three fields
    EXPECT_FALSE(ParseOpen("open\t1\tC:\\\tNixOS\t\t\tx\n").has_value());   // seven fields
    EXPECT_FALSE(ParseOpen("close\t1\tC:\\\tNixOS\t\t\n").has_value());     // wrong verb
    EXPECT_FALSE(ParseOpen("open\t0\tC:\\\tNixOS\t\t\n").has_value());      // sentinel id
    EXPECT_FALSE(ParseOpen("open\tabc\tC:\\\tNixOS\t\t\n").has_value());    // non-numeric id
    EXPECT_FALSE(ParseOpen("\n").has_value());
}

TEST(ShimProtocolTest, OpenAcceptsOrdinaryDistroNames) {
    for (auto name : { L"Ubuntu", L"Ubuntu-22.04", L"my_distro", L"NixOS", L"a" }) {
        auto req = ParseOpen(EncodeOpen(1, L"C:\\", name, L"", L""));
        ASSERT_TRUE(req.has_value()) << name;
        EXPECT_EQ(req->distro, name);
    }
}

TEST(ShimProtocolTest, OpenRejectsDistroNamesThatCouldCarryArguments) {
    // The host splits its command line on whitespace, so a space would
    // turn the rest into wsl.exe or in-distro arguments.
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"Ubuntu --exec calc", L"", L"")).has_value());
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"Ubuntu sh -c x", L"", L"")).has_value());
    // A leading dash would read as an option.
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"-e", L"", L"")).has_value());
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"--cd", L"", L"")).has_value());
    // Quotes, separators and non-ASCII are not forwarded either.
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"\"Ubuntu\"", L"", L"")).has_value());
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"Ubuntu;calc", L"", L"")).has_value());
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"Ubuntu\u3000x", L"", L"")).has_value());
}

TEST(ShimProtocolTest, OpenAcceptsDirectoriesAndUsersWslExeWouldTake) {
    for (auto dir : { L"~", L"/", L"/home/me", L"/srv/app-1.0_x" }) {
        auto req = ParseOpen(EncodeOpen(1, L"C:\\", L"", dir, L""));
        ASSERT_TRUE(req.has_value()) << dir;
        EXPECT_EQ(req->directory, dir);
    }
    for (auto user : { L"root", L"_svc", L"me.you-1" }) {
        auto req = ParseOpen(EncodeOpen(1, L"C:\\", L"", L"", user));
        ASSERT_TRUE(req.has_value()) << user;
        EXPECT_EQ(req->user, user);
    }
}

TEST(ShimProtocolTest, OpenRejectsDirectoriesAndUsersThatCouldCarryArguments) {
    // Same reason as the distribution: the line is split on whitespace
    // and expanded by a shell, so neither quoting nor escaping saves it.
    // A Windows path is left out as well — the cwd field already carries
    // where the asking shell was.
    for (auto dir : { L"/srv/my app", L"/tmp;calc", L"/tmp/$(whoami)", L"relative",
                      L"C:\\Users", L"~/sub" }) {
        EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"", dir, L"")).has_value()) << dir;
    }
    // A Linux name does not start with a digit, and the rest is the same
    // charset the distribution gets.
    for (auto user : { L"1root", L"ro ot", L"root;calc", L"-u" }) {
        EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"", L"", user)).has_value()) << user;
    }
}

TEST(ShimProtocolTest, DriveAbsolutePathsOnly) {
    EXPECT_TRUE(IsDriveAbsolutePath(L"C:\\"));
    EXPECT_TRUE(IsDriveAbsolutePath(L"d:\\src\\ghostty"));
    EXPECT_TRUE(IsDriveAbsolutePath(L"C:/Users"));
    // Paths that could reach the network or a device namespace.
    EXPECT_FALSE(IsDriveAbsolutePath(L"\\\\server\\share"));
    EXPECT_FALSE(IsDriveAbsolutePath(L"\\\\wsl.localhost\\Ubuntu\\home"));
    EXPECT_FALSE(IsDriveAbsolutePath(L"\\\\?\\C:\\Users"));
    EXPECT_FALSE(IsDriveAbsolutePath(L"\\\\.\\pipe\\x"));
    // Relative and truncated forms.
    EXPECT_FALSE(IsDriveAbsolutePath(L"C:"));
    EXPECT_FALSE(IsDriveAbsolutePath(L"C:relative"));
    EXPECT_FALSE(IsDriveAbsolutePath(L"relative\\dir"));
    EXPECT_FALSE(IsDriveAbsolutePath(L""));
}

TEST(ShimProtocolTest, OpenAcceptsCrLf) {
    auto req = ParseOpen("open\t3\tC:\\\t\t\t\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->paneId, 3u);
}

TEST(ShimProtocolTest, DoneRoundTripsExitCode) {
    auto reply = ParseReply(EncodeDone(130));
    ASSERT_TRUE(reply.has_value());
    EXPECT_TRUE(reply->opened);
    EXPECT_EQ(reply->exitCode, 130u);
}

TEST(ShimProtocolTest, RefusedParsesAsNotOpened) {
    auto reply = ParseReply(EncodeRefused());
    ASSERT_TRUE(reply.has_value());
    EXPECT_FALSE(reply->opened);
}

TEST(ShimProtocolTest, ReplyRejectsMalformedLines) {
    EXPECT_FALSE(ParseReply("done\t1").has_value());     // no newline
    EXPECT_FALSE(ParseReply("done\n").has_value());      // no code
    EXPECT_FALSE(ParseReply("done\tx\n").has_value());   // non-numeric
    EXPECT_FALSE(ParseReply("ok\t0\n").has_value());     // unknown verb
}

TEST(ShimProtocolTest, PipeNameCarriesThePid) {
    EXPECT_EQ(PipeNameFor(4242), L"\\\\.\\pipe\\GhosttyWin32.4242");
    EXPECT_EQ(PidFromPipeName(PipeNameFor(4242)), 4242ul);
    EXPECT_EQ(PidFromPipeName(PipeNameFor(4294967295ul)), 4294967295ul);
}

TEST(ShimProtocolTest, PipeNameRejectsAnythingButALocalHostPipe) {
    // Remote and other-namespace forms: the shim must never open them.
    EXPECT_FALSE(PidFromPipeName(L"\\\\evil\\pipe\\GhosttyWin32.4242").has_value());
    EXPECT_FALSE(PidFromPipeName(L"\\\\?\\pipe\\GhosttyWin32.4242").has_value());
    EXPECT_FALSE(PidFromPipeName(L"\\\\.\\pipe\\Other.4242").has_value());
    // Malformed pid parts.
    EXPECT_FALSE(PidFromPipeName(L"\\\\.\\pipe\\GhosttyWin32.").has_value());
    EXPECT_FALSE(PidFromPipeName(L"\\\\.\\pipe\\GhosttyWin32.0").has_value());
    EXPECT_FALSE(PidFromPipeName(L"\\\\.\\pipe\\GhosttyWin32.12a").has_value());
    EXPECT_FALSE(PidFromPipeName(L"\\\\.\\pipe\\GhosttyWin32.12\\..\\x").has_value());
    EXPECT_FALSE(PidFromPipeName(L"\\\\.\\pipe\\GhosttyWin32.4294967296").has_value());
    EXPECT_FALSE(PidFromPipeName(L"").has_value());
}
