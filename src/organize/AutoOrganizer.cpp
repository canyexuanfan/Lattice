#include "organize/AutoOrganizer.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <map>
#include <set>
#include <string_view>
#include <unordered_set>

namespace lattice::organize {
namespace {

constexpr wchar_t kKeepDesktopGroupId[] = L"__lattice_keep_desktop__";

struct PurposeRule {
    std::wstring_view name;
    std::initializer_list<std::wstring_view> keywords;
};

const std::array<PurposeRule, 10> kPurposeRules{{
    {L"AI 创作", {L"stable diffusion", L"webui", L"comfyui", L"deepface", L"midjourney", L"ollama", L"llama", L"chatgpt", L"人工智能", L"ai绘画", L"ai 绘画"}},
    {L"开发与运维", {L"visual studio", L"vscode", L"code.exe", L"terminal", L"powershell", L"git", L"docker", L"kubernetes", L"putty", L"winscp", L"postman", L"developer", L"devops", L"开发", L"运维"}},
    {L"办公与文档", {L"microsoft office", L"word", L"excel", L"powerpoint", L"wps", L"notion", L"onenote", L"文档", L"办公"}},
    {L"设计与创作", {L"photoshop", L"illustrator", L"figma", L"blender", L"sketch", L"canva", L"设计", L"绘图"}},
    {L"音视频", {L"premiere", L"after effects", L"davinci", L"audition", L"vlc", L"potplayer", L"ffmpeg", L"视频", L"音频"}},
    {L"浏览器与网络", {L"chrome", L"msedge", L"firefox", L"browser", L"网络", L"浏览器"}},
    {L"通讯与协作", {L"wechat", L"wecom", L"dingtalk", L"lark", L"feishu", L"teams", L"slack", L"discord", L"zoom", L"通讯", L"协作"}},
    {L"系统与硬件", {L"driver", L"control panel", L"device", L"hardware", L"system", L"系统", L"驱动", L"硬件"}},
    {L"安全工具", {L"antivirus", L"security", L"firewall", L"defender", L"malware", L"安全", L"杀毒"}},
    {L"游戏娱乐", {L"steam", L"epic games", L"xbox", L"game", L"游戏", L"娱乐"}},
}};

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring JoinEvidence(const ItemSnapshot& item, bool strongOnly) {
    std::wstring joined;
    const auto append = [&](const std::wstring& value) {
        if (!value.empty()) {
            if (!joined.empty()) {
                joined.push_back(L' ');
            }
            joined.append(value);
        }
    };
    append(item.targetPath);
    append(item.productName);
    append(item.companyName);
    append(item.description);
    append(item.publisher);
    append(item.appUserModelId);
    append(item.urlHost);
    if (!strongOnly) {
        append(item.displayName);
        append(item.arguments);
        append(item.workingDirectory);
    }
    return Lower(std::move(joined));
}

bool Contains(const std::wstring& haystack, std::wstring_view needle) {
    return !needle.empty() && haystack.find(needle) != std::wstring::npos;
}

bool IsApplication(const ItemSnapshot& item) {
    if (item.kind == DesktopItemKind::Shortcut ||
        item.kind == DesktopItemKind::UrlShortcut) {
        return true;
    }
    const std::wstring extension = Lower(std::filesystem::path(item.path).extension().wstring());
    return extension == L".exe" || extension == L".com" || extension == L".bat" ||
        extension == L".cmd" || extension == L".msi";
}

std::wstring ParentTopic(const ItemSnapshot& item) {
    std::filesystem::path candidate;
    if (!item.workingDirectory.empty()) {
        candidate = item.workingDirectory;
    } else if (!item.path.empty()) {
        candidate = std::filesystem::path(item.path).parent_path();
    }
    const std::wstring leaf = candidate.filename().wstring();
    const std::wstring normalized = Lower(leaf);
    if (normalized.empty() || normalized == L"desktop" ||
        normalized == L"桌面" || normalized == L"public desktop") {
        return L"";
    }
    return leaf;
}

std::wstring TypeFallback(const ItemSnapshot& item) {
    if (item.kind == DesktopItemKind::Folder) {
        return item.displayName.empty() ? L"" : item.displayName;
    }
    const std::wstring extension = Lower(std::filesystem::path(item.path).extension().wstring());
    static const std::set<std::wstring> documentExtensions{
        L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx",
        L".pdf", L".txt", L".md", L".rtf", L".csv"};
    static const std::set<std::wstring> designExtensions{
        L".psd", L".ai", L".svg", L".fig", L".sketch", L".blend"};
    static const std::set<std::wstring> mediaExtensions{
        L".mp3", L".wav", L".flac", L".mp4", L".mkv", L".mov", L".avi"};
    static const std::set<std::wstring> imageExtensions{
        L".png", L".jpg", L".jpeg", L".gif", L".webp", L".bmp"};
    if (documentExtensions.contains(extension)) {
        return L"办公与文档";
    }
    if (designExtensions.contains(extension) || imageExtensions.contains(extension)) {
        return L"设计与创作";
    }
    if (mediaExtensions.contains(extension)) {
        return L"音视频";
    }
    return L"";
}

std::uint64_t HashText(std::uint64_t hash, std::wstring_view value) {
    constexpr std::uint64_t kPrime = 1099511628211ull;
    for (const wchar_t ch : value) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= kPrime;
    }
    return hash;
}

std::wstring Hex(std::uint64_t value) {
    wchar_t buffer[17]{};
    swprintf_s(buffer, L"%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::wstring StableCategoryId(
    const std::wstring& monitorId,
    const std::wstring& name) {
    std::uint64_t hash = 1469598103934665603ull;
    hash = HashText(hash, Lower(monitorId));
    hash = HashText(hash, L"\n");
    hash = HashText(hash, Lower(name));
    return L"auto-" + Hex(hash);
}

std::wstring StablePlanId(const Snapshot& snapshot) {
    std::uint64_t hash = 1469598103934665603ull;
    hash ^= snapshot.configRevision;
    hash *= 1099511628211ull;
    for (const ItemSnapshot& item : snapshot.items) {
        hash = HashText(hash, item.id);
        hash = HashText(hash, item.parsingIdentity);
        hash = HashText(hash, item.sourceCategoryId);
        hash = HashText(hash, item.monitorId);
    }
    return L"plan-" + Hex(hash);
}

struct Classification {
    std::wstring name;
    Confidence confidence = Confidence::Low;
    std::wstring reason;
};

std::wstring VersionString(
    const std::vector<unsigned char>& data,
    WORD language,
    WORD codePage,
    const wchar_t* name) {
    wchar_t query[128]{};
    swprintf_s(
        query, L"\\StringFileInfo\\%04x%04x\\%s",
        language, codePage, name);
    void* value = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(
            const_cast<unsigned char*>(data.data()), query,
            &value, &length) || value == nullptr || length == 0) {
        return {};
    }
    return std::wstring(static_cast<const wchar_t*>(value), length - 1);
}

void ReadVersionMetadata(ItemSnapshot& item) {
    std::wstring target = item.targetPath;
    if (target.empty()) target = item.path;
    if (Lower(std::filesystem::path(target).extension().wstring()) != L".exe") {
        return;
    }
    if (target.size() < 3 || target[1] != L':' ||
        (target[2] != L'\\' && target[2] != L'/')) {
        return;
    }
    const wchar_t root[]{target[0], L':', L'\\', L'\0'};
    const UINT driveType = GetDriveTypeW(root);
    if (driveType != DRIVE_FIXED && driveType != DRIVE_RAMDISK) return;
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(target.c_str(), &ignored);
    if (size == 0 || size > 4 * 1024 * 1024) return;
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(target.c_str(), 0, size, data.data())) return;
    struct Translation { WORD language; WORD codePage; };
    Translation* translations = nullptr;
    UINT translationBytes = 0;
    WORD language = 0x0409;
    WORD codePage = 0x04b0;
    if (VerQueryValueW(
            data.data(), L"\\VarFileInfo\\Translation",
            reinterpret_cast<void**>(&translations), &translationBytes) &&
        translations != nullptr && translationBytes >= sizeof(Translation)) {
        language = translations[0].language;
        codePage = translations[0].codePage;
    }
    item.productName = VersionString(
        data, language, codePage, L"ProductName");
    item.companyName = VersionString(
        data, language, codePage, L"CompanyName");
    item.description = VersionString(
        data, language, codePage, L"FileDescription");
}

