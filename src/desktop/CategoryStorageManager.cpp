#include "desktop/CategoryStorageManager.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <vector>

#include "model/OrganizerModel.h"
#include "util/PathUtil.h"

namespace {

bool SameText(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring FullPath(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return path;
    }
    std::wstring result(required, 0);
    const DWORD copied = GetFullPathNameW(path.c_str(), required, result.data(), nullptr);
    if (copied == 0 || copied >= required) {
        return path;
    }
    result.resize(copied);
    return result;
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return SameText(FullPath(left), FullPath(right));
}

bool PathIsInside(const std::wstring& path, const std::wstring& root) {
    const std::wstring normalizedPath = FullPath(path);
    std::wstring normalizedRoot = FullPath(root);
    if (normalizedPath.empty() || normalizedRoot.empty()) {
        return false;
    }
    if (SameText(normalizedPath, normalizedRoot)) {
        return true;
    }
    if (normalizedRoot.back() != L'\\') {
        normalizedRoot.push_back(L'\\');
    }
    return normalizedPath.size() > normalizedRoot.size() &&
           CompareStringOrdinal(
               normalizedPath.c_str(),
               static_cast<int>(normalizedRoot.size()),
               normalizedRoot.c_str(),
               static_cast<int>(normalizedRoot.size()),
               TRUE) == CSTR_EQUAL;
}

std::wstring ReplaceRoot(
    const std::wstring& path,
    const std::wstring& oldRoot,
    const std::wstring& newRoot) {
    const std::wstring normalizedPath = FullPath(path);
    const std::wstring normalizedOld = FullPath(oldRoot);
    if (!PathIsInside(normalizedPath, normalizedOld)) {
        return path;
    }
    return newRoot + normalizedPath.substr(normalizedOld.size());
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring WindowsError(const std::wstring& action, DWORD error) {
    wchar_t* raw = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<wchar_t*>(&raw),
        0,
        nullptr);
    std::wstring detail = length > 0 && raw != nullptr ? std::wstring(raw, length) : L"未知错误";
    if (raw != nullptr) {
        LocalFree(raw);
    }
    while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n' || detail.back() == L' ')) {
        detail.pop_back();
    }
    return action + L"（错误 " + std::to_wstring(error) + L"：" + detail + L"）";
}

bool IsUncategorized(const std::wstring& categoryId) {
    return SameText(categoryId, kUncategorizedCategoryId);
}

}  // namespace

CategoryStorageManager::CategoryStorageManager(
    ConfigStore& configStore,
    ManagedShortcutStore& managedStore)
    : configStore_(configStore), managedStore_(managedStore) {}

bool CategoryStorageManager::ValidateFolderName(
    const std::wstring& displayName,
    std::wstring& errorMessage) const {
    errorMessage.clear();
    if (displayName.empty() || displayName == L"." || displayName == L"..") {
        errorMessage = L"格子名称不能为空，也不能是 . 或 ..。";
        return false;
    }
    if (displayName.size() > 120) {
        errorMessage = L"格子名称过长，请控制在 120 个字符以内。";
        return false;
    }
    if (displayName.back() == L'.' || displayName.back() == L' ') {
        errorMessage = L"格子名称不能以空格或句点结尾。";
        return false;
    }
    for (const wchar_t character : displayName) {
        if (character < 32 || character == L'<' || character == L'>' || character == L':' ||
            character == L'"' || character == L'/' || character == L'\\' || character == L'|' ||
            character == L'?' || character == L'*') {
            errorMessage = L"格子名称包含 Windows 文件夹不允许使用的字符。";
            return false;
        }
    }
    std::wstring base = displayName.substr(0, displayName.find(L'.'));
    std::transform(base.begin(), base.end(), base.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towupper(value));
    });
    const bool reserved = base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
                          (base.size() == 4 && (base.rfind(L"COM", 0) == 0 || base.rfind(L"LPT", 0) == 0) &&
                           base[3] >= L'1' && base[3] <= L'9');
    if (reserved) {
        errorMessage = L"该名称是 Windows 保留设备名，不能作为格子文件夹名称。";
        return false;
    }
    return true;
}

