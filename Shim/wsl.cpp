// The `wsl` a ConPTY shell finds first on its PATH inside GhosttyWin32
// (#217). A plain interactive launch is handed to the host, which opens
// WSL through the pty bridge in place of the shell's pane and answers
// with WSL's exit code when that session ends; the shell is blocked on
// this process meanwhile, as it would be on wsl.exe. Everything else
// runs the real wsl.exe from System32 with the original arguments.
//
// Built as a self-contained console program (static CRT, no sanitizer):
// it runs as a child of the user's shell, outside the package, where
// neither the VC runtime framework nor the ASan runtime is on hand.

#include "Wsl/ShimProtocol.h"
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>

namespace {

using namespace core::wsl;

struct InteractiveShell {
    std::wstring distro;   // empty = the default distribution
};

// Only `wsl` and `wsl -d NAME` / `wsl --distribution NAME` are taken
// over. Any other argument (a command to run, --cd, -l, --exec, ...)
// keeps wsl.exe's own behaviour, so scripts and one-off commands are
// untouched.
std::optional<InteractiveShell> Classify(int argc, wchar_t** argv) {
    InteractiveShell shell;
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg = argv[i];
        bool selectsDistro = arg == L"-d" || arg == L"--distribution";
        if (selectsDistro && i + 1 < argc && shell.distro.empty()) {
            shell.distro = argv[++i];
            continue;
        }
        return std::nullopt;
    }
    // The host refuses a name it cannot forward safely; asking would
    // only delay the real wsl.exe, which handles any name itself.
    if (!IsForwardableDistro(shell.distro)) return std::nullopt;
    return shell;
}

// A console, not merely a character device: NUL also reports
// FILE_TYPE_CHAR, and `wsl <NUL >NUL` is a script, not a person.
bool IsConsole(DWORD stdHandle) {
    HANDLE h = GetStdHandle(stdHandle);
    DWORD mode = 0;
    return h != nullptr && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
}

std::wstring Env(wchar_t const* name) {
    DWORD len = GetEnvironmentVariableW(name, nullptr, 0);
    if (len == 0) return {};
    std::wstring value(len, L'\0');
    DWORD got = GetEnvironmentVariableW(name, value.data(), len);
    value.resize(got);
    return value;
}

std::wstring CurrentDirectory() {
    DWORD len = GetCurrentDirectoryW(0, nullptr);
    if (len == 0) return {};
    std::wstring dir(len, L'\0');
    DWORD got = GetCurrentDirectoryW(len, dir.data());
    dir.resize(got);
    return dir;
}

// Open the host's pipe, or INVALID_HANDLE_VALUE. `hostPid` is the pid
// the pipe name carries; a server that is not that process is some
// other program holding the name, and is not talked to.
HANDLE ConnectToHost(std::wstring const& pipe, unsigned long hostPid) {
    // Identification only: without SECURITY_SQOS_PRESENT the server may
    // impersonate this process, which in an elevated shell would hand
    // an elevated token to whoever answers.
    constexpr DWORD kFlags = SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION;
    for (int attempt = 0; attempt < 5; ++attempt) {
        HANDLE h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, kFlags, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ULONG serverPid = 0;
            if (GetNamedPipeServerProcessId(h, &serverPid) && serverPid == hostPid) return h;
            CloseHandle(h);
            return INVALID_HANDLE_VALUE;
        }
        // Every instance busy: the host is answering another shim.
        if (GetLastError() != ERROR_PIPE_BUSY) return INVALID_HANDLE_VALUE;
        if (!WaitNamedPipeW(pipe.c_str(), 1000)) return INVALID_HANDLE_VALUE;
    }
    return INVALID_HANDLE_VALUE;
}

// The exit code when the host took the request but gave no answer (it
// exited, or the session was dropped with its tab). The session may
// have run, so starting a second WSL here would be wrong; nonzero so
// the shell does not take it for success.
constexpr uint32_t kNoAnswerExitCode = 1;
// A reply is one short line.
constexpr size_t kMaxReplyBytes = 64;

// WSL's exit code once the host has taken the request, or nullopt when
// there is no host to ask or it declined; only then is the real wsl.exe
// run instead. Blocks for the whole session: the reply only comes when
// WSL ends.
std::optional<uint32_t> AskHost(InteractiveShell const& shell) {
    std::wstring pipe = Env(kPipeEnvVarW);
    std::wstring pane = Env(kPaneEnvVarW);
    auto hostPid = PidFromPipeName(pipe);
    if (!hostPid || pane.empty()) return std::nullopt;

    uint64_t paneId = std::wcstoull(pane.c_str(), nullptr, 10);
    if (paneId == 0) return std::nullopt;

    HANDLE h = ConnectToHost(pipe, *hostPid);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    std::string request = EncodeOpen(paneId, CurrentDirectory(), shell.distro);
    DWORD written = 0;
    if (!WriteFile(h, request.data(), static_cast<DWORD>(request.size()), &written, nullptr)
        || written != request.size()) {
        CloseHandle(h);
        return std::nullopt;
    }

    std::string reply;
    char buf[kMaxReplyBytes];
    while (reply.find('\n') == std::string::npos && reply.size() < kMaxReplyBytes) {
        DWORD n = 0;
        if (!ReadFile(h, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        reply.append(buf, n);
    }
    CloseHandle(h);

    auto parsed = ParseReply(reply);
    if (!parsed) return kNoAnswerExitCode;
    if (!parsed->opened) return std::nullopt;
    return parsed->exitCode;
}

// The command line after the program token, quoting intact, so the
// real wsl.exe sees exactly what the shell wrote.
std::wstring ArgumentsAfterProgram() {
    std::wstring_view cmd = GetCommandLineW();
    size_t i = 0;
    if (!cmd.empty() && cmd[0] == L'"') {
        i = cmd.find(L'"', 1);
        i = (i == std::wstring_view::npos) ? cmd.size() : i + 1;
    } else {
        i = cmd.find_first_of(L" \t");
        if (i == std::wstring_view::npos) i = cmd.size();
    }
    while (i < cmd.size() && (cmd[i] == L' ' || cmd[i] == L'\t')) ++i;
    return std::wstring(cmd.substr(i));
}

// Ctrl+C and Ctrl+Break go to every process on the console, this one
// included. Left to the default handler the shim would exit while the
// session it started goes on: wsl.exe keeps running, or the host has
// already been asked to swap the pane. Either way the shell would take
// its prompt back mid-session. Handled here, the shim keeps waiting
// and the session decides. A handler function, unlike the NULL
// "ignore" form, is not inherited by wsl.exe.
BOOL WINAPI OutlastConsoleBreak(DWORD event) {
    return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
}

int RunRealWsl() {
    wchar_t system32[MAX_PATH];
    UINT len = GetSystemDirectoryW(system32, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return 1;

    std::wstring exe = std::wstring(system32, len) + L"\\wsl.exe";
    std::wstring commandLine = L"\"" + exe + L"\"";
    std::wstring args = ArgumentsAfterProgram();
    if (!args.empty()) commandLine += L" " + args;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Before anything that starts a session, so no break can separate
    // the shim from the session it is waiting on.
    SetConsoleCtrlHandler(OutlastConsoleBreak, TRUE);

    auto shell = Classify(argc, argv);
    // Redirected stdio means a script is driving wsl, not a person.
    if (shell && IsConsole(STD_INPUT_HANDLE) && IsConsole(STD_OUTPUT_HANDLE)) {
        if (auto exitCode = AskHost(*shell)) return static_cast<int>(*exitCode);
    }
    return RunRealWsl();
}
