#include "ui/IconGrid.h"

#include <algorithm>

namespace {

struct DesktopLabelStyle {
    std::wstring faceName = L"Microsoft YaHei UI";
    FLOAT size = 12.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
};

DesktopLabelStyle ReadDesktopLabelStyle() {
    LOGFONTW iconFont{};
    DesktopLabelStyle result;
    if (SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(iconFont), &iconFont, 0) == FALSE) {
        return result;
    }
    if (iconFont.lfFaceName[0] != L'\0') {
        result.faceName = iconFont.lfFaceName;
    }
    const LONG height = iconFont.lfHeight < 0 ? -iconFont.lfHeight : iconFont.lfHeight;
    result.size = static_cast<FLOAT>(std::clamp<LONG>(height, 8, 32));
    result.weight = static_cast<DWRITE_FONT_WEIGHT>(std::clamp<LONG>(iconFont.lfWeight, 100, 900));
    result.style = iconFont.lfItalic != FALSE ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    return result;
}

D2D1_RECT_F ToD2DRect(const RECT& rect) {
    return D2D1::RectF(
        static_cast<FLOAT>(rect.left),
        static_cast<FLOAT>(rect.top),
        static_cast<FLOAT>(rect.right),
        static_cast<FLOAT>(rect.bottom));
}

std::wstring TrimLabel(const std::wstring& label) {
    if (label.size() <= 24) {
        return label;
    }
    return label.substr(0, 21) + L"...";
}

}  // namespace

void IconGrid::SetItems(std::vector<DesktopItem> items) {
    items_ = std::move(items);
    ++itemsGeneration_;
    SetSelectedIndex(selectedIndex_);
    SetDraggingIndex(draggingIndex_);
    RecalculateLayout();
}

bool IconGrid::UpdateItemIdentity(
    const std::wstring& sourcePath,
    const std::wstring& itemId,
    const std::wstring& destinationPath) {
    const auto item = std::find_if(
        items_.begin(),
        items_.end(),
        [&](const DesktopItem& value) {
            return CompareStringOrdinal(
                       value.path.c_str(), -1,
                       sourcePath.c_str(), -1,
                       TRUE) == CSTR_EQUAL;
        });
    if (item == items_.end()) {
        return false;
    }
    item->id = itemId;
    item->path = destinationPath;
    return true;
}

void IconGrid::SetBounds(RECT bounds) {
    bounds_ = bounds;
    RecalculateLayout();
}

void IconGrid::SetIconSize(int iconSize) {
    iconSize_ = std::clamp(iconSize, 32, 72);
    SetDensity(density_);
}

void IconGrid::SetDensity(int density) {
    density_ = std::clamp(density, 0, 2);
    const int standardHorizontalPadding[] = {48, 64, 80};
    const int compactHorizontalPadding[] = {32, 40, 48};
    const int standardVerticalPadding[] = {42, 50, 60};
    const int compactVerticalPadding[] = {46, 52, 58};
    const int* horizontalPadding = compactStyle_ ? compactHorizontalPadding : standardHorizontalPadding;
    const int* verticalPadding = compactStyle_ ? compactVerticalPadding : standardVerticalPadding;
    cellWidth_ = iconSize_ + horizontalPadding[density_];
    cellHeight_ = iconSize_ + verticalPadding[density_];
    if (widgetStyle_) {
        // DeskGo uses an 80x96 DIP desktop slot with a 48 DIP shell icon.
        // At the user's 150% desktop scale this becomes 120x144 and 72px.
        cellWidth_ = 80;
        cellHeight_ = 96;
    }
    RecalculateLayout();
}

void IconGrid::SetCompactStyle(bool compactStyle) {
    compactStyle_ = compactStyle;
    SetDensity(density_);
}

void IconGrid::SetWidgetStyle(bool widgetStyle) {
    widgetStyle_ = widgetStyle;
    SetDensity(density_);
}

void IconGrid::SetListMode(bool listMode) {
    if (listMode_ == listMode) {
        return;
    }
    listMode_ = listMode;
    SetDensity(density_);
}

void IconGrid::SetLightTheme(bool lightTheme) {
    lightTheme_ = lightTheme;
}

