#include "pch.h"
#include "Wsl/ShimServer.h"
#include "Win32/DebugTrace.h"

namespace winrt::GhosttyWin32::implementation::wsl {

namespace {

// A request line is short; anything past this is not a shim talking.
constexpr size_t kMaxRequestBytes = 16 * 1024;

// Read up to and including the first newline, or to end-of-file.
std::string ReadLine(HANDLE pipe) {
    std::string line;
    char buf[512];
    while (line.find('\n') == std::string::npos && line.size() < kMaxRequestBytes) {
        DWORD n = 0;
        if (!ReadFile(pipe, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        line.append(buf, n);
    }
    return line;
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

void ShimServer::Start()
{
    if (m_thread.joinable()) return;
    m_stopping.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread([this]() { Run(); });
}

void ShimServer::Stop() noexcept
{
    if (!m_thread.joinable()) return;
    m_stopping.store(true, std::memory_order_release);
    // ConnectNamedPipe blocks until a client shows up; be that client
    // so the accept returns and the loop sees the flag. A single knock
    // can land while the thread is between instances (one closed, the
    // next not yet created) and connect to nothing, so keep knocking
    // until the loop has actually exited.
    while (m_running.load(std::memory_order_acquire)) {
        HANDLE wake = CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
        Sleep(10);
    }
    m_thread.join();
}

void ShimServer::Run()
{
    struct RunningScope {
        std::atomic<bool>& flag;
        ~RunningScope() { flag.store(false, std::memory_order_release); }
    } running{ m_running };

    while (!m_stopping.load(std::memory_order_acquire)) {
        HANDLE pipe = CreateNamedPipeW(
            m_pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            DEBUG_TRACE(L"ShimServer: CreateNamedPipe failed err=%lu\n", GetLastError());
            Sleep(500);
            continue;
        }

        BOOL connected = ConnectNamedPipe(pipe, nullptr)
            || GetLastError() == ERROR_PIPE_CONNECTED;
        if (m_stopping.load(std::memory_order_acquire) || !connected) {
            CloseHandle(pipe);
            continue;
        }

        // The reply owns the pipe from here; a request that never
        // reaches a window is refused, never left hanging.
        auto reply = std::make_shared<ShimReply>(pipe);
        auto request = core::wsl::ParseOpen(ReadLine(pipe));
        if (!request) {
            reply->Refuse();
            continue;
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
}

}  // namespace winrt::GhosttyWin32::implementation::wsl
