#pragma once

#include <functional>
#include <string>
#include <vector>

#include "desktop/DesktopItem.h"

class DesktopScanner {
public:
    std::vector<DesktopItem> Scan(bool includePublicDesktop = true) const;
    DesktopItem CreateItemFromPath(
        const std::wstring& path,
        bool resolveShortcut = true) const;
    bool TryCreateManagedItemId(
        const std::function<bool(const std::wstring&)>& isInUse,
        std::wstring& itemId) const;
    void MergeRegisteredItem(
        std::vector<DesktopItem>& items,
        const std::wstring& itemId,
        const std::wstring& path,
        const std::wstring& displayName = L"") const;

private:
    void ScanDirectory(const std::wstring& directory, std::vector<DesktopItem>& items) const;
};