bool IconGrid::SetHoverIndex(int index) {
    if (index < -1 || index >= static_cast<int>(items_.size())) {
        index = -1;
    }
    if (hoverIndex_ == index) {
        return false;
    }
    hoverIndex_ = index;
    return true;
}

void IconGrid::SetSelectedIndex(int index) {
    if (index < -1 || index >= static_cast<int>(items_.size())) {
        index = -1;
    }
    selectedIndex_ = index;
}

void IconGrid::SetDraggingIndex(int index) {
    if (index < -1 || index >= static_cast<int>(items_.size())) {
        index = -1;
    }
    draggingIndex_ = index;
}

bool IconGrid::ScrollBy(int deltaPixels) {
    const int oldOffset = scrollOffset_;
    SetScrollOffset(scrollOffset_ + deltaPixels);
    return oldOffset != scrollOffset_;
}

void IconGrid::SetScrollOffset(int scrollOffset) {
    scrollOffset_ = std::clamp(scrollOffset, 0, MaxScrollOffset());
    RecalculateLayout();
}

const DesktopItem* IconGrid::ItemAt(size_t index) const {
    if (index >= items_.size()) {
        return nullptr;
    }
    return &items_[index];
}

RECT IconGrid::CellAt(size_t index) const {
    if (index >= cells_.size()) {
        return RECT{};
    }
    return cells_[index];
}

RECT IconGrid::InsertionCellAt(size_t index) const {
    const int width = bounds_.right - bounds_.left;
    if (width <= 0) {
        return RECT{};
    }

    const size_t clampedIndex = std::min(index, items_.size());
    const int columns = listMode_ ? 1 : std::max(1, width / cellWidth_);
    const int layoutCellHeight = listMode_ ? 32 : cellHeight_;
    const int column = static_cast<int>(clampedIndex % static_cast<size_t>(columns));
    const int row = static_cast<int>(clampedIndex / static_cast<size_t>(columns));
    return RECT{
        bounds_.left + column * cellWidth_,
        bounds_.top + row * layoutCellHeight - scrollOffset_,
        listMode_ ? bounds_.right : bounds_.left + (column + 1) * cellWidth_,
        bounds_.top + (row + 1) * layoutCellHeight - scrollOffset_,
    };
}

