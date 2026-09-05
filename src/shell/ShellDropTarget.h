#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

enum class ShellDropPreviewEvent {
    Enter,
    Over,
    Leave,
};

using ShellDropHandler =
    std::function<bool(const std::vector<std::wstring>&, POINT)>;
using ShellDropEnabledHandler = std::function<bool()>;
using ShellDropPreviewHandler = std::function<void(
    ShellDropPreviewEvent,
    const std::vector<std::wstring>&,
    POINT)>;

bool RegisterShellDropTarget(
    HWND window,
    ShellDropHandler dropHandler,
    ShellDropEnabledHandler enabledHandler,
    ShellDropPreviewHandler previewHandler = {});
void UnregisterShellDropTarget(HWND window);
