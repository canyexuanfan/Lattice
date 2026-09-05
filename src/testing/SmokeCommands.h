#pragma once

#include <Windows.h>

#include <optional>

std::optional<int> RunSmokeOrPreviewCommand(
    HINSTANCE instance,
    PWSTR commandLine);
