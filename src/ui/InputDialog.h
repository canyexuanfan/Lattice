#pragma once

#include <Windows.h>

#include <optional>
#include <string>

class InputDialog {
public:
    static std::optional<std::wstring> Prompt(
        HINSTANCE instance,
        HWND owner,
        const std::wstring& title,
        const std::wstring& label,
        const std::wstring& initialValue);
};
