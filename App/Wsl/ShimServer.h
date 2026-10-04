#pragma once

#include "Wsl/ShimProtocol.h"
#include "Wsl/ShimReply.h"
#include <winrt/Microsoft.UI.Dispatching.h>
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
//
// Any local process can try the pipe, so the server:
//   - owns the name from Start on and never lets it go while running:
//     the first instance refuses to exist if the name is already taken,
//     and the next instance is created before a connected one is
//     handled, so there is no window in which another process could
//     create the name and receive the shims;
//   - lets only this user (and SYSTEM) open it, at no lower integrity
//     than the host, so an unelevated process cannot drive an elevated
//     terminal;
//   - never waits on a client without a bound (see PipeIo.h).
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

    // Create the first pipe instance on the calling thread and start
    // accepting. False, with nothing running, when the name is already
    // taken or the pipe cannot be secured; the caller must then not
    // advertise the name to shells.
    bool Start();
    // Wake the thread and join it. Idempotent. Connections already
    // handed to a window keep their own handles.
    void Stop() noexcept;

private:
    void Run(winrt::handle pending);
    void Serve(winrt::handle client);
    winrt::handle CreateInstance(bool first) const noexcept;

    std::wstring m_pipeName;
    Microsoft::UI::Dispatching::DispatcherQueue m_ui{ nullptr };
    OnOpen m_onOpen;
    // Self-relative security descriptor for every instance; LocalFree.
    std::unique_ptr<void, decltype(&LocalFree)> m_security{ nullptr, &LocalFree };
    winrt::handle m_stop;
    std::thread m_thread;
};

}  // namespace winrt::GhosttyWin32::implementation::wsl
