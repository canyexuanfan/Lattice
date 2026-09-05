#include "app/SingleInstance.h"

#include <string>

SingleInstance::SingleInstance() {
    std::wstring mutexName = L"Global\\Lattice.LocalOnly.V0";
    std::wstring legacyMutexName = L"Global\\Luno.LocalOnly.V0";
    std::wstring oldestMutexName = L"Global\\DesktopOrganizer.LocalOnly.V0";
    wchar_t instanceSuffix[128]{};
    const DWORD suffixLength = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_INSTANCE_SUFFIX", instanceSuffix, static_cast<DWORD>(std::size(instanceSuffix)));
    if (suffixLength > 0 && suffixLength < std::size(instanceSuffix)) {
        mutexName += L".";
        mutexName.append(instanceSuffix, suffixLength);
        legacyMutexName += L".";
        legacyMutexName.append(instanceSuffix, suffixLength);
        oldestMutexName += L".";
        oldestMutexName.append(instanceSuffix, suffixLength);
    }
    mutex_ = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    isPrimary_ = mutex_ != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
    if (isPrimary_) {
        legacyMutex_ = OpenMutexW(SYNCHRONIZE, FALSE, legacyMutexName.c_str());
        if (legacyMutex_ == nullptr) {
            legacyMutex_ = OpenMutexW(SYNCHRONIZE, FALSE, oldestMutexName.c_str());
        }
        if (legacyMutex_ != nullptr) {
            isPrimary_ = false;
        }
    }
}

SingleInstance::~SingleInstance() {
    if (legacyMutex_ != nullptr) {
        CloseHandle(legacyMutex_);
        legacyMutex_ = nullptr;
    }
    if (mutex_ != nullptr) {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
}
