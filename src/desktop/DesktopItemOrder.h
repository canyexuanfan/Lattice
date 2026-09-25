#pragma once

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

#include "config/ConfigStore.h"
#include "desktop/DesktopItem.h"

inline std::wstring DesktopItemOrderLowercase(std::wstring value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return value;
}

inline std::wstring DesktopItemOrderExtension(const DesktopItem& item) {
    return DesktopItemOrderLowercase(
        std::filesystem::path(item.path).extension().wstring());
}

inline ULONGLONG DesktopItemOrderModifiedTime(const DesktopItem& item) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(
            item.path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    ULARGE_INTEGER value{};
    value.HighPart = data.ftLastWriteTime.dwHighDateTime;
    value.LowPart = data.ftLastWriteTime.dwLowDateTime;
    return value.QuadPart;
}

inline void SortDesktopItemsForWindow(
    std::vector<DesktopItem>& items,
    const WindowConfig& config) {
    const int effectiveSortMode =
        config.autoArrange && config.sortMode == 0
        ? 1
        : config.sortMode;
    if (effectiveSortMode == 0) {
        return;
    }
    std::stable_sort(
        items.begin(), items.end(),
        [effectiveSortMode](
            const DesktopItem& left,
            const DesktopItem& right) {
            if (effectiveSortMode == 2) {
                const std::wstring leftExtension =
                    DesktopItemOrderExtension(left);
                const std::wstring rightExtension =
                    DesktopItemOrderExtension(right);
                if (leftExtension != rightExtension) {
                    return leftExtension < rightExtension;
                }
            } else if (effectiveSortMode == 3) {
                const ULONGLONG leftTime =
                    DesktopItemOrderModifiedTime(left);
                const ULONGLONG rightTime =
                    DesktopItemOrderModifiedTime(right);
                if (leftTime != rightTime) {
                    return leftTime > rightTime;
                }
            }
            return DesktopItemOrderLowercase(left.displayName) <
                DesktopItemOrderLowercase(right.displayName);
        });
}
