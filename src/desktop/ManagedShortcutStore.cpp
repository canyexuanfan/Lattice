#include "desktop/ManagedShortcutStore.h"

#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <shellapi.h>
#include <winreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "desktop/DesktopLayout.h"
#include "util/PathUtil.h"
#include "util/StringUtil.h"

namespace {

constexpr std::uint32_t kJournalMagic = 0x4F534D44;  // DMSO
constexpr std::uint32_t kLegacyJournalVersion = 1;
constexpr std::uint32_t kJournalVersion = 2;
constexpr std::uint32_t kMaximumJournalStringLength = 32768;
constexpr std::uint32_t kMaximumJournalEntryCount = 4096;
constexpr int kLegacyAttributeVisibilityMode = 1;
constexpr int kNamespaceVisibilityMode = 2;
constexpr int kOffscreenVisibilityMode = 3;

struct LegacyJournalHeader {
    std::uint32_t magic = kJournalMagic;
    std::uint32_t version = kLegacyJournalVersion;
    std::uint32_t itemIdLength = 0;
    std::uint32_t sourceLength = 0;
    std::uint32_t destinationLength = 0;
    std::uint32_t releaseToDesktop = 0;
};

struct JournalBatchHeader {
    std::uint32_t magic = kJournalMagic;
    std::uint32_t version = kJournalVersion;
    std::uint32_t entryCount = 0;
};

struct JournalEntryHeader {
    std::uint32_t itemIdLength = 0;
    std::uint32_t sourceLength = 0;
    std::uint32_t destinationLength = 0;
    std::uint32_t releaseToDesktop = 0;
};

std::wstring ParentDirectory(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
}

std::wstring ModuleDirectory() {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length + 1 < buffer.size()) {
            return ParentDirectory(std::wstring(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring EnvironmentPath(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0 || copied >= required) {
        return {};
    }
    value.resize(copied);
    return value;
}

std::wstring FullPath(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return path;
    }
    std::wstring result(required, L'\0');
    const DWORD copied = GetFullPathNameW(path.c_str(), required, result.data(), nullptr);
    if (copied == 0 || copied >= required) {
        return path;
    }
    result.resize(copied);
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) {
        result.pop_back();
    }
    return result;
}

bool PathsEqual(const std::wstring& left, const std::wstring& right) {
    const std::wstring normalizedLeft = FullPath(left);
    const std::wstring normalizedRight = FullPath(right);
    return CompareStringOrdinal(
               normalizedLeft.c_str(), static_cast<int>(normalizedLeft.size()),
               normalizedRight.c_str(), static_cast<int>(normalizedRight.size()), TRUE) == CSTR_EQUAL;
}

bool PathIsInside(const std::wstring& path, const std::wstring& root) {
    const std::wstring normalizedPath = FullPath(path);
    std::wstring normalizedRoot = FullPath(root);
    if (normalizedPath.empty() || normalizedRoot.empty()) {
        return false;
    }
    if (PathsEqual(normalizedPath, normalizedRoot)) {
        return true;
    }
    normalizedRoot.push_back(L'\\');
    return normalizedPath.size() > normalizedRoot.size() &&
           CompareStringOrdinal(
               normalizedPath.c_str(), static_cast<int>(normalizedRoot.size()),
               normalizedRoot.c_str(), static_cast<int>(normalizedRoot.size()), TRUE) == CSTR_EQUAL;
}

constexpr wchar_t kNewStartPanelKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\NewStartPanel";
constexpr wchar_t kClassicStartMenuKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\ClassicStartMenu";

std::wstring DesktopNamespaceClsid(const std::wstring& parsingName) {
    const size_t start = parsingName.find(L"::{");
    const size_t end = start == std::wstring::npos ? std::wstring::npos : parsingName.find(L'}', start + 3);
    if (start == std::wstring::npos || end == std::wstring::npos) {
        return {};
    }
    std::wstring clsid = parsingName.substr(start + 2, end - start - 1);
    if (CompareStringOrdinal(clsid.c_str(), -1, L"{26EE0668-A00A-44D7-9371-BEB064C98683}", -1, TRUE) == CSTR_EQUAL) {
        return L"{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}";
    }
    return clsid;
}

int ReadDesktopIconValue(const wchar_t* keyPath, const std::wstring& valueName) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER, keyPath, valueName.c_str(), RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS ? static_cast<int>(value) : -1;
}

bool WriteDesktopIconValue(const wchar_t* keyPath, const std::wstring& valueName, int value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status = value < 0
        ? RegDeleteValueW(key, valueName.c_str())
        : [&]() {
              const DWORD data = static_cast<DWORD>(value);
              return RegSetValueExW(key, valueName.c_str(), 0, REG_DWORD,
                  reinterpret_cast<const BYTE*>(&data), sizeof(data));
          }();
    RegCloseKey(key);
    return status == ERROR_SUCCESS || (value < 0 && status == ERROR_FILE_NOT_FOUND);
}

void RefreshDesktopShell(const std::wstring& path, bool namespaceItem) {
    if (namespaceItem) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
            reinterpret_cast<LPARAM>(L"ShellState"), SMTO_ABORTIFHUNG, 1000, &ignored);
        return;
    }
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, path.c_str(), nullptr);
}

bool EnsureDirectoryTree(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

std::wstring SafeFolderName(const std::wstring& value) {
    std::wstring result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        const bool invalid = character < 32 || character == L'<' || character == L'>' || character == L':' ||
                             character == L'"' || character == L'/' || character == L'\\' || character == L'|' ||
                             character == L'?' || character == L'*';
        result.push_back(invalid ? L'_' : character);
    }
    while (!result.empty() && (result.back() == L'.' || result.back() == L' ')) {
        result.pop_back();
    }
    return result.empty() ? L"uncategorized" : result;
}

