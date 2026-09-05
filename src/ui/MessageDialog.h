#pragma once

#include <Windows.h>

#include <string>

class MessageDialog {
public:
    static int Show(
        HINSTANCE instance,
        HWND owner,
        const std::wstring& message,
        const std::wstring& title,
        UINT type);
};
