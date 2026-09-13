#include "app/UpdateService.h"

#include <Windows.h>
#include <ShlObj.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

#include "util/PathUtil.h"

namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kApiPath[] = L"/repos/canyexuanfan/Lattice/releases/latest";
constexpr std::uint64_t kMaximumMetadataBytes = 2ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumInstallerBytes = 128ULL * 1024ULL * 1024ULL;
#if defined(LATTICE_OFFLINE_PACKAGE) && LATTICE_OFFLINE_PACKAGE
constexpr UpdatePackageVariant kCurrentUpdatePackageVariant =
    UpdatePackageVariant::Offline;
#else
constexpr UpdatePackageVariant kCurrentUpdatePackageVariant =
    UpdatePackageVariant::Standard;
#endif

struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle() {
        if (value != nullptr) WinHttpCloseHandle(value);
    }
};

enum class JsonFieldStatus {
    Missing,
    Found,
    Duplicate,
    Invalid,
};

enum class DownloadFailure {
    None,
    InvalidUrl,
    ConnectionOrHttp,
    Storage,
    Size,
    Digest,
    VersionMetadata,
    Version,
    ProductName,
    Replace,
};

void SkipWhitespace(const std::string& json, size_t& cursor, size_t end) {
    while (cursor < end &&
           std::isspace(static_cast<unsigned char>(json[cursor])) != 0) {
        ++cursor;
    }
}

bool ParseJsonStringAt(
    const std::string& json,
    size_t& cursor,
    size_t end,
    std::string& value) {
    value.clear();
    if (cursor >= end || json[cursor] != '"') return false;
    ++cursor;
    while (cursor < end) {
        const unsigned char character =
            static_cast<unsigned char>(json[cursor++]);
        if (character == '"') return true;
        if (character < 0x20) return false;
        if (character != '\\') {
            value.push_back(static_cast<char>(character));
            continue;
        }
        if (cursor >= end) return false;
        const char escaped = json[cursor++];
        switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: return false;
        }
    }
    return false;
}

bool SkipJsonValue(const std::string& json, size_t& cursor, size_t end) {
    SkipWhitespace(json, cursor, end);
    if (cursor >= end) return false;
    if (json[cursor] == '"') {
        std::string ignored;
        return ParseJsonStringAt(json, cursor, end, ignored);
    }
    if (json[cursor] == '{' || json[cursor] == '[') {
        const char opening = json[cursor];
        const char closing = opening == '{' ? '}' : ']';
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        while (cursor < end) {
            const char character = json[cursor++];
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (character == '\\') {
                    escaped = true;
                } else if (character == '"') {
                    inString = false;
                }
                continue;
            }
            if (character == '"') {
                inString = true;
            } else if (character == opening) {
                ++depth;
            } else if (character == closing && --depth == 0) {
                return true;
            }
        }
        return false;
    }

    const size_t start = cursor;
    while (cursor < end && json[cursor] != ',' && json[cursor] != '}' &&
           json[cursor] != ']') {
        ++cursor;
    }
    size_t valueEnd = cursor;
    while (valueEnd > start &&
           std::isspace(static_cast<unsigned char>(json[valueEnd - 1])) != 0) {
        --valueEnd;
    }
    return valueEnd > start;
}