std::wstring AvailableDestination(const std::wstring& directory, const std::wstring& fileName) {
    const std::wstring direct = JoinPath(directory, fileName);
    if (GetFileAttributesW(direct.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return direct;
    }

    const size_t dot = fileName.find_last_of(L'.');
    const bool hasExtension = dot != std::wstring::npos && dot != 0;
    const std::wstring stem = hasExtension ? fileName.substr(0, dot) : fileName;
    const std::wstring extension = hasExtension ? fileName.substr(dot) : std::wstring{};
    for (int suffix = 2; suffix < 10000; ++suffix) {
        const std::wstring candidate = JoinPath(
            directory,
            stem + L" (" + std::to_wstring(suffix) + L")" + extension);
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    return {};
}

std::wstring ErrorText(const std::wstring& action, DWORD error) {
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

bool WriteBytes(const std::wstring& path, const void* data, DWORD byteCount, std::wstring& errorMessage) {
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        errorMessage = ErrorText(L"无法创建快捷方式移动日志", GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool succeeded = WriteFile(file, data, byteCount, &written, nullptr) != FALSE && written == byteCount &&
                           FlushFileBuffers(file) != FALSE;
    const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!succeeded) {
        errorMessage = ErrorText(L"无法写入快捷方式移动日志", error);
    }
    return succeeded;
}

bool ReadBytes(const std::wstring& path, std::vector<unsigned char>& bytes, std::wstring& errorMessage) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        errorMessage = ErrorText(L"无法读取快捷方式移动日志", GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        errorMessage = ErrorText(L"快捷方式移动日志大小无效", error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool succeeded = bytes.empty() ||
                           (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE &&
                            read == bytes.size());
    const DWORD error = succeeded ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!succeeded) {
        errorMessage = ErrorText(L"无法读取完整的快捷方式移动日志", error);
    }
    return succeeded;
}

void NotifyShellMove(const std::wstring& source, const std::wstring& destination, bool directory) {
    if (PathsEqual(ParentDirectory(source), ParentDirectory(destination))) {
        SHChangeNotify(
            directory ? SHCNE_RENAMEFOLDER : SHCNE_RENAMEITEM,
            SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
            source.c_str(),
            destination.c_str());
        return;
    }

    // A move between the desktop and Lattice's private directory is not merely a
    // rename inside one Shell folder. Notify both namespace changes explicitly
    // so Explorer removes the old desktop icon and materializes the returned one.
    SHChangeNotify(
        directory ? SHCNE_RMDIR : SHCNE_DELETE,
        SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
        source.c_str(),
        nullptr);
    SHChangeNotify(
        directory ? SHCNE_MKDIR : SHCNE_CREATE,
        SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
        destination.c_str(),
        nullptr);
}

bool MovePath(
    const std::wstring& source,
    const std::wstring& destination,
    DWORD attributes,
    bool allowElevation,
    HWND ownerWindow,
    DWORD& error) {
    if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) != FALSE) {
        const bool completed =
            GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES;
        error = completed ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
        return completed;
    }
    error = GetLastError();
    const bool needsCrossVolumeDirectoryMove =
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && error == ERROR_NOT_SAME_DEVICE;
    const bool needsPermissionBroker =
        allowElevation &&
        (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD);
    if (!needsCrossVolumeDirectoryMove && !needsPermissionBroker) {
        return false;
    }
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) {
        error = ERROR_ALREADY_EXISTS;
        return false;
    }

    Microsoft::WRL::ComPtr<IShellItem> sourceItem;
    Microsoft::WRL::ComPtr<IShellItem> destinationFolder;
    Microsoft::WRL::ComPtr<IFileOperation> operation;
    const std::wstring destinationDirectory = ParentDirectory(destination);
    const std::wstring destinationName = FileNameFromPath(destination);
    HRESULT result = SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&sourceItem));
    if (SUCCEEDED(result)) {
        result = SHCreateItemFromParsingName(destinationDirectory.c_str(), nullptr, IID_PPV_ARGS(&destinationFolder));
    }
    if (SUCCEEDED(result)) {
        result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation));
    }
    if (SUCCEEDED(result) && ownerWindow != nullptr && IsWindow(ownerWindow) != FALSE) {
        result = operation->SetOwnerWindow(ownerWindow);
    }
    if (SUCCEEDED(result)) {
        FILEOP_FLAGS flags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
        if (needsPermissionBroker) {
            flags |= FOFX_SHOWELEVATIONPROMPT;
        }
        result = operation->SetOperationFlags(flags);
    }
    if (SUCCEEDED(result)) {
        result = operation->MoveItem(sourceItem.Get(), destinationFolder.Get(), destinationName.c_str(), nullptr);
    }
    if (SUCCEEDED(result)) {
        result = operation->PerformOperations();
    }
    BOOL aborted = FALSE;
    if (SUCCEEDED(result)) {
        result = operation->GetAnyOperationsAborted(&aborted);
    }
    if (SUCCEEDED(result) && aborted == FALSE &&
        GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) {
        error = ERROR_SUCCESS;
        return true;
    }
    if (aborted != FALSE) {
        error = ERROR_CANCELLED;
    } else if (FAILED(result) && HRESULT_FACILITY(result) == FACILITY_WIN32) {
        error = HRESULT_CODE(result);
    } else {
        error = FAILED(result) ? static_cast<DWORD>(result) : ERROR_GEN_FAILURE;
    }
    return false;
}

