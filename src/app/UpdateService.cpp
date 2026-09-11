#include "app/UpdateService.h"

#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
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
    ~InternetHandle() { if (value != nullptr) WinHttpCloseHandle(value); }
};

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), needed) != needed) return {};
    return result;
}

bool ExtractJsonString(const std::string& json, const std::string& key, size_t start, std::string& value, size_t& valueEnd) {
    value.clear();
    const std::string marker = "\"" + key + "\"";
    size_t cursor = json.find(marker, start);
    if (cursor == std::string::npos) return false;
    cursor = json.find(':', cursor + marker.size());
    if (cursor == std::string::npos) return false;
    cursor = json.find('"', cursor + 1);
    if (cursor == std::string::npos) return false;
    ++cursor;
    while (cursor < json.size()) {
        const char ch = json[cursor++];
        if (ch == '"') {
            valueEnd = cursor;
            return true;
        }
        if (ch == '\\' && cursor < json.size()) {
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
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

std::array<int, 4> VersionParts(std::wstring version) {
    if (!version.empty() && (version.front() == L'v' || version.front() == L'V')) version.erase(version.begin());
    std::array<int, 4> parts{};
    size_t part = 0;
    size_t cursor = 0;
    while (part < parts.size() && cursor < version.size()) {
        size_t end = version.find(L'.', cursor);
        const std::wstring token = version.substr(cursor, end == std::wstring::npos ? std::wstring::npos : end - cursor);
        if (token.empty() || !std::all_of(token.begin(), token.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) return {};
        try { parts[part++] = std::stoi(token); } catch (...) { return {}; }
        if (end == std::wstring::npos) break;
        cursor = end + 1;
    }
    return parts;
}

bool ReadResponse(HINTERNET request, std::uint64_t maximumBytes, std::string& output) {
    output.clear();
    for (;;) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(request, &available) == FALSE) return false;
        if (available == 0) return true;
        if (output.size() + available > maximumBytes) return false;
        const size_t previous = output.size();
        output.resize(previous + available);
        DWORD read = 0;
        if (WinHttpReadData(request, output.data() + previous, available, &read) == FALSE) return false;
        output.resize(previous + read);
        if (read == 0) return true;
    }
}

bool RequestHttps(const wchar_t* host, INTERNET_PORT port, const std::wstring& path, HINTERNET& requestOut, InternetHandle& session, InternetHandle& connection, InternetHandle& request) {
    session.value = WinHttpOpen(L"Lattice/0.4.52", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session.value == nullptr) return false;
    WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000);
    connection.value = WinHttpConnect(session.value, host, port, 0);
    if (connection.value == nullptr) return false;
    request.value = WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
    if (request.value == nullptr) return false;
    const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE || WinHttpReceiveResponse(request.value, nullptr) == FALSE) return false;
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) == FALSE || status != 200) return false;
    requestOut = request.value;
    return true;
}

bool FetchLatestRelease(std::string& json) {
    InternetHandle session, connection, request;
    HINTERNET rawRequest = nullptr;
    if (!RequestHttps(kApiHost, INTERNET_DEFAULT_HTTPS_PORT, kApiPath, rawRequest, session, connection, request)) return false;
    return ReadResponse(rawRequest, kMaximumMetadataBytes, json);
}

std::wstring UpdateDirectory() {
    wchar_t overridePath[32768]{};
    const DWORD overrideLength = GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_UPDATE_DIR", overridePath, ARRAYSIZE(overridePath));
    if (overrideLength > 0 && overrideLength < ARRAYSIZE(overridePath)) return overridePath;
    return JoinPath(KnownFolderPath(FOLDERID_LocalAppData), L"Lattice\\Updates");
}

bool DownloadInstaller(const std::wstring& url, const std::wstring& version, std::wstring& finalPath) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components) == FALSE || components.nScheme != INTERNET_SCHEME_HTTPS) return false;
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    InternetHandle session, connection, request;
    HINTERNET rawRequest = nullptr;
    if (!RequestHttps(host.c_str(), components.nPort, path, rawRequest, session, connection, request)) return false;

    const std::wstring directory = UpdateDirectory();
    if (directory.empty() || (SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr) != ERROR_SUCCESS && GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES)) return false;
    finalPath = JoinPath(directory, L"Lattice-Setup-" + version + L".exe");
    const std::wstring partialPath = finalPath + L".part";
    HANDLE file = CreateFileW(partialPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool succeeded = true;
    std::uint64_t total = 0;
    for (;;) {
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(rawRequest, &available) == FALSE) { succeeded = false; break; }
        if (available == 0) break;
        total += available;
        if (total > kMaximumInstallerBytes) { succeeded = false; break; }
        std::vector<std::byte> buffer(available);
        DWORD read = 0;
        if (WinHttpReadData(rawRequest, buffer.data(), available, &read) == FALSE) { succeeded = false; break; }
        DWORD written = 0;
        if (read == 0 || WriteFile(file, buffer.data(), read, &written, nullptr) == FALSE || written != read) { succeeded = false; break; }
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!succeeded || total == 0) {
        DeleteFileW(partialPath.c_str());
        return false;
    }

    DWORD handle = 0;
    const DWORD infoSize = GetFileVersionInfoSizeW(partialPath.c_str(), &handle);
    std::vector<std::byte> versionInfo(infoSize);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedSize = 0;
    bool valid = infoSize > 0 && GetFileVersionInfoW(partialPath.c_str(), 0, infoSize, versionInfo.data()) != FALSE &&
        VerQueryValueW(versionInfo.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixedSize) != FALSE && fixed != nullptr;
    if (valid) {
        const auto expected = VersionParts(version);
        valid = HIWORD(fixed->dwFileVersionMS) == expected[0] && LOWORD(fixed->dwFileVersionMS) == expected[1] &&
            HIWORD(fixed->dwFileVersionLS) == expected[2];
    }
    struct Translation { WORD language; WORD codePage; };
    Translation* translations = nullptr;
    UINT translationBytes = 0;
    bool productMatched = false;
    if (valid && VerQueryValueW(versionInfo.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &translationBytes) != FALSE) {
        for (UINT index = 0; index < translationBytes / sizeof(Translation); ++index) {
            wchar_t query[64]{};
            swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\ProductName", translations[index].language, translations[index].codePage);
            wchar_t* product = nullptr;
            UINT chars = 0;
            if (VerQueryValueW(versionInfo.data(), query, reinterpret_cast<void**>(&product), &chars) != FALSE && product != nullptr && CompareStringOrdinal(product, -1, L"Lattice", -1, TRUE) == CSTR_EQUAL) {
                productMatched = true;
                break;
            }
        }
    }
    if (!valid || !productMatched || MoveFileExW(partialPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(partialPath.c_str());
        return false;
    }
    return true;
}

