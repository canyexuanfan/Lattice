#include "organize/RuleEvaluator.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <set>
#include <stdexcept>

namespace lattice::organize {
namespace {
constexpr std::uint64_t kTicksPerDay = 864000000000ULL;

std::wstring Trim(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t");
    return first == std::wstring::npos ? std::wstring{} :
        value.substr(first, value.find_last_not_of(L" \t") - first + 1);
}

bool ValidText(const std::wstring& value, std::size_t limit) {
    return value.size() <= limit && value.find_first_of(L"\r\n") == std::wstring::npos &&
        value.find(L'\0') == std::wstring::npos;
}

std::vector<std::wstring> Extensions(const std::wstring& input) {
    std::vector<std::wstring> output;
    std::size_t start = 0;
    while (start < input.size()) {
        const auto end = input.find(L';', start);
        auto value = Trim(input.substr(start, end == std::wstring::npos ? end : end - start));
        if (value.empty()) return {};
        if (value.front() != L'.') value.insert(value.begin(), L'.');
        if (value.size() < 2 || value.find_first_of(L"*?\\/:. \t", 1) != std::wstring::npos) return {};
        output.push_back(std::move(value));
        if (end == std::wstring::npos) break;
        start = end + 1;
        if (start == input.size()) return {};
    }
    return output;
}

bool Same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::uint64_t Ticks(FILETIME value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}
}  // namespace

bool ValidateOrganizeRules(const std::vector<OrganizeRule>& rules, std::wstring& error) {
    error.clear();
    if (rules.size() > kMaximumOrganizeRules) { error = L"最多保存64条规则。"; return false; }
    std::set<std::wstring> ids;
    for (const auto& rule : rules) {
        const auto fail = [&](const wchar_t* text) { error = rule.name + L"：" + text; return false; };
        if (Trim(rule.id).empty() || !ValidText(rule.id, 80) || !ids.insert(rule.id).second)
            return fail(L"规则身份缺失或重复。");
        if (Trim(rule.name).empty() || !ValidText(rule.name, 80)) return fail(L"请填写规则名称（最多80字）。");
        if (!ValidText(rule.namePattern, 256) || !ValidText(rule.targetPattern, 1024) ||
            !ValidText(rule.extensions, 256)) return fail(L"匹配条件过长或含换行。");
        if (rule.itemType < RuleItemType::Any || rule.itemType > RuleItemType::UrlShortcut ||
            rule.timeField < RuleTimeField::Any || rule.timeField > RuleTimeField::Created ||
            rule.age < RuleAge::Recent || rule.age > RuleAge::Older)
            return fail(L"规则条件无效。");
        if (!rule.extensions.empty() && (rule.itemType != RuleItemType::File || Extensions(rule.extensions).empty()))
            return fail(L"扩展名须用于普通文件，以分号分隔，例如 .pdf;.docx。");
        if (!rule.targetPattern.empty() && rule.itemType != RuleItemType::Any &&
            rule.itemType != RuleItemType::Shortcut && rule.itemType != RuleItemType::UrlShortcut)
            return fail(L"快捷方式目标条件不能与普通文件或文件夹条件同时使用。");
        if (rule.timeField != RuleTimeField::Any && (rule.days < 1 || rule.days > 36500))
            return fail(L"时间范围须为1–36500天。");
        if (rule.namePattern.empty() && rule.itemType == RuleItemType::Any && rule.targetPattern.empty() &&
            rule.timeField == RuleTimeField::Any) return fail(L"请至少填写一项匹配条件。");
        if (rule.targetCategoryId.empty() == Trim(rule.newCategoryName).empty() ||
            !ValidText(rule.targetCategoryId, 256) || !ValidText(rule.newCategoryName, 80))
            return fail(L"请选择一个已有格子或填写一个新格子名称。");
    }
    return true;
}

bool RuleWildcardMatches(const std::wstring& pattern, const std::wstring& text) {
    // Greedy '*' matching uses constant memory, no recursive or regex engine.
    std::size_t p = 0, t = 0, star = std::wstring::npos, restart = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == L'?') { ++p; ++t; continue; }
        if (p < pattern.size() && pattern[p] == L'*') { star = p++; restart = t; continue; }
        const auto end = pattern.find_first_of(L"*?", p);
        const auto length = (end == std::wstring::npos ? pattern.size() : end) - p;
        // Compare a complete literal span once. A later '*' retry has even
        // less text available, so an overlong required span cannot recover.
        if (length > text.size() - t) return false;
        if (length && CompareStringOrdinal(pattern.data() + p, static_cast<int>(length),
                text.data() + t, static_cast<int>(length), TRUE) == CSTR_EQUAL) {
            p += length; t += length; continue;
        }
        if (star != std::wstring::npos) { p = star + 1; t = ++restart; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == L'*') ++p;
    return p == pattern.size();
}

