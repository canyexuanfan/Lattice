#include "app/TrayIcon.h"
#include "app/resource.h"

#include <shellapi.h>

TrayIcon::~TrayIcon() {
    Remove();
}

bool TrayIcon::Initialize(HWND owner, HINSTANCE instance) {
    if (owner == nullptr) {
        return false;
    }

    owner_ = owner;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = id_;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayIconMessage;
    data.hIcon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        0,
        0,
        LR_DEFAULTSIZE | LR_SHARED));
    if (data.hIcon == nullptr) {
        data.hIcon = static_cast<HICON>(LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, 0, 0, LR_SHARED));
    }
    lstrcpynW(data.szTip, L"Lattice\n右键打开功能菜单", ARRAYSIZE(data.szTip));

    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        owner_ = nullptr;
        return false;
    }

    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    visible_ = true;
    return true;
}

void TrayIcon::Remove() {
    if (!visible_ || owner_ == nullptr) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = id_;
    Shell_NotifyIconW(NIM_DELETE, &data);
    visible_ = false;
    owner_ = nullptr;
}
