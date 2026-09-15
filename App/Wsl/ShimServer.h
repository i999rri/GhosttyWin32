#pragma once

#include "Wsl/ShimProtocol.h"
#include "Wsl/ShimReply.h"
#include <winrt/Microsoft.UI.Dispatching.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace winrt::GhosttyWin32::implementation::wsl {

// Listens for `wsl` shims asking to open WSL in place of their pane
// (#217). One server per process, one named pipe; the pipe name is
// handed to every ConPTY surface through its environment.
//
// The pipe thread only accepts connections and reads the request
// line. Each request is forwarded to the UI thread with its reply
// object, because opening the session is a tree mutation the window
// must do there; the reply is answered later, when the session ends,
// from whichever window took it.
class ShimServer {
public:
    // Runs on the UI thread. Refuses the reply itself when it cannot
    // open a session.
    using OnOpen = std::function<void(core::wsl::OpenRequest, std::shared_ptr<ShimReply>)>;

    ShimServer(Microsoft::UI::Dispatching::DispatcherQueue ui, OnOpen onOpen);
    ~ShimServer();

    ShimServer(ShimServer const&) = delete;
    ShimServer& operator=(ShimServer const&) = delete;

    std::wstring const& PipeName() const noexcept { return m_pipeName; }

    void Start();
    // Unblocks the accept, joins the thread. Idempotent.
    void Stop() noexcept;

private:
    void Run();

    std::wstring m_pipeName;
    Microsoft::UI::Dispatching::DispatcherQueue m_ui{ nullptr };
    OnOpen m_onOpen;
    std::atomic<bool> m_stopping{ false };
    // True from Start until Run leaves its loop; Stop knocks on the
    // pipe until this drops.
    std::atomic<bool> m_running{ false };
    std::thread m_thread;
};

}  // namespace winrt::GhosttyWin32::implementation::wsl
