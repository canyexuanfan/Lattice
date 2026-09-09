#pragma once

#include <Windows.h>

#include <string>
#include <vector>

#include "shell/ShellItemReference.h"

class ShellLauncher {
public:
    bool OpenPath(const std::wstring& path) const;
    bool ShowInExplorer(const std::wstring& path) const;
    bool RunAsAdministrator(const std::wstring& path) const;
    bool ShowContextMenu(
        HWND ownerWindow,
        const std::wstring& path,
        POINT screenPoint) const;
    bool ShowContextMenu(
        HWND ownerWindow,
        const std::vector<ShellItemReference>& items,
        POINT screenPoint) const;
    bool ForwardContextMenuMessage(
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        LRESULT& result) const;
};