JsonFieldStatus FindObjectField(
    const std::string& json,
    size_t objectStart,
    size_t objectEnd,
    std::string_view key,
    size_t& valueStart,
    size_t& valueEnd) {
    if (objectStart >= objectEnd || json[objectStart] != '{' ||
        json[objectEnd - 1] != '}') {
        return JsonFieldStatus::Invalid;
    }
    bool found = false;
    size_t cursor = objectStart + 1;
    while (cursor < objectEnd - 1) {
        SkipWhitespace(json, cursor, objectEnd - 1);
        if (cursor >= objectEnd - 1) break;
        if (json[cursor] == ',') {
            ++cursor;
            SkipWhitespace(json, cursor, objectEnd - 1);
        }
        if (cursor >= objectEnd - 1) break;

        std::string parsedKey;
        if (!ParseJsonStringAt(json, cursor, objectEnd - 1, parsedKey)) {
            return JsonFieldStatus::Invalid;
        }
        SkipWhitespace(json, cursor, objectEnd - 1);
        if (cursor >= objectEnd - 1 || json[cursor++] != ':') {
            return JsonFieldStatus::Invalid;
        }
        SkipWhitespace(json, cursor, objectEnd - 1);
        const size_t currentValueStart = cursor;
        if (!SkipJsonValue(json, cursor, objectEnd - 1)) {
            return JsonFieldStatus::Invalid;
        }
        if (parsedKey == key) {
            if (found) return JsonFieldStatus::Duplicate;
            found = true;
            valueStart = currentValueStart;
            valueEnd = cursor;
        }
        SkipWhitespace(json, cursor, objectEnd - 1);
        if (cursor < objectEnd - 1 && json[cursor] != ',') {
            return JsonFieldStatus::Invalid;
        }
    }
    return found ? JsonFieldStatus::Found : JsonFieldStatus::Missing;
}

bool GetObjectStringField(
    const std::string& json,
    size_t objectStart,
    size_t objectEnd,
    std::string_view key,
    std::string& value) {
    size_t valueStart = 0;
    size_t valueEnd = 0;
    if (FindObjectField(
            json,
            objectStart,
            objectEnd,
            key,
            valueStart,
            valueEnd) != JsonFieldStatus::Found) {
        return false;
    }
    size_t cursor = valueStart;
    return ParseJsonStringAt(json, cursor, valueEnd, value) &&
        cursor == valueEnd;
}

bool GetObjectUnsignedField(
    const std::string& json,
    size_t objectStart,
    size_t objectEnd,
    std::string_view key,
    std::uint64_t& value) {
    size_t valueStart = 0;
    size_t valueEnd = 0;
    if (FindObjectField(
            json,
            objectStart,
            objectEnd,
            key,
            valueStart,
            valueEnd) != JsonFieldStatus::Found) {
        return false;
    }
    SkipWhitespace(json, valueStart, valueEnd);
    while (valueEnd > valueStart &&
           std::isspace(static_cast<unsigned char>(json[valueEnd - 1])) != 0) {
        --valueEnd;
    }
    if (valueStart >= valueEnd) return false;
    std::uint64_t parsed = 0;
    for (size_t cursor = valueStart; cursor < valueEnd; ++cursor) {
        const unsigned char character =
            static_cast<unsigned char>(json[cursor]);
        if (character < '0' || character > '9') return false;
        const std::uint64_t digit = character - '0';
        if (parsed >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

bool ParseRootObject(
    const std::string& json,
    size_t& rootStart,
    size_t& rootEnd) {
    rootStart = 0;
    SkipWhitespace(json, rootStart, json.size());
    if (rootStart >= json.size() || json[rootStart] != '{') return false;
    rootEnd = rootStart;
    if (!SkipJsonValue(json, rootEnd, json.size())) return false;
    size_t trailing = rootEnd;
    SkipWhitespace(json, trailing, json.size());
    return trailing == json.size();
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            needed) != needed) {
        return {};
    }
    return result;
}

bool TryVersionParts(
    std::wstring version,
    std::array<int, 4>& parts) {
    parts = {};
    if (!version.empty() &&
        (version.front() == L'v' || version.front() == L'V')) {
        version.erase(version.begin());
    }
    if (version.empty()) return false;
    size_t part = 0;
    size_t cursor = 0;
    while (cursor < version.size()) {
        if (part >= parts.size()) return false;
        const size_t end = version.find(L'.', cursor);
        const std::wstring token = version.substr(
            cursor,
            end == std::wstring::npos ? std::wstring::npos : end - cursor);
        if (token.empty() ||
            !std::all_of(
                token.begin(),
                token.end(),
                [](wchar_t character) {
                    return character >= L'0' && character <= L'9';
                })) {
            return false;
        }
        try {
            const unsigned long parsed = std::stoul(token);
            if (parsed > 65535) return false;
            parts[part++] = static_cast<int>(parsed);
        } catch (...) {
            return false;
        }
        if (end == std::wstring::npos) break;
        cursor = end + 1;
        if (cursor == version.size()) return false;
    }
    return part > 0;
}

std::array<int, 4> VersionParts(const std::wstring& version) {
    std::array<int, 4> parts{};
    TryVersionParts(version, parts);
    return parts;
}

bool ParseSha256Digest(
    const std::string& digest,
    std::array<std::uint8_t, 32>& output) {
    if (digest.size() != 71 ||
        digest.compare(0, 7, "sha256:") != 0) {
        return false;
    }
    const auto hexValue = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    for (size_t index = 0; index < output.size(); ++index) {
        const int high = hexValue(digest[7 + index * 2]);
        const int low = hexValue(digest[8 + index * 2]);
        if (high < 0 || low < 0) return false;
        output[index] =
            static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool IsHttpsUrl(const std::wstring& url) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    return WinHttpCrackUrl(
               url.c_str(),
               static_cast<DWORD>(url.size()),
               0,
               &components) != FALSE &&
        components.nScheme == INTERNET_SCHEME_HTTPS &&
        components.dwHostNameLength > 0;
}

bool ReadResponse(
    HINTERNET request,
    std::uint64_t maximumBytes,
    std::string& output) {
    output.clear();
    for (;;) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request, &available) == FALSE) return false;
        if (available == 0) return true;
        if (output.size() + available > maximumBytes) return false;
        const size_t previous = output.size();
        output.resize(previous + available);
        DWORD read = 0;
        if (WinHttpReadData(
                request,
                output.data() + previous,
                available,
                &read) == FALSE) {
            return false;
        }
        output.resize(previous + read);
        if (read == 0) return true;
    }
}

