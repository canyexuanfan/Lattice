#pragma once

#include <string>

#include "config/ConfigStore.h"
#include "desktop/DesktopLayout.h"
#include "desktop/ManagedShortcutStore.h"

class DesktopSession {
public:
    bool Activate(ConfigStore& configStore, ManagedShortcutStore& managedStore, std::wstring& errorMessage);
    bool Deactivate(ConfigStore& configStore, ManagedShortcutStore& managedStore, std::wstring& errorMessage);

private:
    static std::wstring CategoryForItem(const AppConfig& config, const std::wstring& itemId);
};
