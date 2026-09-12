#include "organize/AutoOrganizeTransaction.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <utility>

namespace lattice::organize {
namespace {

constexpr wchar_t kUncategorizedId[] = L"uncategorized";
constexpr std::size_t kMaximumUndoRecords = 10;

bool SameText(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(
        left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::vector<std::wstring>* Membership(
    AppConfig& config,
    const std::wstring& categoryId) {
    if (categoryId.empty()) return nullptr;
    if (categoryId == kUncategorizedId) return &config.uncategorizedItemIds;
    const auto found = std::find_if(
        config.categories.begin(), config.categories.end(),
        [&](const CategoryConfig& category) { return category.id == categoryId; });
    return found == config.categories.end() ? nullptr : &found->itemIds;
}

const CategoryConfig* CategoryById(
    const AppConfig& config,
    const std::wstring& categoryId) {
    const auto found = std::find_if(
        config.categories.begin(), config.categories.end(),
        [&](const CategoryConfig& category) { return category.id == categoryId; });
    return found == config.categories.end() ? nullptr : &*found;
}

void RemoveMembership(AppConfig& config, const std::wstring& itemId) {
    const auto remove = [&](std::vector<std::wstring>& values) {
        values.erase(
            std::remove(values.begin(), values.end(), itemId), values.end());
    };
    remove(config.uncategorizedItemIds);
    for (CategoryConfig& category : config.categories) remove(category.itemIds);
}

const ItemConfig* ItemById(const AppConfig& config, const std::wstring& itemId) {
    const auto found = std::find_if(
        config.items.begin(), config.items.end(),
        [&](const ItemConfig& item) { return item.id == itemId; });
    return found == config.items.end() ? nullptr : &*found;
}

bool SameItem(const ItemConfig& left, const ItemConfig& right) {
    return left.id == right.id && SameText(left.path, right.path) &&
        left.displayName == right.displayName &&
        SameText(left.originalDesktopPath, right.originalDesktopPath) &&
        left.desktopX == right.desktopX && left.desktopY == right.desktopY &&
        left.hasDesktopPosition == right.hasDesktopPosition &&
        left.desktopVisibilityMode == right.desktopVisibilityMode &&
        left.desktopVisibilityOriginalFlags == right.desktopVisibilityOriginalFlags &&
        left.desktopVisibilityNewStartValue == right.desktopVisibilityNewStartValue &&
        left.desktopVisibilityClassicValue == right.desktopVisibilityClassicValue;
}

bool SameWindow(const WindowConfig& left, const WindowConfig& right) {
    return left.x == right.x && left.y == right.y &&
        left.width == right.width && left.height == right.height &&
        left.opacity == right.opacity && left.normalHeight == right.normalHeight &&
        left.iconSize == right.iconSize && left.density == right.density &&
        left.viewMode == right.viewMode &&
        left.contentViewMode == right.contentViewMode &&
        left.sortMode == right.sortMode && left.tabSide == right.tabSide &&
        left.titleOpacity == right.titleOpacity && left.dpi == right.dpi &&
        left.monitorId == right.monitorId && left.collapsed == right.collapsed &&
        left.locked == right.locked && left.showBorder == right.showBorder &&
        left.autoArrange == right.autoArrange &&
        left.fixedExpanded == right.fixedExpanded;
}

bool SameCategoryMetadata(
    const CategoryConfig& current,
    const CategoryConfig& created) {
    return current.id == created.id && current.name == created.name &&
        current.storageFolder == created.storageFolder &&
        current.color == created.color && current.icon == created.icon &&
        current.tileCollapsed == created.tileCollapsed &&
        SameWindow(current.layout, created.layout);
}

bool ValidateUniqueMemberships(const AppConfig& config) {
    std::set<std::wstring> seen;
    const auto add = [&](const std::vector<std::wstring>& itemIds) {
        for (const std::wstring& itemId : itemIds) {
            if (itemId.empty() || !seen.insert(itemId).second) return false;
        }
        return true;
    };
    if (!add(config.uncategorizedItemIds)) return false;
    for (const CategoryConfig& category : config.categories) {
        if (category.id.empty() || !add(category.itemIds)) return false;
    }
    return true;
}

std::uint64_t TimestampNow() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace

bool ApplyAutoOrganizeTransaction(
    const AppConfig& current,
    const AutoOrganizeApplyRequest& request,
    AppConfig& candidate,
    AutoOrganizeTransactionResult& result) {
    result = {};
    if (request.transactionId.empty() || request.moves.empty()) {
        result.message = L"整理计划为空，配置未改变。";
        return false;
    }
    candidate = current;
    std::set<std::wstring> moveIds;
    std::set<std::wstring> categoryIds;
    for (const CategoryConfig& category : current.categories) {
        if (!categoryIds.insert(category.id).second) {
            result.conflict = true;
            result.message = L"当前配置含重复格子标识，请刷新后重试。";
            return false;
        }
    }
    for (const CategoryConfig& category : request.newCategories) {
        if (category.id.empty() || category.name.empty() ||
            !categoryIds.insert(category.id).second ||
            !category.itemIds.empty()) {
            result.conflict = true;
            result.message = L"候选格子已经失效，请重新生成建议。";
            return false;
        }
    }

    AutoOrganizeUndoRecord undo;
    undo.transactionId = request.transactionId;
    undo.timestamp = TimestampNow();
    for (const AutoOrganizeMoveRequest& move : request.moves) {
        if (move.item.id.empty() || move.identity.empty() ||
            !moveIds.insert(move.item.id).second ||
            move.targetCategoryId.empty()) {
            result.conflict = true;
            result.message = L"整理计划含重复或无效项目，请重新生成建议。";
            return false;
        }
        const ConfiguredItemMembership source =
            FindConfiguredItemMembership(current, move.item.id);
        if (!source.IsUnique() ||
            source.categoryId != move.expectedSourceCategoryId ||
            source.index != move.expectedSourceIndex) {
            result.conflict = true;
            result.message = L"项目归属或顺序已变化，请重新生成建议。";
            return false;
        }
        const ItemConfig* registered = ItemById(current, move.item.id);
        if (registered != nullptr && !SameText(registered->path, move.identity)) {
            result.conflict = true;
            result.message = L"项目身份已变化，请重新生成建议。";
            return false;
        }
        if (registered == nullptr && !SameText(move.item.path, move.identity)) {
            result.conflict = true;
            result.message = L"新增项目身份无效，请重新生成建议。";
            return false;
        }
        if (!source.categoryId.empty() &&
            source.categoryId != kUncategorizedId) {
            const CategoryConfig* sourceCategory =
                CategoryById(current, source.categoryId);
            if (sourceCategory == nullptr || sourceCategory->layout.locked) {
                result.conflict = true;
                result.message = L"涉及格子已删除或锁定，请重新生成建议。";
                return false;
            }
        }
        const CategoryConfig* target = CategoryById(current, move.targetCategoryId);
        const bool targetWillBeCreated = std::any_of(
            request.newCategories.begin(), request.newCategories.end(),
            [&](const CategoryConfig& category) {
                return category.id == move.targetCategoryId;
            });
        if ((target == nullptr && !targetWillBeCreated) ||
            (target != nullptr && target->layout.locked)) {
            result.conflict = true;
            result.message = L"目标格子已删除或锁定，请重新生成建议。";
            return false;
        }

        AutoOrganizeMembershipChange change;
        change.itemId = move.item.id;
        change.identity = move.identity;
        change.beforeCategoryId = source.categoryId;
        change.beforeIndex = source.index;
        change.afterCategoryId = move.targetCategoryId;
        change.itemWasRegistered = registered != nullptr;
        change.registeredItem = registered == nullptr ? move.item : *registered;
        undo.changes.push_back(std::move(change));
    }

    for (CategoryConfig category : request.newCategories) {
        candidate.categories.push_back(std::move(category));
    }
    for (std::size_t index = 0; index < request.moves.size(); ++index) {
        const AutoOrganizeMoveRequest& move = request.moves[index];
        AutoOrganizeMembershipChange& change = undo.changes[index];
        RemoveMembership(candidate, move.item.id);
        if (ItemById(candidate, move.item.id) == nullptr) {
            candidate.items.push_back(move.item);
        }
        std::vector<std::wstring>* target = Membership(candidate, move.targetCategoryId);
        if (target == nullptr) {
            result.conflict = true;
            result.message = L"目标格子无法建立，请重新生成建议。";
            return false;
        }
        change.afterIndex = static_cast<int>(target->size());
        target->push_back(move.item.id);
    }
    if (!ValidateUniqueMemberships(candidate)) {
        result.conflict = true;
        result.message = L"候选配置未通过唯一归属校验，未应用任何调整。";
        return false;
    }
    for (const CategoryConfig& requested : request.newCategories) {
        const CategoryConfig* created = CategoryById(candidate, requested.id);
        if (created != nullptr) undo.createdCategories.push_back(*created);
    }
    candidate.autoOrganizeUndoHistory.push_back(std::move(undo));
    if (candidate.autoOrganizeUndoHistory.size() > kMaximumUndoRecords) {
        candidate.autoOrganizeUndoHistory.erase(
            candidate.autoOrganizeUndoHistory.begin(),
            candidate.autoOrganizeUndoHistory.begin() +
                static_cast<std::ptrdiff_t>(
                    candidate.autoOrganizeUndoHistory.size() - kMaximumUndoRecords));
    }
    result.succeeded = true;
    result.appliedChanges = static_cast<int>(request.moves.size());
    result.message = L"整理已完整应用，真实文件和 Explorer 设置均未改变。";
    return true;
}

bool UndoLastAutoOrganizeTransaction(
    const AppConfig& current,
    AppConfig& candidate,
    AutoOrganizeTransactionResult& result) {
    result = {};
    if (current.autoOrganizeUndoHistory.empty()) {
        result.message = L"没有可撤销的自动整理记录。";
        return false;
    }
    candidate = current;
    const AutoOrganizeUndoRecord undo = candidate.autoOrganizeUndoHistory.back();
    std::vector<const AutoOrganizeMembershipChange*> safe;
    for (const AutoOrganizeMembershipChange& change : undo.changes) {
        const ConfiguredItemMembership membership =
            FindConfiguredItemMembership(candidate, change.itemId);
        const ItemConfig* item = ItemById(candidate, change.itemId);
        if (!membership.IsUnique() ||
            item == nullptr || !SameText(item->path, change.identity) ||
            membership.categoryId != change.afterCategoryId) {
            ++result.preservedChanges;
            continue;
        }
        if (!change.beforeCategoryId.empty() &&
            change.beforeCategoryId != kUncategorizedId &&
            CategoryById(candidate, change.beforeCategoryId) == nullptr) {
            ++result.preservedChanges;
            continue;
        }
        safe.push_back(&change);
    }
    for (const AutoOrganizeMembershipChange* change : safe) {
        RemoveMembership(candidate, change->itemId);
    }
    std::stable_sort(
        safe.begin(), safe.end(),
        [](const auto* left, const auto* right) {
            if (left->beforeCategoryId != right->beforeCategoryId) {
                return left->beforeCategoryId < right->beforeCategoryId;
            }
            return left->beforeIndex < right->beforeIndex;
        });
    for (const AutoOrganizeMembershipChange* change : safe) {
        if (!change->beforeCategoryId.empty()) {
            std::vector<std::wstring>* destination =
                Membership(candidate, change->beforeCategoryId);
            if (destination == nullptr) {
                ++result.preservedChanges;
                continue;
            }
            const std::size_t position = static_cast<std::size_t>(
                (std::max)(0, (std::min)(
                    change->beforeIndex,
                    static_cast<int>(destination->size()))));
            destination->insert(destination->begin() +
                static_cast<std::ptrdiff_t>(position), change->itemId);
        }
        ++result.appliedChanges;
    }

    for (const CategoryConfig& created : undo.createdCategories) {
        const auto currentCategory = std::find_if(
            candidate.categories.begin(), candidate.categories.end(),
            [&](const CategoryConfig& category) { return category.id == created.id; });
        if (currentCategory == candidate.categories.end()) continue;
        if (currentCategory->itemIds.empty() &&
            SameCategoryMetadata(*currentCategory, created)) {
            candidate.categories.erase(currentCategory);
        } else {
            ++result.preservedChanges;
        }
    }
    for (const AutoOrganizeMembershipChange& change : undo.changes) {
        if (change.itemWasRegistered) continue;
        const ConfiguredItemMembership membership =
            FindConfiguredItemMembership(candidate, change.itemId);
        const ItemConfig* item = ItemById(candidate, change.itemId);
        if (membership.IsUnique() && membership.categoryId.empty() &&
            item != nullptr &&
            SameItem(*item, change.registeredItem)) {
            candidate.items.erase(
                std::remove_if(
                    candidate.items.begin(), candidate.items.end(),
                    [&](const ItemConfig& value) {
                        return value.id == change.itemId;
                    }),
                candidate.items.end());
        }
    }
    candidate.autoOrganizeUndoHistory.pop_back();
    if (!ValidateUniqueMemberships(candidate)) {
        result = {};
        result.conflict = true;
        result.message = L"撤销结果未通过唯一归属校验，配置未改变。";
        return false;
    }
    result.succeeded = true;
    result.message = result.preservedChanges == 0
        ? L"已撤销最近一次自动整理。"
        : L"已撤销仍安全的调整；之后手动修改的内容已保留。";
    return true;
}

}  // namespace lattice::organize