bool RequestHttps(
    const wchar_t* host,
    INTERNET_PORT port,
    const std::wstring& path,
    HINTERNET& requestOut,
    InternetHandle& session,
    InternetHandle& connection,
    InternetHandle& request) {
    session.value = WinHttpOpen(
        L"Lattice/0.4.58",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (session.value == nullptr) return false;
    WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000);
    DWORD redirectPolicy =
        WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (WinHttpSetOption(
            session.value,
            WINHTTP_OPTION_REDIRECT_POLICY,
            &redirectPolicy,
            sizeof(redirectPolicy)) == FALSE) {
        return false;
    }
    connection.value = WinHttpConnect(session.value, host, port, 0);
    if (connection.value == nullptr) return false;
    request.value = WinHttpOpenRequest(
        connection.value,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
    if (request.value == nullptr) return false;
    const wchar_t headers[] =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    if (WinHttpSendRequest(
            request.value,
            headers,
            static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) == FALSE ||
        WinHttpReceiveResponse(request.value, nullptr) == FALSE) {
        return false;
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(
            request.value,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX) == FALSE ||
        status != 200) {
        return false;
    }
    requestOut = request.value;
    return true;
}

bool FetchLatestRelease(std::string& json) {
    InternetHandle session;
    InternetHandle connection;
    InternetHandle request;
    HINTERNET rawRequest = nullptr;
    if (!RequestHttps(
            kApiHost,
            INTERNET_DEFAULT_HTTPS_PORT,
            kApiPath,
            rawRequest,
            session,
            connection,
            request)) {
        return false;
    }
    return ReadResponse(rawRequest, kMaximumMetadataBytes, json);
}

std::wstring UpdateDirectory() {
    wchar_t overridePath[32768]{};
    const DWORD overrideLength = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_UPDATE_DIR",
        overridePath,
        ARRAYSIZE(overridePath));
    if (overrideLength > 0 && overrideLength < ARRAYSIZE(overridePath)) {
        return overridePath;
    }
    return JoinPath(
        KnownFolderPath(FOLDERID_LocalAppData),
        L"Lattice\\Updates");
}

