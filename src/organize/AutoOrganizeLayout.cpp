#include "organize/AutoOrganizeLayout.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace lattice::organize {
namespace {

std::uint64_t HashValue(std::uint64_t hash, std::uint64_t value) noexcept {
    constexpr std::uint64_t kPrime = 1099511628211ull;
    for (unsigned int index = 0; index < 8; ++index) {
        hash ^= value & 0xffu;
        hash *= kPrime;
        value >>= 8;
    }
    return hash;
}

std::uint64_t HashText(std::uint64_t hash, const std::wstring& value) noexcept {
    constexpr std::uint64_t kPrime = 1099511628211ull;
    for (const wchar_t ch : value) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= kPrime;
    }
    return hash;
}

const MonitorLayout* FindMonitor(
    const LayoutContext& context,
    const std::wstring& monitorId) {
    const auto found = std::find_if(
        context.monitors.begin(), context.monitors.end(),
        [&](const MonitorLayout& monitor) { return monitor.id == monitorId; });
    return found == context.monitors.end() ? nullptr : &*found;
}

int ClampGap(int gap) noexcept {
    return (std::max)(0, gap);
}

bool ValidRectangle(const RectI& rectangle) noexcept {
    return rectangle.right > rectangle.left && rectangle.bottom > rectangle.top;
}

WidgetPlacement Unplaced(
    const CandidateWidgetLayout& candidate,
    const std::wstring& reason) {
    WidgetPlacement result;
    result.groupId = candidate.groupId;
    result.monitorId = candidate.monitorId;
    result.manuallyPositioned = candidate.manuallyPositioned;
    result.reason = reason;
    return result;
}

}  // namespace

bool RectanglesOverlap(const RectI& left, const RectI& right) noexcept {
    return left.left < right.right && left.right > right.left &&
        left.top < right.bottom && left.bottom > right.top;
}

bool ContainsRectangle(const RectI& outer, const RectI& inner) noexcept {
    return ValidRectangle(outer) && ValidRectangle(inner) &&
        inner.left >= outer.left && inner.top >= outer.top &&
        inner.right <= outer.right && inner.bottom <= outer.bottom;
}

int RecommendedWidgetWidth(
    const LayoutContext& context,
    const std::wstring& monitorId) {
    std::map<int, std::size_t> frequencies;
    for (const ExistingWidgetLayout& widget : context.existingWidgets) {
        if (widget.monitorId == monitorId && widget.bounds.Width() > 0) {
            ++frequencies[widget.bounds.Width()];
        }
    }
    int selected = (std::max)(1, context.defaultWidth);
    std::size_t selectedCount = 0;
    for (const auto& [width, count] : frequencies) {
        if (count > selectedCount ||
            (count == selectedCount && width < selected)) {
            selected = width;
            selectedCount = count;
        }
    }
    return selected;
}

std::uint64_t MonitorContextSignature(
    const std::vector<MonitorLayout>& monitors) {
    std::vector<MonitorLayout> ordered = monitors;
    std::sort(ordered.begin(), ordered.end(), [](const MonitorLayout& left,
                                                  const MonitorLayout& right) {
        return std::tie(left.id, left.workArea.left, left.workArea.top,
                        left.workArea.right, left.workArea.bottom, left.dpi) <
            std::tie(right.id, right.workArea.left, right.workArea.top,
                     right.workArea.right, right.workArea.bottom, right.dpi);
    });
    std::uint64_t hash = 1469598103934665603ull;
    hash = HashValue(hash, ordered.size());
    for (const MonitorLayout& monitor : ordered) {
        hash = HashText(hash, monitor.id);
        hash = HashValue(hash, static_cast<std::uint32_t>(monitor.workArea.left));
        hash = HashValue(hash, static_cast<std::uint32_t>(monitor.workArea.top));
        hash = HashValue(hash, static_cast<std::uint32_t>(monitor.workArea.right));
        hash = HashValue(hash, static_cast<std::uint32_t>(monitor.workArea.bottom));
        hash = HashValue(hash, monitor.dpi);
    }
    return hash;
}

bool IsMonitorContextCurrent(
    std::uint64_t expectedSignature,
    const std::vector<MonitorLayout>& monitors) {
    return expectedSignature == MonitorContextSignature(monitors);
}

