#pragma once
#include <Windows.h>
#include <algorithm>

// Shared owner-drawn push buttons used by Settings and the rule editor.
namespace lattice::ui {
struct DialogComboStyle { int radius=0; int inset=8; int arrowWidth=5; int arrowBottom=3; };
inline void DrawDialogCombo(HWND hwnd, HDC dc, HFONT font, UINT dpi, bool hovered, DialogComboStyle style={}) {
    const auto scale=[&](int value){ return MulDiv(value, static_cast<int>(dpi),96); };
    RECT bounds{}; GetClientRect(hwnd,&bounds);
    HBRUSH brush=CreateSolidBrush(hovered?RGB(24,65,80):RGB(12,49,62));
    FillRect(dc,&bounds,brush);
    HPEN pen=CreatePen(PS_SOLID,1,hovered||GetFocus()==hwnd?RGB(79,171,204):RGB(66,124,143));
    const auto old=SelectObject(dc,pen); const auto oldBrush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
    if (style.radius) RoundRect(dc,0,0,bounds.right,bounds.bottom,scale(style.radius),scale(style.radius));
    else Rectangle(dc,0,0,bounds.right,bounds.bottom);
    SelectObject(dc,oldBrush); SelectObject(dc,old); DeleteObject(pen); DeleteObject(brush);
    wchar_t label[256]{}; GetWindowTextW(hwnd,label,ARRAYSIZE(label));
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,IsWindowEnabled(hwnd)?RGB(244,249,251):RGB(102,135,145));
    const auto oldFont=SelectObject(dc,font); RECT r=bounds; r.left+=scale(style.inset); r.right-=scale(34);
    DrawTextW(dc,label,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS); SelectObject(dc,oldFont);
    pen=CreatePen(PS_SOLID,std::max(1,scale(1)),hovered?RGB(244,249,251):RGB(157,193,205)); const auto previous=SelectObject(dc,pen);
    const int x=bounds.right-scale(16), y=bounds.bottom/2;
    MoveToEx(dc,x-scale(style.arrowWidth),y-scale(2),nullptr); LineTo(dc,x,y+scale(style.arrowBottom)); LineTo(dc,x+scale(style.arrowWidth),y-scale(2));
    SelectObject(dc,previous); DeleteObject(pen);
}
inline void DrawDialogEditBorder(HWND hwnd, HBRUSH fieldBrush, bool hovered,
                                 COLORREF accent, COLORREF border, int cornerDiameter) {
    HDC dc=GetWindowDC(hwnd); if(!dc)return;
    RECT r{},client{};GetWindowRect(hwnd,&r);GetClientRect(hwnd,&client);
    POINT origin{};ClientToScreen(hwnd,&origin);OffsetRect(&client,origin.x-r.left,origin.y-r.top);
    ExcludeClipRect(dc,client.left,client.top,client.right,client.bottom);
    RECT bounds{0,0,r.right-r.left,r.bottom-r.top};FillRect(dc,&bounds,fieldBrush);
    const auto pen=CreatePen(PS_SOLID,1,hovered||GetFocus()==hwnd?accent:border);
    const auto old=SelectObject(dc,pen);const auto oldBrush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc,0,0,r.right-r.left,r.bottom-r.top,cornerDiameter,cornerDiameter);
    SelectObject(dc,oldBrush);SelectObject(dc,old);DeleteObject(pen);ReleaseDC(hwnd,dc);
}
inline void DrawDialogButton(const DRAWITEMSTRUCT& draw, HFONT font, UINT dpi,
                             bool hovered, bool primary, int cornerDiameter=6) {
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const bool focused = (draw.itemState & ODS_FOCUS) != 0;
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const COLORREF fill = primary
        ? (pressed ? RGB(45,126,157) : hovered ? RGB(61,151,184) : RGB(48,134,166))
        : (pressed ? RGB(25,69,84) : hovered ? RGB(24,65,80) : RGB(12,49,62));
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, hovered || focused || primary ? RGB(79,171,204) : RGB(66,124,143));
    const auto oldBrush = SelectObject(draw.hDC, brush);
    const auto oldPen = SelectObject(draw.hDC, pen);
    const int radius = MulDiv(cornerDiameter, static_cast<int>(dpi), 96);
    RoundRect(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right, draw.rcItem.bottom, radius, radius);
    SelectObject(draw.hDC, oldBrush); SelectObject(draw.hDC, oldPen);
    DeleteObject(brush); DeleteObject(pen);
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, disabled ? RGB(102,135,145) : RGB(244,249,251));
    const auto oldFont = SelectObject(draw.hDC, font);
    wchar_t label[128]{};
    GetWindowTextW(draw.hwndItem, label, ARRAYSIZE(label));
    RECT bounds = draw.rcItem;
    DrawTextW(draw.hDC, label, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(draw.hDC, oldFont);
}
}
