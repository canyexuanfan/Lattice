#include "desktop/DesktopSnapshot.h"

#include <utility>

#include "util/StringUtil.h"

namespace {

const DesktopSnapshot::IndexList& EmptyIndices() {
    static const DesktopSnapshot::IndexList empty;
    return empty;
}

std::wstring NormalizeKey(const std::wstring& value) {
    return ToLowerCopy(value);
}

bool SameKey(const std::wstring& left, const std::wstring& right) {
    return NormalizeKey(left) == NormalizeKey(right);
}

std::vector<std::pair<std::wstring, std::vector<std::wstring>>>
MembershipDefinition(const OrganizerConfig& config) {
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> result;
    result.reserve(config.categories.size() + 1);
    const auto append = [&](
        const std::wstring& categoryId,
        const std::vector<std::wstring>& itemIds) {
        std::vector<std::wstring> normalizedIds;
        normalizedIds.reserve(itemIds.size());
        for (const std::wstring& itemId : itemIds) {
            normalizedIds.push_back(NormalizeKey(itemId));
        }
        result.emplace_back(NormalizeKey(categoryId), std::move(normalizedIds));
    };
    append(kUncategorizedCategoryId, config.uncategorizedItemIds);
    for (const Category& category : config.categories) {
        append(category.id, category.itemIds);
    }
    return result;
}

}  // namespace

const DesktopSnapshot::IndexList& DesktopSnapshot::ItemIndicesById(
    const std::wstring& itemId) const {
    const auto found = itemIndicesById_.find(NormalizeKey(itemId));
    return found == itemIndicesById_.end() ? EmptyIndices() : found->second;
}

const DesktopSnapshot::IndexList& DesktopSnapshot::ItemIndicesByPath(
    const std::wstring& path) const {
    const auto found = itemIndicesByPath_.find(NormalizeKey(path));
    return found == itemIndicesByPath_.end() ? EmptyIndices() : found->second;
}

const DesktopSnapshot::IndexList& DesktopSnapshot::OrderedItemsForCategory(
    const std::wstring& categoryId) const {
    const auto found = orderedItemsByCategory_.find(NormalizeKey(categoryId));
    return found == orderedItemsByCategory_.end() ? EmptyIndices() : found->second;
}

const DesktopSnapshotMembership* DesktopSnapshot::MembershipForItem(
    const std::wstring& itemId) const {
    const auto found = membershipByItemId_.find(NormalizeKey(itemId));
    return found == membershipByItemId_.end() ? nullptr : &found->second;
}

const DesktopItem* DesktopSnapshot::FindUniqueById(
    const std::wstring& itemId) const {
    const IndexList& indices = ItemIndicesById(itemId);
    return indices.size() == 1 && indices.front() < items_.size()
        ? &items_[indices.front()]
        : nullptr;
}

const DesktopItem* DesktopSnapshot::FindUniqueByPath(
    const std::wstring& path) const {
    const IndexList& indices = ItemIndicesByPath(path);
    return indices.size() == 1 && indices.front() < items_.size()
        ? &items_[indices.front()]
        : nullptr;
}

std::vector<DesktopItem> DesktopSnapshot::CopyItemsForCategory(
    const std::wstring& categoryId) const {
    std::vector<DesktopItem> result;
    const IndexList& indices = OrderedItemsForCategory(categoryId);
    result.reserve(indices.size());
    for (const std::size_t index : indices) {
        if (index < items_.size()) {
            result.push_back(items_[index]);
        }
    }
    return result;
}

bool DesktopSnapshot::MembershipMatches(
    const OrganizerConfig& config) const {
    return membershipDefinition_ == MembershipDefinition(config);
}

std::shared_ptr<const DesktopSnapshot> BuildDesktopSnapshot(
    std::uint64_t revision,
    std::vector<DesktopItem> items,
    const OrganizerConfig& config) {
    auto snapshot = std::make_shared<DesktopSnapshot>();
    snapshot->revision_ = revision;
    snapshot->items_ = std::move(items);
    snapshot->membershipDefinition_ = MembershipDefinition(config);

    for (std::size_t index = 0; index < snapshot->items_.size(); ++index) {
        const DesktopItem& item = snapshot->items_[index];
        snapshot->itemIndicesById_[NormalizeKey(item.id)].push_back(index);
        snapshot->itemIndicesByPath_[NormalizeKey(item.path)].push_back(index);
    }

    const auto recordMembership = [&](
        const std::wstring& categoryId,
        const std::vector<std::wstring>& itemIds) {
        for (std::size_t order = 0; order < itemIds.size(); ++order) {
            DesktopSnapshotMembership& membership =
                snapshot->membershipByItemId_[NormalizeKey(itemIds[order])];
            if (membership.matchCount == 0) {
                membership.categoryId = categoryId;
                membership.order = order;
            }
            ++membership.matchCount;
        }
    };
    recordMembership(kUncategorizedCategoryId, config.uncategorizedItemIds);
    for (const Category& category : config.categories) {
        recordMembership(category.id, category.itemIds);
    }

    const auto buildCategory = [&](
        const std::wstring& categoryId,
        const std::vector<std::wstring>& itemIds) {
        DesktopSnapshot::IndexList& ordered =
            snapshot->orderedItemsByCategory_[NormalizeKey(categoryId)];
        ordered.reserve(itemIds.size());
        for (const std::wstring& itemId : itemIds) {
            const DesktopSnapshotMembership* membership =
                snapshot->MembershipForItem(itemId);
            const DesktopSnapshot::IndexList& indices =
                snapshot->ItemIndicesById(itemId);
            if (membership == nullptr || !membership->IsUnique() ||
                !SameKey(membership->categoryId, categoryId) ||
                indices.size() != 1 || indices.front() >= snapshot->items_.size()) {
                continue;
            }
            ordered.push_back(indices.front());
        }
    };
    buildCategory(kUncategorizedCategoryId, config.uncategorizedItemIds);
    for (const Category& category : config.categories) {
        buildCategory(category.id, category.itemIds);
    }

    return snapshot;
}