bool LaunchSilentInstaller(const std::wstring& path) {
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"open";
    execute.lpFile = path.c_str();
    execute.lpParameters = L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";
    execute.nShow = SW_HIDE;
    return ShellExecuteExW(&execute) != FALSE;
}

UpdateServiceResult RunUpdate(bool manual) {
    UpdateServiceResult result;
    result.manual = manual;
    std::string json;
    std::wstring version;
    std::wstring url;
    if (!FetchLatestRelease(json) || !UpdateService::SelectReleaseAsset(json, version, url)) {
        result.message = L"无法连接公开更新源，请稍后重试。";
        return result;
    }
    if (UpdateService::CompareVersions(version, UpdateService::kCurrentVersion) <= 0) {
        result.status = UpdateServiceStatus::UpToDate;
        result.message = std::wstring(L"当前版本：") + UpdateService::kCurrentVersion + L"\n\n已经是最新版本。";
        return result;
    }
    std::wstring installerPath;
    if (!DownloadInstaller(url, version, installerPath)) {
        result.message = L"发现新版本 " + version + L"，但下载安装包或安全校验失败，未执行更新。";
        return result;
    }
    if (!LaunchSilentInstaller(installerPath)) {
        result.message = L"安装包已通过校验，但无法启动静默更新。";
        return result;
    }
    result.status = UpdateServiceStatus::UpdateLaunched;
    result.message = L"Lattice " + version + L" 已下载并通过校验，正在后台更新。";
    return result;
}

}  // namespace

std::atomic<bool> UpdateService::busy_{false};

int UpdateService::CompareVersions(const std::wstring& left, const std::wstring& right) {
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
        json, kCurrentUpdatePackageVariant, version, downloadUrl);
}

bool UpdateService::SelectReleaseAssetForVariant(
    const std::string& json,
    UpdatePackageVariant variant,
    std::wstring& version,
    std::wstring& downloadUrl) {
    version.clear();
    downloadUrl.clear();
    std::string tag;
    size_t end = 0;
    if (!ExtractJsonString(json, "tag_name", 0, tag, end)) return false;
    version = Utf8ToWide(tag);
    if (!version.empty() && (version.front() == L'v' || version.front() == L'V')) version.erase(version.begin());
    if (version.empty()) return false;
    std::string versionAscii;
    versionAscii.reserve(version.size());
    for (const wchar_t character : version) {
        if ((character < L'0' || character > L'9') && character != L'.') return false;
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
        std::string url;
    };
    std::array<Candidate, 4> candidates{};
    size_t cursor = json.find("\"assets\"");
    if (cursor == std::string::npos) return false;
    while (cursor < json.size()) {
        std::string name;
        size_t nameEnd = 0;
        if (!ExtractJsonString(json, "name", cursor, name, nameEnd)) break;
        std::string url;
        size_t urlEnd = 0;
        if (ExtractJsonString(json, "browser_download_url", nameEnd, url, urlEnd)) {
            for (size_t index = 0; index < candidateNames.size(); ++index) {
                if (name != candidateNames[index]) continue;
                Candidate& candidate = candidates[index];
                if (!candidate.seen) {
                    candidate.seen = true;
                    candidate.url = url;
                } else if (candidate.url != url) {
                    candidate.ambiguous = true;
                }
                break;
            }
            cursor = urlEnd;
        } else {
            cursor = nameEnd;
        }
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
        if (candidate.ambiguous) {
            version.clear();
            return false;
        }
        if (!candidate.seen) continue;
        downloadUrl = Utf8ToWide(candidate.url);
        if (!downloadUrl.empty()) return true;
        version.clear();
        return false;
    }
    version.clear();
    return false;
}

bool UpdateService::Start(HWND notificationWindow, bool manual, HWND dialogOwner) {
    if (notificationWindow == nullptr || IsWindow(notificationWindow) == FALSE || busy_.exchange(true)) return false;
    if (!manual && GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DISABLE_AUTO_UPDATE", nullptr, 0) > 0) {
        busy_ = false;
        return true;
    }
    std::thread([notificationWindow, manual, dialogOwner]() {
        UpdateServiceResult* result = new UpdateServiceResult(RunUpdate(manual));
        result->dialogOwner = dialogOwner;
        busy_ = false;
        if (IsWindow(notificationWindow) == FALSE || PostMessageW(notificationWindow, kUpdateServiceResultMessage, 0, reinterpret_cast<LPARAM>(result)) == FALSE) {
            delete result;
        }
    }).detach();
    return true;
}
