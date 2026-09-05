#pragma once

#include <string>

class StartupManager {
public:
    bool IsEnabled() const;
    bool SetEnabled(bool enabled) const;

private:
    static constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static constexpr wchar_t kValueName[] = L"Lattice";
    static constexpr wchar_t kLegacyValueName[] = L"Luno";
    static constexpr wchar_t kOldestValueName[] = L"DesktopOrganizer";
};
