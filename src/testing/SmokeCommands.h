#pragma once

#include <Windows.h>

#include <optional>
#include <string>

#ifndef NDEBUG
bool SaveSmokeWindowScreen(HWND window, const std::wstring& outputPath);
#endif

std::optional<int> RunSmokeOrPreviewCommand(
    HINSTANCE instance,
    PWSTR commandLine);
