#pragma once

#include <Windows.h>
#include <functional>
#include <string>

#include "config/ConfigStore.h"

class SettingsDialog {
public:
    static bool Show(HINSTANCE instance, HWND owner, AppSettings& settings, int* displayMode = nullptr,
                     std::function<bool(const AppSettings&,std::wstring&)> validateAccess = {});
};
