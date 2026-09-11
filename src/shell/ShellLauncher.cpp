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

bool GetCanonicalVerb(
    IContextMenu* contextMenu,
    UINT commandOffset,
    std::wstring& verb) {
    verb.clear();
    wchar_t wideVerb[128]{};
    if (SUCCEEDED(contextMenu->GetCommandString(
            commandOffset,
            GCS_VERBW,
            nullptr,
            reinterpret_cast<LPSTR>(wideVerb),
            ARRAYSIZE(wideVerb) - 1)) &&
        wideVerb[0] != L'\0') {
        verb = wideVerb;
        return true;
    }

    char ansiVerb[128]{};
    if (!SUCCEEDED(contextMenu->GetCommandString(
               commandOffset,
               GCS_VERBA,
               nullptr,
               ansiVerb,
               ARRAYSIZE(ansiVerb) - 1)) ||
        ansiVerb[0] == '\0') {
        return false;
    }
    wchar_t convertedVerb[128]{};
    const int length = MultiByteToWideChar(
        CP_ACP,
        0,
        ansiVerb,
        -1,
        convertedVerb,
        ARRAYSIZE(convertedVerb));
    if (length <= 1) {
        return false;
    }
    verb.assign(convertedVerb);
    return true;
}

bool IsCanonicalVerb(
    IContextMenu* contextMenu,
    UINT commandOffset,
    const wchar_t* expectedVerb) {
    std::wstring verb;
    return GetCanonicalVerb(contextMenu, commandOffset, verb) &&
        CompareStringOrdinal(
            verb.c_str(), -1, expectedVerb, -1, TRUE) == CSTR_EQUAL;
}

ShellContextMenuResult TrackShellContextMenu(
    HWND ownerWindow,
    IContextMenu* contextMenu,
    POINT screenPoint,
    bool allowRename,
    const ShellMenuAppender& appendCommands = {},
    UINT* customCommand = nullptr,
    std::wstring* invokedVerb = nullptr) {
    if (contextMenu == nullptr) {
        return ShellContextMenuResult::Failed;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return ShellContextMenuResult::Failed;
    }
    constexpr UINT kFirstCommand = 1;
    constexpr UINT kLastCommand = 0x6FFF;
    constexpr UINT kFirstCustomCommand = 0x7000;
    if (customCommand != nullptr) {
        *customCommand = 0;
    }
    if (invokedVerb != nullptr) {
        invokedVerb->clear();
    }
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
            if (IsCanonicalVerb(
                    contextMenu, commandOffset, L"rename")) {
                renameCommandOffset = commandOffset;
                break;
            }
        }
    }
    if (appendCommands) {
        appendCommands(menu);
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
    if (selected >= kFirstCustomCommand) {
        if (customCommand != nullptr) {
            *customCommand = selected;
        }
        outcome = ShellContextMenuResult::CustomCommand;
    } else if (selected >= kFirstCommand &&
        selected <= kLastCommand) {
        const UINT commandOffset = selected - kFirstCommand;
        if (commandOffset == renameCommandOffset) {
            outcome = ShellContextMenuResult::RenameRequested;
        } else {
            if (invokedVerb != nullptr) {
                GetCanonicalVerb(
                    contextMenu, commandOffset, *invokedVerb);
            }
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

bool InvokeShellContextMenuVerb(
    HWND ownerWindow,
    IContextMenu* contextMenu,
    const std::wstring& canonicalVerb) {
    if (ownerWindow == nullptr || contextMenu == nullptr ||
        canonicalVerb.empty()) {
        return false;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return false;
    }
    constexpr UINT kFirstCommand = 1;
    constexpr UINT kLastCommand = 0x7FFF;
    const HRESULT query = contextMenu->QueryContextMenu(
        menu, 0, kFirstCommand, kLastCommand, CMF_NORMAL | CMF_CANRENAME);
    bool invoked = false;
    if (SUCCEEDED(query)) {
        const UINT commandCount = (std::min)(
            static_cast<UINT>(HRESULT_CODE(query)),
            kLastCommand - kFirstCommand + 1);
        for (UINT offset = 0; offset < commandCount; ++offset) {
            if (!IsCanonicalVerb(
                    contextMenu, offset, canonicalVerb.c_str())) {
                continue;
            }
            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask = CMIC_MASK_UNICODE;
            invoke.hwnd = ownerWindow;
            invoke.lpVerb = MAKEINTRESOURCEA(offset);
            invoke.lpVerbW = MAKEINTRESOURCEW(offset);
            invoke.nShow = SW_SHOWNORMAL;
            invoked = SUCCEEDED(contextMenu->InvokeCommand(
                reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke)));
            break;
        }
    }
    DestroyMenu(menu);
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

ShellContextMenuResult ShellLauncher::ShowContextMenuWithExtensions(
    HWND ownerWindow,
    const std::wstring& path,
    POINT screenPoint,
    bool allowRename,
    const ShellMenuAppender& appendCommands,
    UINT& customCommand,
    std::wstring& invokedVerb) const {
    if (ownerWindow == nullptr || path.empty()) {
        return ShellContextMenuResult::Failed;
    }
    Microsoft::WRL::ComPtr<IContextMenu> contextMenu;
    const HRESULT result = CreateSingleContextMenu(
        ownerWindow, path, contextMenu.GetAddressOf());
    if (FAILED(result) || contextMenu == nullptr) {
        return ShellContextMenuResult::Failed;
    }
    return TrackShellContextMenu(
        ownerWindow,
        contextMenu.Get(),
        screenPoint,
        allowRename,
        appendCommands,
        &customCommand,
        &invokedVerb);
}

ShellContextMenuResult ShellLauncher::ShowDesktopContextMenuWithExtensions(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items,
    POINT screenPoint,
    bool allowRename,
    const ShellMenuAppender& appendCommands,
    UINT& customCommand,
    std::wstring& invokedVerb) const {
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
        allowRename && items.size() == 1,
        appendCommands,
        &customCommand,
        &invokedVerb);
}

bool ShellLauncher::InvokeContextMenuVerb(
    HWND ownerWindow,
    const std::wstring& path,
    const std::wstring& canonicalVerb) const {
    Microsoft::WRL::ComPtr<IContextMenu> contextMenu;
    return ownerWindow != nullptr && !path.empty() &&
        SUCCEEDED(CreateSingleContextMenu(
            ownerWindow, path, contextMenu.GetAddressOf())) &&
        InvokeShellContextMenuVerb(
            ownerWindow, contextMenu.Get(), canonicalVerb);
}

bool ShellLauncher::InvokeDesktopContextMenuVerb(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items,
    const std::wstring& canonicalVerb) const {
    if (ownerWindow == nullptr || items.empty()) {
        return false;
    }
    Microsoft::WRL::ComPtr<IContextMenu> contextMenu;
    return SUCCEEDED(CreateDesktopShellSelectionObject(
               ownerWindow,
               items,
               IID_IContextMenu,
               reinterpret_cast<void**>(contextMenu.GetAddressOf()))) &&
        InvokeShellContextMenuVerb(
            ownerWindow, contextMenu.Get(), canonicalVerb);
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
