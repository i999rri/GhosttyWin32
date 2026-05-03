#pragma once

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// UTF-16 <-> UTF-8 conversion utilities.
namespace Encoding {

    inline std::string toUtf8(const wchar_t* wstr, int wlen) {
        if (!wstr || wlen == 0) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string result(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, result.data(), len, nullptr, nullptr);
        return result;
    }

    inline std::string toUtf8(const std::wstring& wstr) {
        return toUtf8(wstr.c_str(), static_cast<int>(wstr.size()));
    }

    inline std::wstring toUtf16(const char* utf8, int utf8Len = -1) {
        if (!utf8) return {};
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, utf8Len, nullptr, 0);
        if (wlen <= 0) return {};
        if (utf8Len == -1 && wlen > 0) {
            std::wstring result(wlen - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8, utf8Len, result.data(), wlen);
            return result;
        }
        std::wstring result(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8, utf8Len, result.data(), wlen);
        return result;
    }

} // namespace Encoding