bool FilesHaveSameContents(const std::wstring& leftPath, const std::wstring& rightPath) {
    const DWORD leftAttributes = GetFileAttributesW(leftPath.c_str());
    const DWORD rightAttributes = GetFileAttributesW(rightPath.c_str());
    if (leftAttributes == INVALID_FILE_ATTRIBUTES || rightAttributes == INVALID_FILE_ATTRIBUTES ||
        (leftAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (rightAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    HANDLE left = CreateFileW(
        leftPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (left == INVALID_HANDLE_VALUE) {
        return false;
    }
    HANDLE right = CreateFileW(
        rightPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (right == INVALID_HANDLE_VALUE) {
        CloseHandle(left);
        return false;
    }
    LARGE_INTEGER leftSize{};
    LARGE_INTEGER rightSize{};
    bool same = GetFileSizeEx(left, &leftSize) != FALSE &&
                GetFileSizeEx(right, &rightSize) != FALSE &&
                leftSize.QuadPart == rightSize.QuadPart;
    std::vector<unsigned char> leftBytes(64 * 1024);
    std::vector<unsigned char> rightBytes(leftBytes.size());
    while (same) {
        DWORD leftRead = 0;
        DWORD rightRead = 0;
        if (ReadFile(left, leftBytes.data(), static_cast<DWORD>(leftBytes.size()), &leftRead, nullptr) == FALSE ||
            ReadFile(right, rightBytes.data(), static_cast<DWORD>(rightBytes.size()), &rightRead, nullptr) == FALSE ||
            leftRead != rightRead) {
            same = false;
            break;
        }
        if (leftRead == 0) {
            break;
        }
        same = std::memcmp(leftBytes.data(), rightBytes.data(), leftRead) == 0;
    }
    CloseHandle(right);
    CloseHandle(left);
    return same;
}

bool DeleteFileWithRetry(const std::wstring& path, DWORD& error) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (DeleteFileW(path.c_str()) != FALSE) {
            error = ERROR_SUCCESS;
            return true;
        }
        error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        Sleep(25);
    }
    return false;
}

}  // namespace

ManagedShortcutStore::ManagedShortcutStore() {
    std::wstring dataDirectory = EnvironmentPath(L"DESKTOP_ORGANIZER_DATA_DIR");
    if (dataDirectory.empty()) {
        dataDirectory = JoinPath(ModuleDirectory(), L"Data");
    }
    rootPath_ = FullPath(JoinPath(dataDirectory, L"ManagedShortcuts"));

    desktopPath_ = EnvironmentPath(L"DESKTOP_ORGANIZER_DESKTOP_DIR");
    if (desktopPath_.empty()) {
        desktopPath_ = KnownFolderPath(FOLDERID_Desktop);
    }
    desktopPath_ = FullPath(desktopPath_);
    publicDesktopPath_ = EnvironmentPath(
        L"DESKTOP_ORGANIZER_PUBLIC_DESKTOP_DIR");
    if (publicDesktopPath_.empty()) {
        publicDesktopPath_ = KnownFolderPath(FOLDERID_PublicDesktop);
    } else {
        publicDesktopRequiresElevation_ = false;
    }
    publicDesktopPath_ = FullPath(publicDesktopPath_);
    journalPath_ = JoinPath(rootPath_, L"move-journal.bin");
    journalTempPath_ = journalPath_ + L".tmp";
}

ManagedShortcutStore::ManagedShortcutStore(
    std::wstring dataDirectory,
    std::wstring desktopDirectory,
    std::wstring publicDesktopDirectory) {
    publicDesktopRequiresElevation_ = publicDesktopDirectory.empty();
    rootPath_ = FullPath(JoinPath(std::move(dataDirectory), L"ManagedShortcuts"));
    desktopPath_ = FullPath(std::move(desktopDirectory));
    publicDesktopPath_ = FullPath(
        publicDesktopDirectory.empty()
            ? KnownFolderPath(FOLDERID_PublicDesktop)
            : std::move(publicDesktopDirectory));
    journalPath_ = JoinPath(rootPath_, L"move-journal.bin");
    journalTempPath_ = journalPath_ + L".tmp";
}

bool ManagedShortcutStore::IsSupportedDesktopItem(const std::wstring& path) const {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const std::wstring fileName = FileNameFromPath(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return IsShellNamespaceItem(path);
    }
    const std::wstring parent = ParentDirectory(FullPath(path));
    return !fileName.empty() && fileName != L"." && fileName != L".." &&
        (PathsEqual(parent, desktopPath_) ||
         PathsEqual(parent, publicDesktopPath_));
}

bool ManagedShortcutStore::IsSupportedShortcut(const std::wstring& path) const {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return IsSupportedDesktopItem(path) &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (EndsWithInsensitive(path, L".lnk") || EndsWithInsensitive(path, L".url"));
}

bool ManagedShortcutStore::RequiresManagedStorage(const std::wstring& path) const {
    // ManagedShortcuts is a legacy migration source only. New desktop
    // collection keeps the original Shell identity and absolute path.
    return IsManagedPath(path);
}

bool ManagedShortcutStore::IsManagedPath(const std::wstring& path) const {
    return PathIsInside(path, rootPath_);
}

bool ManagedShortcutStore::IsDesktopPath(const std::wstring& path) const {
    return PathIsInside(path, desktopPath_) || PathIsInside(path, publicDesktopPath_);
}

bool ManagedShortcutStore::IsPublicDesktopPath(const std::wstring& path) const {
    return PathIsInside(path, publicDesktopPath_);
}

bool ManagedShortcutStore::IsShellNamespaceItem(const std::wstring& path) const {
    if (path.empty() || GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    Microsoft::WRL::ComPtr<IShellItem> item;
    return SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) && item != nullptr;
}

#if defined(_DEBUG)
bool ManagedShortcutStore::IsDesktopPositionSuppressed(const ItemConfig& item) const noexcept {
    return item.desktopVisibilityMode == kOffscreenVisibilityMode;
}

bool ManagedShortcutStore::CaptureAndSuppressDesktopVisibility(
    const std::wstring& path,
    ItemConfig& item,
    std::wstring& errorMessage) const {
    errorMessage.clear();
    if (IsManagedPath(path)) {
        return true;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (!IsDesktopPath(path)) {
            return true;
        }
        errorMessage = PathIsInside(path, publicDesktopPath_)
            ? L"共享桌面项目不能通过改变坐标隐藏；当前权限下未移动该项目。"
            : L"该桌面项目必须通过托管存储收纳，不能通过改变坐标隐藏。";
        return false;
    }
    if (!IsShellNamespaceItem(path)) {
        errorMessage = L"该桌面项目当前不可访问。";
        return false;
    }
    const std::wstring clsid = DesktopNamespaceClsid(path);
    if (clsid.empty()) {
        errorMessage = L"无法识别该虚拟桌面项目的稳定标识。";
        return false;
    }
    item.desktopVisibilityMode = kNamespaceVisibilityMode;
    item.desktopVisibilityNewStartValue = ReadDesktopIconValue(kNewStartPanelKey, clsid);
    item.desktopVisibilityClassicValue = ReadDesktopIconValue(kClassicStartMenuKey, clsid);
    if (!WriteDesktopIconValue(kNewStartPanelKey, clsid, 1) ||
        !WriteDesktopIconValue(kClassicStartMenuKey, clsid, 1)) {
        WriteDesktopIconValue(kNewStartPanelKey, clsid, item.desktopVisibilityNewStartValue);
        WriteDesktopIconValue(kClassicStartMenuKey, clsid, item.desktopVisibilityClassicValue);
        item.desktopVisibilityMode = 0;
        errorMessage = L"无法保存并隐藏该虚拟桌面图标。";
        return false;
    }
    RefreshDesktopShell(path, true);
    return true;
}

bool ManagedShortcutStore::SuppressDesktopVisibility(ItemConfig& item, std::wstring& errorMessage) const {
    errorMessage.clear();
    if (item.desktopVisibilityMode == 0) {
        return true;
    }
    if (item.desktopVisibilityMode == kLegacyAttributeVisibilityMode) {
        errorMessage = L"旧版文件隐藏状态只能恢复；该项目需要迁移到托管存储。";
        return false;
    }
    if (item.desktopVisibilityMode == kOffscreenVisibilityMode) {
        errorMessage = L"旧版离屏状态只能恢复；该项目需要迁移到托管存储。";
        return false;
    }
    const std::wstring clsid = DesktopNamespaceClsid(item.path);
    if (clsid.empty() || !WriteDesktopIconValue(kNewStartPanelKey, clsid, 1) ||
        !WriteDesktopIconValue(kClassicStartMenuKey, clsid, 1)) {
        errorMessage = L"无法恢复虚拟桌面图标隐藏状态。";
        return false;
    }
    RefreshDesktopShell(item.path, true);
    return true;
}

bool ManagedShortcutStore::PrepareForManagedStorage(
    ItemConfig& item,
    std::wstring& errorMessage) const {
    errorMessage.clear();
    if (item.desktopVisibilityMode == kNamespaceVisibilityMode) {
        errorMessage = L"虚拟桌面项目不能迁入文件托管目录。";
        return false;
    }
    if (item.desktopVisibilityMode == kLegacyAttributeVisibilityMode &&
        !RestoreDesktopVisibility(item, errorMessage)) {
        return false;
    }
    item.desktopVisibilityMode = 0;
    item.desktopVisibilityOriginalFlags = 0;
    item.desktopVisibilityNewStartValue = -1;
    item.desktopVisibilityClassicValue = -1;
    return true;
}
#endif

bool ManagedShortcutStore::RestoreDesktopVisibility(
    const ItemConfig& item,
    std::wstring& errorMessage,
    const POINT* releaseScreenPoint) const {
    errorMessage.clear();
    if (item.desktopVisibilityMode == 0) {
        return true;
    }
    if (item.desktopVisibilityMode == kLegacyAttributeVisibilityMode) {
        const DWORD attributes = GetFileAttributesW(item.path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            errorMessage = L"桌面原件已不存在，无法恢复其显示状态。";
            return false;
        }
        const DWORD restored = (attributes & ~(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) |
            static_cast<DWORD>(item.desktopVisibilityOriginalFlags);
        if (SetFileAttributesW(item.path.c_str(), restored) == FALSE) {
            errorMessage = ErrorText(L"无法恢复 Explorer 桌面原图标", GetLastError());
            return false;
        }
        RefreshDesktopShell(item.path, false);
        return true;
    }
    if (item.desktopVisibilityMode == kOffscreenVisibilityMode) {
        DesktopLayout desktopLayout;
        if (releaseScreenPoint != nullptr) {
            POINT restoredPoint{};
            return desktopLayout.RestoreScreenPositionOnce(
                item.path,
                *releaseScreenPoint,
                restoredPoint,
                errorMessage);
        }
        if (!item.hasDesktopPosition) {
            errorMessage = L"没有保存该桌面项目的原始位置，无法恢复显示。";
            return false;
        }
        return desktopLayout.RestorePosition(
            item.path,
            POINT{item.desktopX, item.desktopY},
            errorMessage);
    }
    const std::wstring clsid = DesktopNamespaceClsid(item.path);
    if (clsid.empty() ||
        !WriteDesktopIconValue(kNewStartPanelKey, clsid, item.desktopVisibilityNewStartValue) ||
        !WriteDesktopIconValue(kClassicStartMenuKey, clsid, item.desktopVisibilityClassicValue)) {
        errorMessage = L"无法恢复虚拟桌面图标的原始显示状态。";
        return false;
    }
    RefreshDesktopShell(item.path, true);
    return true;
}

#if defined(_DEBUG)
std::wstring ManagedShortcutStore::CategoryPath(const std::wstring& categoryId) const {
    return JoinPath(rootPath_, SafeFolderName(categoryId));
}

bool ManagedShortcutStore::EnsureCategoryDirectory(const std::wstring& categoryId) const {
    return EnsureDirectoryTree(rootPath_) && EnsureDirectoryTree(CategoryPath(categoryId));
}

bool ManagedShortcutStore::MoveIntoCategory(
    const std::wstring& itemId,
    const std::wstring& sourcePath,
    const std::wstring& categoryId,
    const std::function<bool(const std::wstring&)>& persistDestination,
    std::wstring& destinationPath,
    std::wstring& errorMessage,
    HWND ownerWindow) {
    const std::wstring destinationDirectory = CategoryPath(categoryId);
    return ExecuteMove(
        itemId,
        sourcePath,
        destinationDirectory,
        false,
        persistDestination,
        destinationPath,
        errorMessage,
        {},
        ownerWindow);
}

bool ManagedShortcutStore::MoveToDesktop(
    const std::wstring& itemId,
    const std::wstring& sourcePath,
    const std::function<bool(const std::wstring&)>& persistDestination,
    std::wstring& destinationPath,
    std::wstring& errorMessage,
    HWND ownerWindow) {
    if (!IsManagedPath(sourcePath)) {
        errorMessage = L"该项目不在格子的托管目录中，未移动任何文件。";
        return false;
    }
    return ExecuteMove(
        itemId,
        sourcePath,
        desktopPath_,
        true,
        persistDestination,
        destinationPath,
        errorMessage,
        {},
        ownerWindow);
}
#endif

bool ManagedShortcutStore::MoveToOriginalDesktop(
    const std::wstring& itemId,
    const std::wstring& sourcePath,
    const std::wstring& originalDesktopPath,
    const std::function<bool(const std::wstring&)>& persistDestination,
    std::wstring& destinationPath,
    std::wstring& errorMessage,
    HWND ownerWindow,
    bool notifyShell) {
    if (!IsManagedPath(sourcePath)) {
        errorMessage = L"该项目不在格子的托管目录中，未移动任何文件。";
        return false;
    }
    const std::wstring target = FullPath(originalDesktopPath);
    const std::wstring targetDirectory = ParentDirectory(target);
    if (target.empty() || !IsDesktopPath(target) || targetDirectory.empty()) {
        errorMessage = L"记录的原桌面位置无效，已停止归还以避免移动到桌面以外。";
        return false;
    }
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES && !PathsEqual(sourcePath, target)) {
        if (!FilesHaveSameContents(sourcePath, target)) {
            errorMessage = L"原桌面位置已经存在内容不同的同名项目，已保留两份且未覆盖：" + target;
            return false;
        }
        bool persisted = false;
        try {
            persisted = persistDestination(target);
        } catch (...) {
            persisted = false;
        }
        if (!persisted) {
            errorMessage = L"桌面已有完全一致的项目，但无法更新配置，未清理托管副本。";
            return false;
        }
        DWORD deleteError = ERROR_SUCCESS;
        if (!DeleteFileWithRetry(sourcePath, deleteError)) {
            errorMessage = ErrorText(L"桌面已有完全一致的项目，但无法清理托管目录中的重复副本", deleteError);
            return false;
        }
        if (notifyShell) {
            SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, sourcePath.c_str(), nullptr);
        }
        destinationPath = target;
        errorMessage.clear();
        return true;
    }
    return ExecuteMove(
        itemId,
        sourcePath,
        targetDirectory,
        true,
        persistDestination,
        destinationPath,
        errorMessage,
        target,
        ownerWindow,
        notifyShell);
}

bool ManagedShortcutStore::MoveToOriginalDesktopBatch(
    const std::vector<OriginalDesktopMoveRequest>& requests,
    const std::function<bool(const std::vector<std::pair<std::wstring, std::wstring>>&)>& persistDestinations,
    std::vector<std::pair<std::wstring, std::wstring>>& destinations,
    std::wstring& errorMessage,
    HWND ownerWindow) {
    destinations.clear();
    errorMessage.clear();
    if (requests.empty()) {
        return true;
    }
    if (!persistDestinations || ownerWindow == nullptr || IsWindow(ownerWindow) == FALSE) {
        errorMessage = L"历史桌面迁移需要仍然有效的 Lattice 窗口；本次未移动任何项目。";
        return false;
    }

    std::vector<JournalEntry> journals;
    journals.reserve(requests.size());
    for (const OriginalDesktopMoveRequest& request : requests) {
        JournalEntry journal;
        journal.itemId = request.itemId;
        journal.sourcePath = FullPath(request.sourcePath);
        journal.destinationPath = FullPath(request.destinationPath);
        journal.releaseToDesktop = true;
        const DWORD attributes = GetFileAttributesW(journal.sourcePath.c_str());
        const std::wstring destinationDirectory =
            ParentDirectory(journal.destinationPath);
        if (journal.itemId.empty() || !IsManagedPath(journal.sourcePath) ||
            !IsDesktopPath(journal.destinationPath) ||
            (!PathsEqual(destinationDirectory, desktopPath_) &&
             !PathsEqual(destinationDirectory, publicDesktopPath_)) ||
            attributes == INVALID_FILE_ATTRIBUTES) {
            errorMessage = L"历史桌面批量迁移包含无效路径；本次未移动任何项目。";
            return false;
        }
        if (GetFileAttributesW(journal.destinationPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            errorMessage =
                (IsPublicDesktopPath(journal.destinationPath)
                    ? std::wstring(L"Public Desktop")
                    : std::wstring(L"User Desktop")) +
                L" 的原位置已被不同项目占用，已保留两端且未覆盖：" +
                journal.destinationPath;
            return false;
        }
        if (std::any_of(journals.begin(), journals.end(), [&](const JournalEntry& value) {
                return PathsEqual(value.destinationPath, journal.destinationPath) || value.itemId == journal.itemId;
            })) {
            errorMessage = L"历史桌面批量迁移包含重复目标；本次未移动任何项目。";
            return false;
        }
        journals.push_back(std::move(journal));
    }
    if (!WriteJournal(journals, errorMessage)) {
        return false;
    }

    Microsoft::WRL::ComPtr<IFileOperation> operation;
    HRESULT result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation));
    if (SUCCEEDED(result)) {
        result = operation->SetOwnerWindow(ownerWindow);
    }
    if (SUCCEEDED(result)) {
        DWORD flags =
            FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR |
            FOFX_EARLYFAILURE;
        const bool needsPublicDesktop = std::any_of(
            journals.begin(), journals.end(),
            [&](const JournalEntry& journal) {
                return IsPublicDesktopPath(journal.destinationPath);
            });
        if (publicDesktopRequiresElevation_ && needsPublicDesktop) {
            flags |= FOFX_SHOWELEVATIONPROMPT | FOFX_REQUIREELEVATION;
        }
        result = operation->SetOperationFlags(flags);
    }
    for (size_t index = 0; SUCCEEDED(result) && index < journals.size(); ++index) {
        Microsoft::WRL::ComPtr<IShellItem> sourceItem;
        Microsoft::WRL::ComPtr<IShellItem> destinationFolder;
        result = SHCreateItemFromParsingName(
            journals[index].sourcePath.c_str(), nullptr, IID_PPV_ARGS(&sourceItem));
        if (SUCCEEDED(result)) {
            const std::wstring destinationDirectory =
                ParentDirectory(journals[index].destinationPath);
            result = SHCreateItemFromParsingName(
                destinationDirectory.c_str(), nullptr,
                IID_PPV_ARGS(&destinationFolder));
        }
        if (SUCCEEDED(result)) {
            result = operation->MoveItem(
                sourceItem.Get(),
                destinationFolder.Get(),
                FileNameFromPath(journals[index].destinationPath).c_str(),
                nullptr);
        }
    }
    if (SUCCEEDED(result)) {
        result = operation->PerformOperations();
    }
    BOOL aborted = FALSE;
    if (SUCCEEDED(result)) {
        result = operation->GetAnyOperationsAborted(&aborted);
    }

    std::vector<size_t> movedIndexes;
    bool endpointsUnambiguous = true;
    for (size_t index = 0; index < journals.size(); ++index) {
        const bool sourceExists = GetFileAttributesW(journals[index].sourcePath.c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool destinationExists = GetFileAttributesW(journals[index].destinationPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!sourceExists && destinationExists) {
            movedIndexes.push_back(index);
        } else if (!(sourceExists && !destinationExists)) {
            endpointsUnambiguous = false;
        }
    }

    const auto rollbackMoved = [&]() {
        bool rolledBack = true;
        for (auto it = movedIndexes.rbegin(); it != movedIndexes.rend(); ++it) {
            const JournalEntry& journal = journals[*it];
            const DWORD attributes = GetFileAttributesW(journal.destinationPath.c_str());
            DWORD moveError = ERROR_SUCCESS;
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                !MovePath(journal.destinationPath, journal.sourcePath, attributes, true, ownerWindow, moveError)) {
                rolledBack = false;
                continue;
            }
            NotifyShellMove(
                journal.destinationPath,
                journal.sourcePath,
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        }
        if (rolledBack && endpointsUnambiguous) {
            ClearJournal();
        }
        return rolledBack;
    };

    if (!endpointsUnambiguous || movedIndexes.size() != journals.size()) {
        const bool rolledBack = rollbackMoved();
        DWORD error = ERROR_GEN_FAILURE;
        if (aborted != FALSE) {
            error = ERROR_CANCELLED;
        } else if (FAILED(result) && HRESULT_FACILITY(result) == FACILITY_WIN32) {
            error = HRESULT_CODE(result);
        } else if (FAILED(result)) {
            error = static_cast<DWORD>(result);
        }
        errorMessage = ErrorText(
            rolledBack && endpointsUnambiguous
                ? L"历史桌面批量迁移没有完成，已恢复到迁移前状态"
                : L"历史桌面批量迁移没有完成，且自动回滚未完全成功；移动日志已保留",
            error);
        return false;
    }

    destinations.reserve(journals.size());
    for (size_t index = 0; index < journals.size(); ++index) {
        destinations.emplace_back(journals[index].itemId, journals[index].destinationPath);
    }
    bool persisted = false;
    try {
        persisted = persistDestinations(destinations);
    } catch (...) {
        persisted = false;
    }
    if (!persisted) {
        const bool rolledBack = rollbackMoved();
        destinations.clear();
        errorMessage = rolledBack
            ? L"无法保存历史桌面迁移结果，全部项目已恢复到迁移前状态。"
            : L"无法保存历史桌面迁移结果，且自动回滚未完全成功；移动日志已保留。";
        return false;
    }

    for (size_t index = 0; index < journals.size(); ++index) {
        if (requests[index].notifyShell) {
            const DWORD attributes = GetFileAttributesW(journals[index].destinationPath.c_str());
            NotifyShellMove(
                journals[index].sourcePath,
                journals[index].destinationPath,
                attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        }
    }
    ClearJournal();
    return true;
}

bool ManagedShortcutStore::RemoveRedundantDesktopCopy(
    const std::wstring& managedPath,
    const std::wstring& desktopPath,
    std::wstring& errorMessage) const {
    errorMessage.clear();
    if (!IsManagedPath(managedPath) || !IsDesktopPath(desktopPath)) {
        errorMessage = L"重复项目清理路径超出托管目录或桌面，未删除任何内容。";
        return false;
    }
    if (GetFileAttributesW(desktopPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    if (!FilesHaveSameContents(managedPath, desktopPath)) {
        errorMessage = L"桌面存在同名但内容不同的项目，已保留桌面和格子中的两份内容：" + desktopPath;
        return false;
    }
    DWORD deleteError = ERROR_SUCCESS;
    if (!DeleteFileWithRetry(desktopPath, deleteError)) {
        errorMessage = ErrorText(L"无法清理桌面上的历史重复副本", deleteError);
        return false;
    }
    SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, desktopPath.c_str(), nullptr);
    return true;
}

bool ManagedShortcutStore::ExecuteMove(
    const std::wstring& itemId,
    const std::wstring& sourcePath,
    const std::wstring& destinationDirectory,
    bool releaseToDesktop,
    const std::function<bool(const std::wstring&)>& persistDestination,
    std::wstring& destinationPath,
    std::wstring& errorMessage,
    const std::wstring& exactDestinationPath,
    HWND ownerWindow,
    bool notifyShell) {
    destinationPath.clear();
    errorMessage.clear();
    if (itemId.empty() || sourcePath.empty() || destinationDirectory.empty() || !persistDestination) {
        errorMessage = L"桌面项目移动参数不完整。";
        return false;
    }
    const DWORD attributes = GetFileAttributesW(sourcePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        errorMessage = L"要收纳的桌面项目不存在。";
        return false;
    }
    if (!EnsureDirectoryTree(rootPath_) || !EnsureDirectoryTree(destinationDirectory)) {
        errorMessage = ErrorText(L"无法创建桌面项目收纳目录", GetLastError());
        return false;
    }

    if (PathsEqual(ParentDirectory(sourcePath), destinationDirectory)) {
        destinationPath = FullPath(sourcePath);
        if (!persistDestination(destinationPath)) {
            errorMessage = L"快捷方式位置未变化，但配置保存失败。";
            return false;
        }
        return true;
    }

    destinationPath = exactDestinationPath.empty()
        ? AvailableDestination(destinationDirectory, FileNameFromPath(sourcePath))
        : FullPath(exactDestinationPath);
    if (!exactDestinationPath.empty() &&
        (!PathsEqual(ParentDirectory(destinationPath), destinationDirectory) ||
         GetFileAttributesW(destinationPath.c_str()) != INVALID_FILE_ATTRIBUTES)) {
        errorMessage = L"原桌面目标位置已被占用或超出目标目录，未覆盖任何项目。";
        return false;
    }
    if (destinationPath.empty()) {
        errorMessage = L"目标目录中同名项目过多，无法生成安全文件名。";
        return false;
    }

    JournalEntry journal;
    journal.itemId = itemId;
    journal.sourcePath = FullPath(sourcePath);
    journal.destinationPath = FullPath(destinationPath);
    journal.releaseToDesktop = releaseToDesktop;
    if (!WriteJournal(journal, errorMessage)) {
        return false;
    }

    DWORD moveError = ERROR_SUCCESS;
    const bool allowElevation =
        PathIsInside(journal.sourcePath, publicDesktopPath_) ||
        PathIsInside(journal.destinationPath, publicDesktopPath_);
    if (!MovePath(
            journal.sourcePath,
            journal.destinationPath,
            attributes,
            allowElevation,
            ownerWindow,
            moveError)) {
        ClearJournal();
        errorMessage = ErrorText(L"无法移动桌面项目", moveError);
        return false;
    }
    const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (notifyShell) {
        NotifyShellMove(journal.sourcePath, journal.destinationPath, isDirectory);
    }

    bool persisted = false;
    try {
        persisted = persistDestination(journal.destinationPath);
    } catch (...) {
        persisted = false;
    }
    if (persisted) {
        ClearJournal();
        destinationPath = journal.destinationPath;
        return true;
    }

    DWORD rollbackError = ERROR_SUCCESS;
    if (MovePath(
            journal.destinationPath,
            journal.sourcePath,
            attributes,
            allowElevation,
            ownerWindow,
            rollbackError)) {
        if (notifyShell) {
            NotifyShellMove(journal.destinationPath, journal.sourcePath, isDirectory);
        }
        ClearJournal();
        errorMessage = L"配置保存失败，快捷方式已经自动移回原位置。";
    } else {
        errorMessage = ErrorText(L"配置保存失败，并且桌面项目自动回滚失败；下次启动会继续恢复", rollbackError);
    }
    return false;
}

bool ManagedShortcutStore::WriteJournal(const JournalEntry& entry, std::wstring& errorMessage) const {
    return WriteJournal(std::vector<JournalEntry>{entry}, errorMessage);
}

bool ManagedShortcutStore::WriteJournal(
    const std::vector<JournalEntry>& entries,
    std::wstring& errorMessage) const {
    if (!EnsureDirectoryTree(rootPath_)) {
        errorMessage = ErrorText(L"无法创建移动日志目录", GetLastError());
        return false;
    }
    if (entries.empty() || entries.size() > kMaximumJournalEntryCount) {
        errorMessage = L"桌面项目移动日志条目数无效。";
        return false;
    }
    size_t totalSize = sizeof(JournalBatchHeader);
    for (const JournalEntry& entry : entries) {
        if (entry.itemId.size() > kMaximumJournalStringLength ||
            entry.sourcePath.size() > kMaximumJournalStringLength ||
            entry.destinationPath.size() > kMaximumJournalStringLength) {
            errorMessage = L"桌面项目路径过长，无法安全记录移动事务。";
            return false;
        }
        totalSize += sizeof(JournalEntryHeader) +
            (entry.itemId.size() + entry.sourcePath.size() + entry.destinationPath.size()) * sizeof(wchar_t);
    }
    if (totalSize > (std::numeric_limits<DWORD>::max)()) {
        errorMessage = L"快捷方式移动日志过大。";
        return false;
    }
    std::vector<unsigned char> bytes(totalSize);
    size_t offset = 0;
    const auto append = [&](const void* data, size_t size) {
        std::memcpy(bytes.data() + offset, data, size);
        offset += size;
    };
    JournalBatchHeader header;
    header.entryCount = static_cast<std::uint32_t>(entries.size());
    append(&header, sizeof(header));
    for (const JournalEntry& entry : entries) {
        JournalEntryHeader entryHeader;
        entryHeader.itemIdLength = static_cast<std::uint32_t>(entry.itemId.size());
        entryHeader.sourceLength = static_cast<std::uint32_t>(entry.sourcePath.size());
        entryHeader.destinationLength = static_cast<std::uint32_t>(entry.destinationPath.size());
        entryHeader.releaseToDesktop = entry.releaseToDesktop ? 1u : 0u;
        append(&entryHeader, sizeof(entryHeader));
        append(entry.itemId.data(), entry.itemId.size() * sizeof(wchar_t));
        append(entry.sourcePath.data(), entry.sourcePath.size() * sizeof(wchar_t));
        append(entry.destinationPath.data(), entry.destinationPath.size() * sizeof(wchar_t));
    }
    if (!WriteBytes(journalTempPath_, bytes.data(), static_cast<DWORD>(bytes.size()), errorMessage)) {
        return false;
    }
    if (MoveFileExW(
            journalTempPath_.c_str(),
            journalPath_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD error = GetLastError();
        DeleteFileW(journalTempPath_.c_str());
        errorMessage = ErrorText(L"无法提交快捷方式移动日志", error);
        return false;
    }
    return true;
}

bool ManagedShortcutStore::ReadJournal(
    std::vector<JournalEntry>& entries,
    std::wstring& errorMessage) const {
    entries.clear();
    std::vector<unsigned char> bytes;
    if (!ReadBytes(journalPath_, bytes, errorMessage) || bytes.size() < sizeof(JournalBatchHeader)) {
        if (bytes.size() < sizeof(JournalBatchHeader) && errorMessage.empty()) {
            errorMessage = L"快捷方式移动日志已损坏。";
        }
        return false;
    }
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::memcpy(&magic, bytes.data(), sizeof(magic));
    std::memcpy(&version, bytes.data() + sizeof(magic), sizeof(version));
    if (magic != kJournalMagic) {
        errorMessage = L"快捷方式移动日志格式无效。";
        return false;
    }
    if (version == kLegacyJournalVersion) {
        if (bytes.size() < sizeof(LegacyJournalHeader)) {
            errorMessage = L"旧版桌面项目移动日志已损坏。";
            return false;
        }
        LegacyJournalHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (header.itemIdLength > kMaximumJournalStringLength ||
            header.sourceLength > kMaximumJournalStringLength ||
            header.destinationLength > kMaximumJournalStringLength || header.releaseToDesktop > 1) {
            errorMessage = L"旧版桌面项目移动日志格式无效。";
            return false;
        }
        const size_t characterCount = static_cast<size_t>(header.itemIdLength) + header.sourceLength + header.destinationLength;
        if (sizeof(header) + characterCount * sizeof(wchar_t) != bytes.size()) {
            errorMessage = L"旧版桌面项目移动日志长度无效。";
            return false;
        }
        const wchar_t* characters = reinterpret_cast<const wchar_t*>(bytes.data() + sizeof(header));
        JournalEntry entry;
        entry.itemId.assign(characters, header.itemIdLength);
        characters += header.itemIdLength;
        entry.sourcePath.assign(characters, header.sourceLength);
        characters += header.sourceLength;
        entry.destinationPath.assign(characters, header.destinationLength);
        entry.releaseToDesktop = header.releaseToDesktop != 0;
        entries.push_back(std::move(entry));
        return true;
    }
    if (version != kJournalVersion) {
        errorMessage = L"桌面项目移动日志版本不受支持。";
        return false;
    }
    JournalBatchHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.entryCount == 0 || header.entryCount > kMaximumJournalEntryCount) {
        errorMessage = L"桌面项目移动日志条目数无效。";
        return false;
    }
    size_t offset = sizeof(header);
    for (std::uint32_t index = 0; index < header.entryCount; ++index) {
        if (offset > bytes.size() || bytes.size() - offset < sizeof(JournalEntryHeader)) {
            errorMessage = L"桌面项目移动日志长度无效。";
            return false;
        }
        JournalEntryHeader entryHeader{};
        std::memcpy(&entryHeader, bytes.data() + offset, sizeof(entryHeader));
        offset += sizeof(entryHeader);
        if (entryHeader.itemIdLength > kMaximumJournalStringLength ||
            entryHeader.sourceLength > kMaximumJournalStringLength ||
            entryHeader.destinationLength > kMaximumJournalStringLength ||
            entryHeader.releaseToDesktop > 1) {
            errorMessage = L"桌面项目移动日志条目无效。";
            return false;
        }
        const size_t characterCount = static_cast<size_t>(entryHeader.itemIdLength) +
            entryHeader.sourceLength + entryHeader.destinationLength;
        const size_t byteCount = characterCount * sizeof(wchar_t);
        if (offset > bytes.size() || byteCount > bytes.size() - offset) {
            errorMessage = L"桌面项目移动日志长度无效。";
            return false;
        }
        const wchar_t* characters = reinterpret_cast<const wchar_t*>(bytes.data() + offset);
        JournalEntry entry;
        entry.itemId.assign(characters, entryHeader.itemIdLength);
        characters += entryHeader.itemIdLength;
        entry.sourcePath.assign(characters, entryHeader.sourceLength);
        characters += entryHeader.sourceLength;
        entry.destinationPath.assign(characters, entryHeader.destinationLength);
        entry.releaseToDesktop = entryHeader.releaseToDesktop != 0;
        entries.push_back(std::move(entry));
        offset += byteCount;
    }
    if (offset != bytes.size()) {
        errorMessage = L"桌面项目移动日志包含多余数据。";
        return false;
    }
    return true;
}

bool ManagedShortcutStore::ClearJournal() const {
    DeleteFileW(journalTempPath_.c_str());
    return DeleteFileW(journalPath_.c_str()) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool ManagedShortcutStore::RecoverPending(ConfigStore& configStore, std::wstring& errorMessage) {
    errorMessage.clear();
    if (GetFileAttributesW(journalPath_.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    std::vector<JournalEntry> journals;
    if (!ReadJournal(journals, errorMessage)) {
        return false;
    }
    AppConfig config = configStore.LoadAppConfig();
    bool configChanged = false;
    for (const JournalEntry& journal : journals) {
        const bool safeEndpoints =
            (PathIsInside(journal.sourcePath, rootPath_) || IsDesktopPath(journal.sourcePath)) &&
            (PathIsInside(journal.destinationPath, rootPath_) || IsDesktopPath(journal.destinationPath)) &&
            (PathIsInside(journal.sourcePath, rootPath_) || PathIsInside(journal.destinationPath, rootPath_));
        if (!safeEndpoints) {
            errorMessage = L"移动日志中的路径超出应用托管目录和桌面，已停止自动恢复。";
            return false;
        }
        const bool sourceExists = GetFileAttributesW(journal.sourcePath.c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool destinationExists = GetFileAttributesW(journal.destinationPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (sourceExists && destinationExists) {
            errorMessage = L"移动事务的原位置和目标位置同时存在，已保留日志并停止自动覆盖。";
            return false;
        }
        if (!sourceExists && !destinationExists) {
            errorMessage = L"移动事务的原位置和目标位置都不存在，已保留日志等待人工确认。";
            return false;
        }
        auto registered = std::find_if(config.items.begin(), config.items.end(), [&](const ItemConfig& item) {
            return item.id == journal.itemId;
        });
        if (!sourceExists && destinationExists && registered != config.items.end() &&
            !PathsEqual(registered->path, journal.destinationPath)) {
            registered->path = journal.destinationPath;
            configChanged = true;
        } else if (sourceExists && !destinationExists && registered != config.items.end() &&
                   !PathsEqual(registered->path, journal.sourcePath)) {
            registered->path = journal.sourcePath;
            configChanged = true;
        }
    }
    if (configChanged && !configStore.SaveAppConfig(config)) {
        errorMessage = L"移动已经完成，但无法把实际文件位置写入配置；移动日志已保留。";
        return false;
    }
    ClearJournal();
    return true;
}
