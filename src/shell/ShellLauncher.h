#pragma once

#include <string>

class ShellLauncher {
public:
    bool OpenPath(const std::wstring& path) const;
    bool ShowInExplorer(const std::wstring& path) const;
    bool RunAsAdministrator(const std::wstring& path) const;
};
