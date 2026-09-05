#pragma once

#include <string>
#include <vector>

#include "config/ConfigStore.h"

constexpr wchar_t kUncategorizedCategoryId[] = L"uncategorized";

struct Category {
    std::wstring id;
    std::wstring name;
    std::wstring storageFolder;
    std::vector<std::wstring> itemIds;
    std::wstring color = L"#2D8CFF";
    std::wstring icon = L"folder";
    bool tileCollapsed = false;
    WindowConfig layout;
};

struct RegisteredItem {
    std::wstring id;
    std::wstring path;
    std::wstring displayName;
    std::wstring originalDesktopPath;
    int desktopX = 0;
    int desktopY = 0;
    bool hasDesktopPosition = false;
};

struct OrganizerConfig {
    AppSettings settings;
    WindowConfig window;
    std::wstring currentCategoryId = kUncategorizedCategoryId;
    std::wstring uncategorizedName = L"未分类";
    std::wstring uncategorizedStorageFolder = kUncategorizedCategoryId;
    std::vector<RegisteredItem> items;
    std::vector<std::wstring> uncategorizedItemIds;
    std::vector<Category> categories;
};
