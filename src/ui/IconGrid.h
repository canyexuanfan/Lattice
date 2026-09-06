#pragma once

#include <Windows.h>

#include <cstdint>
#include <vector>

#include "desktop/DesktopItem.h"
#include "rendering/D2DContext.h"
#include "rendering/IconCache.h"

class IconGrid {
public:
    void SetItems(std::vector<DesktopItem> items);
    bool UpdateItemIdentity(
        const std::wstring& sourcePath,
        const std::wstring& itemId,
        const std::wstring& destinationPath);
    void SetBounds(RECT bounds);
    void SetIconSize(int iconSize);
    void SetDensity(int density);
    void SetCompactStyle(bool compactStyle);
    void SetWidgetStyle(bool widgetStyle);
    void SetListMode(bool listMode);
    void SetLightTheme(bool lightTheme);
    bool SetHoverIndex(int index);
    void SetSelectedIndex(int index);
    void SetDraggingIndex(int index);
    bool ScrollBy(int deltaPixels);
    void SetScrollOffset(int scrollOffset);
    int HitTest(POINT point) const;
    int SlotIndexForPoint(POINT point) const;
    int InsertionIndexForPoint(POINT point) const;
    void Draw(D2DContext& d2d, IconCache& iconCache);

    const DesktopItem* ItemAt(size_t index) const;
    RECT CellAt(size_t index) const;
    RECT InsertionCellAt(size_t index) const;
    SIZE SlotSize() const noexcept { return SIZE{cellWidth_, cellHeight_}; }
    std::uint64_t ItemsGeneration() const noexcept { return itemsGeneration_; }
    size_t LastFallbackDrawCount() const noexcept {
        return lastFallbackDrawCount_;
    }
    size_t LastPlaceholderDrawCount() const noexcept {
        return lastPlaceholderDrawCount_;
    }

private:
    void RecalculateLayout();
    int ContentHeight() const;
    int MaxScrollOffset() const;

    std::vector<DesktopItem> items_;
    std::vector<RECT> cells_;
    RECT bounds_{};
    int iconSize_ = 48;
    int density_ = 1;
    bool lightTheme_ = false;
    bool compactStyle_ = false;
    bool widgetStyle_ = false;
    bool listMode_ = false;
    int cellWidth_ = 112;
    int cellHeight_ = 92;
    int scrollOffset_ = 0;
    int hoverIndex_ = -1;
    int selectedIndex_ = -1;
    int draggingIndex_ = -1;
    std::uint64_t itemsGeneration_ = 0;
    size_t lastFallbackDrawCount_ = 0;
    size_t lastPlaceholderDrawCount_ = 0;
};
