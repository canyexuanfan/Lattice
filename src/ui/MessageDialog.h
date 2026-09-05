#pragma once

#include <Windows.h>

#include <string>

class MessageDialog {
public:
    static RECT CalculatePlacement(
        const RECT& anchor,
        const RECT& workArea,
        int width,
        int height);
    static int Show(
        HINSTANCE instance,
        HWND owner,
        const std::wstring& message,
        const std::wstring& title,
        UINT type);
};