bool OrganizeRuleMatches(const OrganizeRule& rule, const ItemSnapshot& item, std::uint64_t previewTime) {
    if (!rule.enabled || item.missing || item.path.rfind(L"::", 0) == 0) return false;
    if (!rule.namePattern.empty() && !RuleWildcardMatches(rule.namePattern, item.displayName)) return false;
    const auto kind = item.kind;
    if ((rule.itemType == RuleItemType::File && kind != DesktopItemKind::File) ||
        (rule.itemType == RuleItemType::Folder && kind != DesktopItemKind::Folder) ||
        (rule.itemType == RuleItemType::Shortcut && kind != DesktopItemKind::Shortcut) ||
        (rule.itemType == RuleItemType::UrlShortcut && kind != DesktopItemKind::UrlShortcut)) return false;
    if (!rule.extensions.empty()) {
        const auto extension = std::filesystem::path(item.path).extension().wstring();
        const auto list = Extensions(rule.extensions);
        if (kind != DesktopItemKind::File || std::none_of(list.begin(), list.end(),
            [&](const auto& value) { return Same(value, extension); })) return false;
    }
    if (!rule.targetPattern.empty() && (item.targetPath.empty() ||
        (kind != DesktopItemKind::Shortcut && kind != DesktopItemKind::UrlShortcut) ||
        !RuleWildcardMatches(rule.targetPattern, item.targetPath))) return false;
    if (rule.timeField != RuleTimeField::Any) {
        const auto value = rule.timeField == RuleTimeField::Created ? item.creationTime : item.modificationTime;
        if (value == 0 || previewTime == 0 || rule.days < 1 || rule.days > 36500) return false;
        const auto age = value >= previewTime ? 0 : previewTime - value;
        const bool recent = age <= static_cast<std::uint64_t>(rule.days) * kTicksPerDay;
        if (recent != (rule.age == RuleAge::Recent)) return false;
    }
    return true;
}

void EnrichRuleFileTimes(Snapshot& snapshot, const std::vector<OrganizeRule>& rules,
                         const std::atomic<bool>* cancelRequested) {
    if (std::none_of(rules.begin(), rules.end(), [](const auto& rule) {
            return rule.enabled && rule.timeField != RuleTimeField::Any; })) return;
    for (auto& item : snapshot.items) {
        if (cancelRequested && cancelRequested->load()) return;
        if (item.missing || item.path.rfind(L"::", 0) == 0 || item.path.empty()) continue;
        if (item.path.size() < 3 || item.path[1] != L':' ||
            (item.path[2] != L'\\' && item.path[2] != L'/')) continue;
        const wchar_t root[]{item.path[0], L':', L'\\', L'\0'};
        const UINT drive = GetDriveTypeW(root);
        if (drive != DRIVE_FIXED && drive != DRIVE_RAMDISK) continue;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(item.path.c_str(), GetFileExInfoStandard, &data)) {
            item.creationTime = Ticks(data.ftCreationTime);
            item.modificationTime = Ticks(data.ftLastWriteTime);
        }
    }
}

