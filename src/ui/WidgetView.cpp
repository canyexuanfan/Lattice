#include "ui/WidgetView.h"

#include <algorithm>
#include <cwctype>

#include "ui/WidgetViewRenderer.h"

namespace {

constexpr int kTitleHeight = 32;
constexpr int kResizeGrip = 12;

bool IdEquals(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(
               left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool UseLightTheme(int theme) {
    if (theme == 1) {
        return true;
    }
    if (theme == 0) {
        return false;
    }
    DWORD appsUseLightTheme = 0;
    DWORD size = sizeof(appsUseLightTheme);
    return RegGetValueW(
               HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"AppsUseLightTheme",
               RRF_RT_REG_DWORD,
               nullptr,
               &appsUseLightTheme,
               &size) == ERROR_SUCCESS &&
        appsUseLightTheme != 0;
}

}  // namespace

void WidgetView::Configure(
    std::wstring categoryId,
    std::wstring title,
    const WindowConfig& config,
    int theme,
    RECT hostPixelBounds,
    std::vector<DesktopItem> items) {
    categoryId_ = std::move(categoryId);
    title_ = std::move(title);
    config_ = config;
    theme_ = theme;
    hostPixelBounds_ = hostPixelBounds;
    items_ = std::move(items);
    selectedItemIds_.clear();
    selectionAnchorIndex_ = -1;
    scrollOffset_ = 0;
    headerHovered_ = false;
    hoverHeaderButton_ = -1;
    pressedHeaderButton_ = -1;
    marqueeActive_ = false;
    marqueeRect_ = RECT{};
    insertionIndex_ = -1;

    grid_.SetItems(items_);
    grid_.SetIconSize(config_.iconSize);
    grid_.SetDensity(config_.density);
    grid_.SetCompactStyle(true);
    grid_.SetWidgetStyle(true);
    grid_.SetListMode(config_.contentViewMode != 0);
    grid_.SetLightTheme(UseLightTheme(theme_));
    grid_.SetBounds(LocalGridBounds());
    SyncGridSelection();
}

void WidgetView::SetHostPixelBounds(RECT bounds) {
    hostPixelBounds_ = bounds;
    grid_.SetBounds(LocalGridBounds());
}

void WidgetView::SetCollapsed(bool collapsed) {
    if (config_.collapsed == collapsed) {
        return;
    }
    config_.collapsed = collapsed;
    if (collapsed) {
        grid_.SetHoverIndex(-1);
        grid_.SetDraggingIndex(-1);
        marqueeActive_ = false;
        insertionIndex_ = -1;
    }
    grid_.SetBounds(LocalGridBounds());
}

void WidgetView::SetLocked(bool locked) {
    config_.locked = locked;
}

int WidgetView::Dpi() const noexcept {
    return std::max(96, config_.dpi);
}

POINT WidgetView::HostPixelsToLocalDips(POINT point) const noexcept {
    return POINT{
        MulDiv(point.x - hostPixelBounds_.left, 96, Dpi()),
        MulDiv(point.y - hostPixelBounds_.top, 96, Dpi())};
}

RECT WidgetView::LocalDipsToHostPixels(RECT rect) const noexcept {
    return RECT{
        hostPixelBounds_.left + MulDiv(rect.left, Dpi(), 96),
        hostPixelBounds_.top + MulDiv(rect.top, Dpi(), 96),
        hostPixelBounds_.left + MulDiv(rect.right, Dpi(), 96),
        hostPixelBounds_.top + MulDiv(rect.bottom, Dpi(), 96)};
}

RECT WidgetView::LocalGridBounds() const noexcept {
    if (config_.collapsed) {
        return RECT{};
    }
    const int width = std::max(
        0,
        MulDiv(
            hostPixelBounds_.right - hostPixelBounds_.left,
            96,
            Dpi()));
    const int height = std::max(
        0,
        MulDiv(
            hostPixelBounds_.bottom - hostPixelBounds_.top,
            96,
            Dpi()));
    return RECT{
        3,
        kTitleHeight - 4,
        std::max(3, width - 3),
        std::max(kTitleHeight - 4, height - 2)};
}

bool WidgetView::ContainsHostPoint(POINT point) const noexcept {
    return PtInRect(&hostPixelBounds_, point) != FALSE;
}

const DesktopItem* WidgetView::ItemAt(size_t index) const noexcept {
    return index < items_.size() ? &items_[index] : nullptr;
}

RECT WidgetView::ItemHostCell(size_t index) const noexcept {
    if (config_.collapsed || index >= items_.size()) {
        return RECT{};
    }
    return LocalDipsToHostPixels(grid_.CellAt(index));
}

int WidgetView::HeaderButtonAtLocalPoint(POINT point) const noexcept {
    if (point.y < 0 || point.y >= kTitleHeight) {
        return -1;
    }
    if (point.x >= 0 && point.x < 24) {
        return 1;
    }
    if (point.x >= 24 && point.x < 48) {
        return 2;
    }
    const int width = std::max(
        0,
        MulDiv(
            hostPixelBounds_.right - hostPixelBounds_.left,
            96,
            Dpi()));
    if (point.x >= width - 97 && point.x < width - 73) {
        return 3;
    }
    if (point.x >= width - 75 && point.x < width - 51) {
        return 4;
    }
    if (point.x >= width - 52 && point.x < width - 28) {
        return 5;
    }
    if (point.x >= width - 29 && point.x < width - 5) {
        return 6;
    }
    return -1;
}

WidgetViewHit WidgetView::HitTestHostPoint(POINT point) const {
    if (PtInRect(&hostPixelBounds_, point) == FALSE) {
        return {};
    }
    const POINT local = HostPixelsToLocalDips(point);
    if (local.y >= 0 && local.y < kTitleHeight) {
        const int button = HeaderButtonAtLocalPoint(local);
        return WidgetViewHit{
            button >= 0 ? WidgetViewHitKind::HeaderButton
                        : WidgetViewHitKind::Header,
            -1,
            button};
    }
    if (config_.collapsed) {
        return {};
    }

    const int width = std::max(
        0,
        MulDiv(
            hostPixelBounds_.right - hostPixelBounds_.left,
            96,
            Dpi()));
    const int height = std::max(
        0,
        MulDiv(
            hostPixelBounds_.bottom - hostPixelBounds_.top,
            96,
            Dpi()));
    if (!config_.locked) {
        const int edges =
            (local.x < kResizeGrip ? kWidgetResizeLeft : 0) |
            (local.x >= width - kResizeGrip ? kWidgetResizeRight : 0) |
            (local.y >= height - kResizeGrip ? kWidgetResizeBottom : 0);
        if (edges != 0) {
            return WidgetViewHit{
                WidgetViewHitKind::ResizeBorder, -1, -1, edges};
        }
    }

    const int paintedIndex = grid_.PaintedIconHitTest(local);
    if (paintedIndex >= 0) {
        return WidgetViewHit{
            WidgetViewHitKind::ItemIcon, paintedIndex, -1};
    }
    const int cellIndex = grid_.HitTest(local);
    if (cellIndex >= 0) {
        return WidgetViewHit{
            WidgetViewHitKind::ItemCellGap, cellIndex, -1};
    }
    return {};
}

bool WidgetView::IsSelected(const std::wstring& itemId) const {
    return std::any_of(
        selectedItemIds_.begin(), selectedItemIds_.end(),
        [&](const std::wstring& selected) {
            return IdEquals(selected, itemId);
        });
}

void WidgetView::SelectItem(size_t index, bool controlPressed) {
    if (index >= items_.size()) {
        if (!controlPressed) {
            ClearSelection();
        }
        return;
    }
    const std::wstring& itemId = items_[index].id;
    if (!controlPressed) {
        selectedItemIds_.assign(1, itemId);
    } else if (IsSelected(itemId)) {
        std::erase_if(
            selectedItemIds_,
            [&](const std::wstring& selected) {
                return IdEquals(selected, itemId);
            });
    } else {
        selectedItemIds_.push_back(itemId);
    }
    const std::vector<std::wstring> ordered = ItemIdsInOrder();
    std::stable_sort(
        selectedItemIds_.begin(), selectedItemIds_.end(),
        [&](const std::wstring& left, const std::wstring& right) {
            const auto leftIt = std::find_if(
                ordered.begin(), ordered.end(),
                [&](const std::wstring& value) {
                    return IdEquals(value, left);
                });
            const auto rightIt = std::find_if(
                ordered.begin(), ordered.end(),
                [&](const std::wstring& value) {
                    return IdEquals(value, right);
                });
            return leftIt < rightIt;
        });
    selectionAnchorIndex_ = static_cast<int>(index);
    SyncGridSelection();
}

void WidgetView::SelectAll() {
    selectedItemIds_ = ItemIdsInOrder();
    selectionAnchorIndex_ = items_.empty() ? -1 : 0;
    SyncGridSelection();
}

void WidgetView::ClearSelection() {
    selectedItemIds_.clear();
    selectionAnchorIndex_ = -1;
    SyncGridSelection();
}

void WidgetView::SetSelectedItemIds(
    const std::vector<std::wstring>& itemIds) {
    selectedItemIds_.clear();
    for (const DesktopItem& item : items_) {
        const bool selected = std::any_of(
            itemIds.begin(), itemIds.end(),
            [&](const std::wstring& candidate) {
                return IdEquals(candidate, item.id);
            });
        if (selected) {
            selectedItemIds_.push_back(item.id);
        }
    }
    selectionAnchorIndex_ = selectedItemIds_.empty()
        ? -1
        : static_cast<int>(std::distance(
            items_.begin(),
            std::find_if(
                items_.begin(), items_.end(),
                [&](const DesktopItem& item) {
                    return IdEquals(item.id, selectedItemIds_.back());
                })));
    SyncGridSelection();
}

void WidgetView::SelectItemsIntersectingHostRect(
    RECT hostPixelRect,
    const std::vector<std::wstring>& baselineItemIds) {
    selectedItemIds_.clear();
    for (const DesktopItem& item : items_) {
        if (std::any_of(
                baselineItemIds.begin(), baselineItemIds.end(),
                [&](const std::wstring& candidate) {
                    return IdEquals(candidate, item.id);
                })) {
            selectedItemIds_.push_back(item.id);
        }
    }
    for (size_t index = 0; index < items_.size(); ++index) {
        RECT intersection{};
        const RECT cell = ItemHostCell(index);
        if (IntersectRect(&intersection, &cell, &hostPixelRect) == FALSE ||
            IsRectEmpty(&intersection) != FALSE ||
            IsSelected(items_[index].id)) {
            continue;
        }
        selectedItemIds_.push_back(items_[index].id);
    }
    selectionAnchorIndex_ = selectedItemIds_.empty() ? -1 : 0;
    SyncGridSelection();
}

WidgetViewAction WidgetView::HandleKey(
    UINT virtualKey,
    bool controlPressed,
    bool shiftPressed) {
    WidgetViewAction action;
    if (controlPressed && (virtualKey == L'A' || virtualKey == L'a')) {
        SelectAll();
        return action;
    }
    if (virtualKey == VK_ESCAPE) {
        ClearSelection();
        return action;
    }
    if (virtualKey == VK_LEFT || virtualKey == VK_UP ||
        virtualKey == VK_RIGHT || virtualKey == VK_DOWN) {
        if (items_.empty()) {
            return action;
        }
        int next = selectionAnchorIndex_ < 0 ? 0 : selectionAnchorIndex_;
        if (virtualKey == VK_LEFT || virtualKey == VK_UP) {
            next = std::max(0, next - 1);
        } else {
            next = std::min(
                static_cast<int>(items_.size()) - 1, next + 1);
        }
        SelectItem(static_cast<size_t>(next), controlPressed);
        return action;
    }
    if (selectedItemIds_.empty()) {
        return action;
    }
    if (virtualKey == VK_RETURN) {
        action.type = WidgetViewActionType::OpenSelection;
        action.itemIds = selectedItemIds_;
    } else if (virtualKey == VK_F2) {
        action.type = WidgetViewActionType::RenameSelection;
        action.itemIds = {selectedItemIds_.front()};
    } else if (virtualKey == VK_DELETE) {
        action.type = WidgetViewActionType::DeleteSelection;
        action.itemIds = selectedItemIds_;
    } else if (virtualKey == VK_APPS ||
               (shiftPressed && virtualKey == VK_F10)) {
        action.type = WidgetViewActionType::ShowSelectionMenu;
        action.itemIds = selectedItemIds_;
    }
    return action;
}

WidgetViewAction WidgetView::BuildDropAction(
    POINT hostPoint,
    const std::vector<std::wstring>& sourceItemIds) const {
    WidgetViewAction action;
    action.itemIds = sourceItemIds;
    const WidgetViewHit hit = HitTestHostPoint(hostPoint);
    if (hit.kind == WidgetViewHitKind::ItemIcon &&
        hit.itemIndex >= 0 &&
        static_cast<size_t>(hit.itemIndex) < items_.size()) {
        const std::wstring& targetId = items_[hit.itemIndex].id;
        const bool targetIsSource = std::any_of(
            sourceItemIds.begin(), sourceItemIds.end(),
            [&](const std::wstring& sourceId) {
                return IdEquals(sourceId, targetId);
            });
        if (!targetIsSource) {
            action.type = WidgetViewActionType::ShellDropTarget;
            action.targetItemId = targetId;
            return action;
        }
    }
    if (config_.collapsed ||
        (hit.kind != WidgetViewHitKind::ItemCellGap &&
         hit.kind != WidgetViewHitKind::Empty &&
         hit.kind != WidgetViewHitKind::ItemIcon)) {
        action.type = WidgetViewActionType::None;
        return action;
    }
    const POINT local = HostPixelsToLocalDips(hostPoint);
    const RECT gridBounds = LocalGridBounds();
    if (PtInRect(&gridBounds, local) == FALSE) {
        return action;
    }
    action.type = WidgetViewActionType::ReorderSelection;
    action.insertionIndex = grid_.ReorderInsertionIndexForPoint(local);
    if (action.insertionIndex < 0) {
        action.insertionIndex = static_cast<int>(items_.size());
    }
    return action;
}

std::vector<std::wstring> WidgetView::ItemIdsInOrder() const {
    std::vector<std::wstring> ids;
    ids.reserve(items_.size());
    for (const DesktopItem& item : items_) {
        ids.push_back(item.id);
    }
    return ids;
}

std::vector<std::wstring> WidgetView::ReorderedItemIds(
    const std::vector<std::wstring>& movingItemIds,
    size_t insertionIndex) const {
    const std::vector<std::wstring> ordered = ItemIdsInOrder();
    std::vector<std::wstring> moving;
    std::vector<std::wstring> stationary;
    moving.reserve(movingItemIds.size());
    stationary.reserve(ordered.size());
    size_t selectedBeforeInsertion = 0;
    for (size_t index = 0; index < ordered.size(); ++index) {
        const bool selected = std::any_of(
            movingItemIds.begin(), movingItemIds.end(),
            [&](const std::wstring& itemId) {
                return IdEquals(itemId, ordered[index]);
            });
        if (selected) {
            moving.push_back(ordered[index]);
            if (index < insertionIndex) {
                ++selectedBeforeInsertion;
            }
        } else {
            stationary.push_back(ordered[index]);
        }
    }
    const size_t normalized = std::min(
        stationary.size(),
        insertionIndex >= selectedBeforeInsertion
            ? insertionIndex - selectedBeforeInsertion
            : size_t{0});
    stationary.insert(
        stationary.begin() + static_cast<std::ptrdiff_t>(normalized),
        moving.begin(), moving.end());
    return stationary;
}

bool WidgetView::ScrollBy(int deltaPixels) {
    if (config_.collapsed) {
        return false;
    }
    const bool changed = grid_.ScrollBy(deltaPixels);
    if (changed) {
        scrollOffset_ += deltaPixels;
    }
    return changed;
}

void WidgetView::SyncGridSelection() {
    std::vector<int> indices;
    indices.reserve(selectedItemIds_.size());
    for (size_t index = 0; index < items_.size(); ++index) {
        if (IsSelected(items_[index].id)) {
            indices.push_back(static_cast<int>(index));
        }
    }
    grid_.SetSelectedIndices(indices);
}

void WidgetView::SetVisualPointerState(
    POINT hostPoint,
    int pressedHeaderButton) {
    const WidgetViewHit hit = HitTestHostPoint(hostPoint);
    headerHovered_ =
        hit.kind == WidgetViewHitKind::Header ||
        hit.kind == WidgetViewHitKind::HeaderButton;
    hoverHeaderButton_ = hit.kind == WidgetViewHitKind::HeaderButton
        ? hit.headerButton
        : -1;
    pressedHeaderButton_ = pressedHeaderButton;
    grid_.SetHoverIndex(
        hit.kind == WidgetViewHitKind::ItemIcon ||
                hit.kind == WidgetViewHitKind::ItemCellGap
            ? hit.itemIndex
            : -1);
}

void WidgetView::SetMarquee(RECT localDipRect, bool active) {
    marqueeRect_ = localDipRect;
    marqueeActive_ = active && !config_.collapsed;
}

void WidgetView::SetInsertionIndex(int insertionIndex) {
    insertionIndex_ = config_.collapsed ? -1 : insertionIndex;
}

void WidgetView::Draw(D2DContext& d2d, IconCache& iconCache) {
    ID2D1HwndRenderTarget* target = d2d.Target();
    if (target == nullptr ||
        (config_.collapsed &&
         hostPixelBounds_.bottom <= hostPixelBounds_.top)) {
        return;
    }

    D2D1_MATRIX_3X2_F previousTransform{};
    target->GetTransform(&previousTransform);
    const FLOAT scale = static_cast<FLOAT>(Dpi()) / 96.0f;
    const D2D1_MATRIX_3X2_F transform =
        D2D1::Matrix3x2F::Scale(scale, scale) *
        D2D1::Matrix3x2F::Translation(
            static_cast<FLOAT>(hostPixelBounds_.left),
            static_cast<FLOAT>(hostPixelBounds_.top));
    target->SetTransform(transform);

    const FLOAT width = static_cast<FLOAT>(MulDiv(
        hostPixelBounds_.right - hostPixelBounds_.left, 96, Dpi()));
    const FLOAT height = static_cast<FLOAT>(MulDiv(
        hostPixelBounds_.bottom - hostPixelBounds_.top, 96, Dpi()));
    target->PushAxisAlignedClip(
        D2D1::RectF(0.0f, 0.0f, width, height),
        D2D1_ANTIALIAS_MODE_ALIASED);
    WidgetViewRenderState state;
    state.size = D2D1::SizeF(width, height);
    state.title = title_;
    state.lightTheme = grid_.UsesLightTheme();
    state.collapsed = config_.collapsed;
    state.locked = config_.locked;
    state.headerHovered = headerHovered_;
    state.hoverHeaderButton = hoverHeaderButton_;
    state.pressedHeaderButton = pressedHeaderButton_;
    state.marqueeActive = marqueeActive_;
    state.marqueeRect = marqueeRect_;
    state.insertionIndex = insertionIndex_;
    DrawWidgetViewContent(d2d, iconCache, grid_, state);
    target->PopAxisAlignedClip();
    target->SetTransform(previousTransform);
}
