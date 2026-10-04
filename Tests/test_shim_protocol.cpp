#include "pch.h"
#include "../Core/Wsl/ShimProtocol.h"

using namespace core::wsl;

TEST(ShimProtocolTest, OpenRoundTripsEveryField) {
    auto line = EncodeOpen(42, L"C:\\Users\\日本語\\src", L"-d NixOS --cd ~");
    auto req = ParseOpen(line);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->paneId, 42u);
    EXPECT_EQ(req->cwd, L"C:\\Users\\日本語\\src");
    EXPECT_EQ(req->commandLine, L"-d NixOS --cd ~");
}

TEST(ShimProtocolTest, OpenCarriesABareWslAsAnEmptyLine) {
    auto req = ParseOpen(EncodeOpen(7, L"D:\\", L""));
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->paneId, 7u);
    EXPECT_EQ(req->cwd, L"D:\\");
    EXPECT_TRUE(req->commandLine.empty());
}

TEST(ShimProtocolTest, OpenKeepsTheCommandLineWhole) {
    // The command line is the last field, so anything inside it -- a
    // tab in a quoted argument included -- needs no escaping. Nothing
    // reads it here either: what it may ask for is Invocation's to say.
    auto req = ParseOpen(EncodeOpen(1, L"C:\\", L"--cd \"C:\\a\tb\" -u root"));
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->commandLine, L"--cd \"C:\\a\tb\" -u root");
}

TEST(ShimProtocolTest, OpenRejectsMalformedLines) {
    // Each of these is a well-formed line but for the one thing named.
    EXPECT_FALSE(ParseOpen("open\t1\tC:\\\t").has_value());        // no newline
    EXPECT_FALSE(ParseOpen("open\t1\tC:\\\n").has_value());        // three fields
    EXPECT_FALSE(ParseOpen("close\t1\tC:\\\t\n").has_value());     // wrong verb
    EXPECT_FALSE(ParseOpen("open\t0\tC:\\\t\n").has_value());      // sentinel id
    EXPECT_FALSE(ParseOpen("open\tabc\tC:\\\t\n").has_value());    // non-numeric id
    EXPECT_FALSE(ParseOpen("\n").has_value());
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
    auto req = ParseOpen("open\t3\tC:\\\t\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->paneId, 3u);
}

TEST(ShimProtocolTest, DoneRoundTripsExitCode) {
    auto reply = ParseReply(EncodeDone(130));
    ASSERT_TRUE(reply.has_value());
    EXPECT_TRUE(reply->opened);
    EXPECT_EQ(reply->exitCode, 130u);
    EXPECT_TRUE(reply->message.empty());
}

TEST(ShimProtocolTest, RefusedParsesAsNotOpened) {
    auto reply = ParseReply(EncodeRefused());
    ASSERT_TRUE(reply.has_value());
    EXPECT_FALSE(reply->opened);
    EXPECT_TRUE(reply->message.empty());
}

TEST(ShimProtocolTest, RefusedCarriesWhatTheShimShouldPrint) {
    // The host decides the wording, since it is the side that knows
    // why; the shim only has the console to put it on.
    auto reply = ParseReply(EncodeRefused(L"wsl: 日本語 と tabs\tsurvive"));
    ASSERT_TRUE(reply.has_value());
    EXPECT_FALSE(reply->opened);
    EXPECT_EQ(reply->message, L"wsl: 日本語 と tabs\tsurvive");

    // Nothing to say comes out as a plain refusal rather than an empty
    // field, so the shim has one shape to recognise.
    EXPECT_EQ(EncodeRefused(L""), EncodeRefused());
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
