#pragma once

#include <Windows.h>
#include <ObjIdl.h>

#include <string>
#include <vector>

struct ShellItemReference {
    std::wstring path;
    std::vector<BYTE> desktopChildPidl;
};

enum class ShellRenameDisposition {
    Unchanged,
    Renamed,
};

struct DesktopShellRenameResult {
    ShellRenameDisposition disposition =
        ShellRenameDisposition::Unchanged;
    ShellItemReference item;
    std::wstring displayName;
    HRESULT postCommitError = S_OK;
};

struct ShellPathRenameResult {
    ShellRenameDisposition disposition =
        ShellRenameDisposition::Unchanged;
    std::wstring parsingName;
    std::wstring displayName;
    HRESULT postCommitError = S_OK;
};

HRESULT CanRenameShellPath(
    const std::wstring& parsingName,
    bool& canRename);

HRESULT RenameShellPath(
    HWND ownerWindow,
    const std::wstring& parsingName,
    const std::wstring& newDisplayName,
    ShellPathRenameResult& renamedItem);

HRESULT CreateDesktopShellItemReference(
    const std::wstring& parsingName,
    ShellItemReference& item);

HRESULT ResolveDesktopShellItemReferenceParsingName(
    const ShellItemReference& item,
    std::wstring& parsingName);

HRESULT CanRenameDesktopShellItem(
    const ShellItemReference& item,
    bool& canRename);

HRESULT RenameDesktopShellItem(
    HWND ownerWindow,
    const ShellItemReference& item,
    const std::wstring& newDisplayName,
    DesktopShellRenameResult& renamedItem);

HRESULT CreateDesktopShellSelectionObject(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items,
    REFIID interfaceId,
    void** object);
