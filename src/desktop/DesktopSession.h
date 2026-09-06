#pragma once

#include <string>
#include <vector>

#include "config/ConfigStore.h"
#include "desktop/DesktopLayout.h"
#include "desktop/ManagedShortcutStore.h"

class DesktopSession {
public:
    bool Activate(ConfigStore& configStore, ManagedShortcutStore& managedStore, std::wstring& errorMessage);
    bool RestoreItems(
        ConfigStore& configStore,
        ManagedShortcutStore& managedStore,
        std::wstring& errorMessage,
        HWND ownerWindow);
    bool RestoreLayout(
        ConfigStore& configStore,
        ManagedShortcutStore& managedStore,
        std::wstring& errorMessage);
    bool Deactivate(
        ConfigStore& configStore,
        ManagedShortcutStore& managedStore,
        std::wstring& errorMessage,
        HWND ownerWindow = nullptr);

private:
    static std::wstring CategoryForItem(const AppConfig& config, const std::wstring& itemId);
    std::vector<std::wstring> pendingRequiredPaths_;
    bool restorePrepared_ = false;
};
