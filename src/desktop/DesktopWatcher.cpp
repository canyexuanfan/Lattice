#include "desktop/DesktopWatcher.h"

#include <ShlObj.h>

#include <array>
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

std::vector<std::wstring> DesktopDirectories() {
    std::vector<std::wstring> directories;
    const std::wstring userDesktop = KnownFolderPath(FOLDERID_Desktop);
    const std::wstring publicDesktop = KnownFolderPath(FOLDERID_PublicDesktop);
    if (!userDesktop.empty()) {
        directories.push_back(userDesktop);
    }
    if (!publicDesktop.empty() && publicDesktop != userDesktop) {
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
        watch.directory = CreateFileW(
            path.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (watch.directory == INVALID_HANDLE_VALUE) {
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
        if (completed && callback_) {
            callback_();
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
