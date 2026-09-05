#include "shell/ShellLauncher.h"

#include <Windows.h>
#include <shellapi.h>

bool ShellLauncher::OpenPath(const std::wstring& path) const {
    if (path.empty()) {
        return false;
    }
    HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool ShellLauncher::ShowInExplorer(const std::wstring& path) const {
    if (path.empty()) {
        return false;
    }
    const std::wstring parameters = L"/select,\"" + path + L"\"";
    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool ShellLauncher::RunAsAdministrator(const std::wstring& path) const {
    if (path.empty()) {
        return false;
    }
    HINSTANCE result = ShellExecuteW(nullptr, L"runas", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}
