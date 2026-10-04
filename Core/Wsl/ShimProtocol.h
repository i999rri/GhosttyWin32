#pragma once

// The contract between the `wsl` shim the host puts on PATH and the
// host's named-pipe server (#217): where the shim finds the host, and
// the two messages they exchange. Header-only and free of WinRT so the
// shim, a plain console program, and the host share this one copy.
//
// Wire format is one UTF-8 line per message, fields separated by tabs.
//
//   shim -> host   open\t<pane id>\t<cwd>\t<command line>\n
//   host -> shim   done\t<exit code>\n        the session ended
//                  refused\n                  no session; say nothing
//                  refused\t<text>\n          no session; print this first
//
// The command line is whatever the shell wrote after the program name,
// unread by the shim: deciding what a `wsl` line means belongs to the
// host, which has the option table and the tests for it. It is the last
// field, so a tab inside it needs no escaping, and a newline cannot be
// in it at all -- the line ends at the first one.

#include <windows.h>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace core::wsl {

// Set on every ConPTY surface the host spawns while wsl-bridge is on,
// so a shim started from that shell can find its way back to the pane.
inline constexpr char    kPipeEnvVar[]  = "GHOSTTY_WIN32_PIPE";
inline constexpr char    kPaneEnvVar[]  = "GHOSTTY_WIN32_PANE";
inline constexpr wchar_t kPipeEnvVarW[] = L"GHOSTTY_WIN32_PIPE";
inline constexpr wchar_t kPaneEnvVarW[] = L"GHOSTTY_WIN32_PANE";

inline constexpr std::wstring_view kPipePrefixW = L"\\\\.\\pipe\\GhosttyWin32.";

// One pipe per host process. The pid keeps two running hosts apart.
inline std::wstring PipeNameFor(unsigned long pid) {
    return std::wstring(kPipePrefixW) + std::to_wstring(pid);
}

// The host pid a name made by PipeNameFor carries, or nullopt for any
// other string. The shim reads the name from its environment, which
// anything in the shell's startup can set; accepting only this exact
// shape keeps it to a local pipe (a UNC name would make it authenticate
// to a remote host), and the pid lets it check who answers.
inline std::optional<unsigned long> PidFromPipeName(std::wstring_view name) {
    if (!name.starts_with(kPipePrefixW)) return std::nullopt;
    std::wstring_view digits = name.substr(kPipePrefixW.size());
    // A DWORD has at most 10 decimal digits.
    if (digits.empty() || digits.size() > 10) return std::nullopt;
    unsigned long long pid = 0;
    for (wchar_t c : digits) {
        if (c < L'0' || c > L'9') return std::nullopt;
        pid = pid * 10 + static_cast<unsigned long long>(c - L'0');
    }
    if (pid == 0 || pid > 0xFFFFFFFFull) return std::nullopt;
    return static_cast<unsigned long>(pid);
}

// Shim -> host: open WSL in place of pane `paneId`.
struct OpenRequest {
    uint64_t     paneId{ 0 };
    std::wstring cwd;
    // The arguments as typed, for the host to read. Empty is a bare
    // `wsl`, which is the common case.
    std::wstring commandLine;
};

// Host -> shim.
struct Reply {
    bool     opened{ false };
    uint32_t exitCode{ 0 };
    // What the shim prints before falling back to the real wsl.exe,
    // decided by the host so the shim carries no wording of its own.
    // Empty means say nothing.
    std::wstring message;
};

inline std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

inline std::wstring ToUtf16(std::string_view text) {
    if (text.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                  nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), len);
    return out;
}

// Whether `path` is a drive-letter absolute path (`C:\...` or `C:/...`).
// The host opens a requested directory on its UI thread; a UNC or
// device path could make that wait on the network. A mapped network
// drive also passes this check, so the host still asks the drive type.
inline bool IsDriveAbsolutePath(std::wstring_view path) noexcept {
    if (path.size() < 3) return false;
    wchar_t letter = path[0];
    bool isLetter = (letter >= L'a' && letter <= L'z') || (letter >= L'A' && letter <= L'Z');
    return isLetter && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

inline std::string EncodeOpen(uint64_t paneId,
                              std::wstring_view cwd,
                              std::wstring_view commandLine) {
    return "open\t" + std::to_string(paneId) + "\t" + ToUtf8(cwd) + "\t" +
           ToUtf8(commandLine) + "\n";
}

inline std::string EncodeDone(uint32_t exitCode) {
    return "done\t" + std::to_string(exitCode) + "\n";
}

inline std::string EncodeRefused() {
    return "refused\n";
}

// Refused, with a line for the shim to print first.
inline std::string EncodeRefused(std::wstring_view message) {
    if (message.empty()) return EncodeRefused();
    return "refused\t" + ToUtf8(message) + "\n";
}

namespace detail {

// The line without its terminator, or nullopt when there is none: a
// message that never got its newline was cut off, not sent.
inline std::optional<std::string_view> Line(std::string_view text) {
    auto end = text.find('\n');
    if (end == std::string_view::npos) return std::nullopt;
    std::string_view line = text.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
}

template <class T>
std::optional<T> Number(std::string_view field) {
    T value{};
    auto [end, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
    if (ec != std::errc{} || end != field.data() + field.size()) return std::nullopt;
    return value;
}

}  // namespace detail

inline std::optional<OpenRequest> ParseOpen(std::string_view text) {
    auto line = detail::Line(text);
    if (!line) return std::nullopt;

    // open \t id \t cwd \t command line. The first three are read out
    // one tab at a time and the fourth is the remainder, whatever it
    // holds -- a quoted argument with a tab in it is the shell's
    // business, not this format's.
    std::string_view rest = *line;
    std::string_view head[3];
    for (auto& field : head) {
        auto tab = rest.find('\t');
        if (tab == std::string_view::npos) return std::nullopt;
        field = rest.substr(0, tab);
        rest.remove_prefix(tab + 1);
    }
    if (head[0] != "open") return std::nullopt;

    auto id = detail::Number<uint64_t>(head[1]);
    if (!id || *id == 0) return std::nullopt;

    // Nothing here reads the command line. What it is allowed to ask
    // for is Invocation's to say, on the host, where the rules live
    // with the tests that hold them.
    return OpenRequest{ *id, ToUtf16(head[2]), ToUtf16(rest) };
}

inline std::optional<Reply> ParseReply(std::string_view text) {
    auto line = detail::Line(text);
    if (!line) return std::nullopt;
    if (*line == "refused") return Reply{ false, 0, {} };

    constexpr std::string_view kRefused = "refused\t";
    if (line->starts_with(kRefused)) {
        return Reply{ false, 0, ToUtf16(line->substr(kRefused.size())) };
    }

    constexpr std::string_view kDone = "done\t";
    if (!line->starts_with(kDone)) return std::nullopt;
    auto code = detail::Number<uint32_t>(line->substr(kDone.size()));
    if (!code) return std::nullopt;
    return Reply{ true, *code, {} };
}

}  // namespace core::wsl
