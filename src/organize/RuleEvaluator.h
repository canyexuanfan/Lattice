#pragma once

#include "organize/AutoOrganizer.h"
#include "organize/OrganizeRule.h"

namespace lattice::organize {

bool RuleWildcardMatches(const std::wstring& pattern, const std::wstring& text);
bool OrganizeRuleMatches(const OrganizeRule& rule, const ItemSnapshot& item,
                         std::uint64_t previewTime);
void EnrichRuleFileTimes(Snapshot& snapshot, const std::vector<OrganizeRule>& rules,
                         const std::atomic<bool>* cancelRequested = nullptr);
Plan BuildRulePlan(const Snapshot& snapshot, const std::vector<OrganizeRule>& rules,
                   std::uint64_t previewTime,
                   const std::atomic<bool>* cancelRequested = nullptr);

}  // namespace lattice::organize
