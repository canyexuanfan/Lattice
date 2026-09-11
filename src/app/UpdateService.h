#pragma once

#include <Windows.h>

#include <atomic>
#include <string>

constexpr UINT kUpdateServiceResultMessage = WM_APP + 21;

enum class UpdateServiceStatus {
    UpToDate,
    UpdateLaunched,
    Failed,
};

enum class UpdatePackageVariant {
    Standard,
    Offline,
};

struct UpdateServiceResult {
    UpdateServiceStatus status = UpdateServiceStatus::Failed;
    bool manual = false;
    HWND dialogOwner = nullptr;
    std::wstring message;
};

class UpdateService {
public:
    static constexpr wchar_t kCurrentVersion[] = L"0.4.48";

    static bool Start(HWND notificationWindow, bool manual, HWND dialogOwner = nullptr);
    static int CompareVersions(const std::wstring& left, const std::wstring& right);
    static bool SelectReleaseAsset(
        const std::string& json,
        std::wstring& version,
        std::wstring& downloadUrl);
    static bool SelectReleaseAssetForVariant(
        const std::string& json,
        UpdatePackageVariant variant,
        std::wstring& version,
        std::wstring& downloadUrl);

private:
    static std::atomic<bool> busy_;
};
