#include "shell/ShellItemReference.h"

#include <ShlObj.h>
#include <ShObjIdl.h>
#include <wrl/client.h>

#include <cstring>
#include <limits>
#include <utility>
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

HRESULT SerializeSingleChildPidl(
    PCUITEMID_CHILD child,
    std::vector<BYTE>& bytes) {
    bytes.clear();
    if (child == nullptr || ILIsEmpty(child) ||
        !ILIsEmpty(ILNext(child))) {
        return E_INVALIDARG;
    }
    const UINT size = ILGetSize(child);
    if (size < sizeof(USHORT) * 2U) {
        return E_INVALIDARG;
    }
    bytes.resize(size);
    std::memcpy(bytes.data(), child, size);
    return S_OK;
}

HRESULT DesktopChildDisplayName(
    IShellFolder* desktopFolder,
    PCUITEMID_CHILD child,
    SHGDNF flags,
    std::wstring& value) {
    value.clear();
    if (desktopFolder == nullptr || child == nullptr) {
        return E_INVALIDARG;
    }
    STRRET shellName{};
    HRESULT result = desktopFolder->GetDisplayNameOf(
        child, flags, &shellName);
    if (FAILED(result)) {
        return result;
    }
    if (shellName.uType == STRRET_WSTR) {
        if (shellName.pOleStr == nullptr) {
            return E_UNEXPECTED;
        }
        value = shellName.pOleStr;
        CoTaskMemFree(shellName.pOleStr);
        return S_OK;
    }
    const char* ansiValue = nullptr;
    size_t capacity = 0;
    if (shellName.uType == STRRET_CSTR) {
        ansiValue = shellName.cStr;
        capacity = ARRAYSIZE(shellName.cStr);
    } else if (shellName.uType == STRRET_OFFSET) {
        const UINT childSize = ILGetSize(child);
        if (shellName.uOffset >= childSize) {
            return E_INVALIDARG;
        }
        ansiValue = reinterpret_cast<const char*>(child) +
            shellName.uOffset;
        capacity = childSize - shellName.uOffset;
    } else {
        return E_UNEXPECTED;
    }
    size_t length = 0;
    while (length < capacity && ansiValue[length] != '\0') {
        ++length;
    }
    if (length == capacity ||
        length > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return E_INVALIDARG;
    }
    const int required = MultiByteToWideChar(
        CP_ACP,
        MB_PRECOMPOSED,
        ansiValue,
        static_cast<int>(length),
        nullptr,
        0);
    if (required <= 0 && length != 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    value.resize(static_cast<size_t>(required));
    if (required > 0 &&
        MultiByteToWideChar(
            CP_ACP,
            MB_PRECOMPOSED,
            ansiValue,
            static_cast<int>(length),
            value.data(),
            required) != required) {
        value.clear();
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

HRESULT BindShellPathToParent(
    const std::wstring& parsingName,
    PIDLIST_ABSOLUTE& absolute,
    Microsoft::WRL::ComPtr<IShellFolder>& parent,
    PCUITEMID_CHILD& child) {
    absolute = nullptr;
    parent.Reset();
    child = nullptr;
    if (parsingName.empty()) {
        return E_INVALIDARG;
    }
    HRESULT result = SHParseDisplayName(
        parsingName.c_str(), nullptr, &absolute, 0, nullptr);
    if (SUCCEEDED(result)) {
        result = SHBindToParent(
            absolute,
            IID_PPV_ARGS(parent.GetAddressOf()),
            &child);
    }
    if (FAILED(result)) {
        if (absolute != nullptr) {
            CoTaskMemFree(absolute);
            absolute = nullptr;
        }
        parent.Reset();
        child = nullptr;
    }
    return result;
}

HRESULT ShellNameFromAbsolute(
    PCIDLIST_ABSOLUTE absolute,
    SIGDN kind,
    std::wstring& value) {
    value.clear();
    PWSTR name = nullptr;
    const HRESULT result = SHGetNameFromIDList(
        absolute, kind, &name);
    if (SUCCEEDED(result) && name != nullptr) {
        value = name;
    }
    if (name != nullptr) {
        CoTaskMemFree(name);
    }
    return result;
}

HRESULT ParseRelativeDisplayName(
    IShellFolder* folder,
    const std::wstring& displayName,
    PIDLIST_RELATIVE& child) {
    child = nullptr;
    if (folder == nullptr || displayName.empty()) {
        return E_INVALIDARG;
    }
    std::vector<wchar_t> mutableName(
        displayName.begin(), displayName.end());
    mutableName.push_back(L'\0');
    ULONG eaten = 0;
    SFGAOF attributes = 0;
    return folder->ParseDisplayName(
        nullptr,
        nullptr,
        mutableName.data(),
        &eaten,
        &child,
        &attributes);
}

HRESULT FindDesktopChildByParsingName(
    IShellFolder* desktopFolder,
    const std::wstring& parsingName,
    PIDLIST_RELATIVE& child) {
    child = nullptr;
    if (desktopFolder == nullptr || parsingName.empty()) {
        return E_INVALIDARG;
    }
    Microsoft::WRL::ComPtr<IEnumIDList> enumerator;
    HRESULT result = desktopFolder->EnumObjects(
        nullptr,
        SHCONTF_FOLDERS | SHCONTF_NONFOLDERS |
            SHCONTF_INCLUDEHIDDEN,
        enumerator.GetAddressOf());
    if (FAILED(result) || enumerator == nullptr) {
        return FAILED(result) ? result : E_FAIL;
    }
    size_t enumeratedCount = 0;
    while (true) {
        PIDLIST_RELATIVE candidate = nullptr;
        ULONG fetched = 0;
        result = enumerator->Next(1, &candidate, &fetched);
        if (result == S_FALSE) {
            if (candidate != nullptr) {
                CoTaskMemFree(candidate);
            }
            if (fetched != 0 || candidate != nullptr) {
                if (child != nullptr) {
                    CoTaskMemFree(child);
                    child = nullptr;
                }
                return E_UNEXPECTED;
            }
            return child != nullptr
                ? S_OK
                : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
        if (FAILED(result) || fetched != 1 || candidate == nullptr) {
            if (candidate != nullptr) {
                CoTaskMemFree(candidate);
            }
            if (child != nullptr) {
                CoTaskMemFree(child);
                child = nullptr;
            }
            return FAILED(result) ? result : E_FAIL;
        }
        if (enumeratedCount >= 4096) {
            CoTaskMemFree(candidate);
            if (child != nullptr) {
                CoTaskMemFree(child);
                child = nullptr;
            }
            return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
        }
        ++enumeratedCount;
        if (ILIsEmpty(candidate) ||
            !ILIsEmpty(ILNext(candidate))) {
            CoTaskMemFree(candidate);
            if (child != nullptr) {
                CoTaskMemFree(child);
                child = nullptr;
            }
            return E_UNEXPECTED;
        }
        std::wstring candidateName;
        const HRESULT nameResult = DesktopChildDisplayName(
            desktopFolder,
            candidate,
            SHGDN_FORPARSING,
            candidateName);
        if (FAILED(nameResult)) {
            CoTaskMemFree(candidate);
            if (child != nullptr) {
                CoTaskMemFree(child);
                child = nullptr;
            }
            return nameResult;
        }
        if (CompareStringOrdinal(
                candidateName.c_str(), -1,
                parsingName.c_str(), -1,
                TRUE) == CSTR_EQUAL) {
            if (child != nullptr) {
                CoTaskMemFree(candidate);
                CoTaskMemFree(child);
                child = nullptr;
                return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
            }
            child = candidate;
        } else {
            CoTaskMemFree(candidate);
        }
    }
}

HRESULT PopulateDesktopRenameResult(
    IShellFolder* desktopFolder,
    PCUITEMID_CHILD child,
    DesktopShellRenameResult& renamedItem) {
    DesktopShellRenameResult candidate;
    candidate.disposition = ShellRenameDisposition::Renamed;
    HRESULT result = SerializeSingleChildPidl(
        child, candidate.item.desktopChildPidl);
    if (SUCCEEDED(result)) {
        result = DesktopChildDisplayName(
            desktopFolder,
            child,
            SHGDN_FORPARSING,
            candidate.item.path);
    }
    if (SUCCEEDED(result)) {
        result = DesktopChildDisplayName(
            desktopFolder,
            child,
            SHGDN_NORMAL,
            candidate.displayName);
    }
    if (SUCCEEDED(result)) {
        renamedItem = std::move(candidate);
    }
    return result;
}

HRESULT PopulateShellPathRenameResult(
    PCIDLIST_ABSOLUTE originalAbsolute,
    PCUITEMID_CHILD renamedChild,
    ShellPathRenameResult& renamedItem) {
    PIDLIST_ABSOLUTE parentAbsolute =
        ILCloneFull(originalAbsolute);
    if (parentAbsolute == nullptr) {
        return E_OUTOFMEMORY;
    }
    HRESULT result = S_OK;
    if (ILRemoveLastID(parentAbsolute) == FALSE) {
        result = E_UNEXPECTED;
    }
    PIDLIST_ABSOLUTE renamedAbsolute = nullptr;
    if (SUCCEEDED(result)) {
        renamedAbsolute = ILCombine(
            parentAbsolute, renamedChild);
        if (renamedAbsolute == nullptr) {
            result = E_OUTOFMEMORY;
        }
    }
    ShellPathRenameResult candidate;
    candidate.disposition = ShellRenameDisposition::Renamed;
    if (SUCCEEDED(result)) {
        result = ShellNameFromAbsolute(
            renamedAbsolute,
            SIGDN_DESKTOPABSOLUTEPARSING,
            candidate.parsingName);
    }
    if (SUCCEEDED(result)) {
        result = ShellNameFromAbsolute(
            renamedAbsolute,
            SIGDN_NORMALDISPLAY,
            candidate.displayName);
    }
    if (SUCCEEDED(result)) {
        renamedItem = std::move(candidate);
    }
    if (renamedAbsolute != nullptr) {
        CoTaskMemFree(renamedAbsolute);
    }
    CoTaskMemFree(parentAbsolute);
    return result;
}

}  // namespace

HRESULT CanRenameShellPath(
    const std::wstring& parsingName,
    bool& canRename) {
    canRename = false;
    PIDLIST_ABSOLUTE absolute = nullptr;
    Microsoft::WRL::ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    HRESULT result = BindShellPathToParent(
        parsingName, absolute, parent, child);
    SFGAOF attributes = SFGAO_CANRENAME;
    if (SUCCEEDED(result)) {
        result = parent->GetAttributesOf(1, &child, &attributes);
    }
    if (absolute != nullptr) {
        CoTaskMemFree(absolute);
    }
    if (SUCCEEDED(result)) {
        canRename = (attributes & SFGAO_CANRENAME) != 0;
    }
    return result;
}

HRESULT RenameShellPath(
    HWND ownerWindow,
    const std::wstring& parsingName,
    const std::wstring& newDisplayName,
    ShellPathRenameResult& renamedItem) {
    renamedItem = {};
    if (newDisplayName.empty()) {
        return E_INVALIDARG;
    }
    PIDLIST_ABSOLUTE absolute = nullptr;
    Microsoft::WRL::ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    HRESULT result = BindShellPathToParent(
        parsingName, absolute, parent, child);
    SFGAOF attributes = SFGAO_CANRENAME;
    if (SUCCEEDED(result)) {
        result = parent->GetAttributesOf(1, &child, &attributes);
        if (SUCCEEDED(result) &&
            (attributes & SFGAO_CANRENAME) == 0) {
            result = E_ACCESSDENIED;
        }
    }
    PIDLIST_RELATIVE renamedChild = nullptr;
    if (SUCCEEDED(result)) {
        result = parent->SetNameOf(
            ownerWindow,
            child,
            newDisplayName.c_str(),
            SHGDN_NORMAL,
            &renamedChild);
    }
    if (SUCCEEDED(result)) {
        renamedItem.disposition =
            ShellRenameDisposition::Renamed;
        HRESULT reconstructionResult = renamedChild != nullptr
            ? PopulateShellPathRenameResult(
                absolute, renamedChild, renamedItem)
            : E_UNEXPECTED;
        if (FAILED(reconstructionResult)) {
            PIDLIST_RELATIVE parsedChild = nullptr;
            const HRESULT parseResult = ParseRelativeDisplayName(
                parent.Get(), newDisplayName, parsedChild);
            if (SUCCEEDED(parseResult) && parsedChild != nullptr) {
                reconstructionResult =
                    PopulateShellPathRenameResult(
                        absolute, parsedChild, renamedItem);
            }
            if (parsedChild != nullptr) {
                CoTaskMemFree(parsedChild);
            }
        }
        if (FAILED(reconstructionResult)) {
            renamedItem.disposition =
                ShellRenameDisposition::Renamed;
            renamedItem.postCommitError =
                reconstructionResult;
            result = reconstructionResult;
        }
    }
    if (renamedChild != nullptr) {
        CoTaskMemFree(renamedChild);
    }
    if (absolute != nullptr) {
        CoTaskMemFree(absolute);
    }
    if (FAILED(result) &&
        renamedItem.disposition ==
            ShellRenameDisposition::Unchanged) {
        renamedItem = {};
    }
    return result;
}

HRESULT CreateDesktopShellItemReference(
    const std::wstring& parsingName,
    ShellItemReference& item) {
    item = {};
    if (parsingName.empty()) {
        return E_INVALIDARG;
    }
    Microsoft::WRL::ComPtr<IShellFolder> desktopFolder;
    HRESULT result = SHGetDesktopFolder(
        desktopFolder.GetAddressOf());
    PIDLIST_RELATIVE child = nullptr;
    if (SUCCEEDED(result) && desktopFolder != nullptr) {
        result = FindDesktopChildByParsingName(
            desktopFolder.Get(), parsingName, child);
    }
    if (SUCCEEDED(result)) {
        result = SerializeSingleChildPidl(child, item.desktopChildPidl);
    }
    if (SUCCEEDED(result)) {
        result = DesktopChildDisplayName(
            desktopFolder.Get(),
            child,
            SHGDN_FORPARSING,
            item.path);
    }
    if (child != nullptr) {
        CoTaskMemFree(child);
    }
    if (FAILED(result)) {
        item = {};
    }
    return result;
}

HRESULT ResolveDesktopShellItemReferenceParsingName(
    const ShellItemReference& item,
    std::wstring& parsingName) {
    parsingName.clear();
    Microsoft::WRL::ComPtr<IShellFolder> desktopFolder;
    HRESULT result = SHGetDesktopFolder(
        desktopFolder.GetAddressOf());
    PIDLIST_RELATIVE child = nullptr;
    if (SUCCEEDED(result)) {
        result = CloneSingleChildPidl(
            item.desktopChildPidl, child);
    }
    if (SUCCEEDED(result)) {
        result = DesktopChildDisplayName(
            desktopFolder.Get(),
            child,
            SHGDN_FORPARSING,
            parsingName);
    }
    if (child != nullptr) {
        CoTaskMemFree(child);
    }
    return result;
}

HRESULT CanRenameDesktopShellItem(
    const ShellItemReference& item,
    bool& canRename) {
    canRename = false;
    Microsoft::WRL::ComPtr<IShellFolder> desktopFolder;
    HRESULT result = SHGetDesktopFolder(
        desktopFolder.GetAddressOf());
    PIDLIST_RELATIVE child = nullptr;
    if (SUCCEEDED(result)) {
        result = CloneSingleChildPidl(
            item.desktopChildPidl, child);
    }
    std::wstring resolvedIdentity;
    if (SUCCEEDED(result)) {
        result = DesktopChildDisplayName(
            desktopFolder.Get(),
            child,
            SHGDN_FORPARSING,
            resolvedIdentity);
    }
    if (SUCCEEDED(result) &&
        CompareStringOrdinal(
            resolvedIdentity.c_str(), -1,
            item.path.c_str(), -1,
            TRUE) != CSTR_EQUAL) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    SFGAOF attributes = SFGAO_CANRENAME;
    PCUITEMID_CHILD childPointer = child;
    if (SUCCEEDED(result)) {
        result = desktopFolder->GetAttributesOf(
            1, &childPointer, &attributes);
    }
    if (child != nullptr) {
        CoTaskMemFree(child);
    }
    if (SUCCEEDED(result)) {
        canRename = (attributes & SFGAO_CANRENAME) != 0;
    }
    return result;
}

HRESULT RenameDesktopShellItem(
    HWND ownerWindow,
    const ShellItemReference& item,
    const std::wstring& newDisplayName,
    DesktopShellRenameResult& renamedItem) {
    renamedItem = {};
    if (newDisplayName.empty()) {
        return E_INVALIDARG;
    }
    Microsoft::WRL::ComPtr<IShellFolder> desktopFolder;
    HRESULT result = SHGetDesktopFolder(
        desktopFolder.GetAddressOf());
    PIDLIST_RELATIVE child = nullptr;
    if (SUCCEEDED(result)) {
        result = CloneSingleChildPidl(
            item.desktopChildPidl, child);
    }
    std::wstring resolvedIdentity;
    if (SUCCEEDED(result)) {
        result = DesktopChildDisplayName(
            desktopFolder.Get(),
            child,
            SHGDN_FORPARSING,
            resolvedIdentity);
    }
    if (SUCCEEDED(result) &&
        CompareStringOrdinal(
            resolvedIdentity.c_str(), -1,
            item.path.c_str(), -1,
            TRUE) != CSTR_EQUAL) {
        result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    SFGAOF attributes = SFGAO_CANRENAME;
    PCUITEMID_CHILD childPointer = child;
    if (SUCCEEDED(result)) {
        result = desktopFolder->GetAttributesOf(
            1, &childPointer, &attributes);
        if (SUCCEEDED(result) &&
            (attributes & SFGAO_CANRENAME) == 0) {
            result = E_ACCESSDENIED;
        }
    }
    PIDLIST_RELATIVE renamedChild = nullptr;
    if (SUCCEEDED(result)) {
        result = desktopFolder->SetNameOf(
            ownerWindow,
            child,
            newDisplayName.c_str(),
            SHGDN_NORMAL,
            &renamedChild);
    }
    if (SUCCEEDED(result)) {
        renamedItem.disposition =
            ShellRenameDisposition::Renamed;
        HRESULT reconstructionResult = renamedChild != nullptr
            ? PopulateDesktopRenameResult(
                desktopFolder.Get(), renamedChild, renamedItem)
            : E_UNEXPECTED;
        if (FAILED(reconstructionResult)) {
            renamedItem.disposition =
                ShellRenameDisposition::Renamed;
            renamedItem.postCommitError =
                reconstructionResult;
            result = reconstructionResult;
        }
    }
    if (renamedChild != nullptr) {
        CoTaskMemFree(renamedChild);
    }
    if (child != nullptr) {
        CoTaskMemFree(child);
    }
    if (FAILED(result) &&
        renamedItem.disposition ==
            ShellRenameDisposition::Unchanged) {
        renamedItem = {};
    }
    return result;
}

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
        std::wstring resolvedIdentity;
        result = DesktopChildDisplayName(
            desktopFolder.Get(),
            clone,
            SHGDN_FORPARSING,
            resolvedIdentity);
        if (FAILED(result) ||
            CompareStringOrdinal(
                resolvedIdentity.c_str(), -1,
                item.path.c_str(), -1,
                TRUE) != CSTR_EQUAL) {
            if (SUCCEEDED(result)) {
                result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            break;
        }
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
