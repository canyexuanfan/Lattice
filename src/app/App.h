#pragma once

#include <Windows.h>

#include <memory>

#include "app/SingleInstance.h"
#include "config/ConfigStore.h"
#include "desktop/LegacyStorageMigrator.h"
#include "desktop/ManagedShortcutStore.h"
#include "ui/MainWindow.h"

class App {
public:
    explicit App(HINSTANCE instance);
    ~App();

    bool Initialize(int showCommand);
    bool InitializeForIsolatedSmoke(int showCommand);
    int Run();
    const std::wstring& LastNormalExitError() const noexcept;

private:
    bool InitializeInternal(int showCommand, bool enableDesktopTakeover);

    HINSTANCE instance_;
    SingleInstance singleInstance_;
    ConfigStore configStore_;
    ManagedShortcutStore shortcutStore_;
    LegacyStorageMigrator legacyStorageMigrator_;
    std::unique_ptr<MainWindow> mainWindow_;
};
