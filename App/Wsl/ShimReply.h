#pragma once

#include "Wsl/PipeIo.h"
#include "Wsl/ShimProtocol.h"
#include <windows.h>
#include <cstdint>
#include <string>

namespace winrt::GhosttyWin32::implementation::wsl {

// The host's end of one shim connection (#217). The shim blocks on
// this pipe from the moment it asks for WSL until the host answers,
// and its exit code is whatever the answer says. Owning the handle
// here means a session that is dropped without an answer (tab closed
// mid-session, window torn down) closes the pipe, and the shim reads
// end-of-file instead of hanging on a shell that is already gone.
//
// Answers run on the UI thread as well as the server thread, and the
// peer is any process that opened the pipe, so an answer never waits
// for the peer to read it: the write is bounded, and the handle is
// closed without FlushFileBuffers (which waits for the peer) or
// DisconnectNamedPipe (which discards what the peer has not read yet).
// The client can still read the answer once the server end is closed.
class ShimReply {
public:
    explicit ShimReply(HANDLE pipe) noexcept : m_pipe(pipe) {}
    ~ShimReply() { Close(); }

    ShimReply(ShimReply const&) = delete;
    ShimReply& operator=(ShimReply const&) = delete;

    // The in-place session ended with WSL's exit code.
    void Done(uint32_t exitCode) noexcept {
        Send(core::wsl::EncodeDone(exitCode));
        Close();
    }

    // The host will not open a session; the shim runs the real wsl.exe.
    void Refuse() noexcept {
        Send(core::wsl::EncodeRefused());
        Close();
    }

private:
    // Long enough for a live shim, whose read is already posted, and
    // short enough that a peer that stopped reading costs one hiccup.
    static constexpr DWORD kWriteTimeoutMs = 1000;

    void Send(std::string const& line) noexcept {
        if (m_pipe == INVALID_HANDLE_VALUE) return;
        WritePipe(m_pipe, line, kWriteTimeoutMs);
    }

    void Close() noexcept {
        if (m_pipe == INVALID_HANDLE_VALUE) return;
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }

    HANDLE m_pipe;
};

}  // namespace winrt::GhosttyWin32::implementation::wsl
