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
HRESULT DropShellItemsOnDesktopItem(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& sourceItems,
    const ShellItemReference& targetItem,
    POINT screenPoint,
    DWORD keyState,
    DWORD allowedEffects,
    DWORD* performedEffect);
HRESULT DropShellDataObjectOnTarget(
    IDataObject* dataObject,
    IDropTarget* dropTarget,
    POINT screenPoint,
    DWORD keyState,
    DWORD allowedEffects,
    DWORD* performedEffect);
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
