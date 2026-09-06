#pragma once

#include <Windows.h>

#include <memory>

#include "app/SingleInstance.h"
#include "config/ConfigStore.h"
#include "desktop/DesktopSession.h"
#include "desktop/ManagedShortcutStore.h"
#include "ui/MainWindow.h"

class App {
public:
    explicit App(HINSTANCE instance);

    bool Initialize(int showCommand);
    bool InitializeForIsolatedSmoke(int showCommand);
    int Run();
    const std::wstring& LastNormalExitError() const noexcept;

private:
    bool InitializeInternal(int showCommand, bool activateDesktopSession);

    HINSTANCE instance_;
    SingleInstance singleInstance_;
    ConfigStore configStore_;
    ManagedShortcutStore shortcutStore_;
    DesktopSession desktopSession_;
    std::unique_ptr<MainWindow> mainWindow_;
    bool interactiveDesktopSession_ = true;
    bool desktopItemsRestored_ = false;
    bool desktopSessionDeactivated_ = false;
    std::wstring normalExitFinalizationError_;
};
