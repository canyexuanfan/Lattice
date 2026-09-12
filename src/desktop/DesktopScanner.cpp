#include "desktop/DesktopScanner.h"

#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>

#include "desktop/ShortcutResolver.h"
#include "util/PathUtil.h"
#include "util/StringUtil.h"

namespace {

std::wstring ConfiguredDesktopPath() {
    const DWORD required = GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DESKTOP_DIR", nullptr, 0);
    if (required == 0) {
        return KnownFolderPath(FOLDERID_Desktop);
    }
    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DESKTOP_DIR", value.data(), required);
    if (copied == 0 || copied >= required) {
        return KnownFolderPath(FOLDERID_Desktop);
    }
    value.resize(copied);
    return value;
}

DesktopItemKind KindForEntry(const std::wstring& fullPath, DWORD attributes) {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return DesktopItemKind::Folder;
    }
    if (EndsWithInsensitive(fullPath, L".lnk")) {
        return DesktopItemKind::Shortcut;
    }
    if (EndsWithInsensitive(fullPath, L".url")) {
        return DesktopItemKind::UrlShortcut;
    }
    return DesktopItemKind::File;
}

std::wstring KindPrefix(DesktopItemKind kind) {
    switch (kind) {
        case DesktopItemKind::Shortcut:
            return L"shortcut";
        case DesktopItemKind::UrlShortcut:
            return L"url";
        case DesktopItemKind::Folder:
            return L"folder";
        case DesktopItemKind::File:
        default:
            return L"file";
    }
}

std::wstring DisplayNameForPath(const std::wstring& fullPath, DesktopItemKind kind) {
    const std::wstring fileName = FileNameFromPath(fullPath);
    if (kind == DesktopItemKind::Shortcut || kind == DesktopItemKind::UrlShortcut) {
        return StripExtension(fileName);
    }
    return fileName;
}

}  // namespace

std::vector<DesktopItem> DesktopScanner::Scan(bool includePublicDesktop) const {
    std::vector<DesktopItem> items;
    const std::wstring configuredDesktop = ConfiguredDesktopPath();
    ScanDirectory(configuredDesktop, items);
    if (includePublicDesktop && GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DESKTOP_DIR", nullptr, 0) == 0) {
        ScanDirectory(KnownFolderPath(FOLDERID_PublicDesktop), items);
    }

    std::sort(items.begin(), items.end(), [](const DesktopItem& left, const DesktopItem& right) {
        return ToLowerCopy(left.displayName) < ToLowerCopy(right.displayName);
    });
    return items;
}

DesktopItem DesktopScanner::CreateItemFromPath(
    const std::wstring& path,
    bool resolveShortcut) const {
    DesktopItem item;
    item.path = path;
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        Microsoft::WRL::ComPtr<IShellItem> shellItem;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&shellItem))) && shellItem != nullptr) {
            PWSTR displayName = nullptr;
            if (SUCCEEDED(shellItem->GetDisplayName(SIGDN_NORMALDISPLAY, &displayName)) && displayName != nullptr) {
                item.displayName = displayName;
                CoTaskMemFree(displayName);
            }
            SFGAOF shellAttributes = 0;
            shellItem->GetAttributes(SFGAO_FOLDER | SFGAO_LINK, &shellAttributes);
            item.kind = (shellAttributes & SFGAO_FOLDER) != 0 ? DesktopItemKind::Folder : DesktopItemKind::Shortcut;
            item.id = L"shell|" + ToLowerCopy(path);
            if (item.displayName.empty()) {
                item.displayName = path;
            }
            return item;
        }
        attributes = 0;
        item.missing = true;
    }

    item.kind = KindForEntry(path, attributes);
    item.displayName = DisplayNameForPath(path, item.kind);
    item.id = KindPrefix(item.kind) + L"|" + ToLowerCopy(path);

    if (resolveShortcut && item.kind == DesktopItemKind::Shortcut) {
        ShortcutResolver shortcutResolver;
        const ShortcutInfo info = shortcutResolver.Resolve(path);
        item.targetPath = info.targetPath;
        item.arguments = info.arguments;
        item.workingDirectory = info.workingDirectory;
    }
    return item;
}

bool DesktopScanner::TryCreateManagedItemId(
    const std::function<bool(const std::wstring&)>& isInUse,
    std::wstring& itemId) const {
    itemId.clear();
    if (!isInUse) {
        return false;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) {
            return false;
        }
        wchar_t guidText[40]{};
        const int length = StringFromGUID2(guid, guidText, ARRAYSIZE(guidText));
        if (length <= 3) {
            return false;
        }
        std::wstring candidate = L"item-";
        candidate.append(guidText + 1, guidText + length - 2);
        if (!isInUse(candidate)) {
            itemId = std::move(candidate);
            return true;
        }
    }
    return false;
}

void DesktopScanner::MergeRegisteredItem(
    std::vector<DesktopItem>& items,
    const std::wstring& itemId,
    const std::wstring& path,
    const std::wstring& displayName) const {
    if (itemId.empty() || path.empty()) {
        return;
    }
    const auto findById = [&](const std::wstring& id) {
        return std::find_if(
            items.begin(),
            items.end(),
            [&](const DesktopItem& item) {
                return item.id == id;
            });
    };
    auto existing = findById(itemId);
    if (existing != items.end() &&
        CompareStringOrdinal(
            existing->path.c_str(),
            -1,
            path.c_str(),
            -1,
            TRUE) != CSTR_EQUAL) {
        const std::wstring liveIdBase = existing->id + L"|live";
        std::wstring liveId = liveIdBase;
        size_t suffix = 2;
        while (findById(liveId) != items.end() || liveId == itemId) {
            liveId = liveIdBase + L"-" + std::to_wstring(suffix++);
        }
        existing->id = std::move(liveId);
        existing = items.end();
    }
    if (existing == items.end()) {
        existing = std::find_if(
            items.begin(),
            items.end(),
            [&](const DesktopItem& item) {
                return CompareStringOrdinal(
                           item.path.c_str(), -1,
                           path.c_str(), -1,
                           TRUE) == CSTR_EQUAL;
            });
        if (existing != items.end()) {
            existing->id = itemId;
        }
    }
    if (existing == items.end()) {
        DesktopItem registered = CreateItemFromPath(path);
        registered.id = itemId;
        items.push_back(std::move(registered));
        existing = std::prev(items.end());
    }
    if (!displayName.empty()) {
        existing->displayName = displayName;
    }
}

void DesktopScanner::ScanDirectory(const std::wstring& directory, std::vector<DesktopItem>& items) const {
    if (directory.empty()) {
        return;
    }

    WIN32_FIND_DATAW data{};
    const std::wstring pattern = JoinPath(directory, L"*");
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0) {
            continue;
        }

        const std::wstring fullPath = JoinPath(directory, name);
        DesktopItem item = CreateItemFromPath(fullPath);
        items.push_back(std::move(item));
    } while (FindNextFileW(find, &data));

    FindClose(find);
}
