#pragma once

#include <Windows.h>
#include <objbase.h>

class ComInit {
public:
    ComInit() : initialized_(SUCCEEDED(OleInitialize(nullptr))) {}
    ~ComInit() {
        if (initialized_) {
            OleUninitialize();
        }
    }

    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;

private:
    bool initialized_;
};
