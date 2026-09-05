#pragma once

#include <Windows.h>

#include "config/ConfigStore.h"

class SettingsDialog {
public:
    static bool Show(HINSTANCE instance, HWND owner, AppSettings& settings);
};
