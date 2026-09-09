#pragma once

#include <Windows.h>
#include <ObjIdl.h>

#include <string>
#include <vector>

struct ShellItemReference {
    std::wstring path;
    std::vector<BYTE> desktopChildPidl;
};

HRESULT CreateDesktopShellSelectionObject(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items,
    REFIID interfaceId,
    void** object);
