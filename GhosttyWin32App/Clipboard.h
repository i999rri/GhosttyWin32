#pragma once

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Win32 clipboard read/write. Handles clipboard locking and memory management.
// All text is UTF-16 internally (Windows clipboard format).
namespace Clipboard {

    // Read text from clipboard as UTF-16.
    inline std::wstring read(HWND hwnd) {
        if (!OpenClipboard(hwnd)) return {};
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (!hData) { CloseClipboard(); return {}; }
        wchar_t* wstr = static_cast<wchar_t*>(GlobalLock(hData));
        if (!wstr) { CloseClipboard(); return {}; }
        std::wstring result(wstr);
        GlobalUnlock(hData);
        CloseClipboard();
        return result;
    }

    // Write UTF-16 text to clipboard.
    inline bool write(HWND hwnd, const std::wstring& text) {
        if (text.empty()) return false;
        if (!OpenClipboard(hwnd)) return false;
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
        if (!hMem) { CloseClipboard(); return false; }
        wchar_t* dest = static_cast<wchar_t*>(GlobalLock(hMem));
        memcpy(dest, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
        CloseClipboard();
        return true;
    }

} // namespace Clipboard
