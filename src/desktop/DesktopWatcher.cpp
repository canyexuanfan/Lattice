#include "desktop/DesktopWatcher.h"

#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "util/PathUtil.h"

namespace {

constexpr DWORD kNotifyFilter =
    FILE_NOTIFY_CHANGE_FILE_NAME |
    FILE_NOTIFY_CHANGE_DIR_NAME |
    FILE_NOTIFY_CHANGE_ATTRIBUTES |
    FILE_NOTIFY_CHANGE_SIZE |
    FILE_NOTIFY_CHANGE_LAST_WRITE;
constexpr size_t kBufferSize = 64 * 1024;

std::wstring EnvironmentPath(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        name, value.data(), static_cast<DWORD>(value.size()));
    if (copied == 0 || copied >= value.size()) {
        return {};
    }
    value.resize(copied);
    return value;
}

bool PathsEqual(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(
               left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void AddChangedPath(DesktopChangeBatch& batch, std::wstring path) {
    if (path.empty() || std::any_of(
            batch.paths.begin(), batch.paths.end(),
            [&](const std::wstring& current) {
                return PathsEqual(current, path);
            })) {
        return;
    }
    batch.paths.push_back(std::move(path));
}

std::vector<std::wstring> DesktopDirectories() {
    std::vector<std::wstring> directories;
    std::wstring userDesktop = EnvironmentPath(
        L"DESKTOP_ORGANIZER_DESKTOP_DIR");
    std::wstring publicDesktop = EnvironmentPath(
        L"DESKTOP_ORGANIZER_PUBLIC_DESKTOP_DIR");
    if (userDesktop.empty()) {
        userDesktop = KnownFolderPath(FOLDERID_Desktop);
    }
    if (publicDesktop.empty()) {
        publicDesktop = KnownFolderPath(FOLDERID_PublicDesktop);
    }
    if (!userDesktop.empty()) {
        directories.push_back(userDesktop);
    }
    if (!publicDesktop.empty() && !PathsEqual(publicDesktop, userDesktop)) {
        directories.push_back(publicDesktop);
    }
    return directories;
}

}  // namespace

DesktopWatcher::~DesktopWatcher() {
    Stop();
}

bool DesktopWatcher::Start(Callback callback) {
    Stop();
    callback_ = std::move(callback);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent_ == nullptr) {
        callback_ = {};
        return false;
    }

    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (thread_ == nullptr) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
        callback_ = {};
        return false;
    }
    return true;
}

void DesktopWatcher::Stop() {
    if (stopEvent_ != nullptr) {
        SetEvent(stopEvent_);
    }
    if (thread_ != nullptr) {
        WaitForSingleObject(thread_, 5000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stopEvent_ != nullptr) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    callback_ = {};
}

DWORD WINAPI DesktopWatcher::ThreadProc(LPVOID parameter) {
    auto* watcher = static_cast<DesktopWatcher*>(parameter);
    watcher->Run();
    return 0;
}

void DesktopWatcher::Run() {
    const auto beginDirectoryRead = [](DirectoryWatch& watch) {
        SetLastError(ERROR_SUCCESS);
        const BOOL started = ReadDirectoryChangesW(
            watch.directory,
            watch.buffer.data(),
            static_cast<DWORD>(watch.buffer.size()),
            FALSE,
            kNotifyFilter,
            nullptr,
            &watch.overlapped,
            nullptr);
        return started != FALSE || GetLastError() == ERROR_IO_PENDING;
    };

    std::vector<DirectoryWatch> watches;
    watches.reserve(2);
    for (const std::wstring& path : DesktopDirectories()) {
        watches.emplace_back();
        DirectoryWatch& watch = watches.back();
        watch.path = path;
        watch.directory = CreateFileW(
            path.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (watch.directory == INVALID_HANDLE_VALUE) {
            watches.pop_back();
            continue;
        }
        watch.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        watch.buffer.resize(kBufferSize);
        watch.overlapped.hEvent = watch.event;
        if (watch.event == nullptr || !beginDirectoryRead(watch)) {
            if (watch.event != nullptr) {
                CloseHandle(watch.event);
            }
            CloseHandle(watch.directory);
            watches.pop_back();
            continue;
        }
    }

    if (watches.empty()) {
        return;
    }

    std::array<HANDLE, 3> handles{};
    handles[0] = stopEvent_;
    for (size_t index = 0; index < watches.size() && index + 1 < handles.size(); ++index) {
        handles[index + 1] = watches[index].event;
    }
    const DWORD handleCount = static_cast<DWORD>(watches.size() + 1);

    while (true) {
        const DWORD result = WaitForMultipleObjects(handleCount, handles.data(), FALSE, INFINITE);
        if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) {
            break;
        }
        const size_t watchIndex = static_cast<size_t>(result - WAIT_OBJECT_0 - 1);
        if (watchIndex >= watches.size()) {
            break;
        }

        DirectoryWatch& watch = watches[watchIndex];
        DWORD bytes = 0;
        const bool completed = GetOverlappedResult(watch.directory, &watch.overlapped, &bytes, FALSE) != FALSE;
        ResetEvent(watch.event);
        DesktopChangeBatch changes;
        if (!completed || bytes == 0) {
            changes.requiresRescan = true;
        } else {
            size_t offset = 0;
            constexpr size_t headerSize = offsetof(
                FILE_NOTIFY_INFORMATION, FileName);
            while (offset + headerSize <= bytes) {
                const auto* information =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                        watch.buffer.data() + offset);
                const size_t entryBytes = static_cast<size_t>(
                    information->FileNameLength);
                if ((entryBytes % sizeof(wchar_t)) != 0 ||
                    headerSize + entryBytes > bytes - offset) {
                    changes.requiresRescan = true;
                    break;
                }
                const std::wstring relativePath(
                    information->FileName,
                    entryBytes / sizeof(wchar_t));
                AddChangedPath(
                    changes, JoinPath(watch.path, relativePath));
                if (information->Action != FILE_ACTION_MODIFIED) {
                    changes.requiresRescan = true;
                }
                if (information->NextEntryOffset == 0) {
                    break;
                }
                if (information->NextEntryOffset < headerSize ||
                    information->NextEntryOffset + headerSize >
                        bytes - offset) {
                    changes.requiresRescan = true;
                    break;
                }
                offset += information->NextEntryOffset;
            }
        }
        if (callback_ && !changes.Empty()) {
            callback_(std::move(changes));
        }
        ZeroMemory(&watch.overlapped, sizeof(watch.overlapped));
        watch.overlapped.hEvent = watch.event;
        if (!beginDirectoryRead(watch)) {
            break;
        }
    }

    for (DirectoryWatch& watch : watches) {
        CancelIoEx(watch.directory, &watch.overlapped);
        if (watch.event != nullptr) {
            CloseHandle(watch.event);
        }
        if (watch.directory != INVALID_HANDLE_VALUE) {
            CloseHandle(watch.directory);
        }
    }
}
