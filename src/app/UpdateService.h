#pragma once

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
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

struct UpdateReleaseAsset {
    std::wstring version;
    std::wstring name;
    std::wstring downloadUrl;
    std::uint64_t size = 0;
    std::array<std::uint8_t, 32> sha256{};
};

enum class UpdateInstallerValidationFailure {
    None,
    FileAccess,
    SizeMismatch,
    DigestMismatch,
    VersionMetadataMissing,
    VersionMismatch,
    ProductNameMismatch,
};

class UpdateService {
public:
    static constexpr wchar_t kCurrentVersion[] = L"0.4.58";

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
    static bool SelectReleaseAssetMetadataForVariant(
        const std::string& json,
        UpdatePackageVariant variant,
        UpdateReleaseAsset& asset);
    static bool IsAcceptedInstallerProductName(const std::wstring& productName);
    static UpdateInstallerValidationFailure ValidateDownloadedInstaller(
        const std::wstring& path,
        const UpdateReleaseAsset& asset);

private:
    static std::atomic<bool> busy_;
};
