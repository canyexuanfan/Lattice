#pragma once

#include <Windows.h>

#include <functional>
#include <vector>

class DesktopWatcher {
public:
    using Callback = std::function<void()>;

    ~DesktopWatcher();

    bool Start(Callback callback);
    void Stop();
    bool IsRunning() const noexcept { return thread_ != nullptr; }

private:
    struct DirectoryWatch {
        HANDLE directory = INVALID_HANDLE_VALUE;
        HANDLE event = nullptr;
        OVERLAPPED overlapped{};
        std::vector<BYTE> buffer;
    };

    static DWORD WINAPI ThreadProc(LPVOID parameter);
    void Run();

    HANDLE stopEvent_ = nullptr;
    HANDLE thread_ = nullptr;
    Callback callback_;
};
