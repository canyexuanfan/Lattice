#include "ui/InputDialog.h"

#include <CommCtrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <string>

namespace {

constexpr wchar_t kInputDialogClass[] = L"Lattice.InputDialog";
constexpr int kEditId = 1001;
constexpr int kOkId = IDOK;
constexpr int kCancelId = IDCANCEL;
constexpr UINT kButtonHoverMessage = WM_APP + 41;

constexpr COLORREF kBackground = RGB(7, 37, 48);
constexpr COLORREF kPanel = RGB(12, 49, 62);
constexpr COLORREF kBorder = RGB(66, 124, 143);
constexpr COLORREF kAccent = RGB(79, 171, 204);
constexpr COLORREF kText = RGB(244, 249, 251);
constexpr COLORREF kMutedText = RGB(157, 193, 205);

struct DialogState {
    std::wstring title;
    std::wstring label;
    std::wstring value;
    bool accepted = false;
    HWND edit = nullptr;
    HWND ok = nullptr;
    HWND cancel = nullptr;
    HFONT titleFont = nullptr;
    HFONT textFont = nullptr;
    HBRUSH editBrush = nullptr;
    int hoverButton = 0;
    UINT dpi = 96;
    int width = 390;
    int height = 178;
    bool closeHovered = false;
    bool closePressed = false;
};

class ScopedPerMonitorV2Awareness {
public:
    ScopedPerMonitorV2Awareness() {
        previous_ = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ~ScopedPerMonitorV2Awareness() {
        if (previous_ != nullptr) {
            SetThreadDpiAwarenessContext(previous_);
        }
    }
private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

int Scale(const DialogState& state, int value) {
    return MulDiv(value, static_cast<int>(state.dpi), 96);
}

RECT CloseBounds(const DialogState& state) {
    return RECT{
        state.width - Scale(state, 36),
        Scale(state, 8),
        state.width - Scale(state, 8),
        Scale(state, 36)};
}

LRESULT CALLBACK ButtonSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
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
    const bool primary = draw.CtlID == kOkId;
    const bool hovered = state.hoverButton == static_cast<int>(draw.CtlID);
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const COLORREF fill = primary
        ? (pressed ? RGB(45, 126, 157) : hovered ? RGB(61, 151, 184) : RGB(48, 134, 166))
        : (pressed ? RGB(25, 69, 84) : hovered ? RGB(24, 65, 80) : kPanel);
    HBRUSH fillBrush = CreateSolidBrush(fill);
    HPEN borderPen = CreatePen(PS_SOLID, 1, hovered || primary ? kAccent : kBorder);
    HGDIOBJ previousBrush = SelectObject(draw.hDC, fillBrush);
    HGDIOBJ previousPen = SelectObject(draw.hDC, borderPen);
    RoundRect(
        draw.hDC,
        draw.rcItem.left,
        draw.rcItem.top,
        draw.rcItem.right,
        draw.rcItem.bottom,
        Scale(state, 5),
        Scale(state, 5));
    SelectObject(draw.hDC, previousBrush);
    SelectObject(draw.hDC, previousPen);
    DeleteObject(fillBrush);
    DeleteObject(borderPen);

    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, kText);
    SelectObject(draw.hDC, state.textFont);
    RECT textRect = draw.rcItem;
    DrawTextW(draw.hDC, primary ? L"确定" : L"取消", -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK InputDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    DialogState* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<DialogState*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            return TRUE;
        }