bool ComputeFileSha256(
    const std::wstring& path,
    std::array<std::uint8_t, 32>& digest) {
    digest = {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<std::uint8_t> hashObject;
    bool succeeded = false;

    do {
        if (BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0) < 0) {
            break;
        }
        DWORD objectLength = 0;
        DWORD resultLength = 0;
        if (BCryptGetProperty(
                algorithm,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectLength),
                sizeof(objectLength),
                &resultLength,
                0) < 0 ||
            objectLength == 0) {
            break;
        }
        DWORD digestLength = 0;
        if (BCryptGetProperty(
                algorithm,
                BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&digestLength),
                sizeof(digestLength),
                &resultLength,
                0) < 0 ||
            digestLength != digest.size()) {
            break;
        }
        hashObject.resize(objectLength);
        if (BCryptCreateHash(
                algorithm,
                &hash,
                hashObject.data(),
                static_cast<ULONG>(hashObject.size()),
                nullptr,
                0,
                0) < 0) {
            break;
        }
        file = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) break;

        std::array<std::uint8_t, 64 * 1024> buffer{};
        for (;;) {
            DWORD read = 0;
            if (ReadFile(
                    file,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read,
                    nullptr) == FALSE) {
                break;
            }
            if (read == 0) {
                if (BCryptFinishHash(
                        hash,
                        digest.data(),
                        static_cast<ULONG>(digest.size()),
                        0) >= 0) {
                    succeeded = true;
                }
                break;
            }
            if (BCryptHashData(hash, buffer.data(), read, 0) < 0) break;
        }
    } while (false);

    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!succeeded) digest = {};
    return succeeded;
}

