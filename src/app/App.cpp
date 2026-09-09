#include "app/App.h"

#include <algorithm>

#include "rendering/IconCache.h"
#include "ui/MessageDialog.h"

App::App(HINSTANCE instance) : instance_(instance) {}

App::~App() {
    mainWindow_.reset();
    IconCache::ShutdownSharedLoader();
}

bool App::Initialize(int showCommand) {
    return InitializeInternal(showCommand, true);
}

bool App::InitializeForIsolatedSmoke(int showCommand) {
    return InitializeInternal(showCommand, false);
}

bool App::InitializeInternal(int showCommand, bool enableDesktopTakeover) {
    if (!singleInstance_.IsPrimary()) {
        return false;
    }

    std::wstring recoveryError;
    if (!shortcutStore_.RecoverPending(configStore_, recoveryError)) {
        MessageDialog::Show(instance_,
            nullptr,
            (L"检测到上次未完成的桌面项目移动，但自动恢复没有完成：\n\n" + recoveryError +
             L"\n\n为避免覆盖文件，本次未继续移动该项目。").c_str(),
            L"Lattice 桌面项目恢复",
            MB_OK | MB_ICONWARNING);
        return false;
    }
    mainWindow_ = std::make_unique<MainWindow>(
        instance_,
        [](HWND, std::wstring& errorMessage) {
            errorMessage.clear();
            return true;
        },
        false);
    if (!mainWindow_->Create()) {
        return false;
    }

    if (enableDesktopTakeover) {
        const LegacyStorageMigrator::StartupAttempt migration =
            legacyStorageMigrator_.AttemptForStartup(
                configStore_, shortcutStore_, mainWindow_->Window());
        if (migration.required) {
            mainWindow_->ReloadPersistedState();
        }
        const int effectiveShowCommand =
            mainWindow_->ShouldStartHidden() ? SW_HIDE : showCommand;
        if (effectiveShowCommand != SW_HIDE) {
            std::wstring takeoverError;
            if (!mainWindow_->EnableDesktopDisplayTakeover(
                    takeoverError)) {
                if (takeoverError.find(L"stage=bounded-timeout") !=
                    std::wstring::npos) {
                    OutputDebugStringW(
                        (L"Lattice cancelled desktop display takeover at the bounded startup deadline: " +
                         takeoverError + L"\n").c_str());
                    return false;
                }
                MessageDialog::Show(
                    instance_,
                    mainWindow_->Window(),
                    (L"无法安全接管 Explorer 桌面图标显示层：\n\n" +
                     takeoverError +
                     L"\n\nLattice 未启动格子，以避免桌面项目重复显示。").c_str(),
                    L"Lattice 桌面显示",
                    MB_OK | MB_ICONWARNING);
                return false;
            }
        }
        if (!migration.completed) {
            const std::wstring diagnostic =
                L"Lattice deferred a legacy ManagedShortcuts migration and continued desktop display takeover: " +
                migration.warning + L"\n";
            OutputDebugStringW(diagnostic.c_str());
        }

        mainWindow_->Show(effectiveShowCommand);
        if (!migration.completed) {
            mainWindow_->ShowNonBlockingNotice(
                L"Lattice 历史项目待处理",
                L"一个旧版受管项目与当前桌面项目冲突。两端均已保留，Lattice 已继续正常运行；新收纳不会移动桌面原件。");
        }
        return true;
    }

    mainWindow_->Show(mainWindow_->ShouldStartHidden() ? SW_HIDE : showCommand);
    return true;
}

int App::Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (!ConfigStore::DrainPendingWrites(5000) &&
        !configStore_.SaveAppConfig(configStore_.LoadAppConfig())) {
        MessageDialog::Show(instance_,
            nullptr,
            L"Lattice 无法在退出前保存最新的格子位置或图标顺序。现有配置文件已保留，请检查磁盘或目录权限后重试。",
            L"Lattice 配置保存",
            MB_OK | MB_ICONWARNING);
    }
    if (mainWindow_ != nullptr) {
        mainWindow_->FinishPendingDesktopPlacements();
    }
    if (mainWindow_ != nullptr && mainWindow_->WasUpdateExitRequested()) {
        OutputDebugStringW(
            L"Lattice update exit closed the desktop display surface and preserved original desktop paths.\n");
    }
    return static_cast<int>(message.wParam);
}

const std::wstring& App::LastNormalExitError() const noexcept {
    static const std::wstring empty;
    return mainWindow_ == nullptr ? empty : mainWindow_->LastNormalExitError();
}
