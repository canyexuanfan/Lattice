#include "app/StartupManager.h"

#include <Windows.h>

namespace {

std::wstring CurrentExecutablePath() {
    std::wstring result(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
        if (length == 0) {
            return {};
        }
        if (length < result.size() - 1) {
            result.resize(length);
            return result;
        }
        result.resize(result.size() * 2);
    }
}

}  // namespace

bool StartupManager::IsEnabled() const {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD byteCount = 0;
    LONG result = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &byteCount);
    if (result == ERROR_FILE_NOT_FOUND) {
        result = RegQueryValueExW(key, kLegacyValueName, nullptr, &type, nullptr, &byteCount);
    }
    if (result == ERROR_FILE_NOT_FOUND) {
        result = RegQueryValueExW(key, kOldestValueName, nullptr, &type, nullptr, &byteCount);
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && byteCount > sizeof(wchar_t);
}

bool StartupManager::SetEnabled(bool enabled) const {
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kRunKeyPath,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &key,
            &disposition) != ERROR_SUCCESS) {
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring path = CurrentExecutablePath();
        const std::wstring command = L"\"" + path + L"\"";
        result = RegSetValueExW(
            key,
            kValueName,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        if (result == ERROR_SUCCESS) {
            RegDeleteValueW(key, kLegacyValueName);
            RegDeleteValueW(key, kOldestValueName);
        }
    } else {
        result = RegDeleteValueW(key, kValueName);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
        const LONG legacyResult = RegDeleteValueW(key, kLegacyValueName);
        if (result == ERROR_SUCCESS && legacyResult != ERROR_SUCCESS && legacyResult != ERROR_FILE_NOT_FOUND) {
            result = legacyResult;
        }
        const LONG oldestResult = RegDeleteValueW(key, kOldestValueName);
        if (result == ERROR_SUCCESS && oldestResult != ERROR_SUCCESS && oldestResult != ERROR_FILE_NOT_FOUND) {
            result = oldestResult;
        }
    }

    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
