#include "pch.h"
#include "Wsl/ShimServer.h"
#include "Wsl/PipeIo.h"
#include "Win32/DebugTrace.h"
#include <sddl.h>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace winrt::GhosttyWin32::implementation::wsl {

namespace {

// A request line is short; anything past this is not a shim talking.
constexpr size_t kMaxRequestBytes = 16 * 1024;
// A shim writes its request right after connecting, so a client still
// silent after this long is stalling the thread on purpose.
constexpr DWORD kReadBudgetMs = 2000;
// Back-off before retrying a failed instance creation.
constexpr DWORD kRetryMs = 500;

// String form of a SID from one of this process's token classes, or
// empty on failure.
std::wstring TokenSidString(TOKEN_INFORMATION_CLASS cls) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    winrt::handle owner{ token };

    DWORD size = 0;
    GetTokenInformation(token, cls, nullptr, 0, &size);
    if (size == 0) return {};
    std::vector<BYTE> buf(size);
    if (!GetTokenInformation(token, cls, buf.data(), size, &size)) return {};

    // TOKEN_USER and TOKEN_MANDATORY_LABEL both start with a
    // SID_AND_ATTRIBUTES.
    PSID sid = reinterpret_cast<SID_AND_ATTRIBUTES*>(buf.data())->Sid;
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) return {};
    std::wstring out{ text };
    LocalFree(text);
    return out;
}

// Owned by this user, full access for SYSTEM and this user only, and a
// mandatory label at this process's integrity that denies both writing
// and reading up. The default DACL would let Everyone open the pipe for
// reading; a no-write-up label alone would still let a lower-integrity
// process open it for reading, and every such connection holds the
// serial server for its read budget. The explicit owner keeps an
// elevated host's pipe from being owned by the Administrators group.
PSECURITY_DESCRIPTOR BuildSecurityDescriptor() {
    const std::wstring user = TokenSidString(TokenUser);
    const std::wstring integrity = TokenSidString(TokenIntegrityLevel);
    if (user.empty() || integrity.empty()) return nullptr;

    const std::wstring sddl = L"O:" + user
        + L"D:P(A;;GA;;;SY)(A;;GA;;;" + user + L")"
        + L"S:(ML;;NWNR;;;" + integrity + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        return nullptr;
    }
    return sd;
}

}  // namespace

ShimServer::ShimServer(Microsoft::UI::Dispatching::DispatcherQueue ui, OnOpen onOpen)
    : m_pipeName(core::wsl::PipeNameFor(GetCurrentProcessId()))
    , m_ui(std::move(ui))
    , m_onOpen(std::move(onOpen))
{
}

ShimServer::~ShimServer()
{
    Stop();
}

bool ShimServer::Start()
{
    if (m_thread.joinable()) return true;

    m_security.reset(BuildSecurityDescriptor());
    if (!m_security) {
        DEBUG_TRACE(L"ShimServer: could not build the pipe's security descriptor\n");
        return false;
    }
    m_stop = winrt::handle{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    if (!m_stop) return false;

    winrt::handle first = CreateInstance(true);
    if (!first && GetLastError() == ERROR_ACCESS_DENIED && ShimReply::OpenCount() > 0) {
        // The name is taken, most likely by sessions a previous server
        // of this process handed out (wsl-bridge turned off and on while
        // WSL ran in place). Join those instances. If another process
        // holds the name after all, the shim's server-pid check turns
        // it away, so the worst case is that shims fall back.
        first = CreateInstance(false);
    }
    if (!first) {
        // ERROR_ACCESS_DENIED here means another process holds the name.
        DEBUG_TRACE(L"ShimServer: first instance failed err=%lu\n", GetLastError());
        return false;
    }
    m_thread = std::thread([this, first = std::move(first)]() mutable { Run(std::move(first)); });
    return true;
}

void ShimServer::Stop() noexcept
{
    if (!m_thread.joinable()) return;
    SetEvent(m_stop.get());
    m_thread.join();
}

winrt::handle ShimServer::CreateInstance(bool first) const noexcept
{
    SECURITY_ATTRIBUTES sa{ sizeof(sa), m_security.get(), FALSE };
    DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    // Fails if the name exists, so a process that created it before us
    // is noticed instead of sharing it.
    if (first) openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    HANDLE pipe = CreateNamedPipeW(
        m_pipeName.c_str(),
        openMode,
        // Only shims on this machine ever talk to the host; named
        // pipes accept SMB clients unless told otherwise.
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES,
        4096, 4096, 0, &sa);
    return winrt::handle{ pipe == INVALID_HANDLE_VALUE ? nullptr : pipe };
}

void ShimServer::Run(winrt::handle pending)
{
    winrt::handle connectEvent{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    if (!connectEvent) return;

    // Checked between clients as well as while waiting: a client that
    // is always ready would otherwise keep the loop busy past a stop.
    auto stopping = [this]() { return WaitForSingleObject(m_stop.get(), 0) == WAIT_OBJECT_0; };

    while (!stopping()) {
        if (!pending) {
            pending = CreateInstance(false);
            if (!pending) {
                DEBUG_TRACE(L"ShimServer: CreateNamedPipe failed err=%lu\n", GetLastError());
                if (WaitForSingleObject(m_stop.get(), kRetryMs) == WAIT_OBJECT_0) return;
                continue;
            }
        }

        OVERLAPPED ov{};
        ov.hEvent = connectEvent.get();
        const BOOL accepted = ConnectNamedPipe(pending.get(), &ov);
        const DWORD err = accepted ? ERROR_SUCCESS : GetLastError();
        bool connected = accepted || err == ERROR_PIPE_CONNECTED;
        if (err == ERROR_IO_PENDING) {
            HANDLE waits[2] = { m_stop.get(), ov.hEvent };
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
                DWORD ignored = 0;
                CancelIoEx(pending.get(), &ov);
                GetOverlappedResult(pending.get(), &ov, &ignored, TRUE);
                return;
            }
            DWORD ignored = 0;
            connected = GetOverlappedResult(pending.get(), &ov, &ignored, FALSE) != FALSE;
        }
        if (!connected) {
            pending.close();
            continue;
        }

        // Take the next instance before dealing with this client, so
        // the name stays ours even while this one is being answered.
        winrt::handle client = std::move(pending);
        pending = CreateInstance(false);
        if (stopping()) return;
        Serve(std::move(client));
    }
}

void ShimServer::Serve(winrt::handle client)
{
    auto line = ReadPipeLine(client.get(), m_stop.get(), kReadBudgetMs, kMaxRequestBytes);
    // The reply owns the pipe from here; a request that never reaches a
    // window is refused, never left hanging.
    auto reply = std::make_shared<ShimReply>(client.detach());
    auto request = line ? core::wsl::ParseOpen(*line) : std::nullopt;
    if (!request) {
        reply->Refuse();
        return;
    }

    DEBUG_TRACE(L"ShimServer: open pane=%llu distro=%s\n",
                static_cast<unsigned long long>(request->paneId),
                request->distro.empty() ? L"(default)" : request->distro.c_str());
    bool queued = m_ui.TryEnqueue(
        [onOpen = m_onOpen, req = std::move(*request), reply]() mutable {
            onOpen(std::move(req), reply);
        });
    if (!queued) reply->Refuse();
}

}  // namespace winrt::GhosttyWin32::implementation::wsl
