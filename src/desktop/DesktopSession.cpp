#include "desktop/DesktopSession.h"

#include <Windows.h>

#include <algorithm>
#include <vector>

#include "model/OrganizerModel.h"
#include "util/PathUtil.h"

namespace {

std::wstring FileNameFromPath(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

void AppendError(std::wstring& errors, const std::wstring& item, const std::wstring& detail) {
    if (!errors.empty()) {
        errors += L"\n";
    }
    errors += L"• " + item + L"：" + detail;
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void MergeDesktopPositions(AppConfig& config, const std::vector<DesktopPosition>& positions) {
    for (const DesktopPosition& position : positions) {
        auto existing = std::find_if(
            config.desktopLayout.begin(),
            config.desktopLayout.end(),
            [&](const DesktopPlacementConfig& placement) { return SamePath(placement.path, position.path); });
        if (existing == config.desktopLayout.end()) {
            config.desktopLayout.push_back(
                DesktopPlacementConfig{position.path, position.point.x, position.point.y});
        } else {
            existing->path = position.path;
            existing->x = position.point.x;
            existing->y = position.point.y;
        }
    }
}

const DesktopPlacementConfig* FindDesktopPosition(const AppConfig& config, const std::wstring& path) {
    const auto found = std::find_if(
        config.desktopLayout.begin(),
        config.desktopLayout.end(),
        [&](const DesktopPlacementConfig& placement) { return SamePath(placement.path, path); });
    return found == config.desktopLayout.end() ? nullptr : &*found;
}

std::vector<DesktopPosition> StoredDesktopPositions(const AppConfig& config) {
    std::vector<DesktopPosition> positions;
    positions.reserve(config.desktopLayout.size());
    for (const DesktopPlacementConfig& placement : config.desktopLayout) {
        positions.push_back(DesktopPosition{placement.path, POINT{placement.x, placement.y}});
    }
    return positions;
}

}  // namespace

std::wstring DesktopSession::CategoryForItem(const AppConfig& config, const std::wstring& itemId) {
    if (std::find(config.uncategorizedItemIds.begin(), config.uncategorizedItemIds.end(), itemId) !=
        config.uncategorizedItemIds.end()) {
        return kUncategorizedCategoryId;
    }
    for (const CategoryConfig& category : config.categories) {
        if (std::find(category.itemIds.begin(), category.itemIds.end(), itemId) != category.itemIds.end()) {
            return category.id;
        }
    }
    return {};
}

bool DesktopSession::Activate(
    ConfigStore& configStore,
    ManagedShortcutStore& managedStore,
    std::wstring& errorMessage) {
    errorMessage.clear();
    AppConfig config = configStore.LoadAppConfig();
    DesktopLayout desktopLayout;
    bool allSucceeded = true;

    std::vector<DesktopPosition> startupPositions;
    std::wstring snapshotError;
    if (!desktopLayout.CaptureAllPositions(startupPositions, snapshotError)) {
        errorMessage = snapshotError;
        return false;
    }
    MergeDesktopPositions(config, startupPositions);
    if (!configStore.SaveAppConfig(config)) {
        errorMessage = L"无法保存启动前的完整桌面布局快照，本次未移动任何项目。";
        return false;
    }

    for (size_t index = 0; index < config.items.size(); ++index) {
        ItemConfig& item = config.items[index];
        if (managedStore.IsManagedPath(item.path) &&
            GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES &&
            !item.originalDesktopPath.empty() &&
            managedStore.IsDesktopPath(item.originalDesktopPath) &&
            GetFileAttributesW(item.originalDesktopPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            // An older version may have returned the shortcut to the desktop
            // while an updater preserved the pre-update user configuration.
            // Recover from that transition in memory, then use the normal
            // transactional move path below without changing category order.
            item.path = item.originalDesktopPath;
        }
        if (managedStore.IsManagedPath(item.path)) {
            if (item.originalDesktopPath.empty()) {
                item.originalDesktopPath = JoinPath(managedStore.DesktopPath(), FileNameFromPath(item.path));
                configStore.SaveAppConfig(config);
            }
            std::wstring duplicateError;
            if (!managedStore.RemoveRedundantDesktopCopy(
                    item.path,
                    item.originalDesktopPath,
                    duplicateError)) {
                allSucceeded = false;
                AppendError(errorMessage, FileNameFromPath(item.originalDesktopPath), duplicateError);
            }
            continue;
        }
        if (!managedStore.IsDesktopPath(item.path) || GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        const std::wstring categoryId = CategoryForItem(config, item.id);
        if (categoryId.empty()) {
            continue;
        }

        // Ordinary files and folders are reference-only items. Their original
        // paths must remain untouched across startup; only shortcuts enter the
        // managed storage transaction below.
        if (!managedStore.RequiresManagedStorage(item.path)) {
            continue;
        }

        const DesktopPlacementConfig* placement = FindDesktopPosition(config, item.path);
        if (placement == nullptr) {
            POINT recoveredPoint{};
            std::wstring positionError;
            for (int attempt = 0; attempt < 6 && placement == nullptr; ++attempt) {
                if (attempt > 0) {
                    Sleep(50);
                }
                if (desktopLayout.CapturePosition(item.path, recoveredPoint, positionError)) {
                    MergeDesktopPositions(config, {DesktopPosition{item.path, recoveredPoint}});
                    placement = FindDesktopPosition(config, item.path);
                }
            }
            if (placement == nullptr) {
                allSucceeded = false;
                AppendError(
                    errorMessage,
                    FileNameFromPath(item.path),
                    positionError.empty()
                        ? L"Explorer 暂未返回该项目坐标，已安全保留在桌面。"
                        : positionError);
                continue;
            }
            if (!configStore.SaveAppConfig(config)) {
                allSucceeded = false;
                AppendError(errorMessage, FileNameFromPath(item.path), L"已读取当前桌面坐标，但无法保存快照，已保留在桌面。");
                continue;
            }
        }
        item.originalDesktopPath = item.path;
        item.desktopX = placement->x;
        item.desktopY = placement->y;
        item.hasDesktopPosition = true;

        std::wstring destination;
        std::wstring moveError;
        const std::wstring itemId = item.id;
        if (!managedStore.MoveIntoCategory(
                itemId,
                item.path,
                CategoryStorageFolder(config, categoryId),
                [&](const std::wstring& managedPath) {
                    const auto registered = std::find_if(config.items.begin(), config.items.end(), [&](const ItemConfig& value) {
                        return value.id == itemId;
                    });
                    if (registered == config.items.end()) {
                        return false;
                    }
                    registered->path = managedPath;
                    return configStore.SaveAppConfig(config);
                },
                destination,
                moveError)) {
            allSucceeded = false;
            AppendError(errorMessage, FileNameFromPath(item.originalDesktopPath), moveError);
        }
    }
    return allSucceeded;
}

bool DesktopSession::Deactivate(
    ConfigStore& configStore,
    ManagedShortcutStore& managedStore,
    std::wstring& errorMessage) {
    errorMessage.clear();
    AppConfig config = configStore.LoadAppConfig();
    std::vector<DesktopPosition> positions = StoredDesktopPositions(config);
    bool allSucceeded = true;

    for (size_t index = 0; index < config.items.size(); ++index) {
        ItemConfig& item = config.items[index];
        if (!managedStore.IsManagedPath(item.path)) {
            continue;
        }
        const std::wstring target = item.originalDesktopPath.empty()
            ? JoinPath(managedStore.DesktopPath(), FileNameFromPath(item.path))
            : item.originalDesktopPath;
        const std::wstring itemId = item.id;
        std::wstring destination;
        std::wstring moveError;
        if (!managedStore.MoveToOriginalDesktop(
                itemId,
                item.path,
                target,
                [&](const std::wstring& desktopPath) {
                    const auto registered = std::find_if(config.items.begin(), config.items.end(), [&](const ItemConfig& value) {
                        return value.id == itemId;
                    });
                    if (registered == config.items.end()) {
                        return false;
                    }
                    registered->path = desktopPath;
                    return configStore.SaveAppConfig(config);
                },
                destination,
                moveError)) {
            allSucceeded = false;
            AppendError(errorMessage, FileNameFromPath(item.path), moveError);
            continue;
        }
        if (item.hasDesktopPosition) {
            const auto existing = std::find_if(positions.begin(), positions.end(), [&](const DesktopPosition& value) {
                return SamePath(value.path, destination);
            });
            if (existing == positions.end()) {
                positions.push_back(DesktopPosition{destination, POINT{item.desktopX, item.desktopY}});
            }
        }
    }

    DesktopLayout desktopLayout;
    std::wstring restoreError;
    if (!desktopLayout.RestorePositions(positions, restoreError)) {
        allSucceeded = false;
        if (!errorMessage.empty()) {
            errorMessage += L"\n";
        }
        errorMessage += restoreError;
    }

    std::vector<DesktopPosition> finalPositions;
    std::wstring finalSnapshotError;
    if (desktopLayout.CaptureAllPositions(finalPositions, finalSnapshotError)) {
        MergeDesktopPositions(config, finalPositions);
        for (ItemConfig& item : config.items) {
            const std::wstring desktopPath = item.originalDesktopPath.empty() ? item.path : item.originalDesktopPath;
            const DesktopPlacementConfig* placement = FindDesktopPosition(config, desktopPath);
            if (placement != nullptr) {
                item.desktopX = placement->x;
                item.desktopY = placement->y;
                item.hasDesktopPosition = true;
            }
        }
        if (!configStore.SaveAppConfig(config)) {
            allSucceeded = false;
            AppendError(errorMessage, L"桌面布局", L"最终桌面布局已经恢复，但无法保存为下一次启动基线。");
        }
    } else {
        allSucceeded = false;
        AppendError(errorMessage, L"桌面布局", finalSnapshotError);
    }
    return allSucceeded;
}
