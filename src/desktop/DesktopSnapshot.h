#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "desktop/DesktopItem.h"
#include "model/OrganizerModel.h"

struct DesktopSnapshotMembership {
    std::wstring categoryId;
    std::size_t order = 0;
    int matchCount = 0;

    bool IsAssigned() const noexcept { return matchCount > 0; }
    bool IsUnique() const noexcept { return matchCount == 1; }
};

class DesktopSnapshot {
public:
    using IndexList = std::vector<std::size_t>;

    std::uint64_t Revision() const noexcept { return revision_; }
    const std::vector<DesktopItem>& Items() const noexcept { return items_; }

    const IndexList& ItemIndicesById(const std::wstring& itemId) const;
    const IndexList& ItemIndicesByPath(const std::wstring& path) const;
    const IndexList& OrderedItemsForCategory(
        const std::wstring& categoryId) const;
    const DesktopSnapshotMembership* MembershipForItem(
        const std::wstring& itemId) const;
    const DesktopItem* FindUniqueById(const std::wstring& itemId) const;
    const DesktopItem* FindUniqueByPath(const std::wstring& path) const;
    std::vector<DesktopItem> CopyItemsForCategory(
        const std::wstring& categoryId) const;
    bool MembershipMatches(const OrganizerConfig& config) const;

private:
    friend std::shared_ptr<const DesktopSnapshot> BuildDesktopSnapshot(
        std::uint64_t,
        std::vector<DesktopItem>,
        const OrganizerConfig&);

    std::uint64_t revision_ = 0;
    std::vector<DesktopItem> items_;
    std::unordered_map<std::wstring, IndexList> itemIndicesById_;
    std::unordered_map<std::wstring, IndexList> itemIndicesByPath_;
    std::unordered_map<std::wstring, DesktopSnapshotMembership>
        membershipByItemId_;
    std::unordered_map<std::wstring, IndexList> orderedItemsByCategory_;
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>>
        membershipDefinition_;
};

std::shared_ptr<const DesktopSnapshot> BuildDesktopSnapshot(
    std::uint64_t revision,
    std::vector<DesktopItem> items,
    const OrganizerConfig& config);
