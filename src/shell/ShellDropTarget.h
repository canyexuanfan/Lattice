#pragma once

#include <Windows.h>
#include <ObjIdl.h>

#include <functional>
#include <string>
#include <vector>

enum class ShellDropPreviewEvent {
    Enter,
    Over,
    Leave,
};

using ShellDropHandler =
    std::function<bool(
        IDataObject*,
        const std::vector<std::wstring>&,
        POINT,
        DWORD,
        DWORD,
        DWORD*)>;
using ShellDropEnabledHandler = std::function<bool()>;
using ShellDropPreviewHandler = std::function<void(
    ShellDropPreviewEvent,
    const std::vector<std::wstring>&,
    POINT)>;

std::vector<std::wstring> ExtractShellDropPaths(IDataObject* dataObject);
DWORD PreferredShellDropPreviewEffect(DWORD allowedEffects) noexcept;

bool RegisterShellDropTarget(
    HWND window,
    ShellDropHandler dropHandler,
    ShellDropEnabledHandler enabledHandler,
    ShellDropPreviewHandler previewHandler = {});
void UnregisterShellDropTarget(HWND window);
