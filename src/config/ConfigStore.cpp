#include "config/ConfigStore.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "util/PathUtil.h"
#include "util/StringUtil.h"

namespace {

int ParseInt(const std::wstring& value, int fallback) {
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

bool ParseBool(const std::wstring& value, bool fallback) {
    if (value == L"1" || value == L"true" || value == L"TRUE") {
        return true;
    }
    if (value == L"0" || value == L"false" || value == L"FALSE") {
        return false;
    }
    return fallback;
}

bool IsRegularFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void MigrateLegacyConfigDirectory(
    const std::wstring& roaming,
    const std::wstring& legacyFolderName,
    const std::wstring& destinationDirectory) {
    const std::wstring destinationConfig = JoinPath(destinationDirectory, L"config.ini");
    const std::wstring legacyDirectory = JoinPath(roaming, legacyFolderName);
    const std::wstring legacyConfig = JoinPath(legacyDirectory, L"config.ini");
    if (IsRegularFile(destinationConfig) || !IsRegularFile(legacyConfig)) {
        return;
    }
    const int createResult = SHCreateDirectoryExW(nullptr, destinationDirectory.c_str(), nullptr);
    if (createResult != ERROR_SUCCESS && createResult != ERROR_ALREADY_EXISTS && createResult != ERROR_FILE_EXISTS) {
        return;
    }

    std::vector<std::wstring> fileNames{
        L"config.ini",
        L"config.backup.ini",
        L"layout.profile.ini",
        L"layout.profile.ini.backup"};
    for (int index = 1; index <= 10; ++index) {
        fileNames.push_back(L"config.backup.ini." + std::to_wstring(index));
    }
    for (const std::wstring& fileName : fileNames) {
        const std::wstring source = JoinPath(legacyDirectory, fileName);
        const std::wstring destination = JoinPath(destinationDirectory, fileName);
        if (IsRegularFile(source)) {
            CopyFileW(source.c_str(), destination.c_str(), TRUE);
        }
    }
}

std::wstring Trim(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && iswspace(value.back())) {
        value.pop_back();
    }
    return value;
}

void ApplyConfigLine(WindowConfig& config, const std::wstring& line) {
    const size_t equals = line.find(L'=');
    if (equals == std::wstring::npos) {
        return;
    }

    const std::wstring key = Trim(line.substr(0, equals));
    const std::wstring value = Trim(line.substr(equals + 1));

    if (key == L"window.x") {
        config.x = ParseInt(value, config.x);
    } else if (key == L"window.y") {
        config.y = ParseInt(value, config.y);
    } else if (key == L"window.width") {
        config.width = ParseInt(value, config.width);
    } else if (key == L"window.height") {
        config.height = ParseInt(value, config.height);
    } else if (key == L"window.opacity") {
        config.opacity = ParseInt(value, config.opacity);
    } else if (key == L"window.normalHeight") {
        config.normalHeight = ParseInt(value, config.normalHeight);
    } else if (key == L"window.iconSize") {
        config.iconSize = ParseInt(value, config.iconSize);
    } else if (key == L"window.density") {
        config.density = ParseInt(value, config.density);
    } else if (key == L"window.viewMode") {
        config.viewMode = ParseInt(value, config.viewMode);
    } else if (key == L"window.contentViewMode") {
        config.contentViewMode = ParseInt(value, config.contentViewMode);
    } else if (key == L"window.sortMode") {
        config.sortMode = ParseInt(value, config.sortMode);
    } else if (key == L"window.tabSide") {
        config.tabSide = ParseInt(value, config.tabSide);
    } else if (key == L"window.titleOpacity") {
        config.titleOpacity = ParseInt(value, config.titleOpacity);
    } else if (key == L"window.dpi") {
        config.dpi = ParseInt(value, config.dpi);
    } else if (key == L"window.monitorId") {
        config.monitorId = value;
    } else if (key == L"window.collapsed") {
        config.collapsed = ParseBool(value, config.collapsed);
    } else if (key == L"window.locked") {
        config.locked = ParseBool(value, config.locked);
    } else if (key == L"window.showBorder") {
        config.showBorder = ParseBool(value, config.showBorder);
    } else if (key == L"window.autoArrange") {
        config.autoArrange = ParseBool(value, config.autoArrange);
    } else if (key == L"window.fixedExpanded") {
        config.fixedExpanded = ParseBool(value, config.fixedExpanded);
    }
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::map<std::wstring, std::wstring> ReadKeyValueFile(const std::wstring& path) {
    std::map<std::wstring, std::wstring> values;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return values;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        std::wstring key = Utf8ToWide(line.substr(0, equals));
        std::wstring value = Utf8ToWide(line.substr(equals + 1));
        values[Trim(key)] = Trim(value);
    }
    return values;
}

bool AtomicWriteConfig(
    const std::wstring& configDir,
    const std::wstring& configPath,
    const std::wstring& backupPath,
    const std::wstring& tempPath,
    int backupCount,
    const std::string& content) {
    if (!EnsureDirectoryExists(configDir)) {
        return false;
    }

    {
        std::ifstream existing(configPath, std::ios::binary);
        if (existing.is_open()) {
            std::ostringstream existingContent;
            existingContent << existing.rdbuf();
            if (existingContent.str() == content) {
                return true;
            }
        }
    }

    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            return false;
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output.good()) {
            return false;
        }
    }

    if (GetFileAttributesW(configPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        for (int index = backupCount - 1; index >= 1; --index) {
            const std::wstring source = index == 1
                ? backupPath
                : backupPath + L"." + std::to_wstring(index - 1);
            const std::wstring target = backupPath + L"." + std::to_wstring(index);
            MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        if (!CopyFileW(configPath.c_str(), backupPath.c_str(), FALSE)) {
            return false;
        }
        for (int index = backupCount; index <= 10; ++index) {
            const std::wstring stale = backupPath + L"." + std::to_wstring(index);
            DeleteFileW(stale.c_str());
        }
    }

    return MoveFileExW(
        tempPath.c_str(),
        configPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

struct InteractionMutation {
    std::wstring categoryId;
    WindowConfig layout;
    std::vector<std::wstring> itemIds;
    bool updateItemOrder = false;
    std::uint64_t version = 0;
};

std::mutex& InteractionMutationMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::wstring, std::map<std::wstring, InteractionMutation>>& PendingInteractionMutations() {
    static std::map<std::wstring, std::map<std::wstring, InteractionMutation>> mutations;
    return mutations;
}

std::uint64_t& NextInteractionMutationVersion() {
    static std::uint64_t version = 1;
    return version;
}

std::mutex& ConfigFileWriteMutex() {
    static std::mutex mutex;
    return mutex;
}

struct ConfigFileIdentity {
    DWORD volumeSerial = 0;
    DWORD fileIndexHigh = 0;
    DWORD fileIndexLow = 0;
    DWORD fileSizeHigh = 0;
    DWORD fileSizeLow = 0;
    FILETIME lastWriteTime{};
};

struct ConfigSnapshot {
    ConfigFileIdentity identity;
    AppConfig config;
};

std::mutex& ConfigSnapshotMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::wstring, ConfigSnapshot>& ConfigSnapshots() {
    static std::map<std::wstring, ConfigSnapshot> snapshots;
    return snapshots;
}

bool QueryConfigFileIdentity(
    const std::wstring& path,
    ConfigFileIdentity& identity) {
    HANDLE file = CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const bool succeeded = GetFileInformationByHandle(file, &information) != FALSE;
    CloseHandle(file);
    if (!succeeded) {
        return false;
    }
    identity.volumeSerial = information.dwVolumeSerialNumber;
    identity.fileIndexHigh = information.nFileIndexHigh;
    identity.fileIndexLow = information.nFileIndexLow;
    identity.fileSizeHigh = information.nFileSizeHigh;
    identity.fileSizeLow = information.nFileSizeLow;
    identity.lastWriteTime = information.ftLastWriteTime;
    return true;
}

bool SameConfigFileIdentity(
    const ConfigFileIdentity& left,
    const ConfigFileIdentity& right) {
    return left.volumeSerial == right.volumeSerial &&
           left.fileIndexHigh == right.fileIndexHigh &&
           left.fileIndexLow == right.fileIndexLow &&
           left.fileSizeHigh == right.fileSizeHigh &&
           left.fileSizeLow == right.fileSizeLow &&
           CompareFileTime(&left.lastWriteTime, &right.lastWriteTime) == 0;
}

bool TryLoadConfigSnapshot(
    const std::wstring& path,
    const ConfigFileIdentity& identity,
    AppConfig& config) {
    std::lock_guard<std::mutex> lock(ConfigSnapshotMutex());
    const auto snapshot = ConfigSnapshots().find(path);
    if (snapshot == ConfigSnapshots().end() ||
        !SameConfigFileIdentity(snapshot->second.identity, identity)) {
        return false;
    }
    config = snapshot->second.config;
    return true;
}

void PublishConfigSnapshot(
    const std::wstring& path,
    const ConfigFileIdentity& identity,
    const AppConfig& config) {
    std::lock_guard<std::mutex> lock(ConfigSnapshotMutex());
    ConfigSnapshots().insert_or_assign(path, ConfigSnapshot{identity, config});
}

std::vector<InteractionMutation> SnapshotInteractionMutations(const std::wstring& configPath) {
    std::lock_guard<std::mutex> lock(InteractionMutationMutex());
    std::vector<InteractionMutation> result;
    const auto byPath = PendingInteractionMutations().find(configPath);
    if (byPath == PendingInteractionMutations().end()) {
        return result;
    }
    result.reserve(byPath->second.size());
    for (const auto& [categoryId, mutation] : byPath->second) {
        (void)categoryId;
        result.push_back(mutation);
    }
    return result;
}

void ApplyInteractionMutations(
    const std::vector<InteractionMutation>& mutations,
    AppConfig& config) {
    for (const InteractionMutation& mutation : mutations) {
        if (mutation.categoryId == L"uncategorized") {
            config.window = mutation.layout;
            if (mutation.updateItemOrder) {
                config.uncategorizedItemIds = mutation.itemIds;
            }
            continue;
        }
        const auto category = std::find_if(
            config.categories.begin(),
            config.categories.end(),
            [&](const CategoryConfig& value) {
                return value.id == mutation.categoryId;
            });
        if (category == config.categories.end()) {
            continue;
        }
        category->layout = mutation.layout;
        if (mutation.updateItemOrder) {
            category->itemIds = mutation.itemIds;
        }
    }
}

void RemoveAppliedInteractionMutations(
    const std::wstring& configPath,
    const std::vector<InteractionMutation>& applied) {
    std::lock_guard<std::mutex> lock(InteractionMutationMutex());
    const auto byPath = PendingInteractionMutations().find(configPath);
    if (byPath == PendingInteractionMutations().end()) {
        return;
    }
    for (const InteractionMutation& mutation : applied) {
        const auto current = byPath->second.find(mutation.categoryId);
        if (current != byPath->second.end() && current->second.version == mutation.version) {
            byPath->second.erase(current);
        }
    }
    if (byPath->second.empty()) {
        PendingInteractionMutations().erase(byPath);
    }
}

class AsyncConfigWriter {
public:
    static AsyncConfigWriter& Instance() {
        static AsyncConfigWriter writer;
        return writer;
    }

    bool Enqueue(std::wstring key, std::function<bool()> task) {
        if (key.empty() || !task) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return false;
            }
            for (Job& pending : jobs_) {
                if (pending.key == key) {
                    pending.task = std::move(task);
                    return true;
                }
            }
            if (jobs_.size() >= kCapacity) {
                return false;
            }
            jobs_.push_back(Job{std::move(key), std::move(task)});
        }
        condition_.notify_one();
        return true;
    }

    bool Drain(DWORD timeoutMilliseconds) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto idle = [&]() { return jobs_.empty() && !active_; };
        const bool completed = timeoutMilliseconds == INFINITE
            ? (drained_.wait(lock, idle), true)
            : drained_.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds), idle);
        return completed && lastWriteSucceeded_;
    }

