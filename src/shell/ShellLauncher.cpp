#include "shell/ShellLauncher.h"

#include <Windows.h>
#include <shellapi.h>
#include <ShlObj.h>

bool ShellLauncher::OpenPath(const std::wstring& path) const {
    if (path.empty()) {
        return false;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl != nullptr) {
            SHELLEXECUTEINFOW execute{};
            execute.cbSize = sizeof(execute);
            execute.fMask = SEE_MASK_IDLIST;
            execute.lpIDList = pidl;
            execute.nShow = SW_SHOWNORMAL;
            const bool succeeded = ShellExecuteExW(&execute) != FALSE;
            CoTaskMemFree(pidl);
            return succeeded;
        }
    }
    HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool ShellLauncher::ShowInExplorer(const std::wstring& path) const {
    if (path.empty()) {
        return false;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return OpenPath(path);
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
