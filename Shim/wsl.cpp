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
    return shell;
}

bool IsConsole(DWORD stdHandle) {
    HANDLE h = GetStdHandle(stdHandle);
    return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_CHAR;
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

HANDLE ConnectToHost(std::wstring const& pipe) {
    // The server accepts one connection at a time; a busy pipe means
    // another shim is being read right now, which takes a moment.
    for (int attempt = 0; attempt < 5; ++attempt) {
        HANDLE h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        if (GetLastError() != ERROR_PIPE_BUSY) return INVALID_HANDLE_VALUE;
        if (!WaitNamedPipeW(pipe.c_str(), 1000)) return INVALID_HANDLE_VALUE;
    }
    return INVALID_HANDLE_VALUE;
}

// WSL's exit code once the host has run the session, or nullopt when
// there is no host to ask or it declined. Blocks for the whole
// session: the reply only comes when WSL ends.
std::optional<uint32_t> AskHost(InteractiveShell const& shell) {
    std::wstring pipe = Env(kPipeEnvVarW);
    std::wstring pane = Env(kPaneEnvVarW);
    if (pipe.empty() || pane.empty()) return std::nullopt;

    uint64_t paneId = std::wcstoull(pane.c_str(), nullptr, 10);
    if (paneId == 0) return std::nullopt;

    HANDLE h = ConnectToHost(pipe);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    std::string request = EncodeOpen(paneId, CurrentDirectory(), shell.distro);
    DWORD written = 0;
    if (!WriteFile(h, request.data(), static_cast<DWORD>(request.size()), &written, nullptr)) {
        CloseHandle(h);
        return std::nullopt;
    }

    std::string reply;
    char buf[256];
    while (reply.find('\n') == std::string::npos) {
        DWORD n = 0;
        if (!ReadFile(h, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        reply.append(buf, n);
    }
    CloseHandle(h);

    auto parsed = ParseReply(reply);
    if (!parsed || !parsed->opened) return std::nullopt;
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
    auto shell = Classify(argc, argv);
    // Redirected stdio means a script is driving wsl, not a person.
    if (shell && IsConsole(STD_INPUT_HANDLE) && IsConsole(STD_OUTPUT_HANDLE)) {
        if (auto exitCode = AskHost(*shell)) return static_cast<int>(*exitCode);
    }
    return RunRealWsl();
}
