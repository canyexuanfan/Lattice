#pragma once

#include <string>

enum class DesktopItemKind {
    Shortcut,
    UrlShortcut,
    File,
    Folder
};

struct DesktopItem {
    std::wstring id;
    std::wstring displayName;
    std::wstring path;
    std::wstring targetPath;
    std::wstring arguments;
    std::wstring workingDirectory;
    DesktopItemKind kind = DesktopItemKind::File;
    bool missing = false;
};
