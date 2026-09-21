#pragma once

#include <windows.h>
#include <winrt/base.h>
#include <optional>
#include <string>
#include <string_view>

namespace winrt::GhosttyWin32::implementation::wsl {

// Deadline-bound I/O on an overlapped pipe handle (#217). The shim's
// peer is whatever process opened the pipe, so the host never waits on
// it without a bound: a client that connects and then stalls would
// otherwise hold the server thread, or the UI thread when a reply is
// sent from there.

// A manual-reset event for one overlapped call at a time.
inline winrt::handle MakePipeIoEvent() noexcept {
    return winrt::handle{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
}

// Wait for the overlapped operation `ov` on `pipe`, until it completes,
// `stop` is signalled (when given) or `timeoutMs` elapses. On anything
// but completion the operation is cancelled, and the cancellation is
// waited for so `ov` may go out of scope. True with the byte count on
// success.
inline bool FinishPipeIo(HANDLE pipe, OVERLAPPED& ov, HANDLE stop, DWORD timeoutMs,
                         DWORD& bytes) noexcept {
    // `stop` goes first: WaitForMultipleObjects reports the lowest
    // signalled index, so a peer that completes I/O as fast as it can
    // must not be able to hide a stop request behind it.
    HANDLE waits[2];
    DWORD count = 0;
    if (stop) waits[count++] = stop;
    waits[count++] = ov.hEvent;
    const DWORD completed = WAIT_OBJECT_0 + count - 1;
    if (WaitForMultipleObjects(count, waits, FALSE, timeoutMs) != completed) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &bytes, FALSE) != FALSE;
}

// Read up to and including the first newline within `budgetMs`, or
// nothing: a line not complete by then, longer than `maxBytes`, or cut
// short by end-of-file or `stop` was not a shim talking.
inline std::optional<std::string> ReadPipeLine(HANDLE pipe, HANDLE stop, DWORD budgetMs,
                                               size_t maxBytes) {
    winrt::handle event = MakePipeIoEvent();
    if (!event) return std::nullopt;

    const ULONGLONG deadline = GetTickCount64() + budgetMs;
    std::string line;
    char buf[512];
    while (line.find('\n') == std::string::npos) {
        const ULONGLONG now = GetTickCount64();
        if (line.size() >= maxBytes || now >= deadline) return std::nullopt;

        OVERLAPPED ov{};
        ov.hEvent = event.get();
        if (!ReadFile(pipe, buf, sizeof(buf), nullptr, &ov)
            && GetLastError() != ERROR_IO_PENDING) {
            return std::nullopt;
        }
        DWORD bytes = 0;
        if (!FinishPipeIo(pipe, ov, stop, static_cast<DWORD>(deadline - now), bytes)
            || bytes == 0) {
            return std::nullopt;
        }
        line.append(buf, bytes);
    }
    return line;
}

// Write all of `data` within `timeoutMs`. A reply is one short line, so
// it fits the pipe buffer and completes at once unless the peer has
// stopped reading and filled it.
inline bool WritePipe(HANDLE pipe, std::string_view data, DWORD timeoutMs) noexcept {
    winrt::handle event = MakePipeIoEvent();
    if (!event) return false;

    OVERLAPPED ov{};
    ov.hEvent = event.get();
    if (!WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), nullptr, &ov)
        && GetLastError() != ERROR_IO_PENDING) {
        return false;
    }
    DWORD bytes = 0;
    return FinishPipeIo(pipe, ov, nullptr, timeoutMs, bytes) && bytes == data.size();
}

}  // namespace winrt::GhosttyWin32::implementation::wsl
