#pragma once

#include <string>

#include "config/ConfigStore.h"
class CategoryStorageManager {
public:
    explicit CategoryStorageManager(ConfigStore& configStore);

    bool CanUseName(
        const std::wstring& categoryId,
        const std::wstring& displayName,
        std::wstring& errorMessage) const;
    bool Rename(
        const std::wstring& categoryId,
        const std::wstring& displayName,
        std::wstring& errorMessage);
    bool RemoveEmpty(const std::wstring& categoryId, std::wstring& errorMessage);
    bool SynchronizeAll(std::wstring& errorMessage);

private:
    bool ValidateFolderName(const std::wstring& displayName, std::wstring& errorMessage) const;
    bool NameIsAvailable(
        const AppConfig& config,
        const std::wstring& categoryId,
        const std::wstring& displayName,
        std::wstring& errorMessage) const;

    ConfigStore& configStore_;
};
