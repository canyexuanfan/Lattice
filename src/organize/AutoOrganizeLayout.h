#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lattice::organize {

struct RectI {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int Width() const noexcept { return right - left; }
    int Height() const noexcept { return bottom - top; }
};

struct MonitorLayout {
    std::wstring id;
    RectI workArea;
    unsigned int dpi = 96;
};

struct ExistingWidgetLayout {
    std::wstring id;
    std::wstring monitorId;
    RectI bounds;
};

struct CandidateWidgetLayout {
    std::wstring groupId;
    std::wstring name;
    std::wstring monitorId;
    int width = 0;
    int desiredHeight = 0;
    std::size_t itemCount = 0;
    bool manuallyPositioned = false;
    RectI manualBounds;
};

struct LayoutContext {
    std::vector<MonitorLayout> monitors;
    std::vector<ExistingWidgetLayout> existingWidgets;
    int gap = 12;
    int defaultWidth = 390;
};

struct WidgetPlacement {
    std::wstring groupId;
    std::wstring monitorId;
    RectI bounds;
    bool placed = false;
    bool manuallyPositioned = false;
    bool internalScrollRequired = false;
    std::wstring reason;
};

struct LayoutPlan {
    std::uint64_t monitorContextSignature = 0;
    std::vector<WidgetPlacement> placements;
};

bool RectanglesOverlap(const RectI& left, const RectI& right) noexcept;
bool ContainsRectangle(const RectI& outer, const RectI& inner) noexcept;
int RecommendedWidgetWidth(
    const LayoutContext& context,
    const std::wstring& monitorId);
std::uint64_t MonitorContextSignature(
    const std::vector<MonitorLayout>& monitors);
bool IsMonitorContextCurrent(
    std::uint64_t expectedSignature,
    const std::vector<MonitorLayout>& monitors);
LayoutPlan PlanWidgetLayout(
    const LayoutContext& context,
    const std::vector<CandidateWidgetLayout>& candidates);

}  // namespace lattice::organize
