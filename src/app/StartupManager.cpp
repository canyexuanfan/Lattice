#include "app/StartupManager.h"

#include <Windows.h>
#include <shellapi.h>

#include <vector>

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
    LONG result = RegQueryValueExW(
        key, kValueName, nullptr, &type, nullptr, &byteCount);
    if (result != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) ||
        byteCount < sizeof(wchar_t)) {
        RegCloseKey(key);
        return false;
    }
    std::vector<wchar_t> buffer(
        static_cast<size_t>(byteCount / sizeof(wchar_t)) + 1, L'\0');
    result = RegQueryValueExW(
        key,
        kValueName,
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(buffer.data()),
        &byteCount);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    buffer.back() = L'\0';
    std::wstring command(buffer.data());
    if (type == REG_EXPAND_SZ) {
        const DWORD required = ExpandEnvironmentStringsW(
            command.c_str(), nullptr, 0);
        if (required == 0) {
            return false;
        }
        std::wstring expanded(required, L'\0');
        const DWORD copied = ExpandEnvironmentStringsW(
            command.c_str(), expanded.data(), required);
        if (copied == 0 || copied > required) {
            return false;
        }
        expanded.resize(copied - 1);
        command = std::move(expanded);
    }
    return CommandTargetsExecutable(command, CurrentExecutablePath());
}

bool StartupManager::CommandTargetsExecutable(
    const std::wstring& command,
    const std::wstring& executablePath) {
    if (command.empty() || executablePath.empty()) {
        return false;
    }
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(command.c_str(), &argumentCount);
    if (arguments == nullptr) {
        return false;
    }
    const bool matches =
        argumentCount == 1 && arguments[0] != nullptr &&
        CompareStringOrdinal(
            arguments[0], -1,
            executablePath.c_str(), -1,
            TRUE) == CSTR_EQUAL;
    LocalFree(arguments);
    return matches;
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
        if (path.empty()) {
            RegCloseKey(key);
            return false;
        }
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
