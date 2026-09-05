#pragma once

#include <Windows.h>

#include <string>

inline std::wstring LastWin32ErrorMessage(DWORD error = GetLastError()) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);
    std::wstring result = length > 0 && message != nullptr ? message : L"Unknown Win32 error";
    if (message != nullptr) {
        LocalFree(message);
    }
    return result;
}
