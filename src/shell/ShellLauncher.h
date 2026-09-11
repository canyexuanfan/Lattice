#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

#include "shell/ShellItemReference.h"

enum class ShellContextMenuResult {
    Failed,
    Cancelled,
    Invoked,
    RenameRequested,
    CustomCommand,
};

using ShellMenuAppender = std::function<void(HMENU)>;

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
    ShellContextMenuResult ShowDesktopContextMenu(
        HWND ownerWindow,
        const std::vector<ShellItemReference>& items,
        POINT screenPoint,
        bool allowRename) const;
    ShellContextMenuResult ShowContextMenuWithExtensions(
        HWND ownerWindow,
        const std::wstring& path,
        POINT screenPoint,
        bool allowRename,
        const ShellMenuAppender& appendCommands,
        UINT& customCommand,
        std::wstring& invokedVerb) const;
    ShellContextMenuResult ShowDesktopContextMenuWithExtensions(
        HWND ownerWindow,
        const std::vector<ShellItemReference>& items,
        POINT screenPoint,
        bool allowRename,
        const ShellMenuAppender& appendCommands,
        UINT& customCommand,
        std::wstring& invokedVerb) const;
    bool InvokeContextMenuVerb(
        HWND ownerWindow,
        const std::wstring& path,
        const std::wstring& canonicalVerb) const;
    bool InvokeDesktopContextMenuVerb(
        HWND ownerWindow,
        const std::vector<ShellItemReference>& items,
        const std::wstring& canonicalVerb) const;
    bool ForwardContextMenuMessage(
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        LRESULT& result) const;
};
