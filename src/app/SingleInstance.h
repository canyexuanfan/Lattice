#pragma once

#include <Windows.h>

class SingleInstance {
public:
    SingleInstance();
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    bool IsPrimary() const noexcept { return isPrimary_; }

private:
    HANDLE mutex_ = nullptr;
    HANDLE legacyMutex_ = nullptr;
    bool isPrimary_ = false;
};