        case WM_CREATE: {
            const BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode));
            state->titleFont = CreateFontW(Scale(*state, -18), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            state->textFont = CreateFontW(Scale(*state, -15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                          DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            state->editBrush = CreateSolidBrush(kPanel);
            state->edit = CreateWindowExW(
                0, L"EDIT", state->value.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                Scale(*state, 18), Scale(*state, 76), Scale(*state, 354), Scale(*state, 32),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), nullptr, nullptr);
            state->ok = CreateWindowW(
                L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_DEFPUSHBUTTON,
                Scale(*state, 198), Scale(*state, 128), Scale(*state, 82), Scale(*state, 32),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOkId)), nullptr, nullptr);
            state->cancel = CreateWindowW(
                L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                Scale(*state, 290), Scale(*state, 128), Scale(*state, 82), Scale(*state, 32),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)), nullptr, nullptr);
            SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->textFont), TRUE);
            SendMessageW(state->ok, WM_SETFONT, reinterpret_cast<WPARAM>(state->textFont), TRUE);
            SendMessageW(state->cancel, WM_SETFONT, reinterpret_cast<WPARAM>(state->textFont), TRUE);
            SetWindowSubclass(state->ok, ButtonSubclass, kOkId, 0);
            SetWindowSubclass(state->cancel, ButtonSubclass, kCancelId, 0);
            SendMessageW(state->edit, EM_SETSEL, 0, -1);
            SetFocus(state->edit);
            return 0;
        }

        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            const RECT close = CloseBounds(*state);
            if (point.y >= 0 && point.y < Scale(*state, 44) && !PtInRect(&close, point)) {
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

        case WM_MOUSELEAVE: {
            state->closeHovered = false;
            state->closePressed = false;
            const RECT close = CloseBounds(*state);
            InvalidateRect(hwnd, &close, FALSE);
            return 0;
        }

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

        case WM_COMMAND:
            if (LOWORD(wParam) == kOkId) {
                wchar_t buffer[256]{};
                GetWindowTextW(state->edit, buffer, ARRAYSIZE(buffer));
                state->value = buffer;
                state->accepted = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == kCancelId) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case kButtonHoverMessage:
            state->hoverButton = static_cast<int>(wParam);
            InvalidateRect(state->ok, nullptr, FALSE);
            InvalidateRect(state->cancel, nullptr, FALSE);
            return 0;

        case WM_DRAWITEM:
            DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam), *state);
            return TRUE;

        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kText);
            SetBkColor(dc, kPanel);
            return reinterpret_cast<LRESULT>(state->editBrush);
        }

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

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kText);
            SelectObject(dc, state->titleFont);
            RECT titleRect{
                Scale(*state, 18),
                Scale(*state, 10),
                state->width - Scale(*state, 50),
                Scale(*state, 38)};
            DrawTextW(dc, state->title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SetTextColor(dc, kMutedText);
            SelectObject(dc, state->textFont);
            RECT labelRect{
                Scale(*state, 18),
                Scale(*state, 48),
                state->width - Scale(*state, 18),
                Scale(*state, 70)};
            DrawTextW(dc, state->label.c_str(), -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SetTextColor(dc, kText);
            RECT close = CloseBounds(*state);
            if (state->closeHovered || state->closePressed) {
                HBRUSH closeBrush = CreateSolidBrush(
                    state->closePressed ? RGB(33, 79, 94) : RGB(24, 65, 80));
                FillRect(dc, &close, closeBrush);
                DeleteObject(closeBrush);
            }
            SetTextColor(dc, state->closeHovered ? kText : kMutedText);
            DrawTextW(dc, L"×", -1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(hwnd, &paint);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            DeleteObject(state->titleFont);
            DeleteObject(state->textFont);
            DeleteObject(state->editBrush);
            return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

std::optional<std::wstring> InputDialog::Prompt(
    HINSTANCE instance,
    HWND owner,
    const std::wstring& title,
    const std::wstring& label,
    const std::wstring& initialValue) {
    ScopedPerMonitorV2Awareness dpiAwareness;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = InputDialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = CreateSolidBrush(kBackground);
    windowClass.lpszClassName = kInputDialogClass;
    RegisterClassExW(&windowClass);

    DialogState state{title, label, initialValue};
    state.dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (state.dpi < 96) {
        state.dpi = 96;
    }
    state.width = Scale(state, 390);
    state.height = Scale(state, 178);
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kInputDialogClass,
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
        return std::nullopt;
    }

    RECT ownerRect{};
    if (owner == nullptr || !GetWindowRect(owner, &ownerRect)) {
        HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(monitor, &monitorInfo)) {
            ownerRect = monitorInfo.rcWork;
        }
    }
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - state.width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - state.height) / 2;
    SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);

    const bool ownerEnabled = owner != nullptr && IsWindowEnabled(owner);
    if (ownerEnabled) {
        EnableWindow(owner, FALSE);
    }
    MSG message{};
    while (IsWindow(hwnd) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (ownerEnabled) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }

    if (!state.accepted || state.value.empty()) {
        return std::nullopt;
    }
    return state.value;
}
