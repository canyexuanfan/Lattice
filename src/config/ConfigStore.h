#pragma once

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

struct AppConfig {
    AppSettings settings;
    WindowConfig window;
    std::wstring currentCategoryId;
    std::wstring uncategorizedName = L"未分类";
    std::wstring uncategorizedStorageFolder = L"uncategorized";
    std::vector<ItemConfig> items;
    std::vector<DesktopPlacementConfig> desktopLayout;
    std::vector<std::wstring> uncategorizedItemIds;
    std::vector<CategoryConfig> categories;
};

std::wstring CategoryStorageFolder(const AppConfig& config, const std::wstring& categoryId);

class ConfigStore {
public:
    ConfigStore();

    WindowConfig Load() const;
    bool Save(const WindowConfig& config) const;
    AppConfig LoadAppConfig() const;
    bool SaveAppConfig(const AppConfig& config) const;
    bool ExportAppConfig(const std::wstring& path) const;
    bool ImportAppConfig(const std::wstring& path) const;
    bool ExportCategoryConfig(const CategoryConfig& category, const std::wstring& path) const;
    bool ImportCategoryConfig(const std::wstring& path, CategoryConfig& category) const;
    bool SaveLayoutProfile(const WindowConfig& config) const;
    bool LoadLayoutProfile(WindowConfig& config) const;
    std::wstring ConfigPath() const { return configPath_; }

private:
    std::wstring configDir_;
    std::wstring configPath_;
    std::wstring backupPath_;
    std::wstring tempPath_;
};