int IconGrid::HitTest(POINT point) const {
    for (size_t index = 0; index < cells_.size(); ++index) {
        if (PtInRect(&cells_[index], point)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int IconGrid::SlotIndexForPoint(POINT point) const {
    if (cells_.empty()) {
        return -1;
    }
    for (size_t index = 0; index < cells_.size(); ++index) {
        if (PtInRect(&cells_[index], point)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int IconGrid::InsertionIndexForPoint(POINT point) const {
    const int itemCount = static_cast<int>(items_.size());
    const int width = bounds_.right - bounds_.left;
    const int height = bounds_.bottom - bounds_.top;
    if (width <= 0 || height <= 0) {
        return itemCount;
    }
    if (point.y < bounds_.top) {
        return 0;
    }
    if (point.y >= bounds_.bottom) {
        return itemCount;
    }

    const int columns = listMode_ ? 1 : std::max(1, width / cellWidth_);
    const int layoutCellHeight = listMode_ ? 32 : cellHeight_;
    const int contentY = point.y - bounds_.top + scrollOffset_;
    const int row = std::max(0, contentY / layoutCellHeight);
    int column = 0;
    if (!listMode_) {
        const int contentX = point.x - bounds_.left;
        column = std::clamp(contentX / cellWidth_, 0, columns - 1);
    }

    const long long candidate =
        static_cast<long long>(row) * columns + column;
    return static_cast<int>(std::clamp<long long>(candidate, 0, itemCount));
}

void IconGrid::Draw(D2DContext& d2d, IconCache& iconCache) {
    lastFallbackDrawCount_ = 0;
    lastPlaceholderDrawCount_ = 0;
    ID2D1HwndRenderTarget* target = d2d.Target();
    if (target == nullptr) {
        return;
    }
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fallbackBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fallbackBorderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> scrollTrackBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> scrollThumbBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBorderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selectedFillBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selectedBorderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> labelShadowBrush;
    target->CreateSolidColorBrush(
        D2D1::ColorF(lightTheme_ ? 0x15303B : 0xFFFFFF, 1.0f),
        textBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(lightTheme_ ? 0x8BC9DE : 0x2E7DD7, 0.85f),
        fallbackBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(lightTheme_ ? 0x4C90A3 : 0xB9D7FF, 0.95f),
        fallbackBorderBrush.GetAddressOf());
    target->CreateSolidColorBrush(D2D1::ColorF(0x0A1E27, 0.46f), scrollTrackBrush.GetAddressOf());
    target->CreateSolidColorBrush(D2D1::ColorF(0x87DDF4, 0.82f), scrollThumbBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(lightTheme_ ? 0xFFFFFF : 0x1B1E26, lightTheme_ ? 0.18f : 0.20f),
        hoverBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(lightTheme_ ? 0x4C90A3 : 0xFFFFFF, lightTheme_ ? 0.52f : 0.35f),
        hoverBorderBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(lightTheme_ ? 0xE5F6FA : 0x1B1E26, lightTheme_ ? 0.24f : 0.20f),
        selectedFillBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(lightTheme_ ? 0x2D8CFF : 0x4C90A3, 0.95f),
        selectedBorderBrush.GetAddressOf());
    if (widgetStyle_) {
        target->CreateSolidColorBrush(D2D1::ColorF(0x00070A, 0.88f), labelShadowBrush.GetAddressOf());
    }

    DesktopLabelStyle desktopLabelStyle = widgetStyle_ ? ReadDesktopLabelStyle() : DesktopLabelStyle{L"Microsoft YaHei UI", 13.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL};
    if (widgetStyle_) {
        desktopLabelStyle.size = 12.0f;
    }
    Microsoft::WRL::ComPtr<IDWriteTextFormat> labelFormat;
    d2d.WriteFactory()->CreateTextFormat(
        desktopLabelStyle.faceName.c_str(),
        nullptr,
        desktopLabelStyle.weight,
        desktopLabelStyle.style,
        DWRITE_FONT_STRETCH_NORMAL,
        desktopLabelStyle.size,
        L"zh-cn",
        labelFormat.GetAddressOf());
    if (labelFormat != nullptr) {
        labelFormat->SetTextAlignment(listMode_ ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
        labelFormat->SetParagraphAlignment(listMode_ ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        labelFormat->SetWordWrapping(listMode_ ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
    }

    target->PushAxisAlignedClip(ToD2DRect(bounds_), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t index = 0; index < items_.size() && index < cells_.size(); ++index) {
        const RECT& cell = cells_[index];
        if (cell.bottom < bounds_.top || cell.top > bounds_.bottom) {
            continue;
        }
        const int iconInset = 0;
        const int paintedIconSize = listMode_ ? 24 : std::max(32, iconSize_ - iconInset * 2);
        const FLOAT iconLeft = listMode_
            ? static_cast<FLOAT>(cell.left + 6)
            : static_cast<FLOAT>(cell.left + (cellWidth_ - paintedIconSize) / 2) +
                (widgetStyle_ ? -1.0f / 3.0f : 0.0f);
        const FLOAT iconTop = listMode_
            ? static_cast<FLOAT>(cell.top + (32 - paintedIconSize) / 2)
            : static_cast<FLOAT>(cell.top + (widgetStyle_ ? 0 : (compactStyle_ ? 8 : 4))) +
                (widgetStyle_ ? 6.0f : 0.0f);
        const FLOAT cellRadius = widgetStyle_ ? 1.0f : (compactStyle_ ? 4.0f : 8.0f);
        if (static_cast<int>(index) == hoverIndex_ && hoverBrush != nullptr) {
            const D2D1_ROUNDED_RECT hoverRect = D2D1::RoundedRect(ToD2DRect(cell), cellRadius, cellRadius);
            target->FillRoundedRectangle(hoverRect, hoverBrush.Get());
            if (hoverBorderBrush != nullptr) {
                target->DrawRoundedRectangle(hoverRect, hoverBorderBrush.Get(), 1.0f);
            }
        }
        if (static_cast<int>(index) == selectedIndex_ && selectedBorderBrush != nullptr) {
            const D2D1_ROUNDED_RECT selectedRect = D2D1::RoundedRect(ToD2DRect(cell), cellRadius, cellRadius);
            if (selectedFillBrush != nullptr && static_cast<int>(index) != hoverIndex_) {
                target->FillRoundedRectangle(selectedRect, selectedFillBrush.Get());
            }
            target->DrawRoundedRectangle(
                selectedRect,
                selectedBorderBrush.Get(),
                widgetStyle_ ? 1.0f : (compactStyle_ ? 1.0f : 2.0f));
        }
        if (static_cast<int>(index) == draggingIndex_) {
            continue;
        }
        const D2D1_RECT_F iconRect = D2D1::RectF(
            static_cast<FLOAT>(iconLeft),
            iconTop + static_cast<FLOAT>(iconInset),
            iconLeft + static_cast<FLOAT>(paintedIconSize),
            iconTop + static_cast<FLOAT>(iconInset + paintedIconSize));

        bool usedPlaceholder = false;
        const IconPlaceholderKind placeholderKind =
            items_[index].kind == DesktopItemKind::Folder
                ? IconPlaceholderKind::Folder
                : IconPlaceholderKind::File;
        ID2D1Bitmap* bitmap = iconCache.GetIcon(
            target,
            items_[index].path,
            items_[index].displayName,
            placeholderKind,
            &usedPlaceholder);
        if (bitmap != nullptr) {
            target->DrawBitmap(bitmap, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (usedPlaceholder) {
                ++lastPlaceholderDrawCount_;
            }
        } else {
            ++lastFallbackDrawCount_;
            target->FillRoundedRectangle(D2D1::RoundedRect(iconRect, 7.0f, 7.0f), fallbackBrush.Get());
            target->DrawRoundedRectangle(D2D1::RoundedRect(iconRect, 7.0f, 7.0f), fallbackBorderBrush.Get(), 1.0f);
        }
        if (items_[index].kind == DesktopItemKind::Shortcut || items_[index].kind == DesktopItemKind::UrlShortcut) {
            ID2D1Bitmap* overlay = iconCache.GetShortcutOverlay(target);
            if (overlay != nullptr) {
                const FLOAT overlaySize = listMode_ ? 10.0f : (widgetStyle_ ? 16.0f : 14.0f);
                const D2D1_RECT_F overlayRect = D2D1::RectF(
                    iconRect.left - (widgetStyle_ ? 1.0f : 0.0f),
                    iconRect.bottom - overlaySize + (widgetStyle_ ? 1.0f : 0.0f),
                    iconRect.left + overlaySize - (widgetStyle_ ? 1.0f : 0.0f),
                    iconRect.bottom + (widgetStyle_ ? 1.0f : 0.0f));
                target->DrawBitmap(overlay, overlayRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }
        if (labelFormat != nullptr && textBrush != nullptr) {
            const int labelInset = widgetStyle_ ? 2 : 2;
            const FLOAT labelOffset = widgetStyle_ ? 4.6667f : static_cast<FLOAT>(compactStyle_ ? 4 : 7);
            const FLOAT labelHorizontalOffset = widgetStyle_ ? -1.0f / 3.0f : 0.0f;
            const D2D1_RECT_F labelRect = listMode_
                ? D2D1::RectF(
                    iconLeft + static_cast<FLOAT>(paintedIconSize + 8),
                    static_cast<FLOAT>(cell.top),
                    static_cast<FLOAT>(cell.right - 8),
                    static_cast<FLOAT>(cell.bottom))
                : D2D1::RectF(
                    static_cast<FLOAT>(cell.left + labelInset) + labelHorizontalOffset,
                    iconTop + static_cast<FLOAT>(iconSize_ + labelOffset),
                    static_cast<FLOAT>(cell.right - labelInset) + labelHorizontalOffset,
                    static_cast<FLOAT>(cell.bottom));
            std::wstring label = items_[index].displayName;
            if (items_[index].missing) {
                label = L"[missing] " + label;
            }
            label = TrimLabel(label);
            if (widgetStyle_ && labelShadowBrush != nullptr) {
                constexpr D2D1_POINT_2F kShadowOffsets[] = {
                    {-0.7f, 0.0f}, {0.7f, 0.0f}, {0.0f, -0.5f}, {0.0f, 0.8f}};
                for (const D2D1_POINT_2F offset : kShadowOffsets) {
                    const D2D1_RECT_F shadowRect = D2D1::RectF(
                        labelRect.left + offset.x,
                        labelRect.top + offset.y,
                        labelRect.right + offset.x,
                        labelRect.bottom + offset.y);
                    target->DrawTextW(
                        label.c_str(),
                        static_cast<UINT32>(label.size()),
                        labelFormat.Get(),
                        shadowRect,
                        labelShadowBrush.Get());
                }
            }
            target->DrawTextW(
                label.c_str(),
                static_cast<UINT32>(label.size()),
                labelFormat.Get(),
                labelRect,
                textBrush.Get());
        }
    }
    target->PopAxisAlignedClip();

    const int maxScroll = MaxScrollOffset();
    if (maxScroll > 0 && scrollTrackBrush != nullptr && scrollThumbBrush != nullptr) {
        const float trackLeft = static_cast<float>(bounds_.right - 7);
        const float trackTop = static_cast<float>(bounds_.top + 6);
        const float trackBottom = static_cast<float>(bounds_.bottom - 6);
        const float trackHeight = std::max(1.0f, trackBottom - trackTop);
        const float visibleHeight = static_cast<float>(std::max(1, static_cast<int>(bounds_.bottom - bounds_.top)));
        const float contentHeight = static_cast<float>(std::max(visibleHeight, static_cast<float>(ContentHeight())));
        const float thumbHeight = std::max(32.0f, trackHeight * (visibleHeight / contentHeight));
        const float thumbTop = trackTop + (trackHeight - thumbHeight) * (static_cast<float>(scrollOffset_) / static_cast<float>(maxScroll));
        const D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
            D2D1::RectF(trackLeft, trackTop, trackLeft + 4.0f, trackBottom),
            2.0f,
            2.0f);
        const D2D1_ROUNDED_RECT thumb = D2D1::RoundedRect(
            D2D1::RectF(trackLeft, thumbTop, trackLeft + 4.0f, thumbTop + thumbHeight),
            2.0f,
            2.0f);
        target->FillRoundedRectangle(track, scrollTrackBrush.Get());
        target->FillRoundedRectangle(thumb, scrollThumbBrush.Get());
    }
}

void IconGrid::RecalculateLayout() {
    cells_.clear();
    const int width = bounds_.right - bounds_.left;
    if (width <= 0) {
        return;
    }

    scrollOffset_ = std::clamp(scrollOffset_, 0, MaxScrollOffset());
    const int columns = listMode_ ? 1 : std::max(1, width / cellWidth_);
    const int layoutCellHeight = listMode_ ? 32 : cellHeight_;
    for (size_t index = 0; index < items_.size(); ++index) {
        const int column = static_cast<int>(index % columns);
        const int row = static_cast<int>(index / columns);
        RECT cell{
            bounds_.left + column * cellWidth_,
            bounds_.top + row * layoutCellHeight - scrollOffset_,
            listMode_ ? bounds_.right : bounds_.left + (column + 1) * cellWidth_,
            bounds_.top + (row + 1) * layoutCellHeight - scrollOffset_,
        };
        cells_.push_back(cell);
    }
}

int IconGrid::ContentHeight() const {
    const int width = bounds_.right - bounds_.left;
    if (width <= 0 || items_.empty()) {
        return 0;
    }
    const int columns = listMode_ ? 1 : std::max(1, width / cellWidth_);
    const int rows = static_cast<int>((items_.size() + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
    return rows * (listMode_ ? 32 : cellHeight_);
}

int IconGrid::MaxScrollOffset() const {
    const int visibleHeight = bounds_.bottom - bounds_.top;
    return std::max(0, ContentHeight() - std::max(0, visibleHeight));
}