Plan BuildRulePlan(const Snapshot& snapshot, const std::vector<OrganizeRule>& rules, std::uint64_t previewTime,
                   const std::atomic<bool>* cancelRequested) {
    std::wstring error;
    if (!ValidateOrganizeRules(rules, error)) throw std::invalid_argument("invalid organize rules");
    if (cancelRequested && cancelRequested->load()) return {};
    // Reuse the existing identity projection and group builder. No purpose
    // inference survives in rule mode; unmatched items retain their source.
    Plan plan = BuildPlan(snapshot);
    plan.usesRules = true;
    plan.evaluatedRules = rules;
    plan.id += L"-rules-" + std::to_wstring(previewTime);
    for (auto& decision : plan.decisions) {
        if (cancelRequested && cancelRequested->load()) return {};
        decision.targetCategoryId = decision.sourceCategoryId;
        decision.targetCategoryName = decision.sourceCategoryName;
        decision.targetIsExistingCategory = !decision.sourceCategoryId.empty();
        decision.selected = false;
        decision.changesExistingOwnership = false;
        decision.confidence = Confidence::Low;
        decision.reason = L"没有匹配的启用规则，保持原位置。";
        const auto item = std::find_if(snapshot.items.begin(), snapshot.items.end(), [&](const auto& value) {
            return value.id == decision.itemId &&
                (value.parsingIdentity.empty() ? value.id : value.parsingIdentity) == decision.parsingIdentity;
        });
        if (item == snapshot.items.end()) continue;
        for (const auto& rule : rules) {
            if (cancelRequested && cancelRequested->load()) return {};
            if (!OrganizeRuleMatches(rule, *item, previewTime)) continue;
            decision.reason = L"命中规则“" + rule.name + L"”：所有已填写条件均满足；按列表优先级采用首条匹配。";
            if (!rule.namePattern.empty()) decision.reason += L" 名称：" + rule.namePattern + L"。";
            if (!rule.extensions.empty()) decision.reason += L" 扩展名：" + rule.extensions + L"。";
            if (!rule.targetPattern.empty()) decision.reason += L" 快捷方式目标：" + rule.targetPattern + L"。";
            if (rule.timeField != RuleTimeField::Any) decision.reason +=
                std::wstring(rule.timeField == RuleTimeField::Created ? L" 创建时间" : L" 修改时间") +
                (rule.age == RuleAge::Recent ? L"最近" : L"超过") + std::to_wstring(rule.days) + L"天。";
            const auto source = std::find_if(snapshot.categories.begin(), snapshot.categories.end(), [&](const auto& cat) {
                return cat.id == item->sourceCategoryId;
            });
            if (source != snapshot.categories.end() && source->locked) {
                decision.reason += L" 源格子已锁定，保持原位。"; break;
            }
            if (!rule.targetCategoryId.empty()) {
                const auto target = std::find_if(snapshot.categories.begin(), snapshot.categories.end(), [&](const auto& cat) {
                    return cat.id == rule.targetCategoryId;
                });
                if (target == snapshot.categories.end() || target->locked) {
                    decision.reason += L" 目标格子缺失或已锁定，保持原位。"; break;
                }
                decision.targetCategoryId = target->id;
                decision.targetCategoryName = target->name;
                decision.targetIsExistingCategory = true;
                decision.monitorId = target->monitorId;
            } else {
                const auto name = Trim(rule.newCategoryName);
                const auto target = std::find_if(snapshot.categories.begin(), snapshot.categories.end(), [&](const auto& cat) {
                    return cat.monitorId == item->monitorId && Same(cat.name, name);
                });
                if (target != snapshot.categories.end() && target->locked) {
                    decision.reason += L" 同名目标格子已锁定，保持原位。"; break;
                }
                decision.targetCategoryId = target == snapshot.categories.end()
                    ? OrganizeCategoryId(item->monitorId, name) : target->id;
                decision.targetCategoryName = name;
                decision.targetIsExistingCategory = target != snapshot.categories.end();
            }
            decision.confidence = Confidence::High;
            decision.changesExistingOwnership = !item->sourceCategoryId.empty() && IsOwnershipAdjustment(decision);
            decision.selected = IsOwnershipAdjustment(decision);
            if (!decision.selected) decision.reason += L" 已在目标格子，无需调整。";
            break;
        }
    }
    RebuildPlanGroups(plan, 1);
    return plan;
}

}  // namespace lattice::organize
