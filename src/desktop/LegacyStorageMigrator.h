#pragma once

#include <Windows.h>

#include <string>

#include "config/ConfigStore.h"
#include "desktop/ManagedShortcutStore.h"

class LegacyStorageMigrator {
public:
    struct StartupAttempt {
        bool required = false;
        bool completed = true;
        std::wstring warning;
    };

    static bool IsRequired(
        const AppConfig& config,
        const ManagedShortcutStore& managedStore);

    StartupAttempt AttemptForStartup(
        ConfigStore& configStore,
        ManagedShortcutStore& managedStore,
        HWND ownerWindow) const;

    bool Migrate(
        ConfigStore& configStore,
        ManagedShortcutStore& managedStore,
        HWND ownerWindow,
        std::wstring& errorMessage) const;
};