private:
    struct Job {
        std::wstring key;
        std::function<bool()> task;
    };
    static constexpr size_t kCapacity = 32;

    AsyncConfigWriter() : worker_(&AsyncConfigWriter::WorkerLoop, this) {}
    ~AsyncConfigWriter() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void WorkerLoop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&]() { return stopping_ || !jobs_.empty(); });
                if (stopping_ && jobs_.empty()) {
                    break;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
                active_ = true;
            }
            bool succeeded = false;
            try {
                succeeded = job.task();
            } catch (...) {
                succeeded = false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                lastWriteSucceeded_ = succeeded;
                active_ = false;
                if (jobs_.empty()) {
                    drained_.notify_all();
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        drained_.notify_all();
    }

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable drained_;
    std::deque<Job> jobs_;
    bool active_ = false;
    bool stopping_ = false;
    bool lastWriteSucceeded_ = true;
};

}  // namespace

std::wstring CategoryStorageFolder(const AppConfig& config, const std::wstring& categoryId) {
    if (CompareStringOrdinal(categoryId.c_str(), -1, L"uncategorized", -1, TRUE) == CSTR_EQUAL) {
        return config.uncategorizedStorageFolder.empty()
            ? std::wstring(L"uncategorized")
            : config.uncategorizedStorageFolder;
    }
    const auto category = std::find_if(
        config.categories.begin(),
        config.categories.end(),
        [&](const CategoryConfig& value) {
            return CompareStringOrdinal(value.id.c_str(), -1, categoryId.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
    if (category == config.categories.end()) {
        return categoryId;
    }
    return category->storageFolder.empty() ? category->id : category->storageFolder;
}

ConfigStore::ConfigStore() {
    wchar_t overrideDir[MAX_PATH]{};
    const DWORD overrideLength = GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_CONFIG_DIR", overrideDir, MAX_PATH);
    if (overrideLength > 0 && overrideLength < MAX_PATH) {
        configDir_ = overrideDir;
    } else {
        wchar_t roamingOverride[MAX_PATH]{};
        const DWORD roamingOverrideLength = GetEnvironmentVariableW(
            L"LATTICE_ROAMING_DIR", roamingOverride, MAX_PATH);
        const std::wstring roaming = roamingOverrideLength > 0 && roamingOverrideLength < MAX_PATH
            ? std::wstring(roamingOverride, roamingOverrideLength)
            : KnownFolderPath(FOLDERID_RoamingAppData);
        configDir_ = JoinPath(roaming, L"Lattice");
        MigrateLegacyConfigDirectory(roaming, L"Luno", configDir_);
        MigrateLegacyConfigDirectory(roaming, L"DesktopOrganizer", configDir_);
    }
    configPath_ = JoinPath(configDir_, L"config.ini");
    backupPath_ = JoinPath(configDir_, L"config.backup.ini");
    tempPath_ = JoinPath(configDir_, L"config.tmp");
}

WindowConfig ConfigStore::Load() const {
    return LoadAppConfig().window;
}

bool ConfigStore::Save(const WindowConfig& config) const {
    AppConfig appConfig = LoadAppConfig();
    appConfig.window = config;
    return SaveAppConfig(appConfig);
}

AppConfig ConfigStore::LoadAppConfig() const {
    ConfigFileIdentity identityBefore{};
    AppConfig config;
    if (QueryConfigFileIdentity(configPath_, identityBefore) &&
        TryLoadConfigSnapshot(configPath_, identityBefore, config)) {
        ApplyInteractionMutations(SnapshotInteractionMutations(configPath_), config);
        return config;
    }

    config = LoadAppConfigFromDisk();
    ConfigFileIdentity identityAfter{};
    if (QueryConfigFileIdentity(configPath_, identityAfter)) {
        if (SameConfigFileIdentity(identityBefore, identityAfter)) {
            PublishConfigSnapshot(configPath_, identityAfter, config);
        } else {
            config = LoadAppConfigFromDisk();
            ConfigFileIdentity retryIdentity{};
            if (QueryConfigFileIdentity(configPath_, retryIdentity)) {
                PublishConfigSnapshot(configPath_, retryIdentity, config);
            }
        }
    }
    ApplyInteractionMutations(SnapshotInteractionMutations(configPath_), config);
    return config;
}

AppConfig ConfigStore::LoadAppConfigFromDisk() const {
    AppConfig config;
    config.window = WindowConfig{};
    config.currentCategoryId = L"uncategorized";

    auto values = ReadKeyValueFile(configPath_);
    const auto isReadable = [](const std::map<std::wstring, std::wstring>& candidate) {
        const auto schemaIt = candidate.find(L"schemaVersion");
        if (schemaIt == candidate.end()) {
            return false;
        }
        return ParseInt(schemaIt->second, 0) > 0;
    };
    if (!isReadable(values)) {
        std::vector<std::wstring> candidates;
        candidates.push_back(backupPath_);
        for (int index = 1; index <= 9; ++index) {
            candidates.push_back(backupPath_ + L"." + std::to_wstring(index));
        }
        for (const std::wstring& candidatePath : candidates) {
            const auto backupValues = ReadKeyValueFile(candidatePath);
            if (isReadable(backupValues)) {
                values = backupValues;
                break;
            }
        }
    }

    const auto schemaIt = values.find(L"schemaVersion");
    const int schemaVersion = schemaIt == values.end() ? 0 : ParseInt(schemaIt->second, 0);
    for (const auto& [key, value] : values) {
        ApplyConfigLine(config.window, key + L"=" + value);
    }
    if (schemaVersion > 0 && schemaVersion < 7) {
        config.window.viewMode = 1;
    }
    const WindowConfig defaultWindow;
    if (config.window.width < 320) {
        config.window.width = defaultWindow.width;
    }
    if (!config.window.collapsed && config.window.height < 220) {
        config.window.height = defaultWindow.height;
    }
    if (config.window.opacity < 80 || config.window.opacity > 255) {
        config.window.opacity = 235;
    }
    if (config.window.normalHeight < 220) {
        config.window.normalHeight = config.window.height;
    }
    if (config.window.iconSize < 32 || config.window.iconSize > 72) {
        config.window.iconSize = 48;
    }
    config.window.density = std::clamp(config.window.density, 0, 2);
    config.window.viewMode = std::clamp(config.window.viewMode, 0, 1);
    config.window.contentViewMode = std::clamp(config.window.contentViewMode, 0, 1);
    config.window.sortMode = std::clamp(config.window.sortMode, 0, 3);
    config.window.tabSide = std::clamp(config.window.tabSide, 0, 3);
    config.window.titleOpacity = std::clamp(config.window.titleOpacity, 80, 255);
    const auto readBool = [&](const std::wstring& key, bool fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : ParseBool(it->second, fallback);
    };
    const auto readInt = [&](const std::wstring& key, int fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : ParseInt(it->second, fallback);
    };
    config.settings.launchOnStartup = readBool(L"settings.launchOnStartup", config.settings.launchOnStartup);
    config.settings.showPublicDesktopItems = readBool(L"settings.showPublicDesktopItems", config.settings.showPublicDesktopItems);
    config.settings.restoreHiddenState = readBool(L"settings.restoreHiddenState", config.settings.restoreHiddenState);
    config.settings.startHidden = readBool(L"settings.startHidden", config.settings.startHidden);
    config.settings.lastVisible = readBool(L"settings.lastVisible", config.settings.lastVisible);
    config.settings.singleClickOpen = readBool(L"settings.singleClickOpen", config.settings.singleClickOpen);
    config.settings.backupCount = std::clamp(readInt(L"settings.backupCount", config.settings.backupCount), 1, 10);
    config.settings.iconCacheSize = std::clamp(readInt(L"settings.iconCacheSize", config.settings.iconCacheSize), 64, 4096);
    config.settings.theme = std::clamp(readInt(L"settings.theme", config.settings.theme), 0, 2);
    const auto currentIt = values.find(L"currentCategoryId");
    if (currentIt != values.end() && !currentIt->second.empty()) {
        config.currentCategoryId = currentIt->second;
    }
    const auto uncategorizedNameIt = values.find(L"uncategorized.name");
    if (uncategorizedNameIt != values.end() && !uncategorizedNameIt->second.empty()) {
        config.uncategorizedName = uncategorizedNameIt->second;
    }
    const auto uncategorizedStorageIt = values.find(L"uncategorized.storageFolder");
    if (uncategorizedStorageIt != values.end() && !uncategorizedStorageIt->second.empty()) {
        config.uncategorizedStorageFolder = uncategorizedStorageIt->second;
    }

    int itemCount = 0;
    const auto registryCountIt = values.find(L"item.count");
    if (registryCountIt != values.end()) {
        itemCount = ParseInt(registryCountIt->second, 0);
    }
    for (int itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
        const std::wstring prefix = L"item." + std::to_wstring(itemIndex) + L".";
        const auto idIt = values.find(prefix + L"id");
        const auto pathIt = values.find(prefix + L"path");
        if (idIt != values.end() && pathIt != values.end() && !idIt->second.empty() && !pathIt->second.empty()) {
            ItemConfig item;
            item.id = idIt->second;
            item.path = pathIt->second;
            const auto displayNameIt = values.find(prefix + L"displayName");
            if (displayNameIt != values.end()) {
                item.displayName = displayNameIt->second;
            }
            const auto originalPathIt = values.find(prefix + L"originalDesktopPath");
            if (originalPathIt != values.end()) {
                item.originalDesktopPath = originalPathIt->second;
            }
            item.desktopX = readInt(prefix + L"desktopX", 0);
            item.desktopY = readInt(prefix + L"desktopY", 0);
            item.hasDesktopPosition = readBool(prefix + L"hasDesktopPosition", false);
            item.desktopVisibilityMode = readInt(prefix + L"desktopVisibilityMode", 0);
            item.desktopVisibilityOriginalFlags = readInt(prefix + L"desktopVisibilityOriginalFlags", 0);
            item.desktopVisibilityNewStartValue = readInt(prefix + L"desktopVisibilityNewStartValue", -1);
            item.desktopVisibilityClassicValue = readInt(prefix + L"desktopVisibilityClassicValue", -1);
            config.items.push_back(std::move(item));
        }
    }

    const int desktopLayoutCount = readInt(L"desktopLayout.count", 0);
    for (int layoutIndex = 0; layoutIndex < desktopLayoutCount; ++layoutIndex) {
        const std::wstring prefix = L"desktopLayout." + std::to_wstring(layoutIndex) + L".";
        const auto pathIt = values.find(prefix + L"path");
        if (pathIt == values.end() || pathIt->second.empty()) {
            continue;
        }
        DesktopPlacementConfig placement;
        placement.path = pathIt->second;
        placement.x = readInt(prefix + L"x", 0);
        placement.y = readInt(prefix + L"y", 0);
        config.desktopLayout.push_back(std::move(placement));
    }

    int uncategorizedItemCount = 0;
    const auto uncategorizedCountIt = values.find(L"uncategorized.item.count");
    if (uncategorizedCountIt != values.end()) {
        uncategorizedItemCount = ParseInt(uncategorizedCountIt->second, 0);
    }
    for (int itemIndex = 0; itemIndex < uncategorizedItemCount; ++itemIndex) {
        const auto itemIt = values.find(L"uncategorized.item." + std::to_wstring(itemIndex));
        if (itemIt != values.end() && !itemIt->second.empty()) {
            config.uncategorizedItemIds.push_back(itemIt->second);
        }
    }

    int categoryCount = 0;
    const auto countIt = values.find(L"category.count");
    if (countIt != values.end()) {
        categoryCount = ParseInt(countIt->second, 0);
    }

    for (int index = 0; index < categoryCount; ++index) {
        const std::wstring prefix = L"category." + std::to_wstring(index) + L".";
        const auto idIt = values.find(prefix + L"id");
        const auto nameIt = values.find(prefix + L"name");
        if (idIt == values.end() || idIt->second.empty() || nameIt == values.end() || nameIt->second.empty()) {
            continue;
        }

        CategoryConfig category;
        category.id = idIt->second;
        category.name = nameIt->second;
        const auto storageIt = values.find(prefix + L"storageFolder");
        category.storageFolder = storageIt == values.end() || storageIt->second.empty()
            ? category.id
            : storageIt->second;
        const auto colorIt = values.find(prefix + L"color");
        if (colorIt != values.end() && !colorIt->second.empty()) {
            category.color = colorIt->second;
        }
        const auto iconIt = values.find(prefix + L"icon");
        if (iconIt != values.end() && !iconIt->second.empty()) {
            category.icon = iconIt->second;
        }
        category.tileCollapsed = readBool(prefix + L"tileCollapsed", category.tileCollapsed);
        category.layout.x = readInt(prefix + L"x", category.layout.x);
        category.layout.y = readInt(prefix + L"y", category.layout.y);
        category.layout.width = readInt(prefix + L"width", category.layout.width);
        category.layout.height = readInt(prefix + L"height", category.layout.height);
        category.layout.opacity = readInt(prefix + L"opacity", category.layout.opacity);
        category.layout.normalHeight = readInt(prefix + L"normalHeight", category.layout.normalHeight);
        category.layout.iconSize = readInt(prefix + L"iconSize", category.layout.iconSize);
        category.layout.density = readInt(prefix + L"density", category.layout.density);
        category.layout.contentViewMode = std::clamp(readInt(prefix + L"contentViewMode", category.layout.contentViewMode), 0, 1);
        category.layout.sortMode = std::clamp(readInt(prefix + L"sortMode", category.layout.sortMode), 0, 3);
        category.layout.tabSide = std::clamp(readInt(prefix + L"tabSide", category.layout.tabSide), 0, 3);
        category.layout.titleOpacity = std::clamp(readInt(prefix + L"titleOpacity", category.layout.titleOpacity), 80, 255);
        category.layout.dpi = readInt(prefix + L"dpi", category.layout.dpi);
        const auto monitorIt = values.find(prefix + L"monitorId");
        if (monitorIt != values.end()) {
            category.layout.monitorId = monitorIt->second;
        }
        category.layout.collapsed = readBool(prefix + L"collapsed", category.layout.collapsed);
        category.layout.locked = readBool(prefix + L"locked", category.layout.locked);
        category.layout.showBorder = readBool(prefix + L"showBorder", category.layout.showBorder);
        category.layout.autoArrange = readBool(prefix + L"autoArrange", category.layout.autoArrange);
        category.layout.fixedExpanded = readBool(prefix + L"fixedExpanded", category.layout.fixedExpanded);

        int categoryItemCount = 0;
        const auto itemCountIt = values.find(prefix + L"item.count");
        if (itemCountIt != values.end()) {
            categoryItemCount = ParseInt(itemCountIt->second, 0);
        }
        for (int itemIndex = 0; itemIndex < categoryItemCount; ++itemIndex) {
            const auto itemIt = values.find(prefix + L"item." + std::to_wstring(itemIndex));
            if (itemIt != values.end() && !itemIt->second.empty()) {
                category.itemIds.push_back(itemIt->second);
            }
        }
        config.categories.push_back(std::move(category));
    }
    return config;
}

bool ConfigStore::SaveAppConfig(const AppConfig& config) const {
    std::lock_guard<std::mutex> fileLock(ConfigFileWriteMutex());
    const std::vector<InteractionMutation> mutations =
        SnapshotInteractionMutations(configPath_);
    AppConfig merged = config;
    ApplyInteractionMutations(mutations, merged);
    if (!SaveAppConfigToDisk(merged)) {
        return false;
    }
    RemoveAppliedInteractionMutations(configPath_, mutations);
    return true;
}

bool ConfigStore::SaveInteractionStateAsync(
    const std::wstring& categoryId,
    const WindowConfig& layout,
    const std::vector<std::wstring>& itemIds,
    bool updateItemOrder) const {
    if (categoryId.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(InteractionMutationMutex());
        std::uint64_t& nextVersion = NextInteractionMutationVersion();
        if (nextVersion == 0) {
            ++nextVersion;
        }
        InteractionMutation mutation;
        mutation.categoryId = categoryId;
        mutation.layout = layout;
        mutation.itemIds = itemIds;
        mutation.updateItemOrder = updateItemOrder;
        mutation.version = nextVersion++;
        PendingInteractionMutations()[configPath_].insert_or_assign(
            categoryId,
            std::move(mutation));
    }
    const ConfigStore store = *this;
    return AsyncConfigWriter::Instance().Enqueue(
        configPath_,
        [store]() { return store.FlushInteractionStateToDisk(); });
}

bool ConfigStore::DrainPendingWrites(unsigned long timeoutMilliseconds) {
    return AsyncConfigWriter::Instance().Drain(timeoutMilliseconds);
}

bool ConfigStore::FlushInteractionStateToDisk() const {
    std::lock_guard<std::mutex> fileLock(ConfigFileWriteMutex());
    const std::vector<InteractionMutation> mutations =
        SnapshotInteractionMutations(configPath_);
    if (mutations.empty()) {
        return true;
    }
    AppConfig config = LoadAppConfigFromDisk();
    ApplyInteractionMutations(mutations, config);
    if (!SaveAppConfigToDisk(config)) {
        return false;
    }
    RemoveAppliedInteractionMutations(configPath_, mutations);
    return true;
}

bool ConfigStore::SaveAppConfigToDisk(const AppConfig& config) const {
    std::ostringstream output;
    output << "schemaVersion=12\n";
    output << "settings.launchOnStartup=" << (config.settings.launchOnStartup ? 1 : 0) << "\n";
    output << "settings.showPublicDesktopItems=" << (config.settings.showPublicDesktopItems ? 1 : 0) << "\n";
    output << "settings.restoreHiddenState=" << (config.settings.restoreHiddenState ? 1 : 0) << "\n";
    output << "settings.startHidden=" << (config.settings.startHidden ? 1 : 0) << "\n";
    output << "settings.lastVisible=" << (config.settings.lastVisible ? 1 : 0) << "\n";
    output << "settings.singleClickOpen=" << (config.settings.singleClickOpen ? 1 : 0) << "\n";
    output << "settings.backupCount=" << config.settings.backupCount << "\n";
    output << "settings.iconCacheSize=" << std::clamp(config.settings.iconCacheSize, 64, 4096) << "\n";
    output << "settings.theme=" << config.settings.theme << "\n";
    output << "window.x=" << config.window.x << "\n";
    output << "window.y=" << config.window.y << "\n";
    output << "window.width=" << config.window.width << "\n";
    output << "window.height=" << config.window.height << "\n";
    output << "window.opacity=" << config.window.opacity << "\n";
    output << "window.normalHeight=" << config.window.normalHeight << "\n";
    output << "window.iconSize=" << config.window.iconSize << "\n";
    output << "window.density=" << std::clamp(config.window.density, 0, 2) << "\n";
    output << "window.viewMode=" << std::clamp(config.window.viewMode, 0, 1) << "\n";
    output << "window.contentViewMode=" << std::clamp(config.window.contentViewMode, 0, 1) << "\n";
    output << "window.sortMode=" << std::clamp(config.window.sortMode, 0, 3) << "\n";
    output << "window.tabSide=" << std::clamp(config.window.tabSide, 0, 3) << "\n";
    output << "window.titleOpacity=" << std::clamp(config.window.titleOpacity, 80, 255) << "\n";
    output << "window.dpi=" << config.window.dpi << "\n";
    output << "window.monitorId=" << WideToUtf8(config.window.monitorId) << "\n";
    output << "window.collapsed=" << (config.window.collapsed ? 1 : 0) << "\n";
    output << "window.locked=" << (config.window.locked ? 1 : 0) << "\n";
    output << "window.showBorder=" << (config.window.showBorder ? 1 : 0) << "\n";
    output << "window.autoArrange=" << (config.window.autoArrange ? 1 : 0) << "\n";
    output << "window.fixedExpanded=" << (config.window.fixedExpanded ? 1 : 0) << "\n";
    output << "currentCategoryId=" << WideToUtf8(config.currentCategoryId) << "\n";
    output << "uncategorized.name=" << WideToUtf8(config.uncategorizedName.empty() ? L"未分类" : config.uncategorizedName) << "\n";
    output << "uncategorized.storageFolder=" << WideToUtf8(
        config.uncategorizedStorageFolder.empty() ? L"uncategorized" : config.uncategorizedStorageFolder) << "\n";
    output << "item.count=" << config.items.size() << "\n";
    for (size_t itemIndex = 0; itemIndex < config.items.size(); ++itemIndex) {
        output << "item." << itemIndex << ".id=" << WideToUtf8(config.items[itemIndex].id) << "\n";
        output << "item." << itemIndex << ".path=" << WideToUtf8(config.items[itemIndex].path) << "\n";
        output << "item." << itemIndex << ".displayName=" << WideToUtf8(config.items[itemIndex].displayName) << "\n";
        output << "item." << itemIndex << ".originalDesktopPath=" << WideToUtf8(config.items[itemIndex].originalDesktopPath) << "\n";
        output << "item." << itemIndex << ".desktopX=" << config.items[itemIndex].desktopX << "\n";
        output << "item." << itemIndex << ".desktopY=" << config.items[itemIndex].desktopY << "\n";
        output << "item." << itemIndex << ".hasDesktopPosition=" << (config.items[itemIndex].hasDesktopPosition ? 1 : 0) << "\n";
        output << "item." << itemIndex << ".desktopVisibilityMode=" << config.items[itemIndex].desktopVisibilityMode << "\n";
        output << "item." << itemIndex << ".desktopVisibilityOriginalFlags=" << config.items[itemIndex].desktopVisibilityOriginalFlags << "\n";
        output << "item." << itemIndex << ".desktopVisibilityNewStartValue=" << config.items[itemIndex].desktopVisibilityNewStartValue << "\n";
        output << "item." << itemIndex << ".desktopVisibilityClassicValue=" << config.items[itemIndex].desktopVisibilityClassicValue << "\n";
    }
    output << "desktopLayout.count=" << config.desktopLayout.size() << "\n";
    for (size_t layoutIndex = 0; layoutIndex < config.desktopLayout.size(); ++layoutIndex) {
        output << "desktopLayout." << layoutIndex << ".path=" << WideToUtf8(config.desktopLayout[layoutIndex].path) << "\n";
        output << "desktopLayout." << layoutIndex << ".x=" << config.desktopLayout[layoutIndex].x << "\n";
        output << "desktopLayout." << layoutIndex << ".y=" << config.desktopLayout[layoutIndex].y << "\n";
    }
    output << "uncategorized.item.count=" << config.uncategorizedItemIds.size() << "\n";
    for (size_t itemIndex = 0; itemIndex < config.uncategorizedItemIds.size(); ++itemIndex) {
        output << "uncategorized.item." << itemIndex << "=" << WideToUtf8(config.uncategorizedItemIds[itemIndex]) << "\n";
    }
    output << "category.count=" << config.categories.size() << "\n";

    for (size_t index = 0; index < config.categories.size(); ++index) {
        const CategoryConfig& category = config.categories[index];
        output << "category." << index << ".id=" << WideToUtf8(category.id) << "\n";
        output << "category." << index << ".name=" << WideToUtf8(category.name) << "\n";
        output << "category." << index << ".storageFolder=" << WideToUtf8(
            category.storageFolder.empty() ? category.id : category.storageFolder) << "\n";
        output << "category." << index << ".color=" << WideToUtf8(category.color) << "\n";
        output << "category." << index << ".icon=" << WideToUtf8(category.icon) << "\n";
        output << "category." << index << ".tileCollapsed=" << (category.tileCollapsed ? 1 : 0) << "\n";
        output << "category." << index << ".x=" << category.layout.x << "\n";
        output << "category." << index << ".y=" << category.layout.y << "\n";
        output << "category." << index << ".width=" << category.layout.width << "\n";
        output << "category." << index << ".height=" << category.layout.height << "\n";
        output << "category." << index << ".opacity=" << category.layout.opacity << "\n";
        output << "category." << index << ".normalHeight=" << category.layout.normalHeight << "\n";
        output << "category." << index << ".iconSize=" << category.layout.iconSize << "\n";
        output << "category." << index << ".density=" << std::clamp(category.layout.density, 0, 2) << "\n";
        output << "category." << index << ".contentViewMode=" << std::clamp(category.layout.contentViewMode, 0, 1) << "\n";
        output << "category." << index << ".sortMode=" << std::clamp(category.layout.sortMode, 0, 3) << "\n";
        output << "category." << index << ".tabSide=" << std::clamp(category.layout.tabSide, 0, 3) << "\n";
        output << "category." << index << ".titleOpacity=" << std::clamp(category.layout.titleOpacity, 80, 255) << "\n";
        output << "category." << index << ".dpi=" << category.layout.dpi << "\n";
        output << "category." << index << ".monitorId=" << WideToUtf8(category.layout.monitorId) << "\n";
        output << "category." << index << ".collapsed=" << (category.layout.collapsed ? 1 : 0) << "\n";
        output << "category." << index << ".locked=" << (category.layout.locked ? 1 : 0) << "\n";
        output << "category." << index << ".showBorder=" << (category.layout.showBorder ? 1 : 0) << "\n";
        output << "category." << index << ".autoArrange=" << (category.layout.autoArrange ? 1 : 0) << "\n";
        output << "category." << index << ".fixedExpanded=" << (category.layout.fixedExpanded ? 1 : 0) << "\n";
        output << "category." << index << ".item.count=" << category.itemIds.size() << "\n";
        for (size_t itemIndex = 0; itemIndex < category.itemIds.size(); ++itemIndex) {
            output << "category." << index << ".item." << itemIndex << "=" << WideToUtf8(category.itemIds[itemIndex]) << "\n";
        }
    }

    const bool saved = AtomicWriteConfig(
        configDir_,
        configPath_,
        backupPath_,
        tempPath_,
        std::clamp(config.settings.backupCount, 1, 10),
        output.str());
    if (saved) {
        ConfigFileIdentity identity{};
        if (QueryConfigFileIdentity(configPath_, identity)) {
            PublishConfigSnapshot(configPath_, identity, config);
        }
    }
    return saved;
}

bool ConfigStore::ExportAppConfig(const std::wstring& path) const {
    if (path.empty()) {
        return false;
    }
    if (!DrainPendingWrites(5000) && !SaveAppConfig(LoadAppConfig())) {
        return false;
    }
    if (GetFileAttributesW(configPath_.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!SaveAppConfig(LoadAppConfig())) {
            return false;
        }
    }
    return CopyFileW(configPath_.c_str(), path.c_str(), TRUE) != FALSE;
}

bool ConfigStore::ImportAppConfig(const std::wstring& path) const {
    if (path.empty()) {
        return false;
    }
    const auto candidate = ReadKeyValueFile(path);
    const auto schemaIt = candidate.find(L"schemaVersion");
    if (schemaIt == candidate.end() || ParseInt(schemaIt->second, 0) <= 0) {
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    std::ostringstream content;
    content << input.rdbuf();
    const auto backupIt = candidate.find(L"settings.backupCount");
    const int backupCount = backupIt == candidate.end() ? 3 : std::clamp(ParseInt(backupIt->second, 3), 1, 10);
    if (!DrainPendingWrites(5000)) {
        return false;
    }
    std::lock_guard<std::mutex> fileLock(ConfigFileWriteMutex());
    return AtomicWriteConfig(configDir_, configPath_, backupPath_, tempPath_, backupCount, content.str());
}

bool ConfigStore::ExportCategoryConfig(const CategoryConfig& category, const std::wstring& path) const {
    if (path.empty() || category.id.empty() || category.name.empty()) {
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    output << "categoryFormat=1\n";
    output << "id=" << WideToUtf8(category.id) << "\n";
    output << "name=" << WideToUtf8(category.name) << "\n";
    output << "color=" << WideToUtf8(category.color) << "\n";
    output << "icon=" << WideToUtf8(category.icon) << "\n";
    output << "tileCollapsed=" << (category.tileCollapsed ? 1 : 0) << "\n";
    output << "x=" << category.layout.x << "\n";
    output << "y=" << category.layout.y << "\n";
    output << "width=" << category.layout.width << "\n";
    output << "height=" << category.layout.height << "\n";
    output << "opacity=" << category.layout.opacity << "\n";
    output << "normalHeight=" << category.layout.normalHeight << "\n";
    output << "iconSize=" << category.layout.iconSize << "\n";
    output << "density=" << category.layout.density << "\n";
    output << "tabSide=" << std::clamp(category.layout.tabSide, 0, 3) << "\n";
    output << "titleOpacity=" << std::clamp(category.layout.titleOpacity, 80, 255) << "\n";
    output << "dpi=" << category.layout.dpi << "\n";
    output << "monitorId=" << WideToUtf8(category.layout.monitorId) << "\n";
    output << "collapsed=" << (category.layout.collapsed ? 1 : 0) << "\n";
    output << "locked=" << (category.layout.locked ? 1 : 0) << "\n";
    output << "showBorder=" << (category.layout.showBorder ? 1 : 0) << "\n";
    output << "item.count=" << category.itemIds.size() << "\n";
    for (size_t index = 0; index < category.itemIds.size(); ++index) {
        output << "item." << index << "=" << WideToUtf8(category.itemIds[index]) << "\n";
    }
    return output.good();
}

bool ConfigStore::ImportCategoryConfig(const std::wstring& path, CategoryConfig& category) const {
    if (path.empty()) {
        return false;
    }
    const auto values = ReadKeyValueFile(path);
    const auto formatIt = values.find(L"categoryFormat");
    const auto idIt = values.find(L"id");
    const auto nameIt = values.find(L"name");
    if (formatIt == values.end() || formatIt->second != L"1" || idIt == values.end() || idIt->second.empty() ||
        nameIt == values.end() || nameIt->second.empty()) {
        return false;
    }
    category = CategoryConfig{};
    category.id = idIt->second;
    category.name = nameIt->second;
    const auto readString = [&](const std::wstring& key, const std::wstring& fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    };
    const auto readInt = [&](const std::wstring& key, int fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : ParseInt(it->second, fallback);
    };
    const auto readBool = [&](const std::wstring& key, bool fallback) {
        const auto it = values.find(key);
        return it == values.end() ? fallback : ParseBool(it->second, fallback);
    };
    category.color = readString(L"color", category.color);
    category.icon = readString(L"icon", category.icon);
    category.tileCollapsed = readBool(L"tileCollapsed", category.tileCollapsed);
    category.layout.x = readInt(L"x", category.layout.x);
    category.layout.y = readInt(L"y", category.layout.y);
    category.layout.width = readInt(L"width", category.layout.width);
    category.layout.height = readInt(L"height", category.layout.height);
    category.layout.opacity = readInt(L"opacity", category.layout.opacity);
    category.layout.normalHeight = readInt(L"normalHeight", category.layout.normalHeight);
    category.layout.iconSize = readInt(L"iconSize", category.layout.iconSize);
    category.layout.density = std::clamp(readInt(L"density", category.layout.density), 0, 2);
    category.layout.tabSide = std::clamp(readInt(L"tabSide", category.layout.tabSide), 0, 3);
    category.layout.titleOpacity = std::clamp(readInt(L"titleOpacity", category.layout.titleOpacity), 80, 255);
    category.layout.dpi = readInt(L"dpi", category.layout.dpi);
    category.layout.monitorId = readString(L"monitorId", category.layout.monitorId);
    category.layout.collapsed = readBool(L"collapsed", category.layout.collapsed);
    category.layout.locked = readBool(L"locked", category.layout.locked);
    category.layout.showBorder = readBool(L"showBorder", category.layout.showBorder);
    const int itemCount = std::max(0, readInt(L"item.count", 0));
    for (int index = 0; index < itemCount; ++index) {
        const auto itemIt = values.find(L"item." + std::to_wstring(index));
        if (itemIt != values.end() && !itemIt->second.empty()) {
            category.itemIds.push_back(itemIt->second);
        }
    }
    return true;
}

bool ConfigStore::SaveLayoutProfile(const WindowConfig& config) const {
    const std::wstring profilePath = configDir_ + L"\\layout.profile.ini";
    const std::wstring profileBackupPath = profilePath + L".backup";
    const std::wstring profileTempPath = profilePath + L".tmp";
    std::ostringstream output;
    output << "schemaVersion=1\n";
    output << "window.x=" << config.x << "\n";
    output << "window.y=" << config.y << "\n";
    output << "window.width=" << config.width << "\n";
    output << "window.height=" << config.height << "\n";
    output << "window.opacity=" << config.opacity << "\n";
    output << "window.normalHeight=" << config.normalHeight << "\n";
    output << "window.iconSize=" << config.iconSize << "\n";
    output << "window.density=" << config.density << "\n";
    output << "window.viewMode=" << config.viewMode << "\n";
    output << "window.tabSide=" << config.tabSide << "\n";
    output << "window.titleOpacity=" << config.titleOpacity << "\n";
    output << "window.dpi=" << config.dpi << "\n";
    output << "window.monitorId=" << WideToUtf8(config.monitorId) << "\n";
    output << "window.collapsed=" << (config.collapsed ? 1 : 0) << "\n";
    output << "window.locked=" << (config.locked ? 1 : 0) << "\n";
    output << "window.showBorder=" << (config.showBorder ? 1 : 0) << "\n";
    return AtomicWriteConfig(configDir_, profilePath, profileBackupPath, profileTempPath, 2, output.str());
}

bool ConfigStore::LoadLayoutProfile(WindowConfig& config) const {
    const std::wstring profilePath = configDir_ + L"\\layout.profile.ini";
    const auto values = ReadKeyValueFile(profilePath);
    const auto schemaIt = values.find(L"schemaVersion");
    if (schemaIt == values.end() || ParseInt(schemaIt->second, 0) <= 0) {
        return false;
    }
    WindowConfig loaded;
    for (const auto& [key, value] : values) {
        ApplyConfigLine(loaded, key + L"=" + value);
    }
    loaded.density = std::clamp(loaded.density, 0, 2);
    loaded.viewMode = std::clamp(loaded.viewMode, 0, 1);
    loaded.tabSide = std::clamp(loaded.tabSide, 0, 3);
    loaded.titleOpacity = std::clamp(loaded.titleOpacity, 80, 255);
    loaded.iconSize = std::clamp(loaded.iconSize, 32, 72);
    config = loaded;
    return true;
}
