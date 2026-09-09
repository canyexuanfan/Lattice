#include "shell/ShellLauncher.h"

#include <Windows.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <wrl/client.h>

#include <vector>

namespace {

thread_local IContextMenu2* gActiveContextMenu2 = nullptr;
thread_local IContextMenu3* gActiveContextMenu3 = nullptr;

HRESULT CreateSingleContextMenu(
    HWND ownerWindow,
    const std::wstring& path,
    IContextMenu** contextMenu) {
    *contextMenu = nullptr;
    PIDLIST_ABSOLUTE absolute = nullptr;
    HRESULT result = SHParseDisplayName(
        path.c_str(), nullptr, &absolute, 0, nullptr);
    Microsoft::WRL::ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    if (SUCCEEDED(result) && absolute != nullptr) {
        result = SHBindToParent(
            absolute,
            IID_PPV_ARGS(parent.GetAddressOf()),
            &child);
    }
    if (SUCCEEDED(result) &&
        parent != nullptr && child != nullptr) {
        result = parent->GetUIObjectOf(
            ownerWindow,
            1,
            &child,
            IID_IContextMenu,
            nullptr,
            reinterpret_cast<void**>(contextMenu));
    }
    if (absolute != nullptr) {
        CoTaskMemFree(absolute);
    }
    return result;
}

bool TrackShellContextMenu(
    HWND ownerWindow,
    IContextMenu* contextMenu,
    POINT screenPoint) {
    if (contextMenu == nullptr) {
        return false;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return false;
    }
    constexpr UINT kFirstCommand = 1;
    constexpr UINT kLastCommand = 0x7FFF;
    HRESULT result = contextMenu->QueryContextMenu(
        menu, 0, kFirstCommand, kLastCommand, CMF_NORMAL);
    if (FAILED(result)) {
        DestroyMenu(menu);
        return false;
    }
    Microsoft::WRL::ComPtr<IContextMenu2> contextMenu2;
    Microsoft::WRL::ComPtr<IContextMenu3> contextMenu3;
    contextMenu->QueryInterface(
        IID_PPV_ARGS(contextMenu2.GetAddressOf()));
    contextMenu->QueryInterface(
        IID_PPV_ARGS(contextMenu3.GetAddressOf()));
    gActiveContextMenu2 = contextMenu2.Get();
    gActiveContextMenu3 = contextMenu3.Get();
    SetForegroundWindow(ownerWindow);
    const UINT selected = TrackPopupMenuEx(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x,
        screenPoint.y,
        ownerWindow,
        nullptr);
    gActiveContextMenu3 = nullptr;
    gActiveContextMenu2 = nullptr;
    bool invoked = selected == 0;
    if (selected >= kFirstCommand &&
        selected <= kLastCommand) {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask =
            CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = ownerWindow;
        invoke.lpVerb = MAKEINTRESOURCEA(
            selected - kFirstCommand);
        invoke.lpVerbW = MAKEINTRESOURCEW(
            selected - kFirstCommand);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        invoked = SUCCEEDED(contextMenu->InvokeCommand(
            reinterpret_cast<LPCMINVOKECOMMANDINFO>(
                &invoke)));
    }
    DestroyMenu(menu);
    PostMessageW(ownerWindow, WM_NULL, 0, 0);
    return invoked;
}

}  // namespace

bool ShellLauncher::OpenPath(const std::wstring& path) const {
    if (path.empty()) {
        return false;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl != nullptr) {
            SHELLEXECUTEINFOW execute{};
            execute.cbSize = sizeof(execute);
            execute.fMask = SEE_MASK_IDLIST;
            execute.lpIDList = pidl;
            execute.nShow = SW_SHOWNORMAL;
            const bool succeeded = ShellExecuteExW(&execute) != FALSE;
            CoTaskMemFree(pidl);
            return succeeded;
        }
    }
    HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool ShellLauncher::ShowInExplorer(const std::wstring& path) const {
    if (path.empty()) {
        return false;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return OpenPath(path);
    }
    const std::wstring parameters = L"/select,\"" + path + L"\"";
    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool ShellLauncher::RunAsAdministrator(const std::wstring& path) const {
    if (path.empty()) {
        return false;
    }
    HINSTANCE result = ShellExecuteW(nullptr, L"runas", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool ShellLauncher::ShowContextMenu(
    HWND ownerWindow,
    const std::wstring& path,
    POINT screenPoint) const {
    if (ownerWindow == nullptr || path.empty()) {
        return false;
    }
    Microsoft::WRL::ComPtr<IContextMenu> contextMenu;
    const HRESULT result = CreateSingleContextMenu(
        ownerWindow,
        path,
        contextMenu.GetAddressOf());
    return SUCCEEDED(result) &&
        TrackShellContextMenu(
            ownerWindow, contextMenu.Get(), screenPoint);
}

bool ShellLauncher::ShowContextMenu(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items,
    POINT screenPoint) const {
    if (ownerWindow == nullptr || items.empty()) {
        return false;
    }
    Microsoft::WRL::ComPtr<IContextMenu> contextMenu;
    const HRESULT result =
        CreateDesktopShellSelectionObject(
            ownerWindow,
            items,
            IID_IContextMenu,
            reinterpret_cast<void**>(
                contextMenu.GetAddressOf()));
    if (FAILED(result) || contextMenu == nullptr) {
        return false;
    }
    return TrackShellContextMenu(
        ownerWindow, contextMenu.Get(), screenPoint);
}

bool ShellLauncher::ForwardContextMenuMessage(
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    LRESULT& result) const {
    result = 0;
    if (gActiveContextMenu3 != nullptr &&
        SUCCEEDED(gActiveContextMenu3->HandleMenuMsg2(
            message, wParam, lParam, &result))) {
        return true;
    }
    return gActiveContextMenu2 != nullptr &&
        SUCCEEDED(gActiveContextMenu2->HandleMenuMsg(
            message, wParam, lParam));
}
