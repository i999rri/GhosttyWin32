#pragma once

#include <string>
#include <thread>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

namespace core::win32 {

// Opens `target` with the shell's "open" verb -- a URL in the default
// browser, a path in its handler -- on a thread of its own, and
// returns at once.
//
// ShellExecuteW blocks until the shell has found the handler, and for
// a packaged process that is a walk over the AppX associations that
// can take seconds (#247). It also pumps messages while it waits, so
// a caller that holds a lock when it calls, or is on the UI thread,
// is held or re-entered for that whole time. Neither is acceptable
// from a libghostty action callback, which arrives under ghostty's
// renderer lock.
//
// `owner` only parents the shell's own error dialogs. The thread is
// detached: nothing waits on the shell, and a process exit mid-open
// just ends it.
inline void OpenWithShell(HWND owner, std::wstring target)
{
    std::thread([owner, target = std::move(target)] {
        // ShellExecuteW wants COM up on its thread; it will do this
        // itself if not, but with a mode it chooses.
        HRESULT const com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(owner, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(com)) CoUninitialize();
    }).detach();
}

}  // namespace core::win32
