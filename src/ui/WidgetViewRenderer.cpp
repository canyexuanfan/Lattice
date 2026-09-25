#include "ui/WidgetViewRenderer.h"

#include <algorithm>
#include <wrl/client.h>

namespace {

constexpr FLOAT kHeaderVisualHeight = 24.0f;
constexpr FLOAT kHeaderVisualTop =
    (static_cast<FLOAT>(kWidgetTitleHeight) - kHeaderVisualHeight) / 2.0f;
constexpr FLOAT kHeaderControlOffsetY = kHeaderVisualTop - 7.5f;

}  // namespace

void DrawWidgetViewContent(
    D2DContext& d2d,
    IconCache& iconCache,
    IconGrid& iconGrid,
    const WidgetViewRenderState& state) {
    ID2D1HwndRenderTarget* target = d2d.Target();
    if (target == nullptr || state.size.width <= 0.0f ||
        state.size.height <= 0.0f) {
        return;
    }

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> backgroundTintBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> iconBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> titleShadowBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> headerHoverBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> headerPressedBrush;
    target->CreateSolidColorBrush(
        D2D1::ColorF(
            state.lightTheme ? 0xDDEFF3 : 0x082A32,
            state.lightTheme ? 0.34f : 0.68f),
        backgroundTintBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(
            state.lightTheme ? 0x4C90A3 : 0x2E829D,
            state.lightTheme ? 0.32f : 0.18f),
        borderBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(
            state.lightTheme ? 0x15303B : 0xFFFFFF, 1.0f),
        textBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(
            state.lightTheme ? 0x15303B : 0xDAE0E2, 1.0f),
        iconBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(
            state.lightTheme ? 0x15303B : 0x000000,
            state.lightTheme ? 0.18f : 0.72f),
        titleShadowBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(
            state.lightTheme ? 0x79CBE2 : 0xBCEEFF,
            state.lightTheme ? 0.24f : 0.28f),
        headerHoverBrush.GetAddressOf());
    target->CreateSolidColorBrush(
        D2D1::ColorF(
            state.lightTheme ? 0x4AAFCB : 0x8ADDF6,
            state.lightTheme ? 0.38f : 0.42f),
        headerPressedBrush.GetAddressOf());

    const D2D1_SIZE_F size = state.size;
    if (backgroundTintBrush != nullptr) {
        target->FillRectangle(
            D2D1::RectF(0.0f, 0.0f, size.width, size.height),
            backgroundTintBrush.Get());
    }
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    if (borderBrush != nullptr) {
        target->DrawLine(
            D2D1::Point2F(0.5f, 0.0f),
            D2D1::Point2F(0.5f, size.height),
            borderBrush.Get(), 1.0f);
        target->DrawLine(
            D2D1::Point2F(size.width - 0.5f, 0.0f),
            D2D1::Point2F(size.width - 0.5f, size.height),
            borderBrush.Get(), 1.0f);
        target->DrawLine(
            D2D1::Point2F(0.0f, 0.5f),
            D2D1::Point2F(size.width, 0.5f),
            borderBrush.Get(), 1.0f);
    }
    target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const auto drawHeaderHover = [&](int button, const D2D1_RECT_F& rect) {
        ID2D1SolidColorBrush* stateBrush = nullptr;
        if (state.pressedHeaderButton == button &&
            state.hoverHeaderButton == button) {
            stateBrush = headerPressedBrush.Get();
        } else if (state.hoverHeaderButton == button) {
            stateBrush = headerHoverBrush.Get();
        }
        if (stateBrush != nullptr) {
            target->FillRoundedRectangle(
                D2D1::RoundedRect(rect, 1.5f, 1.5f), stateBrush);
        }
    };
    const bool showHeaderControls =
        state.headerHovered || state.pressedHeaderButton >= 0;
    if (showHeaderControls) {
        drawHeaderHover(1, D2D1::RectF(
            0.0f, 7.5f + kHeaderControlOffsetY,
            24.0f, 31.5f + kHeaderControlOffsetY));
        drawHeaderHover(2, D2D1::RectF(
            24.0f, 7.5f + kHeaderControlOffsetY,
            48.0f, 31.5f + kHeaderControlOffsetY));
        drawHeaderHover(3, D2D1::RectF(
            size.width - 97.0f, 7.5f + kHeaderControlOffsetY,
            size.width - 73.0f, 31.5f + kHeaderControlOffsetY));
        drawHeaderHover(4, D2D1::RectF(
            size.width - 75.0f, 7.5f + kHeaderControlOffsetY,
            size.width - 51.0f, 31.5f + kHeaderControlOffsetY));
        drawHeaderHover(5, D2D1::RectF(
            size.width - 52.0f, 7.5f + kHeaderControlOffsetY,
            size.width - 28.0f, 31.5f + kHeaderControlOffsetY));
        drawHeaderHover(6, D2D1::RectF(
            size.width - 29.0f, 7.5f + kHeaderControlOffsetY,
            size.width - 5.0f, 31.5f + kHeaderControlOffsetY));
    }

    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat;
    if (d2d.WriteFactory() != nullptr) {
        d2d.WriteFactory()->CreateTextFormat(
            L"Microsoft YaHei UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f, L"zh-cn", titleFormat.GetAddressOf());
    }
    if (titleFormat != nullptr && textBrush != nullptr) {
        titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        titleFormat->SetParagraphAlignment(
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const D2D1_RECT_F titleRect = D2D1::RectF(
            0.0f, 0.0f, size.width,
            static_cast<FLOAT>(kWidgetTitleHeight));
        if (titleShadowBrush != nullptr) {
            target->DrawTextW(
                state.title.c_str(),
                static_cast<UINT32>(state.title.size()),
                titleFormat.Get(),
                D2D1::RectF(
                    titleRect.left + 0.7f,
                    titleRect.top + 1.1f,
                    titleRect.right + 0.7f,
                    titleRect.bottom + 1.1f),
                titleShadowBrush.Get());
        }
        target->DrawTextW(
            state.title.c_str(),
            static_cast<UINT32>(state.title.size()),
            titleFormat.Get(), titleRect, textBrush.Get());
    }

    if (showHeaderControls && iconBrush != nullptr) {
        const FLOAT stroke = 2.0f / 3.0f;
        const auto lineWithWidth =
            [&](FLOAT x1, FLOAT y1, FLOAT x2, FLOAT y2, FLOAT width) {
                target->DrawLine(
                    D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2),
                    iconBrush.Get(), width);
            };
        const auto line = [&](FLOAT x1, FLOAT y1, FLOAT x2, FLOAT y2) {
            lineWithWidth(x1, y1, x2, y2, stroke);
        };
        if (state.collapsed) {
            line(7.0f, 16.35f + kHeaderControlOffsetY,
                 12.0f, 21.2f + kHeaderControlOffsetY);
            line(12.0f, 21.2f + kHeaderControlOffsetY,
                 17.0f, 16.35f + kHeaderControlOffsetY);
        } else {
            line(7.0f, 21.2f + kHeaderControlOffsetY,
                 12.0f, 16.35f + kHeaderControlOffsetY);
            line(12.0f, 16.35f + kHeaderControlOffsetY,
                 17.0f, 21.2f + kHeaderControlOffsetY);
        }

        target->DrawRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(
                    31.25f, 18.5f + kHeaderControlOffsetY,
                    40.75f, 24.5f + kHeaderControlOffsetY),
                0.8f, 0.8f),
            iconBrush.Get(), 0.8f);
        if (state.locked) {
            lineWithWidth(32.75f, 18.5f + kHeaderControlOffsetY,
                          32.75f, 16.2f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(32.75f, 16.2f + kHeaderControlOffsetY,
                          34.35f, 14.4f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(34.35f, 14.4f + kHeaderControlOffsetY,
                          37.75f, 14.4f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(37.75f, 14.4f + kHeaderControlOffsetY,
                          39.25f, 16.2f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(39.25f, 16.2f + kHeaderControlOffsetY,
                          39.25f, 18.5f + kHeaderControlOffsetY, 0.8f);
        } else {
            lineWithWidth(34.75f, 18.5f + kHeaderControlOffsetY,
                          34.75f, 16.25f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(34.75f, 16.25f + kHeaderControlOffsetY,
                          36.15f, 14.55f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(36.15f, 14.55f + kHeaderControlOffsetY,
                          39.05f, 14.55f + kHeaderControlOffsetY, 0.8f);
            lineWithWidth(39.05f, 14.55f + kHeaderControlOffsetY,
                          40.65f, 16.1f + kHeaderControlOffsetY, 0.8f);
        }

        const FLOAT rightY = 20.0f + kHeaderControlOffsetY;
        const FLOAT actionLeft = size.width - 97.5f;
        const FLOAT actionTop = 7.8333f + kHeaderControlOffsetY;
        constexpr FLOAT actionCenterX = 12.20f;
        constexpr FLOAT actionCenterY = 11.575f;
        constexpr FLOAT actionScale = 0.84f;
        const auto actionX = [&](FLOAT value) {
            return actionLeft + actionCenterX +
                (value - actionCenterX) * actionScale;
        };
        const auto actionY = [&](FLOAT value) {
            return actionTop + actionCenterY +
                (value - actionCenterY) * actionScale;
        };
        const auto actionLine =
            [&](FLOAT x1, FLOAT y1, FLOAT x2, FLOAT y2) {
                lineWithWidth(
                    actionX(x1), actionY(y1),
                    actionX(x2), actionY(y2), 0.8f);
            };
        actionLine(13.77f, 5.86f, 7.49f, 5.86f);
        actionLine(7.49f, 5.86f, 6.86f, 6.49f);
        actionLine(6.86f, 6.49f, 6.86f, 16.66f);
        actionLine(6.86f, 16.66f, 7.49f, 17.29f);
        actionLine(7.49f, 17.29f, 16.29f, 17.29f);
        actionLine(16.29f, 17.29f, 16.91f, 16.66f);
        actionLine(16.91f, 16.66f, 16.91f, 10.89f);
        actionLine(8.74f, 13.40f, 12.51f, 13.40f);
        actionLine(17.54f, 6.49f, 13.77f, 10.26f);

        const FLOAT listX = size.width - 62.5f;
        for (int row = 0; row < 3; ++row) {
            const FLOAT top = 14.6667f + kHeaderControlOffsetY +
                static_cast<FLOAT>(row) * 4.0f;
            target->DrawRectangle(
                D2D1::RectF(
                    listX - 5.5f, top,
                    listX - 2.5f, top + 2.0f),
                iconBrush.Get(), 0.8f);
            lineWithWidth(
                listX + 0.5f, top + 1.0f,
                listX + 4.5f, top + 1.0f, 0.8f);
        }

        const FLOAT filterX = size.width - 40.0f;
        line(filterX - 5.5f, rightY - 5.0f,
             filterX + 5.5f, rightY - 5.0f);
        line(filterX - 5.5f, rightY - 5.0f,
             filterX - 0.8f, rightY + 0.5f);
        line(filterX + 5.5f, rightY - 5.0f,
             filterX + 0.8f, rightY + 0.5f);
        line(filterX + 0.8f, rightY + 0.5f,
             filterX + 0.8f, rightY + 4.3f);
        line(filterX + 3.5f, rightY - 1.5f,
             filterX + 5.5f, rightY - 1.5f);
        line(filterX + 3.5f, rightY + 1.0f,
             filterX + 5.2f, rightY + 1.0f);
        line(filterX + 3.5f, rightY + 3.5f,
             filterX + 4.9f, rightY + 3.5f);

        const FLOAT menuX = size.width - 17.0f;
        target->FillRectangle(
            D2D1::RectF(
                menuX - 5.0f, 15.3333f + kHeaderControlOffsetY,
                menuX + 5.0f, 16.0f + kHeaderControlOffsetY),
            iconBrush.Get());
        target->FillRectangle(
            D2D1::RectF(
                menuX - 5.0f, 19.3333f + kHeaderControlOffsetY,
                menuX + 5.0f, 20.0f + kHeaderControlOffsetY),
            iconBrush.Get());
        target->FillRectangle(
            D2D1::RectF(
                menuX - 5.0f, 23.3333f + kHeaderControlOffsetY,
                menuX + 5.0f, 24.0f + kHeaderControlOffsetY),
            iconBrush.Get());
    }

    if (state.collapsed) {
        return;
    }
    iconGrid.Draw(d2d, iconCache);
    if (state.marqueeActive &&
        !IsRectEmpty(&state.marqueeRect)) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> marqueeFillBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> marqueeBorderBrush;
        target->CreateSolidColorBrush(
            D2D1::ColorF(0x2D8CFF, 0.18f),
            marqueeFillBrush.GetAddressOf());
        target->CreateSolidColorBrush(
            D2D1::ColorF(0x5AA8FF, 0.92f),
            marqueeBorderBrush.GetAddressOf());
        const D2D1_RECT_F marquee = D2D1::RectF(
            static_cast<FLOAT>(state.marqueeRect.left),
            static_cast<FLOAT>(state.marqueeRect.top),
            static_cast<FLOAT>(state.marqueeRect.right),
            static_cast<FLOAT>(state.marqueeRect.bottom));
        if (marqueeFillBrush != nullptr) {
            target->FillRectangle(marquee, marqueeFillBrush.Get());
        }
        if (marqueeBorderBrush != nullptr) {
            target->DrawRectangle(
                marquee, marqueeBorderBrush.Get(), 1.0f);
        }
    }
    if (state.insertionIndex < 0) {
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> insertionBrush;
    target->CreateSolidColorBrush(
        D2D1::ColorF(0x63D3F1, 0.92f),
        insertionBrush.GetAddressOf());
    const RECT slot = iconGrid.InsertionCellAt(
        static_cast<size_t>(state.insertionIndex));
    const RECT gridBounds = [&]() {
        RECT result{};
        if (state.size.width > 0.0f && state.size.height > 0.0f) {
            result = RECT{
                3,
                kWidgetTitleHeight - 4,
                static_cast<LONG>(state.size.width) - 3,
                static_cast<LONG>(state.size.height) - 2};
        }
        return result;
    }();
    RECT clipped{};
    if (insertionBrush != nullptr &&
        IntersectRect(&clipped, &slot, &gridBounds) &&
        clipped.right > clipped.left &&
        clipped.bottom > clipped.top) {
        const D2D1_RECT_F marker = D2D1::RectF(
            static_cast<FLOAT>(clipped.left) + 3.0f,
            static_cast<FLOAT>(clipped.top) + 3.0f,
            static_cast<FLOAT>(clipped.right) - 3.0f,
            static_cast<FLOAT>(clipped.bottom) - 3.0f);
        if (marker.right > marker.left &&
            marker.bottom > marker.top) {
            target->DrawRoundedRectangle(
                D2D1::RoundedRect(marker, 5.0f, 5.0f),
                insertionBrush.Get(), 2.0f);
        }
    }
}
