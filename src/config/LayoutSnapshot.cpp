#include "config/LayoutSnapshot.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <unordered_set>

namespace {

bool EqualNoCase(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(
               left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring Lowercase(std::wstring value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

int Width(const LayoutRect& rect) {
    return rect.right - rect.left;
}

int Height(const LayoutRect& rect) {
    return rect.bottom - rect.top;
}

bool ValidWindow(const WindowConfig& config) {
    return config.width >= 48 && config.width <= 32768 &&
           config.height >= 24 && config.height <= 32768 &&
           config.normalHeight >= 24 && config.normalHeight <= 32768 &&
           config.opacity >= 0 && config.opacity <= 255 &&
           config.iconSize >= 16 && config.iconSize <= 256 &&
           config.density >= 0 && config.density <= 2 &&
           config.viewMode >= 0 && config.viewMode <= 1 &&
           config.contentViewMode >= 0 && config.contentViewMode <= 1 &&
           config.sortMode >= 0 && config.sortMode <= 3 &&
           config.tabSide >= 0 && config.tabSide <= 3 &&
           config.titleOpacity >= 0 && config.titleOpacity <= 255 &&
           config.dpi >= 48 && config.dpi <= 960;
}

const LayoutMonitorSnapshot* PrimaryMonitor(
    const std::vector<LayoutMonitorSnapshot>& monitors) {
    const auto primary = std::find_if(
        monitors.begin(), monitors.end(),
        [](const LayoutMonitorSnapshot& monitor) { return monitor.primary; });
    return primary == monitors.end()
        ? (monitors.empty() ? nullptr : &monitors.front())
        : &*primary;
}

const LayoutMonitorSnapshot* FindMonitor(
    const std::vector<LayoutMonitorSnapshot>& monitors,
    const std::wstring& id) {
    const auto found = std::find_if(
        monitors.begin(), monitors.end(),
        [&](const LayoutMonitorSnapshot& monitor) {
            return !id.empty() && EqualNoCase(monitor.id, id);
        });
    return found == monitors.end() ? nullptr : &*found;
}

const LayoutMonitorSnapshot* MonitorContaining(
    const std::vector<LayoutMonitorSnapshot>& monitors,
    double x,
    double y) {
    const auto found = std::find_if(
        monitors.begin(), monitors.end(),
        [&](const LayoutMonitorSnapshot& monitor) {
            return x >= monitor.workArea.left && x < monitor.workArea.right &&
                   y >= monitor.workArea.top && y < monitor.workArea.bottom;
        });
    return found == monitors.end() ? nullptr : &*found;
}

const LayoutMonitorSnapshot* NearestMonitor(
    const std::vector<LayoutMonitorSnapshot>& monitors,
    double x,
    double y) {
    const LayoutMonitorSnapshot* nearest = nullptr;
    double nearestDistance = std::numeric_limits<double>::max();
    for (const LayoutMonitorSnapshot& monitor : monitors) {
        const double centerX =
            (static_cast<double>(monitor.workArea.left) +
             static_cast<double>(monitor.workArea.right)) / 2.0;
        const double centerY =
            (static_cast<double>(monitor.workArea.top) +
             static_cast<double>(monitor.workArea.bottom)) / 2.0;
        const double dx = centerX - x;
        const double dy = centerY - y;
        const double distance = dx * dx + dy * dy;
        if (nearest == nullptr || distance < nearestDistance) {
            nearest = &monitor;
            nearestDistance = distance;
        }
    }
    return nearest == nullptr ? PrimaryMonitor(monitors) : nearest;
}

WindowConfig MapWindow(
    const WindowConfig& saved,
    const std::vector<LayoutMonitorSnapshot>& savedMonitors,
    const std::vector<LayoutMonitorSnapshot>& currentMonitors,
    bool exactTopology) {
    WindowConfig mapped = saved;
    if (currentMonitors.empty()) {
        return mapped;
    }

    const double savedCenterX =
        static_cast<double>(saved.x) + static_cast<double>(saved.width) / 2.0;
    const double savedCenterY =
        static_cast<double>(saved.y) + static_cast<double>(saved.height) / 2.0;
    const LayoutMonitorSnapshot* source =
        FindMonitor(savedMonitors, saved.monitorId);
    if (source == nullptr) {
        source = MonitorContaining(savedMonitors, savedCenterX, savedCenterY);
    }
    if (source == nullptr) {
        source = PrimaryMonitor(savedMonitors);
    }

    const LayoutMonitorSnapshot* target =
        FindMonitor(currentMonitors, saved.monitorId);
    if (target == nullptr) {
        target = NearestMonitor(currentMonitors, savedCenterX, savedCenterY);
    }
    if (target == nullptr) {
        return mapped;
    }

    if (!exactTopology && source != nullptr) {
        const double scale =
            static_cast<double>(std::max(48, target->dpi)) /
            static_cast<double>(std::max(48, source->dpi));
        mapped.width = static_cast<int>(std::llround(saved.width * scale));
        mapped.height = static_cast<int>(std::llround(saved.height * scale));
        mapped.normalHeight = static_cast<int>(
            std::llround(saved.normalHeight * scale));

        const int sourceRangeX = std::max(1, Width(source->workArea) - saved.width);
        const int sourceRangeY = std::max(1, Height(source->workArea) - saved.height);
        const double relativeX = std::clamp(
            static_cast<double>(saved.x - source->workArea.left) /
                static_cast<double>(sourceRangeX),
            0.0, 1.0);
        const double relativeY = std::clamp(
            static_cast<double>(saved.y - source->workArea.top) /
                static_cast<double>(sourceRangeY),
            0.0, 1.0);
        const int targetRangeX =
            std::max(0, Width(target->workArea) - mapped.width);
        const int targetRangeY =
            std::max(0, Height(target->workArea) - mapped.height);
        mapped.x = target->workArea.left + static_cast<int>(
            std::llround(relativeX * targetRangeX));
        mapped.y = target->workArea.top + static_cast<int>(
            std::llround(relativeY * targetRangeY));
    }

    mapped.width = std::clamp(
        mapped.width, 48, std::max(48, Width(target->workArea)));
    mapped.height = std::clamp(
        mapped.height, 24, std::max(24, Height(target->workArea)));
    mapped.normalHeight = std::clamp(
        mapped.normalHeight, 24, std::max(24, Height(target->workArea)));
    mapped.x = std::clamp(
        mapped.x,
        target->workArea.left,
        std::max(target->workArea.left,
                 target->workArea.right - mapped.width));
    mapped.y = std::clamp(
        mapped.y,
        target->workArea.top,
        std::max(target->workArea.top,
                 target->workArea.bottom - mapped.height));
    mapped.monitorId = target->id;
    mapped.dpi = target->dpi;
    return mapped;
}

bool ContainsNoCase(
    const std::vector<std::wstring>& values,
    const std::wstring& value) {
    return std::any_of(
        values.begin(), values.end(),
        [&](const std::wstring& candidate) {
            return EqualNoCase(candidate, value);
        });
}

int CountExact(
    const std::vector<std::wstring>& values,
    const std::wstring& value) {
    return static_cast<int>(std::count(values.begin(), values.end(), value));
}

std::vector<std::wstring> RestoreItemOrder(
    const std::vector<std::wstring>& saved,
    const std::vector<std::wstring>& current,
    const std::wstring& categoryId,
    const AppConfig& config,
    const LayoutIdentityIndex& identities,
    int& restoredCount) {
    std::vector<std::wstring> result;
    std::unordered_set<std::wstring> restored;
    result.reserve(current.size());
    for (const std::wstring& itemId : saved) {
        const ConfiguredItemMembership membership =
            FindConfiguredItemMembership(config, itemId);
        if (!ContainsNoCase(identities.uniqueItemIds, itemId) ||
            !membership.IsUnique() || !membership.IsAssigned() ||
            membership.categoryId != categoryId ||
            CountExact(current, itemId) != 1 ||
            !restored.insert(itemId).second) {
            continue;
        }
        result.push_back(itemId);
        ++restoredCount;
    }
    for (const std::wstring& itemId : current) {
        if (!restored.contains(itemId)) {
            result.push_back(itemId);
        }
    }
    return result;
}

}  // namespace

LayoutSnapshot BuildLayoutSnapshot(
    const AppConfig& config,
    const std::vector<LayoutMonitorSnapshot>& monitors,
    std::uint64_t timestamp) {
    LayoutSnapshot snapshot;
    snapshot.timestamp = timestamp;
    snapshot.globalVisible = config.settings.lastVisible;
    snapshot.viewMode = std::clamp(config.window.viewMode, 0, 1);
    snapshot.main = config.window;
    snapshot.uncategorizedItemOrder = config.uncategorizedItemIds;
    snapshot.desktopDisplayLayout = config.desktopDisplayLayout;
    snapshot.monitors = monitors;
    snapshot.categories.reserve(config.categories.size());
    for (const CategoryConfig& category : config.categories) {
        snapshot.categories.push_back(LayoutCategorySnapshot{
            category.id, category.layout, category.itemIds});
    }
    return snapshot;
}

bool ValidateLayoutSnapshot(
    const LayoutSnapshot& snapshot,
    std::wstring& errorMessage) {
    errorMessage.clear();
    if (snapshot.schemaVersion != 1 && snapshot.schemaVersion != 2) {
        errorMessage = L"布局方案版本不受支持。";
        return false;
    }
    if (!ValidWindow(snapshot.main)) {
        errorMessage = L"主窗口布局字段无效。";
        return false;
    }
    if (snapshot.schemaVersion == 1 || snapshot.legacyMainOnly) {
        return true;
    }
    if (snapshot.viewMode < 0 || snapshot.viewMode > 1 ||
        snapshot.categories.size() > 4096 ||
        snapshot.desktopDisplayLayout.size() > 100000 ||
        snapshot.monitors.empty() || snapshot.monitors.size() > 64) {
        errorMessage = L"布局方案数量或全局字段无效。";
        return false;
    }

    std::unordered_set<std::wstring> categoryIds;
    for (const LayoutCategorySnapshot& category : snapshot.categories) {
        if (category.categoryId.empty() || !ValidWindow(category.layout) ||
            category.itemOrder.size() > 100000 ||
            !categoryIds.insert(Lowercase(category.categoryId)).second) {
            errorMessage = L"布局方案包含无效或重复的格子。";
            return false;
        }
        std::unordered_set<std::wstring> itemIds;
        for (const std::wstring& itemId : category.itemOrder) {
            if (itemId.empty() ||
                !itemIds.insert(Lowercase(itemId)).second) {
                errorMessage = L"格子项目顺序包含空值或重复 identity。";
                return false;
            }
        }
    }
    {
        std::unordered_set<std::wstring> itemIds;
        for (const std::wstring& itemId : snapshot.uncategorizedItemOrder) {
            if (itemId.empty() ||
                !itemIds.insert(Lowercase(itemId)).second) {
                errorMessage = L"未分类项目顺序包含空值或重复 identity。";
                return false;
            }
        }
    }
    {
        std::unordered_set<std::wstring> paths;
        for (const DesktopPlacementConfig& placement :
             snapshot.desktopDisplayLayout) {
            if (placement.path.empty() ||
                !paths.insert(Lowercase(placement.path)).second) {
                errorMessage = L"桌面显示坐标包含空值或重复 identity。";
                return false;
            }
        }
    }
    int primaryCount = 0;
    std::unordered_set<std::wstring> monitorIds;
    for (const LayoutMonitorSnapshot& monitor : snapshot.monitors) {
        if (monitor.id.empty() || Width(monitor.workArea) <= 0 ||
            Height(monitor.workArea) <= 0 || monitor.dpi < 48 ||
            monitor.dpi > 960 ||
            !monitorIds.insert(Lowercase(monitor.id)).second) {
            errorMessage = L"显示器拓扑包含无效或重复记录。";
            return false;
        }
        if (monitor.primary) ++primaryCount;
    }
    if (primaryCount != 1) {
        errorMessage = L"显示器拓扑必须包含且只包含一个主屏。";
        return false;
    }
    return true;
}
bool SameLayoutMonitorTopology(
    const std::vector<LayoutMonitorSnapshot>& left,
    const std::vector<LayoutMonitorSnapshot>& right) {
    if (left.size() != right.size()) return false;
    for (const LayoutMonitorSnapshot& monitor : left) {
        const LayoutMonitorSnapshot* candidate = FindMonitor(right, monitor.id);
        if (candidate == nullptr ||
            candidate->workArea.left != monitor.workArea.left ||
            candidate->workArea.top != monitor.workArea.top ||
            candidate->workArea.right != monitor.workArea.right ||
            candidate->workArea.bottom != monitor.workArea.bottom ||
            candidate->dpi != monitor.dpi ||
            candidate->primary != monitor.primary) {
            return false;
        }
    }
    return true;
}

bool BuildLayoutRestorePlan(
    const LayoutSnapshot& snapshot,
    const AppConfig& current,
    const std::vector<LayoutMonitorSnapshot>& currentMonitors,
    const LayoutIdentityIndex& identities,
    LayoutRestorePlan& plan,
    std::wstring& errorMessage) {
    plan = {};
    if (!ValidateLayoutSnapshot(snapshot, errorMessage)) {
        return false;
    }
    if (currentMonitors.empty()) {
        errorMessage = L"当前没有可用于恢复布局的显示器。";
        return false;
    }

    plan.before = current;
    plan.candidate = current;
    plan.legacyMainOnly = snapshot.legacyMainOnly ||
        snapshot.schemaVersion == 1;
    plan.exactMonitorTopology = SameLayoutMonitorTopology(
        snapshot.monitors, currentMonitors);
    plan.candidate.window = MapWindow(
        snapshot.main, snapshot.monitors, currentMonitors,
        plan.exactMonitorTopology);

    if (plan.legacyMainOnly) {
        return true;
    }
    plan.candidate.window.viewMode = snapshot.viewMode;
    plan.candidate.settings.lastVisible = snapshot.globalVisible;

    int restoredItems = 0;
    plan.candidate.uncategorizedItemIds = RestoreItemOrder(
        snapshot.uncategorizedItemOrder,
        current.uncategorizedItemIds,
        L"uncategorized",
        current,
        identities,
        restoredItems);

    std::vector<CategoryConfig> reordered;
    reordered.reserve(current.categories.size());
    std::unordered_set<std::wstring> emitted;
    for (const LayoutCategorySnapshot& savedCategory : snapshot.categories) {
        const int currentMatches = static_cast<int>(std::count_if(
            current.categories.begin(), current.categories.end(),
            [&](const CategoryConfig& category) {
                return EqualNoCase(category.id, savedCategory.categoryId);
            }));
        if (currentMatches != 1) continue;
        const auto found = std::find_if(
            current.categories.begin(), current.categories.end(),
            [&](const CategoryConfig& category) {
                return EqualNoCase(category.id, savedCategory.categoryId);
            });
        CategoryConfig restored = *found;
        restored.layout = MapWindow(
            savedCategory.layout, snapshot.monitors, currentMonitors,
            plan.exactMonitorTopology);
        restored.itemIds = RestoreItemOrder(
            savedCategory.itemOrder,
            found->itemIds,
            found->id,
            current,
            identities,
            restoredItems);
        emitted.insert(Lowercase(found->id));
        reordered.push_back(std::move(restored));
        ++plan.restoredCategories;
    }
    for (const CategoryConfig& category : current.categories) {
        if (!emitted.contains(Lowercase(category.id))) {
            reordered.push_back(category);
        }
    }
    plan.candidate.categories = std::move(reordered);
    plan.restoredItemOrders = restoredItems;

    for (const DesktopPlacementConfig& savedPlacement :
         snapshot.desktopDisplayLayout) {
        if (!ContainsNoCase(identities.uniquePaths, savedPlacement.path)) {
            continue;
        }
        const int currentMatches = static_cast<int>(std::count_if(
            plan.candidate.desktopDisplayLayout.begin(),
            plan.candidate.desktopDisplayLayout.end(),
            [&](const DesktopPlacementConfig& placement) {
                return EqualNoCase(placement.path, savedPlacement.path);
            }));
        if (currentMatches > 1) continue;
        const auto found = std::find_if(
            plan.candidate.desktopDisplayLayout.begin(),
            plan.candidate.desktopDisplayLayout.end(),
            [&](const DesktopPlacementConfig& placement) {
                return EqualNoCase(placement.path, savedPlacement.path);
            });
        if (found == plan.candidate.desktopDisplayLayout.end()) {
            plan.candidate.desktopDisplayLayout.push_back(savedPlacement);
        } else {
            found->x = savedPlacement.x;
            found->y = savedPlacement.y;
        }
        ++plan.restoredDesktopPositions;
    }
    return true;
}
