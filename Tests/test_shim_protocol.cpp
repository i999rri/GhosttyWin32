#include "pch.h"
#include "../Core/Wsl/ShimProtocol.h"

using namespace core::wsl;

TEST(ShimProtocolTest, OpenRoundTripsNonAsciiPathAndDistro) {
    auto line = EncodeOpen(42, L"C:\\Users\\日本語\\src", L"NixOS");
    auto req = ParseOpen(line);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->paneId, 42u);
    EXPECT_EQ(req->cwd, L"C:\\Users\\日本語\\src");
    EXPECT_EQ(req->distro, L"NixOS");
}

TEST(ShimProtocolTest, OpenWithoutDistroLeavesItEmpty) {
    auto req = ParseOpen(EncodeOpen(7, L"D:\\", L""));
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->paneId, 7u);
    EXPECT_EQ(req->cwd, L"D:\\");
    EXPECT_TRUE(req->distro.empty());
}

TEST(ShimProtocolTest, OpenRejectsMalformedLines) {
    EXPECT_FALSE(ParseOpen("open\t1\tC:\\\tNixOS").has_value());       // no newline
    EXPECT_FALSE(ParseOpen("open\t1\tC:\\\n").has_value());              // three fields
    EXPECT_FALSE(ParseOpen("open\t1\tC:\\\tNixOS\textra\n").has_value()); // five fields
    EXPECT_FALSE(ParseOpen("close\t1\tC:\\\tNixOS\n").has_value());      // wrong verb
    EXPECT_FALSE(ParseOpen("open\t0\tC:\\\tNixOS\n").has_value());       // sentinel id
    EXPECT_FALSE(ParseOpen("open\tabc\tC:\\\tNixOS\n").has_value());     // non-numeric id
    EXPECT_FALSE(ParseOpen("\n").has_value());
}

TEST(ShimProtocolTest, OpenAcceptsOrdinaryDistroNames) {
    for (auto name : { L"Ubuntu", L"Ubuntu-22.04", L"my_distro", L"NixOS", L"a" }) {
        auto req = ParseOpen(EncodeOpen(1, L"C:\\", name));
        ASSERT_TRUE(req.has_value()) << name;
        EXPECT_EQ(req->distro, name);
    }
}

TEST(ShimProtocolTest, OpenRejectsDistroNamesThatCouldCarryArguments) {
    // The host splits its command line on whitespace, so a space would
    // turn the rest into wsl.exe or in-distro arguments.
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"Ubuntu --exec calc")).has_value());
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"Ubuntu sh -c x")).has_value());
    // A leading dash would read as an option.
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"-e")).has_value());
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"--cd")).has_value());
    // Quotes, separators and non-ASCII are not forwarded either.
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"\"Ubuntu\"")).has_value());
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"Ubuntu;calc")).has_value());
    EXPECT_FALSE(ParseOpen(EncodeOpen(1, L"C:\\", L"Ubuntu\u3000x")).has_value());
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
