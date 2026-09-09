#include "shell/ShellItemReference.h"

#include <ShlObj.h>
#include <ShObjIdl.h>
#include <wrl/client.h>

#include <cstring>
#include <limits>
#include <vector>

namespace {

HRESULT CloneSingleChildPidl(
    const std::vector<BYTE>& bytes,
    PIDLIST_RELATIVE& clone) {
    clone = nullptr;
    if (bytes.size() < sizeof(USHORT) * 2U) {
        return E_INVALIDARG;
    }
    USHORT itemSize = 0;
    std::memcpy(&itemSize, bytes.data(), sizeof(itemSize));
    if (itemSize < sizeof(USHORT) ||
        static_cast<size_t>(itemSize) + sizeof(USHORT) !=
            bytes.size()) {
        return E_INVALIDARG;
    }
    USHORT terminator = 1;
    std::memcpy(
        &terminator,
        bytes.data() + itemSize,
        sizeof(terminator));
    if (terminator != 0) {
        return E_INVALIDARG;
    }
    clone = static_cast<PIDLIST_RELATIVE>(
        CoTaskMemAlloc(bytes.size()));
    if (clone == nullptr) {
        return E_OUTOFMEMORY;
    }
    std::memcpy(clone, bytes.data(), bytes.size());
    if (ILIsEmpty(clone) ||
        !ILIsEmpty(ILNext(clone)) ||
        ILGetSize(clone) != bytes.size()) {
        CoTaskMemFree(clone);
        clone = nullptr;
        return E_INVALIDARG;
    }
    return S_OK;
}

}  // namespace

HRESULT CreateDesktopShellSelectionObject(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items,
    REFIID interfaceId,
    void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (items.empty() ||
        items.size() >
            static_cast<size_t>(
                (std::numeric_limits<UINT>::max)())) {
        return E_INVALIDARG;
    }

    Microsoft::WRL::ComPtr<IShellFolder> desktopFolder;
    HRESULT result = SHGetDesktopFolder(
        desktopFolder.GetAddressOf());
    if (FAILED(result) || desktopFolder == nullptr) {
        return FAILED(result) ? result : E_FAIL;
    }

    std::vector<PIDLIST_RELATIVE> ownedPidls;
    std::vector<PCUITEMID_CHILD> children;
    ownedPidls.reserve(items.size());
    children.reserve(items.size());
    for (const ShellItemReference& item : items) {
        PIDLIST_RELATIVE clone = nullptr;
        result = CloneSingleChildPidl(
            item.desktopChildPidl, clone);
        if (FAILED(result)) {
            break;
        }
        ownedPidls.push_back(clone);
        children.push_back(clone);
    }
    if (SUCCEEDED(result) &&
        children.size() == items.size()) {
        result = desktopFolder->GetUIObjectOf(
            ownerWindow,
            static_cast<UINT>(children.size()),
            children.data(),
            interfaceId,
            nullptr,
            object);
        if (SUCCEEDED(result) && *object == nullptr) {
            result = E_FAIL;
        }
    } else if (SUCCEEDED(result)) {
        result = E_FAIL;
    }
    for (PIDLIST_RELATIVE pidl : ownedPidls) {
        CoTaskMemFree(pidl);
    }
    return result;
}