bool ConstantTimeDigestEquals(
    const std::array<std::uint8_t, 32>& left,
    const std::array<std::uint8_t, 32>& right) {
    std::uint8_t difference = 0;
    for (size_t index = 0; index < left.size(); ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0;
}

DownloadFailure MapValidationFailure(
    UpdateInstallerValidationFailure failure) {
    switch (failure) {
        case UpdateInstallerValidationFailure::None:
            return DownloadFailure::None;
        case UpdateInstallerValidationFailure::FileAccess:
            return DownloadFailure::Storage;
        case UpdateInstallerValidationFailure::SizeMismatch:
            return DownloadFailure::Size;
        case UpdateInstallerValidationFailure::DigestMismatch:
            return DownloadFailure::Digest;
        case UpdateInstallerValidationFailure::VersionMetadataMissing:
            return DownloadFailure::VersionMetadata;
        case UpdateInstallerValidationFailure::VersionMismatch:
            return DownloadFailure::Version;
        case UpdateInstallerValidationFailure::ProductNameMismatch:
            return DownloadFailure::ProductName;
    }
    return DownloadFailure::VersionMetadata;
}

DownloadFailure DownloadInstaller(
    const UpdateReleaseAsset& asset,
    std::wstring& finalPath) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(
            asset.downloadUrl.c_str(),
            static_cast<DWORD>(asset.downloadUrl.size()),
            0,
            &components) == FALSE ||
        components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.dwHostNameLength == 0) {
        return DownloadFailure::InvalidUrl;
    }
    const std::wstring host(
        components.lpszHostName,
        components.dwHostNameLength);
    std::wstring requestPath(
        components.lpszUrlPath,
        components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        requestPath.append(
            components.lpszExtraInfo,
            components.dwExtraInfoLength);
    }

    InternetHandle session;
    InternetHandle connection;
    InternetHandle request;
    HINTERNET rawRequest = nullptr;
    if (!RequestHttps(
            host.c_str(),
            components.nPort,
            requestPath,
            rawRequest,
            session,
            connection,
            request)) {
        return DownloadFailure::ConnectionOrHttp;
    }

    const std::wstring directory = UpdateDirectory();
    if (directory.empty()) return DownloadFailure::Storage;
    const int createResult =
        SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if ((createResult != ERROR_SUCCESS &&
         createResult != ERROR_ALREADY_EXISTS &&
         createResult != ERROR_FILE_EXISTS) ||
        attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return DownloadFailure::Storage;
    }

    finalPath = JoinPath(directory, asset.name);
    const std::wstring partialPath = finalPath + L".part";
    HANDLE file = CreateFileW(
        partialPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return DownloadFailure::Storage;

    DownloadFailure failure = DownloadFailure::None;
    std::uint64_t total = 0;
    std::array<std::byte, 64 * 1024> buffer{};
    for (;;) {
        DWORD read = 0;
        if (WinHttpReadData(
                rawRequest,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read) == FALSE) {
            failure = DownloadFailure::ConnectionOrHttp;
            break;
        }
        if (read == 0) break;
        if (read > kMaximumInstallerBytes - total ||
            read > asset.size - std::min(asset.size, total)) {
            failure = DownloadFailure::Size;
            break;
        }
        DWORD written = 0;
        if (WriteFile(
                file,
                buffer.data(),
                read,
                &written,
                nullptr) == FALSE ||
            written != read) {
            failure = DownloadFailure::Storage;
            break;
        }
        total += read;
    }
    if (failure == DownloadFailure::None &&
        FlushFileBuffers(file) == FALSE) {
        failure = DownloadFailure::Storage;
    }
    CloseHandle(file);

    if (failure == DownloadFailure::None && total != asset.size) {
        failure = DownloadFailure::Size;
    }
    if (failure == DownloadFailure::None) {
        failure = MapValidationFailure(
            UpdateService::ValidateDownloadedInstaller(partialPath, asset));
    }
    if (failure == DownloadFailure::None &&
        MoveFileExW(
            partialPath.c_str(),
            finalPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        failure = DownloadFailure::Replace;
    }
    if (failure != DownloadFailure::None) {
        DeleteFileW(partialPath.c_str());
    }
    return failure;
}

bool LaunchSilentInstaller(const std::wstring& path) {
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"open";
    execute.lpFile = path.c_str();
    execute.lpParameters =
        L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";
    execute.nShow = SW_HIDE;
    return ShellExecuteExW(&execute) != FALSE;
}

std::wstring DownloadFailureMessage(
    DownloadFailure failure,
    const std::wstring& version) {
    const std::wstring prefix = L"发现新版本 " + version + L"，但";
    switch (failure) {
        case DownloadFailure::InvalidUrl:
            return prefix + L"更新地址不是安全的HTTPS地址，未执行更新。";
        case DownloadFailure::ConnectionOrHttp:
            return prefix + L"连接或下载安装包失败。请检查网络后重试。";
        case DownloadFailure::Storage:
            return prefix + L"无法写入本地更新目录，未执行更新。";
        case DownloadFailure::Size:
            return prefix +
                L"安装包大小与公开Release记录不一致，已删除临时文件。";
        case DownloadFailure::Digest:
            return prefix +
                L"安装包SHA-256与公开Release记录不一致，已删除临时文件。";
        case DownloadFailure::VersionMetadata:
            return prefix +
                L"安装包缺少有效的Windows版本信息，未执行更新。";
        case DownloadFailure::Version:
            return prefix + L"安装包版本与公开Release不一致，未执行更新。";
        case DownloadFailure::ProductName:
            return prefix + L"安装包产品身份不是Lattice，未执行更新。";
        case DownloadFailure::Replace:
            return prefix + L"无法完成更新文件的原子替换，未执行更新。";
        case DownloadFailure::None:
            break;
    }
    return prefix + L"安全校验失败，未执行更新。";
}

UpdateServiceResult RunUpdate(bool manual) {
    UpdateServiceResult result;
    result.manual = manual;
    std::string json;
    if (!FetchLatestRelease(json)) {
        result.message = L"无法连接公开更新源，请检查网络后重试。";
        return result;
    }

    UpdateReleaseAsset asset;
    if (!UpdateService::SelectReleaseAssetMetadataForVariant(
            json,
            kCurrentUpdatePackageVariant,
            asset)) {
        result.message =
            L"公开更新源返回的安装包信息不完整或不唯一（需要HTTPS地址、大小和SHA-256），未执行更新。";
        return result;
    }
    if (UpdateService::CompareVersions(
            asset.version,
            UpdateService::kCurrentVersion) <= 0) {
        result.status = UpdateServiceStatus::UpToDate;
        result.message = std::wstring(L"当前版本：") +
            UpdateService::kCurrentVersion + L"\n\n已经是最新版本。";
        return result;
    }

    std::wstring installerPath;
    const DownloadFailure failure = DownloadInstaller(asset, installerPath);
    if (failure != DownloadFailure::None) {
        result.message = DownloadFailureMessage(failure, asset.version);
        return result;
    }
    if (!LaunchSilentInstaller(installerPath)) {
        result.message =
            L"安装包已通过大小、SHA-256和产品身份校验，但无法启动静默更新。";
        return result;
    }
    result.status = UpdateServiceStatus::UpdateLaunched;
    result.message = L"Lattice " + asset.version +
        L" 已下载并通过完整性校验，正在后台更新。";
    return result;
}

}  // namespace

