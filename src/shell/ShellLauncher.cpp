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

bool IsCanonicalRenameVerb(
    IContextMenu* contextMenu,
    UINT commandOffset) {
    wchar_t wideVerb[128]{};
    if (SUCCEEDED(contextMenu->GetCommandString(
            commandOffset,
            GCS_VERBW,
            nullptr,
            reinterpret_cast<LPSTR>(wideVerb),
            ARRAYSIZE(wideVerb) - 1)) &&
        CompareStringOrdinal(
            wideVerb, -1, L"rename", -1, TRUE) == CSTR_EQUAL) {
        return true;
    }

    char ansiVerb[128]{};
    return SUCCEEDED(contextMenu->GetCommandString(
               commandOffset,
               GCS_VERBA,
               nullptr,
               ansiVerb,
               ARRAYSIZE(ansiVerb) - 1)) &&
           lstrcmpiA(ansiVerb, "rename") == 0;
}

ShellContextMenuResult TrackShellContextMenu(
    HWND ownerWindow,
    IContextMenu* contextMenu,
    POINT screenPoint,
    bool allowRename) {
    if (contextMenu == nullptr) {
        return ShellContextMenuResult::Failed;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return ShellContextMenuResult::Failed;
    }
    constexpr UINT kFirstCommand = 1;
    constexpr UINT kLastCommand = 0x7FFF;
    UINT flags = CMF_NORMAL;
    if (allowRename) {
        flags |= CMF_CANRENAME;
    }
    HRESULT result = contextMenu->QueryContextMenu(
        menu, 0, kFirstCommand, kLastCommand, flags);
    if (FAILED(result)) {
        DestroyMenu(menu);
        return ShellContextMenuResult::Failed;
    }
    constexpr UINT kNoCommand = static_cast<UINT>(-1);
    UINT renameCommandOffset = kNoCommand;
    if (allowRename) {
        const UINT reportedCommandCount =
            static_cast<UINT>(HRESULT_CODE(result));
        const UINT maximumCommandCount =
            kLastCommand - kFirstCommand + 1;
        const UINT commandCount = reportedCommandCount < maximumCommandCount
            ? reportedCommandCount
            : maximumCommandCount;
        for (UINT commandOffset = 0;
             commandOffset < commandCount;
             ++commandOffset) {
            if (IsCanonicalRenameVerb(
                    contextMenu, commandOffset)) {
                renameCommandOffset = commandOffset;
                break;
            }
        }
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
    ShellContextMenuResult outcome = selected == 0
        ? ShellContextMenuResult::Cancelled
        : ShellContextMenuResult::Failed;
    if (selected >= kFirstCommand &&
        selected <= kLastCommand) {
        const UINT commandOffset = selected - kFirstCommand;
        if (commandOffset == renameCommandOffset) {
            outcome = ShellContextMenuResult::RenameRequested;
        } else {
            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask =
                CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            invoke.hwnd = ownerWindow;
            invoke.lpVerb = MAKEINTRESOURCEA(
                commandOffset);
            invoke.lpVerbW = MAKEINTRESOURCEW(
                commandOffset);
            invoke.nShow = SW_SHOWNORMAL;
            invoke.ptInvoke = screenPoint;
            outcome = SUCCEEDED(contextMenu->InvokeCommand(
                reinterpret_cast<LPCMINVOKECOMMANDINFO>(
                    &invoke)))
                ? ShellContextMenuResult::Invoked
                : ShellContextMenuResult::Failed;
        }
    }
    DestroyMenu(menu);
    PostMessageW(ownerWindow, WM_NULL, 0, 0);
    return outcome;
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
            ownerWindow, contextMenu.Get(), screenPoint, false) !=
            ShellContextMenuResult::Failed;
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
        ownerWindow, contextMenu.Get(), screenPoint, false) !=
        ShellContextMenuResult::Failed;
}

ShellContextMenuResult ShellLauncher::ShowDesktopContextMenu(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items,
    POINT screenPoint,
    bool allowRename) const {
    if (ownerWindow == nullptr || items.empty()) {
        return ShellContextMenuResult::Failed;
    }
    Microsoft::WRL::ComPtr<IContextMenu> contextMenu;
    const HRESULT result = CreateDesktopShellSelectionObject(
        ownerWindow,
        items,
        IID_IContextMenu,
        reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    if (FAILED(result) || contextMenu == nullptr) {
        return ShellContextMenuResult::Failed;
    }
    return TrackShellContextMenu(
        ownerWindow,
        contextMenu.Get(),
        screenPoint,
        allowRename && items.size() == 1);
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
