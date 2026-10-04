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

#include "Wsl/HelperProbe.h"
#include "Wsl/Invocation.h"
#include <windows.h>
#include <aclapi.h>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace {

using namespace core::wsl;

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

// Whether `sid` is the user this process runs as.
bool IsCurrentUser(PSID sid) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buf(size);
    bool ours = size != 0
        && GetTokenInformation(token, TokenUser, buf.data(), size, &size)
        && EqualSid(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, sid);
    CloseHandle(token);
    return ours;
}

// Whether the server on the other end is the host that put this pipe
// in the environment: the instance was created by the pid the name
// carries, and by this user. The pid alone is not enough, since pids
// are reused and any account may create a name in the pipe namespace.
// Once a host has exited, another user's process holding its old pid
// can take the name over, be handed the working directory, and report
// a session that never ran.
bool IsTheHost(HANDLE pipe, unsigned long hostPid) {
    ULONG serverPid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &serverPid) || serverPid != hostPid) return false;

    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(pipe, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                        &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS) {
        return false;
    }

    bool ours = IsCurrentUser(owner);
    LocalFree(sd);
    return ours;
}

// Open the host's pipe, or INVALID_HANDLE_VALUE. Anyone else answering
// the name is some other program holding it, and is not talked to.
HANDLE ConnectToHost(std::wstring const& pipe, unsigned long hostPid) {
    // Identification only: without SECURITY_SQOS_PRESENT the server may
    // impersonate this process, which in an elevated shell would hand
    // an elevated token to whoever answers.
    constexpr DWORD kFlags = SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION;
    for (int attempt = 0; attempt < 5; ++attempt) {
        HANDLE h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, kFlags, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            if (IsTheHost(h, hostPid)) return h;
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
std::optional<uint32_t> AskHost(Invocation const& invocation) {
    std::wstring pipe = Env(kPipeEnvVarW);
    std::wstring pane = Env(kPaneEnvVarW);
    auto hostPid = PidFromPipeName(pipe);
    if (!hostPid || pane.empty()) return std::nullopt;

    uint64_t paneId = std::wcstoull(pane.c_str(), nullptr, 10);
    if (paneId == 0) return std::nullopt;

    HANDLE h = ConnectToHost(pipe, *hostPid);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    // A Windows `--cd` is asked for as where the pane starts, since that
    // is what wsl.exe would have made of it; without one the pane starts
    // where this shell is, as a child of it would. A Linux `--cd` is
    // sent as well and the host prefers it, which is the precedence
    // wsl.exe gives the two forms.
    std::wstring directory = invocation.WorkingDirectory();
    if (directory.empty()) directory = CurrentDirectory();

    std::string request = EncodeOpen(paneId, directory, invocation.Distribution(),
                                     invocation.Directory(), invocation.User());
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

// The real one, by absolute path. This program is also called wsl.exe
// and comes first on PATH, so anything resolved by name would be itself.
std::wstring RealWslPath() {
    wchar_t system32[MAX_PATH];
    UINT len = GetSystemDirectoryW(system32, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::wstring(system32, len) + L"\\wsl.exe";
}

// Whether `distro` has the bridge's in-distro half on its PATH, asked of
// the distribution because nothing on this side can see it (#229). A
// session started without it swaps the pane, dies on the shell's "not
// found", and swaps back before the message can be read, so it is worth
// one wsl.exe here: this runs when a person types `wsl`, where the wait
// does not show.
bool HelperInstalled(std::wstring const& distro) {
    const std::wstring exe = RealWslPath();
    if (exe.empty()) return false;

    std::wstring commandLine = HelperProbeCommandLine(exe, distro);

    // No console and no inherited handles: the answer is the exit code,
    // and anything it printed would land in the user's shell.
    STARTUPINFOW si{ .cb = sizeof(STARTUPINFOW) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// Said once per `wsl`, on stderr, so it stays out of anything the user
// pipes. The distribution is named because the binary is installed per
// distribution, and having it in one is easy to mistake for having it.
void ReportMissingHelper(std::wstring const& distro) {
    std::wstring message = L"wsl: ";
    message += kHelperName;
    message += L" is not installed in ";
    message += distro.empty() ? L"the default distribution" : distro;
    message += L"; running wsl.exe instead. See docs/WSL.md.\n";

    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err == nullptr || err == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteConsoleW(err, message.c_str(), static_cast<DWORD>(message.size()), &written, nullptr);
}

int RunRealWsl() {
    const std::wstring exe = RealWslPath();
    if (exe.empty()) return 1;

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

    const Invocation invocation = Invocation::Parse(argc, argv);
    // Redirected stdio means a script is driving wsl, not a person.
    if (invocation.TakesOver() && IsConsole(STD_INPUT_HANDLE) && IsConsole(STD_OUTPUT_HANDLE)) {
        if (HelperInstalled(invocation.Distribution())) {
            if (auto exitCode = AskHost(invocation)) return static_cast<int>(*exitCode);
        } else {
            // Only someone who set wsl-bridge gets here: the shim is on
            // PATH while it is on. Having asked for the bridge and not
            // installed its half, they are owed the reason the pane
            // stays where it is.
            ReportMissingHelper(invocation.Distribution());
        }
    }
    return RunRealWsl();
}