std::atomic<bool> UpdateService::busy_{false};

int UpdateService::CompareVersions(
    const std::wstring& left,
    const std::wstring& right) {
    const auto lhs = VersionParts(left);
    const auto rhs = VersionParts(right);
    if (lhs < rhs) return -1;
    if (lhs > rhs) return 1;
    return 0;
}

bool UpdateService::SelectReleaseAsset(
    const std::string& json,
    std::wstring& version,
    std::wstring& downloadUrl) {
    return SelectReleaseAssetForVariant(
        json,
        kCurrentUpdatePackageVariant,
        version,
        downloadUrl);
}

bool UpdateService::SelectReleaseAssetForVariant(
    const std::string& json,
    UpdatePackageVariant variant,
    std::wstring& version,
    std::wstring& downloadUrl) {
    UpdateReleaseAsset asset;
    if (!SelectReleaseAssetMetadataForVariant(json, variant, asset)) {
        version.clear();
        downloadUrl.clear();
        return false;
    }
    version = std::move(asset.version);
    downloadUrl = std::move(asset.downloadUrl);
    return true;
}

bool UpdateService::SelectReleaseAssetMetadataForVariant(
    const std::string& json,
    UpdatePackageVariant variant,
    UpdateReleaseAsset& asset) {
    asset = {};
    size_t rootStart = 0;
    size_t rootEnd = 0;
    if (!ParseRootObject(json, rootStart, rootEnd)) return false;

    std::string tag;
    if (!GetObjectStringField(
            json,
            rootStart,
            rootEnd,
            "tag_name",
            tag)) {
        return false;
    }
    std::wstring version = Utf8ToWide(tag);
    if (!version.empty() &&
        (version.front() == L'v' || version.front() == L'V')) {
        version.erase(version.begin());
    }
    std::array<int, 4> parsedVersion{};
    if (!TryVersionParts(version, parsedVersion)) return false;

    std::string versionAscii;
    versionAscii.reserve(version.size());
    for (const wchar_t character : version) {
        if ((character < L'0' || character > L'9') && character != L'.') {
            return false;
        }
        versionAscii.push_back(static_cast<char>(character));
    }
    const std::array<std::string, 4> candidateNames{
        "Lattice-Setup-" + versionAscii + ".exe",
        "Lattice-Setup-Latest.exe",
        "Lattice-Setup-" + versionAscii + "-Offline.exe",
        "Lattice-Setup-Latest-Offline.exe",
    };
    struct Candidate {
        bool seen = false;
        bool ambiguous = false;
        UpdateReleaseAsset asset;
    };
    std::array<Candidate, 4> candidates{};

    size_t assetsStart = 0;
    size_t assetsEnd = 0;
    if (FindObjectField(
            json,
            rootStart,
            rootEnd,
            "assets",
            assetsStart,
            assetsEnd) != JsonFieldStatus::Found ||
        assetsStart >= assetsEnd || json[assetsStart] != '[' ||
        json[assetsEnd - 1] != ']') {
        return false;
    }

    size_t cursor = assetsStart + 1;
    while (cursor < assetsEnd - 1) {
        SkipWhitespace(json, cursor, assetsEnd - 1);
        if (cursor >= assetsEnd - 1) break;
        if (json[cursor] == ',') {
            ++cursor;
            SkipWhitespace(json, cursor, assetsEnd - 1);
        }
        if (cursor >= assetsEnd - 1 || json[cursor] != '{') return false;
        const size_t objectStart = cursor;
        if (!SkipJsonValue(json, cursor, assetsEnd - 1)) return false;
        const size_t objectEnd = cursor;

        std::string name;
        if (!GetObjectStringField(
                json,
                objectStart,
                objectEnd,
                "name",
                name)) {
            return false;
        }
        const auto foundName =
            std::find(candidateNames.begin(), candidateNames.end(), name);
        if (foundName != candidateNames.end()) {
            const size_t candidateIndex =
                static_cast<size_t>(foundName - candidateNames.begin());
            Candidate& candidate = candidates[candidateIndex];
            if (candidate.seen) {
                candidate.ambiguous = true;
            } else {
                std::string url;
                std::string digest;
                std::uint64_t size = 0;
                std::array<std::uint8_t, 32> sha256{};
                const std::wstring wideName = Utf8ToWide(name);
                const bool valid = GetObjectStringField(
                                       json,
                                       objectStart,
                                       objectEnd,
                                       "browser_download_url",
                                       url) &&
                    GetObjectUnsignedField(
                        json,
                        objectStart,
                        objectEnd,
                        "size",
                        size) &&
                    GetObjectStringField(
                        json,
                        objectStart,
                        objectEnd,
                        "digest",
                        digest) &&
                    size > 0 && size <= kMaximumInstallerBytes &&
                    ParseSha256Digest(digest, sha256);
                const std::wstring wideUrl =
                    valid ? Utf8ToWide(url) : std::wstring{};
                if (!valid || wideName.empty() || wideUrl.empty() ||
                    !IsHttpsUrl(wideUrl)) {
                    return false;
                }
                candidate.seen = true;
                candidate.asset.version = version;
                candidate.asset.name = wideName;
                candidate.asset.downloadUrl = wideUrl;
                candidate.asset.size = size;
                candidate.asset.sha256 = sha256;
            }
        }
        SkipWhitespace(json, cursor, assetsEnd - 1);
        if (cursor < assetsEnd - 1 && json[cursor] != ',') return false;
    }

    std::array<size_t, 4> priority{0, 1, 2, 3};
    size_t priorityCount = priority.size();
    if (variant == UpdatePackageVariant::Offline) {
        priority = {2, 3, 0, 1};
        priorityCount = 2;
    }
    for (size_t priorityIndex = 0;
         priorityIndex < priorityCount;
         ++priorityIndex) {
        const Candidate& candidate = candidates[priority[priorityIndex]];
        if (candidate.ambiguous) return false;
        if (!candidate.seen) continue;
        asset = candidate.asset;
        return true;
    }
    return false;
}

