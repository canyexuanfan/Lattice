#include "ui/MessageDialog.h"

#include <CommCtrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

namespace {

constexpr wchar_t kMessageDialogClass[] = L"Lattice.MessageDialog";
constexpr int kDialogWidth = 430;
constexpr int kMinimumHeight = 190;
constexpr int kMaximumHeight = 500;
constexpr int kTitleHeight = 46;
constexpr int kHorizontalPadding = 18;
constexpr int kIconSize = 30;
constexpr int kBodyGap = 14;
constexpr int kButtonWidth = 88;
constexpr int kButtonHeight = 32;
constexpr int kBottomPadding = 18;
constexpr UINT kButtonHoverMessage = WM_APP + 42;

constexpr COLORREF kBackground = RGB(7, 37, 48);
constexpr COLORREF kPanel = RGB(12, 49, 62);
constexpr COLORREF kPanelHover = RGB(24, 65, 80);
constexpr COLORREF kBorder = RGB(66, 124, 143);
constexpr COLORREF kAccent = RGB(79, 171, 204);
constexpr COLORREF kText = RGB(244, 249, 251);
constexpr COLORREF kMutedText = RGB(174, 203, 213);
constexpr COLORREF kWarning = RGB(226, 177, 86);
constexpr COLORREF kError = RGB(226, 92, 107);

struct DialogState {
    std::wstring title;
    std::wstring message;
    UINT type = MB_OK;
    int result = IDCANCEL;
    int width = kDialogWidth;
    int height = kMinimumHeight;
    int messageHeight = 48;
    UINT dpi = 96;
    HWND primary = nullptr;
    HWND secondary = nullptr;
    HFONT titleFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT buttonFont = nullptr;
    HFONT glyphFont = nullptr;
    int hoverButton = 0;
    bool closeHovered = false;
    bool closePressed = false;
};

class ScopedPerMonitorV2Awareness {
public:
    ScopedPerMonitorV2Awareness() {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 != nullptr) {
            setContext_ = reinterpret_cast<SetContextFunction>(
                GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
        }
        if (setContext_ != nullptr) {
            previous_ = setContext_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    ~ScopedPerMonitorV2Awareness() {
        if (setContext_ != nullptr && previous_ != nullptr) {
            setContext_(previous_);
        }
    }

private:
    using SetContextFunction = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    SetContextFunction setContext_ = nullptr;
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

int Scale(UINT dpi, int value) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

int Scale(const DialogState& state, int value) {
    return Scale(state.dpi, value);
}

bool HasYesNoButtons(UINT type) {
    return (type & MB_TYPEMASK) == MB_YESNO;
}

bool DefaultToSecondary(UINT type) {
    return (type & MB_DEFMASK) == MB_DEFBUTTON2;
}

COLORREF SeverityColor(UINT type) {
    if ((type & MB_ICONMASK) == MB_ICONERROR) {
        return kError;
    }
    if ((type & MB_ICONMASK) == MB_ICONWARNING) {
        return kWarning;
    }
    return kAccent;
}

const wchar_t* SeverityGlyph(UINT type) {
    if ((type & MB_ICONMASK) == MB_ICONERROR) {
        return L"×";
    }
    if ((type & MB_ICONMASK) == MB_ICONQUESTION) {
        return L"?";
    }
    if ((type & MB_ICONMASK) == MB_ICONWARNING) {
        return L"!";
    }
    return L"i";
}

RECT CloseBounds(const DialogState& state) {
    return RECT{
        state.width - Scale(state, 38),
        Scale(state, 8),
        state.width - Scale(state, 10),
        Scale(state, 36)};
}

int MeasureMessageHeight(const std::wstring& message, UINT dpi) {
    HDC dc = GetDC(nullptr);
    if (dc == nullptr) {
        return 64;
    }
    HFONT font = CreateFontW(
        Scale(dpi, -15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HGDIOBJ previousFont = SelectObject(dc, font);
    RECT bounds{
        0,
        0,
        Scale(dpi, kDialogWidth - kHorizontalPadding * 2 - kIconSize - kBodyGap),
        0};
    DrawTextW(
        dc,
        message.c_str(),
        -1,
        &bounds,
        DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(dc, previousFont);
    DeleteObject(font);
    ReleaseDC(nullptr, dc);
    return std::max(Scale(dpi, 44), static_cast<int>(bounds.bottom - bounds.top));
}

LRESULT CALLBACK ButtonSubclass(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR id,
    DWORD_PTR) {
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tracking);
        SendMessageW(GetParent(hwnd), kButtonHoverMessage, id, 0);
    } else if (message == WM_MOUSELEAVE) {
        SendMessageW(GetParent(hwnd), kButtonHoverMessage, 0, 0);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, ButtonSubclass, id);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void DrawButton(const DRAWITEMSTRUCT& draw, const DialogState& state) {
    const bool primary = draw.CtlID == IDYES || draw.CtlID == IDOK;
    const bool hovered = state.hoverButton == static_cast<int>(draw.CtlID);
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const COLORREF fill = primary
        ? (pressed ? RGB(45, 126, 157) : hovered ? RGB(61, 151, 184) : RGB(48, 134, 166))
        : (pressed ? RGB(25, 69, 84) : hovered ? kPanelHover : kPanel);
    HBRUSH fillBrush = CreateSolidBrush(fill);
    HPEN borderPen = CreatePen(PS_SOLID, 1, hovered || focused || primary ? kAccent : kBorder);
    HGDIOBJ previousBrush = SelectObject(draw.hDC, fillBrush);
    HGDIOBJ previousPen = SelectObject(draw.hDC, borderPen);
    RoundRect(
        draw.hDC,
        draw.rcItem.left,
        draw.rcItem.top,
        draw.rcItem.right,
        draw.rcItem.bottom,
        Scale(state, 6),
        Scale(state, 6));
    SelectObject(draw.hDC, previousBrush);
    SelectObject(draw.hDC, previousPen);
    DeleteObject(fillBrush);
    DeleteObject(borderPen);

    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, kText);
    SelectObject(draw.hDC, state.buttonFont);
    wchar_t label[32]{};
    GetWindowTextW(draw.hwndItem, label, ARRAYSIZE(label));
    RECT textRect = draw.rcItem;
    DrawTextW(draw.hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawSeverityGlyph(HDC dc, const DialogState& state) {
    const int top = Scale(state, kTitleHeight + 14);
    RECT circle{
        Scale(state, kHorizontalPadding),
        top,
        Scale(state, kHorizontalPadding + kIconSize),
        top + Scale(state, kIconSize)};
    const COLORREF severity = SeverityColor(state.type);
    HBRUSH fill = CreateSolidBrush(kPanel);
    HPEN pen = CreatePen(PS_SOLID, 1, severity);
    HGDIOBJ oldBrush = SelectObject(dc, fill);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, circle.left, circle.top, circle.right, circle.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(fill);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, severity);
    SelectObject(dc, state.glyphFont);
    DrawTextW(dc, SeverityGlyph(state.type), -1, &circle, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK MessageDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    DialogState* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = static_cast<DialogState*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            return TRUE;
        }

        case WM_CREATE: {
            const BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode));
            const DWORD cornerPreference = 2;
            DwmSetWindowAttribute(hwnd, 33, &cornerPreference, sizeof(cornerPreference));
            state->titleFont = CreateFontW(
                Scale(*state, -18), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            state->bodyFont = CreateFontW(
                Scale(*state, -15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            state->buttonFont = CreateFontW(
                Scale(*state, -15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            state->glyphFont = CreateFontW(
                Scale(*state, -17), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

            const bool yesNo = HasYesNoButtons(state->type);
            const int buttonY = state->height - Scale(*state, kBottomPadding + kButtonHeight);
            const int secondaryX = state->width - Scale(*state, kHorizontalPadding + kButtonWidth);
            const int primaryX = yesNo
                ? secondaryX - Scale(*state, 10 + kButtonWidth)
                : secondaryX;
            const int primaryId = yesNo ? IDYES : IDOK;
            state->primary = CreateWindowW(
                L"BUTTON",
                yesNo ? L"继续" : L"确定",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW |
                    (DefaultToSecondary(state->type) ? 0 : BS_DEFPUSHBUTTON),
                primaryX,
                buttonY,
                Scale(*state, kButtonWidth),
                Scale(*state, kButtonHeight),
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(primaryId)),
                nullptr,
                nullptr);
            SetWindowSubclass(state->primary, ButtonSubclass, primaryId, 0);
            SendMessageW(state->primary, WM_SETFONT, reinterpret_cast<WPARAM>(state->buttonFont), TRUE);
            if (yesNo) {
                state->secondary = CreateWindowW(
                    L"BUTTON",
                    L"取消",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW |
                        (DefaultToSecondary(state->type) ? BS_DEFPUSHBUTTON : 0),
                    secondaryX,
                    buttonY,
                    Scale(*state, kButtonWidth),
                    Scale(*state, kButtonHeight),
                    hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDNO)),
                    nullptr,
                    nullptr);
                SetWindowSubclass(state->secondary, ButtonSubclass, IDNO, 0);
                SendMessageW(state->secondary, WM_SETFONT, reinterpret_cast<WPARAM>(state->buttonFont), TRUE);
            }
            SetFocus(DefaultToSecondary(state->type) && state->secondary != nullptr
                         ? state->secondary
                         : state->primary);
            return 0;
        }

        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            const RECT close = CloseBounds(*state);
            if (point.y >= 0 && point.y < Scale(*state, kTitleHeight) && !PtInRect(&close, point)) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const RECT close = CloseBounds(*state);
            const bool hovered = PtInRect(&close, point) != FALSE;
            if (hovered != state->closeHovered) {
                state->closeHovered = hovered;
                InvalidateRect(hwnd, &close, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            state->closeHovered = false;
            state->closePressed = false;
            {
                const RECT close = CloseBounds(*state);
                InvalidateRect(hwnd, &close, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const RECT close = CloseBounds(*state);
            if (PtInRect(&close, point)) {
                state->closePressed = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, &close, FALSE);
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (state->closePressed) {
                const RECT close = CloseBounds(*state);
                state->closePressed = false;
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                if (PtInRect(&close, point)) {
                    DestroyWindow(hwnd);
                } else {
                    InvalidateRect(hwnd, &close, FALSE);
                }
                return 0;
            }
            break;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDYES || LOWORD(wParam) == IDOK) {
                state->result = LOWORD(wParam);
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == IDNO || LOWORD(wParam) == IDCANCEL) {
                state->result = LOWORD(wParam);
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case kButtonHoverMessage:
            state->hoverButton = static_cast<int>(wParam);
            if (state->primary != nullptr) {
                InvalidateRect(state->primary, nullptr, FALSE);
            }
            if (state->secondary != nullptr) {
                InvalidateRect(state->secondary, nullptr, FALSE);
            }
            return 0;

        case WM_DRAWITEM:
            DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam), *state);
            return TRUE;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client{};
            GetClientRect(hwnd, &client);
            HBRUSH background = CreateSolidBrush(kBackground);
            FillRect(dc, &client, background);
            DeleteObject(background);

            HPEN border = CreatePen(PS_SOLID, 1, kBorder);
            HGDIOBJ oldPen = SelectObject(dc, border);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, 0, 0, client.right, client.bottom);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(border);

            HPEN divider = CreatePen(PS_SOLID, 1, RGB(25, 69, 84));
            oldPen = SelectObject(dc, divider);
            const int titleHeight = Scale(*state, kTitleHeight);
            MoveToEx(dc, 0, titleHeight, nullptr);
            LineTo(dc, client.right, titleHeight);
            SelectObject(dc, oldPen);
            DeleteObject(divider);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kText);
            SelectObject(dc, state->titleFont);
            RECT titleRect{
                Scale(*state, kHorizontalPadding),
                Scale(*state, 9),
                state->width - Scale(*state, 48),
                Scale(*state, 38)};
            DrawTextW(
                dc,
                state->title.c_str(),
                -1,
                &titleRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            DrawSeverityGlyph(dc, *state);
            SetTextColor(dc, kMutedText);
            SelectObject(dc, state->bodyFont);
            RECT messageRect{
                Scale(*state, kHorizontalPadding + kIconSize + kBodyGap),
                Scale(*state, kTitleHeight + 13),
                state->width - Scale(*state, kHorizontalPadding),
                state->height - Scale(*state, kBottomPadding + kButtonHeight + 14)};
            DrawTextW(
                dc,
                state->message.c_str(),
                -1,
                &messageRect,
                DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL | DT_END_ELLIPSIS);

            RECT close = CloseBounds(*state);
            if (state->closeHovered || state->closePressed) {
                HBRUSH closeFill = CreateSolidBrush(
                    state->closePressed ? RGB(33, 79, 94) : kPanelHover);
                FillRect(dc, &close, closeFill);
                DeleteObject(closeFill);
            }
            SetTextColor(dc, state->closeHovered ? kText : kMutedText);
            SelectObject(dc, state->titleFont);
            DrawTextW(dc, L"×", -1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            DeleteObject(state->titleFont);
            DeleteObject(state->bodyFont);
            DeleteObject(state->buttonFont);
            DeleteObject(state->glyphFont);
            return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int MessageDialog::Show(
    HINSTANCE instance,
    HWND owner,
    const std::wstring& message,
    const std::wstring& title,
    UINT type) {
    ScopedPerMonitorV2Awareness dpiAwareness;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = MessageDialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kMessageDialogClass;
    RegisterClassExW(&windowClass);

    DialogState state;
    state.title = title;
    state.message = message;
    state.type = type;
    state.dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (state.dpi < 96) {
        state.dpi = 96;
    }
    state.width = Scale(state, kDialogWidth);
    state.messageHeight = MeasureMessageHeight(message, state.dpi);
    state.height = std::clamp(
        Scale(state, kTitleHeight + 13 + 14 + kButtonHeight + kBottomPadding) + state.messageHeight,
        Scale(state, kMinimumHeight),
        Scale(state, kMaximumHeight));
    state.result = HasYesNoButtons(type) ? IDNO : IDCANCEL;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kMessageDialogClass,
        title.c_str(),
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        state.width,
        state.height,
        owner,
        nullptr,
        instance,
        &state);
    if (hwnd == nullptr) {
        return state.result;
    }

    RECT anchor{};
    if (owner == nullptr || !GetWindowRect(owner, &anchor)) {
        HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(monitor, &monitorInfo)) {
            anchor = monitorInfo.rcWork;
        }
    }
    const int x = anchor.left + ((anchor.right - anchor.left) - state.width) / 2;
    const int y = anchor.top + ((anchor.bottom - anchor.top) - state.height) / 2;
    SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);

    const bool ownerEnabled = owner != nullptr && IsWindowEnabled(owner);
    if (ownerEnabled) {
        EnableWindow(owner, FALSE);
    }
    MSG modalMessage{};
    while (IsWindow(hwnd) && GetMessageW(&modalMessage, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &modalMessage)) {
            TranslateMessage(&modalMessage);
            DispatchMessageW(&modalMessage);
        }
    }
    if (ownerEnabled) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return state.result;
}
