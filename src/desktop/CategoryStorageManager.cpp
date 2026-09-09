#include "desktop/CategoryStorageManager.h"

#include <Windows.h>
#include <algorithm>

#include "model/OrganizerModel.h"

namespace {

bool SameText(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool IsUncategorized(const std::wstring& categoryId) {
    return SameText(categoryId, kUncategorizedCategoryId);
}

}  // namespace

CategoryStorageManager::CategoryStorageManager(ConfigStore& configStore)
    : configStore_(configStore) {}

bool CategoryStorageManager::ValidateFolderName(
    const std::wstring& displayName,
    std::wstring& errorMessage) const {
    errorMessage.clear();
    if (displayName.empty()) {
        errorMessage = L"格子名称不能为空。";
        return false;
    }
    if (displayName.size() > 120) {
        errorMessage = L"格子名称过长，请控制在 120 个字符以内。";
        return false;
    }
    for (const wchar_t character : displayName) {
        if (character < 32) {
            errorMessage = L"格子名称不能包含控制字符。";
            return false;
        }
    }
    return true;
}

bool CategoryStorageManager::NameIsAvailable(
    const AppConfig& config,
    const std::wstring& categoryId,
    const std::wstring& displayName,
    std::wstring& errorMessage) const {
    if (!IsUncategorized(categoryId) &&
        SameText(config.uncategorizedName, displayName)) {
        errorMessage = L"已有同名格子，请使用另一个名称。";
        return false;
    }
    for (const CategoryConfig& category : config.categories) {
        if (SameText(category.id, categoryId)) {
            continue;
        }
        if (SameText(category.name, displayName)) {
            errorMessage = L"已有同名格子，请使用另一个名称。";
            return false;
        }
    }
    return true;
}

bool CategoryStorageManager::CanUseName(
    const std::wstring& categoryId,
    const std::wstring& displayName,
    std::wstring& errorMessage) const {
    if (!ValidateFolderName(displayName, errorMessage)) {
        return false;
    }
    const AppConfig config = configStore_.LoadAppConfig();
    if (!NameIsAvailable(config, categoryId, displayName, errorMessage)) {
        return false;
    }
    return true;
}

bool CategoryStorageManager::Rename(
    const std::wstring& categoryId,
    const std::wstring& displayName,
    std::wstring& errorMessage) {
    errorMessage.clear();
    AppConfig config = configStore_.LoadAppConfig();
    if (!ValidateFolderName(displayName, errorMessage) ||
        !NameIsAvailable(config, categoryId, displayName, errorMessage)) {
        return false;
    }

    std::wstring* storedName = nullptr;
    if (IsUncategorized(categoryId)) {
        storedName = &config.uncategorizedName;
    } else {
        const auto category = std::find_if(
            config.categories.begin(),
            config.categories.end(),
            [&](const CategoryConfig& value) { return SameText(value.id, categoryId); });
        if (category == config.categories.end()) {
            errorMessage = L"没有找到要重命名的格子配置。";
            return false;
        }
        storedName = &category->name;
    }
    if (*storedName == displayName) {
        return true;
    }
    *storedName = displayName;
    if (!configStore_.SaveAppConfig(config)) {
        errorMessage = L"配置保存失败，格子名称没有改变。";
        return false;
    }
    return true;
}

bool CategoryStorageManager::RemoveEmpty(
    const std::wstring& categoryId,
    std::wstring& errorMessage) {
    errorMessage.clear();
    if (IsUncategorized(categoryId)) {
        errorMessage = L"未分类格子是固定格子，不能删除。";
        return false;
    }
    const AppConfig config = configStore_.LoadAppConfig();
    const auto category = std::find_if(
        config.categories.begin(),
        config.categories.end(),
        [&](const CategoryConfig& value) { return SameText(value.id, categoryId); });
    if (category == config.categories.end()) {
        errorMessage = L"没有找到要解散的格子配置。";
        return false;
    }
    return true;
}

bool CategoryStorageManager::SynchronizeAll(std::wstring& errorMessage) {
    errorMessage.clear();
    const AppConfig config = configStore_.LoadAppConfig();
    if (!ValidateFolderName(config.uncategorizedName, errorMessage)) {
        return false;
    }
    for (const CategoryConfig& category : config.categories) {
        if (!ValidateFolderName(category.name, errorMessage) ||
            !NameIsAvailable(config, category.id, category.name, errorMessage)) {
            return false;
        }
    }
    return true;
}