void ReadUrlMetadata(ItemSnapshot& item) {
    if (item.kind != DesktopItemKind::UrlShortcut || item.path.empty()) return;
    wchar_t url[4096]{};
    if (GetPrivateProfileStringW(
            L"InternetShortcut", L"URL", L"", url,
            ARRAYSIZE(url), item.path.c_str()) == 0) {
        return;
    }
    if (item.targetPath.empty()) item.targetPath = url;
    std::wstring value = Lower(url);
    std::size_t start = value.find(L"://");
    start = start == std::wstring::npos ? 0 : start + 3;
    const std::size_t end = value.find_first_of(L"/?#", start);
    std::wstring authority = value.substr(start, end - start);
    const std::size_t at = authority.rfind(L'@');
    if (at != std::wstring::npos) authority.erase(0, at + 1);
    if (!authority.empty() && authority.front() == L'[') {
        const std::size_t close = authority.find(L']');
        item.urlHost = close == std::wstring::npos
            ? authority : authority.substr(0, close + 1);
    } else {
        const std::size_t colon = authority.find(L':');
        item.urlHost = authority.substr(0, colon);
    }
}

Classification ClassifyApplication(const ItemSnapshot& item) {
    const std::wstring strong = JoinEvidence(item, true);
    const std::wstring all = JoinEvidence(item, false);
    std::vector<std::pair<const PurposeRule*, bool>> matches;
    for (const PurposeRule& rule : kPurposeRules) {
        bool strongMatch = false;
        bool weakMatch = false;
        for (const std::wstring_view keyword : rule.keywords) {
            strongMatch = strongMatch || Contains(strong, keyword);
            weakMatch = weakMatch || Contains(all, keyword);
        }
        if (strongMatch || weakMatch) {
            matches.emplace_back(&rule, strongMatch);
        }
    }
    if (matches.empty()) {
        return {L"", Confidence::Low,
                L"没有解析到足够稳定的软件身份或用途证据，保持原位。"};
    }
    if (matches.size() > 1) {
        return {std::wstring(matches.front().first->name), Confidence::Medium,
                L"本机身份信息同时匹配多个用途，需要确认后才调整。"};
    }
    const bool strongMatch = matches.front().second;
    return {
        std::wstring(matches.front().first->name),
        strongMatch ? Confidence::High : Confidence::Medium,
        strongMatch
            ? L"快捷方式目标或本机产品身份明确匹配该应用用途。"
            : L"名称和工作目录提示该用途，但缺少唯一产品身份，需要确认。"};
}

