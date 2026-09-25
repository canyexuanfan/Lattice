#pragma once

#include <Windows.h>

#include <string>
#include <vector>

#include "config/ConfigStore.h"
#include "desktop/DesktopItem.h"
#include "rendering/D2DContext.h"
#include "rendering/IconCache.h"
#include "ui/IconGrid.h"

enum class WidgetViewHitKind {
    Empty,
    Header,
    HeaderButton,
    ItemIcon,
    ItemCellGap,
    ResizeBorder,
};

constexpr int kWidgetResizeLeft = 1;
constexpr int kWidgetResizeRight = 2;
constexpr int kWidgetResizeBottom = 4;

struct WidgetViewHit {
    WidgetViewHitKind kind = WidgetViewHitKind::Empty;
    int itemIndex = -1;
    int headerButton = -1;
    int resizeEdges = 0;
};

enum class WidgetViewActionType {
    None,
    OpenSelection,
    RenameSelection,
    DeleteSelection,
    ShowSelectionMenu,
    ShellDropTarget,
    ReorderSelection,
};

struct WidgetViewAction {
    WidgetViewActionType type = WidgetViewActionType::None;
    std::vector<std::wstring> itemIds;
    std::wstring targetItemId;
    int insertionIndex = -1;
};

class WidgetView {
public:
    void Configure(
        std::wstring categoryId,
        std::wstring title,
        const WindowConfig& config,
        int theme,
        RECT hostPixelBounds,
        std::vector<DesktopItem> items);
    void SetHostPixelBounds(RECT bounds);
    void SetCollapsed(bool collapsed);
    void SetLocked(bool locked);

    POINT HostPixelsToLocalDips(POINT point) const noexcept;
    RECT LocalDipsToHostPixels(RECT rect) const noexcept;
    RECT LocalGridBounds() const noexcept;
    const std::wstring& CategoryId() const noexcept { return categoryId_; }
    const WindowConfig& Config() const noexcept { return config_; }
    RECT HostPixelBounds() const noexcept { return hostPixelBounds_; }
    bool ContainsHostPoint(POINT point) const noexcept;
    const DesktopItem* ItemAt(size_t index) const noexcept;
    RECT ItemHostCell(size_t index) const noexcept;
    WidgetViewHit HitTestHostPoint(POINT point) const;

    void SelectItem(size_t index, bool controlPressed);
    void SelectAll();
    void ClearSelection();
    void SetSelectedItemIds(
        const std::vector<std::wstring>& itemIds);
    void SelectItemsIntersectingHostRect(
        RECT hostPixelRect,
        const std::vector<std::wstring>& baselineItemIds);
    const std::vector<std::wstring>& SelectedItemIds() const noexcept {
        return selectedItemIds_;
    }
    WidgetViewAction HandleKey(
        UINT virtualKey,
        bool controlPressed,
        bool shiftPressed);
    WidgetViewAction BuildDropAction(
        POINT hostPoint,
        const std::vector<std::wstring>& sourceItemIds) const;
    std::vector<std::wstring> ReorderedItemIds(
        const std::vector<std::wstring>& movingItemIds,
        size_t insertionIndex) const;
    bool ScrollBy(int deltaPixels);
    int ScrollOffset() const noexcept { return scrollOffset_; }
    SIZE SlotSize() const noexcept { return grid_.SlotSize(); }
    void SetVisualPointerState(
        POINT hostPoint,
        int pressedHeaderButton = -1);
    void SetMarquee(RECT localDipRect, bool active);
    void SetInsertionIndex(int insertionIndex);
    void Draw(D2DContext& d2d, IconCache& iconCache);

    bool OwnsWindow() const noexcept { return false; }
    bool OwnsRenderTarget() const noexcept { return false; }
    bool OwnsWallpaper() const noexcept { return false; }
    bool OwnsIconCache() const noexcept { return false; }

private:
    int Dpi() const noexcept;
    int HeaderButtonAtLocalPoint(POINT point) const noexcept;
    std::vector<std::wstring> ItemIdsInOrder() const;
    bool IsSelected(const std::wstring& itemId) const;
    void SyncGridSelection();

    std::wstring categoryId_;
    std::wstring title_;
    WindowConfig config_{};
    int theme_ = 0;
    RECT hostPixelBounds_{};
    std::vector<DesktopItem> items_;
    IconGrid grid_;
    std::vector<std::wstring> selectedItemIds_;
    int selectionAnchorIndex_ = -1;
    int scrollOffset_ = 0;
    bool headerHovered_ = false;
    int hoverHeaderButton_ = -1;
    int pressedHeaderButton_ = -1;
    bool marqueeActive_ = false;
    RECT marqueeRect_{};
    int insertionIndex_ = -1;
};
