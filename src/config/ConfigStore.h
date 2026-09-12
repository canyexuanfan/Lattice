#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct WindowConfig {
    int x = 80;
    int y = 80;
    int width = 390;
    int height = 489;
    int opacity = 230;
    int normalHeight = 489;
    int iconSize = 48;
    int density = 0;
    int viewMode = 1;
    int contentViewMode = 0;
    int sortMode = 0;
    int tabSide = 0;
    int titleOpacity = 210;
    int dpi = 96;
    std::wstring monitorId;
    bool collapsed = false;
    bool locked = false;
    bool showBorder = true;
    bool autoArrange = false;
    bool fixedExpanded = false;
};

struct AppSettings {
    bool launchOnStartup = false;
    bool showPublicDesktopItems = true;
    bool restoreHiddenState = true;
    bool startHidden = false;
    bool lastVisible = true;
    bool singleClickOpen = false;
    int backupCount = 3;
    int iconCacheSize = 256;
    int theme = 0;
};

struct ItemConfig {
    std::wstring id;
    std::wstring path;
    std::wstring displayName;
    std::wstring originalDesktopPath;
    int desktopX = 0;
    int desktopY = 0;
    bool hasDesktopPosition = false;
    int desktopVisibilityMode = 0;
    int desktopVisibilityOriginalFlags = 0;
    int desktopVisibilityNewStartValue = -1;
    int desktopVisibilityClassicValue = -1;
};

struct DesktopPlacementConfig {
    std::wstring path;
    int x = 0;
    int y = 0;
};

struct CategoryConfig {
    std::wstring id;
    std::wstring name;
    std::wstring storageFolder;
    std::vector<std::wstring> itemIds;
    std::wstring color = L"#2D8CFF";
    std::wstring icon = L"folder";
    bool tileCollapsed = false;
    WindowConfig layout;
};

struct AutoOrganizeMembershipChange {
    std::wstring itemId;
    std::wstring identity;
    std::wstring beforeCategoryId;
    std::wstring afterCategoryId;
    int beforeIndex = -1;
    int afterIndex = -1;
    bool itemWasRegistered = true;
    ItemConfig registeredItem;
};

struct AutoOrganizeUndoRecord {
    std::wstring transactionId;
    std::uint64_t timestamp = 0;
    std::vector<AutoOrganizeMembershipChange> changes;
    std::vector<CategoryConfig> createdCategories;
};

struct AppConfig {
    AppSettings settings;
    WindowConfig window;
    std::wstring currentCategoryId;
    std::wstring uncategorizedName = L"未分类";
    std::wstring uncategorizedStorageFolder = L"uncategorized";
    std::vector<ItemConfig> items;
    std::vector<DesktopPlacementConfig> desktopLayout;
    std::vector<DesktopPlacementConfig> desktopDisplayLayout;
    std::vector<std::wstring> uncategorizedItemIds;
    std::vector<CategoryConfig> categories;
    std::vector<AutoOrganizeUndoRecord> autoOrganizeUndoHistory;
};

struct ConfiguredItemMembership {
    std::wstring categoryId;
    int index = -1;
    int matchCount = 0;

    bool IsAssigned() const noexcept { return matchCount > 0; }
    bool IsUnique() const noexcept { return matchCount <= 1; }
};

template <typename Category>
ConfiguredItemMembership FindConfiguredItemMembership(
    const std::vector<std::wstring>& uncategorizedItemIds,
    const std::vector<Category>& categories,
    const std::wstring& itemId) {
    ConfiguredItemMembership result;
    const auto inspect = [&](const std::wstring& categoryId,
                             const std::vector<std::wstring>& itemIds) {
        for (std::size_t index = 0; index < itemIds.size(); ++index) {
            if (itemIds[index] != itemId) continue;
            result.categoryId = categoryId;
            result.index = static_cast<int>(index);
            ++result.matchCount;
        }
    };
    inspect(L"uncategorized", uncategorizedItemIds);
    for (const Category& category : categories) {
        inspect(category.id, category.itemIds);
    }
    return result;
}

struct AutoOrganizeMoveRequest {
    ItemConfig item;
    std::wstring identity;
    std::wstring expectedSourceCategoryId;
    int expectedSourceIndex = -1;
    std::wstring targetCategoryId;
};

struct AutoOrganizeApplyRequest {
    std::wstring transactionId;
    std::vector<AutoOrganizeMoveRequest> moves;
    std::vector<CategoryConfig> newCategories;
};

struct AutoOrganizeTransactionResult {
    std::uint64_t token = 0;
    bool succeeded = false;
    bool conflict = false;
    int appliedChanges = 0;
    int preservedChanges = 0;
    std::wstring message;
};

std::wstring CategoryStorageFolder(const AppConfig& config, const std::wstring& categoryId);
inline ConfiguredItemMembership FindConfiguredItemMembership(
    const AppConfig& config,
    const std::wstring& itemId) {
    return FindConfiguredItemMembership(
        config.uncategorizedItemIds, config.categories, itemId);
}

class ConfigStore {
public:
    ConfigStore();

    WindowConfig Load() const;
    bool Save(const WindowConfig& config) const;
    AppConfig LoadAppConfig() const;
    bool SaveAppConfig(const AppConfig& config) const;
    bool SaveInteractionStateAsync(
        const std::wstring& categoryId,
        const WindowConfig& layout,
        const std::vector<std::wstring>& itemIds,
        bool updateItemOrder) const;
    bool SaveDesktopDisplayPositionsAsync(
        const std::vector<DesktopPlacementConfig>& positions) const;
    bool RemoveItemsAsync(
        const std::vector<std::wstring>& itemIds) const;
    bool SaveShellRenameAsync(
        const std::wstring& previousIdentity,
        const std::wstring& newIdentity,
        const std::wstring& newDisplayName) const;
    bool ApplyAutoOrganizeAsync(
        const AutoOrganizeApplyRequest& request,
        HWND notificationWindow,
        UINT notificationMessage,
        std::uint64_t token) const;
    bool UndoAutoOrganizeAsync(
        HWND notificationWindow,
        UINT notificationMessage,
        std::uint64_t token) const;
    static bool DrainPendingWrites(unsigned long timeoutMilliseconds);
    bool ExportAppConfig(const std::wstring& path) const;
    bool ImportAppConfig(const std::wstring& path) const;
    bool ExportCategoryConfig(const CategoryConfig& category, const std::wstring& path) const;
    bool ImportCategoryConfig(const std::wstring& path, CategoryConfig& category) const;
    bool SaveLayoutProfile(const WindowConfig& config) const;
    bool LoadLayoutProfile(WindowConfig& config) const;
    std::wstring ConfigPath() const { return configPath_; }

private:
    AppConfig LoadAppConfigFromDisk() const;
    bool SaveAppConfigToDisk(const AppConfig& config) const;
    bool FlushInteractionStateToDisk() const;
    std::wstring configDir_;
    std::wstring configPath_;
    std::wstring backupPath_;
    std::wstring tempPath_;
};
