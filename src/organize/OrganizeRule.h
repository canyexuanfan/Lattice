#pragma once

#include <string>
#include <vector>

namespace lattice::organize {

enum class RuleItemType { Any, File, Folder, Shortcut, UrlShortcut };
enum class RuleTimeField { Any, Modified, Created };
enum class RuleAge { Recent, Older };

// A rule is configuration only. It never owns desktop items or executes moves.
struct OrganizeRule {
    std::wstring id;
    std::wstring name;
    bool enabled = true;
    std::wstring namePattern;
    RuleItemType itemType = RuleItemType::Any;
    std::wstring extensions;
    std::wstring targetPattern;
    RuleTimeField timeField = RuleTimeField::Any;
    RuleAge age = RuleAge::Recent;
    int days = 30;
    std::wstring targetCategoryId;
    std::wstring newCategoryName;
    bool operator==(const OrganizeRule&) const = default;
};

constexpr std::size_t kMaximumOrganizeRules = 64;
bool ValidateOrganizeRules(const std::vector<OrganizeRule>& rules, std::wstring& error);

}  // namespace lattice::organize