bool CategoryStorageManager::NameIsAvailable(
    const AppConfig& config,
    const std::wstring& categoryId,
    const std::wstring& displayName,
    std::wstring& errorMessage) const {
    if (!IsUncategorized(categoryId) &&
        (SameText(config.uncategorizedName, displayName) ||
         SameText(config.uncategorizedStorageFolder, displayName))) {
        errorMessage = L"已有同名格子，请使用另一个名称。";
        return false;
    }
    for (const CategoryConfig& category : config.categories) {
        if (SameText(category.id, categoryId)) {
            continue;
        }
        const std::wstring storageFolder = category.storageFolder.empty() ? category.id : category.storageFolder;
        if (SameText(category.name, displayName) || SameText(storageFolder, displayName)) {
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
    const std::wstring targetPath = managedStore_.CategoryPath(displayName);
    const std::wstring currentFolder = CategoryStorageFolder(config, categoryId);
    if (DirectoryExists(targetPath) && !SamePath(targetPath, managedStore_.CategoryPath(currentFolder))) {
        errorMessage = L"受管目录中已经存在同名文件夹，未接管或覆盖其中的文件。请先使用另一个格子名称。";
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
    std::wstring* storageFolder = nullptr;
    if (IsUncategorized(categoryId)) {
        storedName = &config.uncategorizedName;
        storageFolder = &config.uncategorizedStorageFolder;
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
        storageFolder = &category->storageFolder;
        if (storageFolder->empty()) {
            *storageFolder = category->id;
        }
    }
    if (storageFolder->empty()) {
        *storageFolder = kUncategorizedCategoryId;
    }

    const std::wstring oldFolder = *storageFolder;
    const std::wstring sourcePath = managedStore_.CategoryPath(oldFolder);
    const std::wstring targetPath = managedStore_.CategoryPath(displayName);
    if (*storedName == displayName && *storageFolder == displayName) {
        if (!DirectoryExists(targetPath) &&
            !managedStore_.EnsureCategoryDirectory(displayName)) {
            errorMessage = L"无法创建与格子同名的受管文件夹。";
            return false;
        }
        return true;
    }
    const bool samePath = SamePath(sourcePath, targetPath);
    const bool sourceExists = DirectoryExists(sourcePath);
    const bool targetExists = DirectoryExists(targetPath);
    bool movedDirectory = false;
    bool createdDirectory = false;

    if (!samePath && targetExists) {
        errorMessage = L"受管目录中已经存在同名文件夹，未覆盖任何文件。请先为格子使用其他名称。";
        return false;
    }
    if (!samePath && sourceExists) {
        if (MoveFileExW(sourcePath.c_str(), targetPath.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) {
            errorMessage = WindowsError(L"无法同步重命名格子文件夹", GetLastError());
            return false;
        }
        movedDirectory = true;
    } else if (!samePath && !sourceExists) {
        if (!managedStore_.EnsureCategoryDirectory(displayName)) {
            errorMessage = L"无法创建与格子同名的受管文件夹。";
            return false;
        }
        createdDirectory = true;
    } else if (samePath && !sourceExists) {
        if (!managedStore_.EnsureCategoryDirectory(displayName)) {
            errorMessage = L"无法创建与格子同名的受管文件夹。";
            return false;
        }
        createdDirectory = true;
    }

    for (ItemConfig& item : config.items) {
        if (PathIsInside(item.path, sourcePath)) {
            item.path = ReplaceRoot(item.path, sourcePath, targetPath);
        }
    }
    *storedName = displayName;
    *storageFolder = displayName;
    if (!configStore_.SaveAppConfig(config)) {
        bool rolledBack = true;
        if (movedDirectory) {
            rolledBack = MoveFileExW(targetPath.c_str(), sourcePath.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
        } else if (createdDirectory) {
            rolledBack = RemoveDirectoryW(targetPath.c_str()) != FALSE;
        }
        errorMessage = rolledBack
            ? L"格子文件夹已回滚，但配置保存失败，名称没有改变。"
            : L"配置保存失败，而且文件夹回滚没有完成；文件仍保留在受管目录中，请不要重复重命名。";
        return false;
    }
    if (movedDirectory) {
        SHChangeNotify(SHCNE_RENAMEFOLDER, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, sourcePath.c_str(), targetPath.c_str());
    }
    return true;
}

bool CategoryStorageManager::RemoveEmpty(
    const std::wstring& categoryId,
    std::wstring& errorMessage) {
    errorMessage.clear();
    if (IsUncategorized(categoryId)) {
        errorMessage = L"未分类格子是固定格子，不能删除其受管目录。";
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
    const std::wstring directory = managedStore_.CategoryPath(
        category->storageFolder.empty() ? category->id : category->storageFolder);
    if (!DirectoryExists(directory)) {
        return true;
    }
    if (RemoveDirectoryW(directory.c_str()) == FALSE) {
        const DWORD error = GetLastError();
        errorMessage = error == ERROR_DIR_NOT_EMPTY
            ? L"格子目录中仍有未登记的文件，为避免误删已保留格子和目录。"
            : WindowsError(L"无法移除已经清空的格子目录", error);
        return false;
    }
    SHChangeNotify(SHCNE_RMDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, directory.c_str(), nullptr);
    return true;
}

bool CategoryStorageManager::SynchronizeAll(std::wstring& errorMessage) {
    errorMessage.clear();
    const AppConfig initial = configStore_.LoadAppConfig();
    std::vector<std::pair<std::wstring, std::wstring>> categories;
    categories.emplace_back(kUncategorizedCategoryId, initial.uncategorizedName);
    for (const CategoryConfig& category : initial.categories) {
        categories.emplace_back(category.id, category.name);
    }
    bool succeeded = true;
    for (const auto& [categoryId, displayName] : categories) {
        std::wstring renameError;
        if (Rename(categoryId, displayName, renameError)) {
            continue;
        }
        succeeded = false;
        if (!errorMessage.empty()) {
            errorMessage += L"\n";
        }
        errorMessage += L"• " + displayName + L"：" + renameError;
    }
    return succeeded;
}
