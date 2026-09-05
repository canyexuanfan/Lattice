#pragma once

#include <string>

struct ShortcutInfo {
    std::wstring targetPath;
    std::wstring arguments;
    std::wstring workingDirectory;
};

class ShortcutResolver {
public:
    ShortcutInfo Resolve(const std::wstring& shortcutPath) const;
};