LayoutPlan PlanWidgetLayout(
    const LayoutContext& context,
    const std::vector<CandidateWidgetLayout>& candidates) {
    LayoutPlan plan;
    plan.monitorContextSignature = MonitorContextSignature(context.monitors);
    const int gap = ClampGap(context.gap);

    std::map<std::wstring, std::vector<RectI>> occupiedByMonitor;
    for (const ExistingWidgetLayout& widget : context.existingWidgets) {
        if (ValidRectangle(widget.bounds)) {
            occupiedByMonitor[widget.monitorId].push_back(widget.bounds);
        }
    }

    std::vector<CandidateWidgetLayout> ordered = candidates;
    std::stable_sort(
        ordered.begin(), ordered.end(),
        [](const CandidateWidgetLayout& left,
           const CandidateWidgetLayout& right) {
            if (left.manuallyPositioned != right.manuallyPositioned) {
                return left.manuallyPositioned;
            }
            if (left.monitorId != right.monitorId) {
                return left.monitorId < right.monitorId;
            }
            if (left.manuallyPositioned) {
                return left.groupId < right.groupId;
            }
            if (left.itemCount != right.itemCount) {
                return left.itemCount > right.itemCount;
            }
            if (left.name != right.name) {
                return left.name < right.name;
            }
            return left.groupId < right.groupId;
        });

    for (const CandidateWidgetLayout& candidate : ordered) {
        const MonitorLayout* monitor = FindMonitor(context, candidate.monitorId);
        if (monitor == nullptr || !ValidRectangle(monitor->workArea)) {
            plan.placements.push_back(Unplaced(
                candidate, L"目标显示器或工作区已不可用，需要重新生成布局。"));
            continue;
        }

        std::vector<RectI>& occupied = occupiedByMonitor[candidate.monitorId];
        if (candidate.manuallyPositioned) {
            if (!ContainsRectangle(monitor->workArea, candidate.manualBounds)) {
                plan.placements.push_back(Unplaced(
                    candidate, L"手动位置超出目标显示器工作区。"));
                continue;
            }
            const bool collision = std::any_of(
                occupied.begin(), occupied.end(),
                [&](const RectI& bounds) {
                    return RectanglesOverlap(bounds, candidate.manualBounds);
                });
            if (collision) {
                plan.placements.push_back(Unplaced(
                    candidate, L"手动位置与已有格子或另一候选格子重叠。"));
                continue;
            }
            WidgetPlacement placement;
            placement.groupId = candidate.groupId;
            placement.monitorId = candidate.monitorId;
            placement.bounds = candidate.manualBounds;
            placement.placed = true;
            placement.manuallyPositioned = true;
            placement.internalScrollRequired = false;
            occupied.push_back(candidate.manualBounds);
            plan.placements.push_back(std::move(placement));
            continue;
        }

        const int width = candidate.width > 0
            ? candidate.width
            : RecommendedWidgetWidth(context, candidate.monitorId);
        const int maximumHeight = static_cast<int>(std::floor(
            static_cast<double>(monitor->workArea.Height()) * 0.60));
        const int desiredHeight = candidate.desiredHeight;
        if (width <= 0 || desiredHeight <= 0 || maximumHeight <= 0 ||
            width + gap * 2 > monitor->workArea.Width()) {
            plan.placements.push_back(Unplaced(
                candidate, L"候选格子尺寸无法放入目标显示器工作区。"));
            continue;
        }
        const int height = (std::min)(desiredHeight, maximumHeight);
        if (height + gap * 2 > monitor->workArea.Height()) {
            plan.placements.push_back(Unplaced(
                candidate, L"候选格子高度无法放入目标显示器工作区。"));
            continue;
        }

        bool placed = false;
        RectI selected;
        for (int right = monitor->workArea.right - gap;
             right - width >= monitor->workArea.left + gap && !placed;
             right -= width + gap) {
            int top = monitor->workArea.top + gap;
            while (top + height <= monitor->workArea.bottom - gap) {
                RectI trial{right - width, top, right, top + height};
                int nextTop = top;
                for (const RectI& bounds : occupied) {
                    if (RectanglesOverlap(trial, bounds)) {
                        nextTop = (std::max)(nextTop, bounds.bottom + gap);
                    }
                }
                if (nextTop == top) {
                    selected = trial;
                    placed = true;
                    break;
                }
                top = nextTop;
            }
        }

        if (!placed) {
            plan.placements.push_back(Unplaced(
                candidate,
                L"没有找到不重叠的完整位置；不会自动跨屏或覆盖已有格子。"));
            continue;
        }
        WidgetPlacement placement;
        placement.groupId = candidate.groupId;
        placement.monitorId = candidate.monitorId;
        placement.bounds = selected;
        placement.placed = true;
        placement.internalScrollRequired = desiredHeight > height;
        occupied.push_back(selected);
        plan.placements.push_back(std::move(placement));
    }
    return plan;
}

}  // namespace lattice::organize
