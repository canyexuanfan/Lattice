#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "config/ConfigStore.h"

class ManagedShortcutStore {
public:
    struct OriginalDesktopMoveRequest {
        std::wstring itemId;
        std::wstring sourcePath;
        std::wstring destinationPath;
        bool notifyShell = true;
    };

    ManagedShortcutStore();
    ManagedShortcutStore(
        std::wstring dataDirectory,
        std::wstring desktopDirectory,
        std::wstring publicDesktopDirectory = {});

    bool IsSupportedDesktopItem(const std::wstring& path) const;
    bool IsSupportedShortcut(const std::wstring& path) const;
    bool RequiresManagedStorage(const std::wstring& path) const;
    bool IsManagedPath(const std::wstring& path) const;
    bool IsDesktopPath(const std::wstring& path) const;
    bool IsPublicDesktopPath(const std::wstring& path) const;
    bool IsShellNamespaceItem(const std::wstring& path) const;
#if defined(_DEBUG)
    bool IsDesktopPositionSuppressed(const ItemConfig& item) const noexcept;
    bool CaptureAndSuppressDesktopVisibility(
        const std::wstring& path,
        ItemConfig& item,
        std::wstring& errorMessage) const;
    bool SuppressDesktopVisibility(ItemConfig& item, std::wstring& errorMessage) const;
    bool PrepareForManagedStorage(
        ItemConfig& item,
        std::wstring& errorMessage) const;
#endif
    bool RestoreDesktopVisibility(
        const ItemConfig& item,
        std::wstring& errorMessage,
        const POINT* releaseScreenPoint = nullptr) const;

#if defined(_DEBUG)
    bool MoveIntoCategory(
        const std::wstring& itemId,
        const std::wstring& sourcePath,
        const std::wstring& categoryId,
        const std::function<bool(const std::wstring&)>& persistDestination,
        std::wstring& destinationPath,
        std::wstring& errorMessage,
        HWND ownerWindow = nullptr);

    bool MoveToDesktop(
        const std::wstring& itemId,
        const std::wstring& sourcePath,
        const std::function<bool(const std::wstring&)>& persistDestination,
        std::wstring& destinationPath,
        std::wstring& errorMessage,
        HWND ownerWindow = nullptr);
#endif

    bool MoveToOriginalDesktop(
        const std::wstring& itemId,
        const std::wstring& sourcePath,
        const std::wstring& originalDesktopPath,
        const std::function<bool(const std::wstring&)>& persistDestination,
        std::wstring& destinationPath,
        std::wstring& errorMessage,
        HWND ownerWindow = nullptr,
        bool notifyShell = true);

    bool MoveToOriginalDesktopBatch(
        const std::vector<OriginalDesktopMoveRequest>& requests,
        const std::function<bool(const std::vector<std::pair<std::wstring, std::wstring>>&)>& persistDestinations,
        std::vector<std::pair<std::wstring, std::wstring>>& destinations,
        std::wstring& errorMessage,
        HWND ownerWindow);

    bool RemoveRedundantDesktopCopy(
        const std::wstring& managedPath,
        const std::wstring& desktopPath,
        std::wstring& errorMessage) const;

    bool RecoverPending(ConfigStore& configStore, std::wstring& errorMessage);

    const std::wstring& RootPath() const noexcept { return rootPath_; }
    const std::wstring& DesktopPath() const noexcept { return desktopPath_; }
#if defined(_DEBUG)
    std::wstring CategoryPath(const std::wstring& categoryId) const;
    bool EnsureCategoryDirectory(const std::wstring& categoryId) const;
#endif

private:
    struct JournalEntry {
        std::wstring itemId;
        std::wstring sourcePath;
        std::wstring destinationPath;
        bool releaseToDesktop = false;
    };

    bool ExecuteMove(
        const std::wstring& itemId,
        const std::wstring& sourcePath,
        const std::wstring& destinationDirectory,
        bool releaseToDesktop,
        const std::function<bool(const std::wstring&)>& persistDestination,
        std::wstring& destinationPath,
        std::wstring& errorMessage,
        const std::wstring& exactDestinationPath = {},
        HWND ownerWindow = nullptr,
        bool notifyShell = true);
    bool WriteJournal(const JournalEntry& entry, std::wstring& errorMessage) const;
    bool WriteJournal(const std::vector<JournalEntry>& entries, std::wstring& errorMessage) const;
    bool ReadJournal(std::vector<JournalEntry>& entries, std::wstring& errorMessage) const;
    bool ClearJournal() const;

    std::wstring rootPath_;
    std::wstring desktopPath_;
    std::wstring publicDesktopPath_;
    bool publicDesktopRequiresElevation_ = true;
    std::wstring journalPath_;
    std::wstring journalTempPath_;
};
