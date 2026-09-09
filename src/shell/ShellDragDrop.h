#pragma once

#include <Windows.h>
#include <ObjIdl.h>
#include <ShObjIdl.h>

#include <string>
#include <vector>

#include "shell/ShellItemReference.h"

HRESULT CreateFileDropDataObject(
    const std::vector<std::wstring>& paths,
    IDataObject** dataObject);
HRESULT CreateShellDragDataObject(
    HWND ownerWindow,
    const std::vector<std::wstring>& paths,
    IDataObject** dataObject);
HRESULT CreateShellDragDataObject(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items,
    IDataObject** dataObject);
HRESULT LastShellDragImageInitializationResultForTesting();
bool StartShellDrag(const std::wstring& path);
bool StartShellDrag(HWND ownerWindow, const std::wstring& path);
bool StartShellDrag(const std::vector<std::wstring>& paths);
bool StartShellDrag(
    HWND ownerWindow,
    const std::vector<std::wstring>& paths);
bool StartShellDrag(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items);
bool StartShellDrag(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items,
    const SHDRAGIMAGE* dragImage);