const ExistingCategorySnapshot* FindExistingCategory(
    const Snapshot& snapshot,
    const std::wstring& monitorId,
    const std::wstring& suggestedName) {
    const std::wstring normalizedName = Lower(suggestedName);
    const auto found = std::find_if(
        snapshot.categories.begin(), snapshot.categories.end(),
        [&](const ExistingCategorySnapshot& category) {
            return Lower(category.name) == normalizedName &&
                category.monitorId == monitorId;
        });
    return found == snapshot.categories.end() ? nullptr : &*found;
}

}  // namespace

void EnrichSnapshotLocalMetadata(
    Snapshot& snapshot,
    const std::atomic<bool>* cancelRequested) {
    for (ItemSnapshot& item : snapshot.items) {
        if (cancelRequested != nullptr && cancelRequested->load()) return;
        if (item.missing) continue;
        ReadUrlMetadata(item);
        ReadVersionMetadata(item);
        if (item.publisher.empty()) item.publisher = item.companyName;
    }
}

bool IsOwnershipAdjustment(const Decision& decision) noexcept {
    return !decision.targetCategoryId.empty() &&
        decision.targetCategoryId != decision.sourceCategoryId;
}

Plan BuildPlan(const Snapshot& snapshot) {
    Plan plan;
    plan.id = StablePlanId(snapshot);
    plan.baseConfigRevision = snapshot.configRevision;

    std::map<std::pair<std::wstring, std::wstring>, std::size_t> topicCounts;
    for (const ItemSnapshot& item : snapshot.items) {
        if (item.missing || IsApplication(item) ||
            !item.sourceCategoryId.empty()) {
            continue;
        }
        const std::wstring topic = ParentTopic(item);
        if (!topic.empty()) {
            ++topicCounts[{item.monitorId, Lower(topic)}];
        }
    }

    std::unordered_set<std::wstring> seenItems;
    for (const ItemSnapshot& item : snapshot.items) {
        const std::wstring stableIdentity =
            item.parsingIdentity.empty() ? item.id : item.parsingIdentity;
        if (item.id.empty() || stableIdentity.empty() ||
            !seenItems.insert(stableIdentity).second) {
            continue;
        }

        Decision decision;
        decision.itemId = item.id;
        decision.parsingIdentity = stableIdentity;
        decision.sourceCategoryId = item.sourceCategoryId;
        decision.sourceCategoryName = item.sourceCategoryName;
        decision.monitorId = item.monitorId;
        decision.sourceIndex = item.sourceIndex;

        if (item.missing) {
            decision.reason = L"项目当前不可访问，本次保持原位，不创建缺失引用。";
            plan.decisions.push_back(std::move(decision));
            continue;
        }

        if (!item.sourceCategoryId.empty()) {
            decision.targetCategoryId = item.sourceCategoryId;
            decision.targetCategoryName = item.sourceCategoryName;
            decision.targetIsExistingCategory = true;
            decision.confidence = Confidence::High;
            decision.reason = L"已有格子归属是最高优先级证据，本次保持不变。";
            plan.decisions.push_back(std::move(decision));
            continue;
        }

        Classification classification;
        if (IsApplication(item)) {
            classification = ClassifyApplication(item);
        } else {
            const std::wstring topic = ParentTopic(item);
            if (!topic.empty() &&
                topicCounts[{item.monitorId, Lower(topic)}] >= 2) {
                classification = {
                    topic, Confidence::High,
                    L"与同一项目目录中的多个工作项目具有稳定目录关系。"};
            } else {
                const std::wstring fallback = TypeFallback(item);
                if (!fallback.empty()) {
                    classification = {
                        fallback, Confidence::Medium,
                        L"仅有文件类型或文件夹主题证据，需要确认后才调整。"};
                } else {
                    classification = {
                        L"", Confidence::Low,
                        L"没有足够的项目、业务或类型关系，保持原位。"};
                }
            }
        }

        decision.targetCategoryName = classification.name;
        decision.confidence = classification.confidence;
        decision.reason = classification.reason;
        if (!classification.name.empty()) {
            if (const ExistingCategorySnapshot* existing = FindExistingCategory(
                    snapshot, item.monitorId, classification.name)) {
                decision.targetCategoryId = existing->id;
                decision.targetCategoryName = existing->name;
                decision.targetIsExistingCategory = true;
                decision.selected = classification.confidence == Confidence::High &&
                    !existing->locked;
                if (existing->locked) {
                    decision.confidence = Confidence::Medium;
                    decision.selected = false;
                    decision.reason += L" 目标格子已锁定，需要确认或解锁。";
                }
            } else {
                decision.targetCategoryId = StableCategoryId(
                    item.monitorId, classification.name);
            }
        }
        plan.decisions.push_back(std::move(decision));
    }

    std::map<std::wstring, std::size_t> highConfidenceNewCounts;
    for (const Decision& decision : plan.decisions) {
        if (!decision.targetCategoryId.empty() &&
            !decision.targetIsExistingCategory &&
            decision.confidence == Confidence::High) {
            ++highConfidenceNewCounts[decision.targetCategoryId];
        }
    }

    for (Decision& decision : plan.decisions) {
        if (decision.targetCategoryId.empty()) {
            continue;
        }
        if (!decision.targetIsExistingCategory) {
            const std::size_t count = highConfidenceNewCounts[decision.targetCategoryId];
            if (count < 2) {
                decision.selected = false;
                if (decision.confidence == Confidence::High) {
                    decision.confidence = Confidence::Low;
                    decision.reason += L" 同屏高置信项目不足2个，不新建单项目格子。";
                }
            } else {
                decision.selected = decision.confidence == Confidence::High;
            }
        }
    }

    std::map<std::wstring, GroupPlan> groupsById;
    for (const Decision& decision : plan.decisions) {
        if (decision.targetCategoryId.empty()) {
            continue;
        }
        GroupPlan& group = groupsById[decision.targetCategoryId];
        group.id = decision.targetCategoryId;
        group.name = decision.targetCategoryName;
        group.monitorId = decision.monitorId;
        group.existingCategory = decision.targetIsExistingCategory;
        group.createNewCategory = !decision.targetIsExistingCategory &&
            highConfidenceNewCounts[decision.targetCategoryId] >= 2;
        if (IsOwnershipAdjustment(decision)) {
            group.itemIds.push_back(decision.itemId);
        }
    }
    for (auto& [id, group] : groupsById) {
        (void)id;
        plan.groups.push_back(std::move(group));
    }
    std::sort(plan.groups.begin(), plan.groups.end(), [](const GroupPlan& left,
                                                         const GroupPlan& right) {
        if (left.monitorId != right.monitorId) {
            return left.monitorId < right.monitorId;
        }
        if (left.existingCategory != right.existingCategory) {
            return left.existingCategory;
        }
        if (left.itemIds.size() != right.itemIds.size()) {
            return left.itemIds.size() > right.itemIds.size();
        }
        return left.name < right.name;
    });
    GroupPlan keepDesktop;
    keepDesktop.id = kKeepDesktopGroupId;
    keepDesktop.name = L"保持桌面";
    for (const Decision& decision : plan.decisions) {
        const bool sourceIsDesktop = decision.sourceCategoryId.empty();
        if (sourceIsDesktop &&
            (decision.targetCategoryId.empty() || !decision.selected)) {
            keepDesktop.itemIds.push_back(decision.itemId);
        }
    }
    // Keep this synthetic destination available even when the initial plan has
    // no unselected items. A user may still drag an accepted suggestion back
    // to the desktop during preview.
    plan.groups.push_back(std::move(keepDesktop));
    return plan;
}

}  // namespace lattice::organize
