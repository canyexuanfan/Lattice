#include "desktop/LegacyStorageMigrator.h"

#include <algorithm>
#include <vector>

#include "desktop/DesktopLayout.h"
#include "util/PathUtil.h"

namespace {

std::wstring FileNameFromPath(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos
        ? path
        : path.substr(separator + 1);
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(
        left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void UpsertPosition(
    std::vector<DesktopPosition>& positions,
    const std::wstring& path,
    POINT point) {
    const auto existing = std::find_if(
        positions.begin(), positions.end(),
        [&](const DesktopPosition& value) {
            return SamePath(value.path, path);
        });
    if (existing == positions.end()) {
        positions.push_back(DesktopPosition{path, point});
    } else {
        existing->point = point;
    }
}

void ClearLegacyVisibility(ItemConfig& item) {
    item.desktopVisibilityMode = 0;
    item.desktopVisibilityOriginalFlags = 0;
    item.desktopVisibilityNewStartValue = -1;
    item.desktopVisibilityClassicValue = -1;
}

}  // namespace

bool LegacyStorageMigrator::IsRequired(
    const AppConfig& config,
    const ManagedShortcutStore& managedStore) {
    return std::any_of(
        config.items.begin(), config.items.end(),
        [&](const ItemConfig& item) {
            return managedStore.IsManagedPath(item.path) ||
                item.desktopVisibilityMode != 0;
        });
}

LegacyStorageMigrator::StartupAttempt
LegacyStorageMigrator::AttemptForStartup(
    ConfigStore& configStore,
    ManagedShortcutStore& managedStore,
    HWND ownerWindow) const {
    StartupAttempt result;
    result.required = IsRequired(
        configStore.LoadAppConfig(), managedStore);
    if (!result.required) {
        return result;
    }
    result.completed = Migrate(
        configStore, managedStore, ownerWindow, result.warning);
    return result;
}

bool LegacyStorageMigrator::Migrate(
    ConfigStore& configStore,
    ManagedShortcutStore& managedStore,
    HWND ownerWindow,
    std::wstring& errorMessage) const {
    errorMessage.clear();
    AppConfig config = configStore.LoadAppConfig();
    if (!IsRequired(config, managedStore)) {
        return true;
    }
    if (ownerWindow == nullptr || IsWindow(ownerWindow) == FALSE) {
        errorMessage = L"一次性历史迁移需要有效的 Lattice 窗口。";
        return false;
    }

    DesktopLayout layout;
    const bool isolatedDesktop =
        GetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_DESKTOP_DIR", nullptr, 0) != 0;
    DWORD flagsBefore = 0;
    std::vector<DesktopPosition> positionsBefore;
    if (!isolatedDesktop &&
        (!layout.CaptureViewFlags(flagsBefore, errorMessage) ||
         !layout.CaptureAllPositions(positionsBefore, errorMessage))) {
        return false;
    }

    const AppConfig originalConfig = config;
    std::vector<ManagedShortcutStore::OriginalDesktopMoveRequest> moves;
    for (const ItemConfig& item : config.items) {
        if (!managedStore.IsManagedPath(item.path)) {
            continue;
        }
        const std::wstring target = item.originalDesktopPath.empty()
            ? JoinPath(
                managedStore.DesktopPath(),
                FileNameFromPath(item.path))
            : item.originalDesktopPath;
        moves.push_back(
            ManagedShortcutStore::OriginalDesktopMoveRequest{
                item.id, item.path, target,
                !item.hasDesktopPosition});
    }

    std::vector<std::pair<std::wstring, std::wstring>> destinations;
    if (!managedStore.MoveToOriginalDesktopBatch(
            moves,
            [&](const std::vector<std::pair<std::wstring, std::wstring>>& moved) {
                AppConfig next = config;
                std::vector<DesktopPosition> desired = positionsBefore;
                std::vector<std::wstring> requiredPaths;
                for (const auto& [itemId, desktopPath] : moved) {
                    const auto nextItem = std::find_if(
                        next.items.begin(), next.items.end(),
                        [&](const ItemConfig& item) {
                            return item.id == itemId;
                        });
                    const auto oldItem = std::find_if(
                        originalConfig.items.begin(),
                        originalConfig.items.end(),
                        [&](const ItemConfig& item) {
                            return item.id == itemId;
                        });
                    if (nextItem == next.items.end() ||
                        oldItem == originalConfig.items.end()) {
                        return false;
                    }
                    nextItem->path = desktopPath;
                    nextItem->originalDesktopPath = desktopPath;
                    ClearLegacyVisibility(*nextItem);
                    if (oldItem->hasDesktopPosition) {
                        UpsertPosition(
                            desired,
                            desktopPath,
                            POINT{oldItem->desktopX, oldItem->desktopY});
                        requiredPaths.push_back(desktopPath);
                    }
                }
                std::wstring positionError;
                if (!isolatedDesktop && !layout.RestorePositions(
                        desired, requiredPaths, positionError)) {
                    errorMessage = positionError;
                    return false;
                }
                DWORD flagsAfter = 0;
                std::wstring flagsError;
                if (!isolatedDesktop &&
                    (!layout.CaptureViewFlags(flagsAfter, flagsError) ||
                     flagsAfter != flagsBefore)) {
                    errorMessage = flagsError.empty()
                        ? L"历史迁移改变了 Explorer 桌面视图设置。"
                        : flagsError;
                    return false;
                }
                if (!configStore.SaveAppConfig(next)) {
                    errorMessage =
                        L"历史项目已到达原桌面路径，但无法保存迁移结果。";
                    return false;
                }
                config = std::move(next);
                return true;
            },
            destinations,
            errorMessage,
            ownerWindow)) {
        return false;
    }

    std::vector<DesktopPosition> desired = positionsBefore;
    std::vector<std::wstring> requiredPaths;
    AppConfig visibleConfig = config;
    for (ItemConfig& item : visibleConfig.items) {
        if (item.desktopVisibilityMode == 0) {
            continue;
        }
        std::wstring visibilityError;
        if (!managedStore.RestoreDesktopVisibility(
                item, visibilityError)) {
            errorMessage = visibilityError;
            return false;
        }
        if (item.hasDesktopPosition) {
            UpsertPosition(
                desired,
                item.path,
                POINT{item.desktopX, item.desktopY});
            requiredPaths.push_back(item.path);
        }
        ClearLegacyVisibility(item);
    }
    if (!isolatedDesktop && !requiredPaths.empty() &&
        !layout.RestorePositions(
            desired, requiredPaths, errorMessage)) {
        return false;
    }
    DWORD flagsAfter = 0;
    if (!isolatedDesktop &&
        (!layout.CaptureViewFlags(flagsAfter, errorMessage) ||
         flagsAfter != flagsBefore)) {
        if (errorMessage.empty()) {
            errorMessage = L"历史迁移改变了 Explorer 桌面视图设置。";
        }
        return false;
    }
    if (!configStore.SaveAppConfig(visibleConfig)) {
        errorMessage =
            L"旧版桌面显示状态已恢复，但无法保存迁移结果。";
        return false;
    }
    return true;
}