bool UpdateService::IsAcceptedInstallerProductName(
    const std::wstring& productName) {
    size_t visibleLength = productName.size();
    while (visibleLength > 0 &&
           productName[visibleLength - 1] == L' ') {
        --visibleLength;
    }
    constexpr wchar_t expected[] = L"Lattice";
    constexpr int expectedLength = ARRAYSIZE(expected) - 1;
    return visibleLength == expectedLength &&
        CompareStringOrdinal(
            productName.data(),
            static_cast<int>(visibleLength),
            expected,
            expectedLength,
            TRUE) == CSTR_EQUAL;
}

UpdateInstallerValidationFailure UpdateService::ValidateDownloadedInstaller(
    const std::wstring& path,
    const UpdateReleaseAsset& asset) {
    WIN32_FILE_ATTRIBUTE_DATA fileData{};
    if (GetFileAttributesExW(
            path.c_str(),
            GetFileExInfoStandard,
            &fileData) == FALSE ||
        (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return UpdateInstallerValidationFailure::FileAccess;
    }
    ULARGE_INTEGER fileSize{};
    fileSize.HighPart = fileData.nFileSizeHigh;
    fileSize.LowPart = fileData.nFileSizeLow;
    if (fileSize.QuadPart != asset.size || asset.size == 0 ||
        asset.size > kMaximumInstallerBytes) {
        return UpdateInstallerValidationFailure::SizeMismatch;
    }

    std::array<std::uint8_t, 32> actualDigest{};
    if (!ComputeFileSha256(path, actualDigest)) {
        return UpdateInstallerValidationFailure::FileAccess;
    }
    if (!ConstantTimeDigestEquals(actualDigest, asset.sha256)) {
        return UpdateInstallerValidationFailure::DigestMismatch;
    }

    DWORD handle = 0;
    const DWORD infoSize = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (infoSize == 0) {
        return UpdateInstallerValidationFailure::VersionMetadataMissing;
    }
    std::vector<std::byte> versionInfo(infoSize);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedSize = 0;
    if (GetFileVersionInfoW(
            path.c_str(),
            0,
            infoSize,
            versionInfo.data()) == FALSE ||
        VerQueryValueW(
            versionInfo.data(),
            L"\\",
            reinterpret_cast<void**>(&fixed),
            &fixedSize) == FALSE ||
        fixed == nullptr || fixedSize < sizeof(VS_FIXEDFILEINFO)) {
        return UpdateInstallerValidationFailure::VersionMetadataMissing;
    }
    std::array<int, 4> expected{};
    if (!TryVersionParts(asset.version, expected) ||
        HIWORD(fixed->dwFileVersionMS) != expected[0] ||
        LOWORD(fixed->dwFileVersionMS) != expected[1] ||
        HIWORD(fixed->dwFileVersionLS) != expected[2] ||
        LOWORD(fixed->dwFileVersionLS) != expected[3]) {
        return UpdateInstallerValidationFailure::VersionMismatch;
    }

    struct Translation {
        WORD language;
        WORD codePage;
    };
    Translation* translations = nullptr;
    UINT translationBytes = 0;
    bool productMatched = false;
    if (VerQueryValueW(
            versionInfo.data(),
            L"\\VarFileInfo\\Translation",
            reinterpret_cast<void**>(&translations),
            &translationBytes) != FALSE &&
        translations != nullptr) {
        for (UINT index = 0;
             index < translationBytes / sizeof(Translation);
             ++index) {
            wchar_t query[64]{};
            swprintf_s(
                query,
                L"\\StringFileInfo\\%04x%04x\\ProductName",
                translations[index].language,
                translations[index].codePage);
            wchar_t* product = nullptr;
            UINT characters = 0;
            if (VerQueryValueW(
                    versionInfo.data(),
                    query,
                    reinterpret_cast<void**>(&product),
                    &characters) == FALSE ||
                product == nullptr || characters == 0) {
                continue;
            }
            size_t visibleLength = 0;
            while (visibleLength < characters &&
                   product[visibleLength] != L'\0') {
                ++visibleLength;
            }
            if (IsAcceptedInstallerProductName(
                    std::wstring(product, visibleLength))) {
                productMatched = true;
                break;
            }
        }
    }
    return productMatched
        ? UpdateInstallerValidationFailure::None
        : UpdateInstallerValidationFailure::ProductNameMismatch;
}

bool UpdateService::Start(
    HWND notificationWindow,
    bool manual,
    HWND dialogOwner) {
    if (notificationWindow == nullptr || IsWindow(notificationWindow) == FALSE ||
        busy_.exchange(true)) {
        return false;
    }
    if (!manual &&
        GetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_DISABLE_AUTO_UPDATE",
            nullptr,
            0) > 0) {
        busy_ = false;
        return true;
    }
    std::thread([notificationWindow, manual, dialogOwner]() {
        UpdateServiceResult* result =
            new UpdateServiceResult(RunUpdate(manual));
        result->dialogOwner = dialogOwner;
        busy_ = false;
        if (IsWindow(notificationWindow) == FALSE ||
            PostMessageW(
                notificationWindow,
                kUpdateServiceResultMessage,
                0,
                reinterpret_cast<LPARAM>(result)) == FALSE) {
            delete result;
        }
    }).detach();
    return true;
}
