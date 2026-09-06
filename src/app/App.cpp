#include "app/App.h"

#include "desktop/CategoryStorageManager.h"
#include "ui/MessageDialog.h"

App::App(HINSTANCE instance) : instance_(instance) {}

bool App::Initialize(int showCommand) {
    return InitializeInternal(showCommand, true);
}

bool App::InitializeForIsolatedSmoke(int showCommand) {
    return InitializeInternal(showCommand, false);
}

bool App::InitializeInternal(int showCommand, bool activateDesktopSession) {
    interactiveDesktopSession_ = activateDesktopSession;
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
    }

    CategoryStorageManager storageManager(configStore_, shortcutStore_);
    std::wstring storageError;
    if (!storageManager.SynchronizeAll(storageError) && !storageError.empty()) {
        MessageDialog::Show(instance_,
            nullptr,
            (L"部分格子文件夹没有同步为格子名称，原文件夹和文件均已保留：\n\n" + storageError).c_str(),
            L"Lattice 格子文件夹同步",
            MB_OK | MB_ICONWARNING);
    }

    if (activateDesktopSession) {
        std::wstring activationError;
        if (!desktopSession_.Activate(configStore_, shortcutStore_, activationError) && !activationError.empty()) {
            MessageDialog::Show(instance_,
                nullptr,
                (L"为保护原桌面布局，以下项目没有被移入格子：\n\n" + activationError).c_str(),
                L"Lattice 桌面布局保护",
                MB_OK | MB_ICONWARNING);
        }
    }

    mainWindow_ = std::make_unique<MainWindow>(
        instance_,
        [this](HWND ownerWindow, std::wstring& errorMessage) {
            if (desktopSessionDeactivated_) {
                errorMessage.clear();
                return true;
            }
            if (!desktopSession_.RestoreItems(configStore_, shortcutStore_, errorMessage, ownerWindow)) {
                return false;
            }
            desktopItemsRestored_ = true;
            return true;
        },
        activateDesktopSession);
    if (!mainWindow_->Create()) {
        return false;
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
    if (mainWindow_ != nullptr && mainWindow_->WasNormalExitCompleted() && desktopItemsRestored_) {
        normalExitFinalizationError_.clear();
        if (desktopSession_.RestoreLayout(configStore_, shortcutStore_, normalExitFinalizationError_)) {
            desktopSessionDeactivated_ = true;
        } else if (interactiveDesktopSession_) {
            MessageDialog::Show(
                instance_,
                nullptr,
                (L"Lattice 已安全归还桌面项目，但部分图标坐标没有完全恢复：\n\n" +
                 normalExitFinalizationError_).c_str(),
                L"Lattice 桌面布局恢复",
                MB_OK | MB_ICONWARNING);
        }
    }
    if (mainWindow_ != nullptr && mainWindow_->WasUpdateExitRequested()) {
        OutputDebugStringW(
            L"Lattice update exit preserved managed desktop items and user configuration in place.\n");
    } else if (!desktopSessionDeactivated_) {
        OutputDebugStringW(
            L"Lattice window ended without a completed normal desktop restore; managed items and the saved layout were left unchanged.\n");
    }
    return static_cast<int>(message.wParam);
}

const std::wstring& App::LastNormalExitError() const noexcept {
    static const std::wstring empty;
    if (!normalExitFinalizationError_.empty()) {
        return normalExitFinalizationError_;
    }
    return mainWindow_ == nullptr ? empty : mainWindow_->LastNormalExitError();
}
