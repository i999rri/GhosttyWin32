#pragma once

// The contract between the `wsl` shim the host puts on PATH and the
// host's named-pipe server (#217): where the shim finds the host, and
// the two messages they exchange. Header-only and free of WinRT so the
// shim, a plain console program, and the host share this one copy.
//
// Wire format is one UTF-8 line per message, fields separated by tabs.
// Windows paths and distribution names cannot contain tabs or
// newlines, so no escaping is needed.
//
//   shim -> host   open\t<pane id>\t<cwd>\t<distro>\n
//   host -> shim   done\t<exit code>\n        the session ended
//                  refused\n                  the host will not open one

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
    std::wstring distro;   // empty = the default distribution
};

// Host -> shim.
struct Reply {
    bool     opened{ false };
    uint32_t exitCode{ 0 };
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

inline std::string EncodeOpen(uint64_t paneId, std::wstring_view cwd, std::wstring_view distro) {
    return "open\t" + std::to_string(paneId) + "\t" + ToUtf8(cwd) + "\t" + ToUtf8(distro) + "\n";
}

inline std::string EncodeDone(uint32_t exitCode) {
    return "done\t" + std::to_string(exitCode) + "\n";
}

inline std::string EncodeRefused() {
    return "refused\n";
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

    // open \t id \t cwd \t distro: exactly four fields.
    std::string_view fields[4];
    size_t count = 0;
    std::string_view rest = *line;
    while (true) {
        auto tab = rest.find('\t');
        if (count == 4) return std::nullopt;
        fields[count++] = rest.substr(0, tab);
        if (tab == std::string_view::npos) break;
        rest.remove_prefix(tab + 1);
    }
    if (count != 4 || fields[0] != "open") return std::nullopt;

    auto id = detail::Number<uint64_t>(fields[1]);
    if (!id || *id == 0) return std::nullopt;

    return OpenRequest{ *id, ToUtf16(fields[2]), ToUtf16(fields[3]) };
}

inline std::optional<Reply> ParseReply(std::string_view text) {
    auto line = detail::Line(text);
    if (!line) return std::nullopt;
    if (*line == "refused") return Reply{ false, 0 };

    constexpr std::string_view kDone = "done\t";
    if (!line->starts_with(kDone)) return std::nullopt;
    auto code = detail::Number<uint32_t>(line->substr(kDone.size()));
    if (!code) return std::nullopt;
    return Reply{ true, *code };
}

}  // namespace core::wsl
