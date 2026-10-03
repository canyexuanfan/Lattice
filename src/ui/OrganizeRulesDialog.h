#pragma once
#include <Windows.h>
#include "config/ConfigStore.h"

class OrganizeRulesDialog {
public:
    enum class Result { Cancelled, Saved, Preview };
    static Result Show(HINSTANCE instance, HWND owner, const ConfigStore& store);
};
