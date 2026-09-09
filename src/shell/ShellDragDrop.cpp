#include "shell/ShellDragDrop.h"

#include <Windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace {

std::atomic<HRESULT> g_lastDragImageInitializationResult{E_UNEXPECTED};

class ShellDataObject final : public IDataObject {
public:
    explicit ShellDataObject(std::vector<std::wstring> paths)
        : paths_(std::move(paths)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *object = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (format == nullptr || medium == nullptr) {
            return E_POINTER;
        }
        if (format->cfFormat != CF_HDROP || (format->tymed & TYMED_HGLOBAL) == 0 ||
            format->dwAspect != DVASPECT_CONTENT || format->lindex != -1) {
            return DV_E_FORMATETC;
        }

        size_t characterCount = 1;
        for (const std::wstring& path : paths_) {
            if (path.size() >
                (std::numeric_limits<size_t>::max)() -
                    characterCount - 1) {
                return E_OUTOFMEMORY;
            }
            characterCount += path.size() + 1;
        }
        if (characterCount >
            ((std::numeric_limits<SIZE_T>::max)() -
                sizeof(DROPFILES)) / sizeof(wchar_t)) {
            return E_OUTOFMEMORY;
        }
        const SIZE_T pathBytes =
            characterCount * sizeof(wchar_t);
        const SIZE_T totalBytes = sizeof(DROPFILES) + pathBytes;
        HGLOBAL global = GlobalAlloc(GHND, totalBytes);
        if (global == nullptr) {
            return E_OUTOFMEMORY;
        }
        auto* dropFiles = static_cast<DROPFILES*>(GlobalLock(global));
        if (dropFiles == nullptr) {
            GlobalFree(global);
            return E_OUTOFMEMORY;
        }
        dropFiles->pFiles = sizeof(DROPFILES);
        dropFiles->fWide = TRUE;
        auto* destination = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(dropFiles) + sizeof(DROPFILES));
        for (const std::wstring& path : paths_) {
            std::memcpy(
                destination,
                path.c_str(),
                path.size() * sizeof(wchar_t));
            destination += path.size();
            *destination++ = L'\0';
        }
        *destination = L'\0';
        GlobalUnlock(global);

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = global;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (format == nullptr) {
            return E_POINTER;
        }
        return format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL) != 0
                   ? S_OK
                   : DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* result) override {
        if (result != nullptr) {
            result->ptd = nullptr;
        }
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** enumerator) override {
        if (enumerator == nullptr) {
            return E_POINTER;
        }
        if (direction != DATADIR_GET) {
            return E_NOTIMPL;
        }
        FORMATETC format{};
        format.cfFormat = CF_HDROP;
        format.dwAspect = DVASPECT_CONTENT;
        format.lindex = -1;
        format.tymed = TYMED_HGLOBAL;
        return SHCreateStdEnumFmtEtc(1, &format, enumerator);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    std::atomic<ULONG> references_{1};
    std::vector<std::wstring> paths_;
};

class ShellDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *object = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
        if (escapePressed) {
            return DRAGDROP_S_CANCEL;
        }
        if ((keyState & MK_LBUTTON) == 0) {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    std::atomic<ULONG> references_{1};
};

HRESULT InitializeShellDragImage(
    IDataObject* dataObject,
    const SHDRAGIMAGE* dragImage) {
    if (dataObject == nullptr || dragImage == nullptr ||
        dragImage->hbmpDragImage == nullptr ||
        dragImage->sizeDragImage.cx <= 0 ||
        dragImage->sizeDragImage.cy <= 0) {
        g_lastDragImageInitializationResult.store(E_INVALIDARG);
        return E_INVALIDARG;
    }
    Microsoft::WRL::ComPtr<IDragSourceHelper> helper;
    HRESULT result = CoCreateInstance(
            CLSID_DragDropHelper,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(helper.GetAddressOf()));
    if (FAILED(result) || helper == nullptr) {
        result = FAILED(result) ? result : E_NOINTERFACE;
        g_lastDragImageInitializationResult.store(result);
        return result;
    }

    Microsoft::WRL::ComPtr<IDragSourceHelper2> helper2;
    helper.As(&helper2);
    IDragSourceHelper* activeHelper = helper.Get();
    if (helper2 != nullptr) {
        helper2->SetFlags(DSH_ALLOWDROPDESCRIPTIONTEXT);
        activeHelper = helper2.Get();
    }
    SHDRAGIMAGE image = *dragImage;
    result = activeHelper->InitializeFromBitmap(&image, dataObject);
    g_lastDragImageInitializationResult.store(result);
    return result;
}

bool StartShellDragWithDataObject(
    IDataObject* dataObject,
    const SHDRAGIMAGE* dragImage) {
    if (dataObject == nullptr) {
        return false;
    }
    InitializeShellDragImage(dataObject, dragImage);
    auto* dropSource = new (std::nothrow) ShellDropSource();
    if (dropSource == nullptr) {
        return false;
    }
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT result = DoDragDrop(
        dataObject,
        dropSource,
        DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK,
        &effect);
    dropSource->Release();
    return SUCCEEDED(result) && effect != DROPEFFECT_NONE;
}

bool AreFileSystemPaths(
    const std::vector<std::wstring>& paths) {
    return !paths.empty() &&
        std::all_of(
            paths.begin(), paths.end(),
            [](const std::wstring& path) {
                return !path.empty() &&
                    GetFileAttributesW(path.c_str()) !=
                        INVALID_FILE_ATTRIBUTES;
            });
}

HRESULT CreateNativeSingleDataObject(
    HWND ownerWindow,
    const std::wstring& path,
    IDataObject** dataObject) {
    *dataObject = nullptr;
    PIDLIST_ABSOLUTE absolute = nullptr;
    HRESULT result = SHParseDisplayName(
        path.c_str(), nullptr, &absolute, 0, nullptr);
    if (SUCCEEDED(result) && absolute != nullptr) {
        Microsoft::WRL::ComPtr<IShellFolder> parent;
        PCUITEMID_CHILD child = nullptr;
        result = SHBindToParent(
            absolute,
            IID_PPV_ARGS(parent.GetAddressOf()),
            &child);
        if (SUCCEEDED(result) &&
            parent != nullptr && child != nullptr) {
            result = parent->GetUIObjectOf(
                ownerWindow,
                1,
                &child,
                IID_IDataObject,
                nullptr,
                reinterpret_cast<void**>(dataObject));
        }
    }
    if (absolute != nullptr) {
        CoTaskMemFree(absolute);
    }
    return result;
}

std::vector<std::wstring> ReferencePaths(
    const std::vector<ShellItemReference>& items) {
    std::vector<std::wstring> paths;
    paths.reserve(items.size());
    for (const ShellItemReference& item : items) {
        paths.push_back(item.path);
    }
    return paths;
}

}  // namespace

HRESULT LastShellDragImageInitializationResultForTesting() {
    return g_lastDragImageInitializationResult.load();
}

HRESULT CreateFileDropDataObject(
    const std::vector<std::wstring>& paths,
    IDataObject** dataObject) {
    if (dataObject == nullptr) {
        return E_POINTER;
    }
    *dataObject = nullptr;
    if (!AreFileSystemPaths(paths)) {
        return E_INVALIDARG;
    }
    auto* created =
        new (std::nothrow) ShellDataObject(paths);
    if (created == nullptr) {
        return E_OUTOFMEMORY;
    }
    *dataObject = created;
    return S_OK;
}

HRESULT CreateShellDragDataObject(
    HWND ownerWindow,
    const std::vector<std::wstring>& paths,
    IDataObject** dataObject) {
    if (dataObject == nullptr) {
        return E_POINTER;
    }
    *dataObject = nullptr;
    if (paths.empty() ||
        std::any_of(
            paths.begin(), paths.end(),
            [](const std::wstring& path) {
                return path.empty();
            })) {
        return E_INVALIDARG;
    }
    HRESULT result = paths.size() == 1
        ? CreateNativeSingleDataObject(
              ownerWindow, paths.front(), dataObject)
        : E_INVALIDARG;
    if (SUCCEEDED(result) && *dataObject != nullptr) {
        return result;
    }
    if (*dataObject != nullptr) {
        (*dataObject)->Release();
        *dataObject = nullptr;
    }
    // A namespace item must never be silently omitted from a group.
    return AreFileSystemPaths(paths)
        ? CreateFileDropDataObject(paths, dataObject)
        : (FAILED(result) ? result : E_FAIL);
}

HRESULT CreateShellDragDataObject(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items,
    IDataObject** dataObject) {
    if (dataObject == nullptr) {
        return E_POINTER;
    }
    *dataObject = nullptr;
    if (items.empty() ||
        std::any_of(
            items.begin(), items.end(),
            [](const ShellItemReference& item) {
                return item.path.empty();
            })) {
        return E_INVALIDARG;
    }
    HRESULT result = CreateDesktopShellSelectionObject(
        ownerWindow,
        items,
        IID_IDataObject,
        reinterpret_cast<void**>(dataObject));
    if (SUCCEEDED(result) && *dataObject != nullptr) {
        return result;
    }
    if (*dataObject != nullptr) {
        (*dataObject)->Release();
        *dataObject = nullptr;
    }
    const std::vector<std::wstring> paths =
        ReferencePaths(items);
    // Only an all-filesystem group may fall back to CF_HDROP.
    return AreFileSystemPaths(paths)
        ? CreateFileDropDataObject(paths, dataObject)
        : (FAILED(result) ? result : E_FAIL);
}

bool StartShellDrag(
    HWND ownerWindow,
    const std::vector<std::wstring>& paths) {
    Microsoft::WRL::ComPtr<IDataObject> dataObject;
    if (FAILED(CreateShellDragDataObject(
            ownerWindow,
            paths,
            dataObject.GetAddressOf())) ||
        dataObject == nullptr) {
        return false;
    }
    return StartShellDragWithDataObject(dataObject.Get(), nullptr);
}

bool StartShellDrag(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items) {
    return StartShellDrag(ownerWindow, items, nullptr);
}

bool StartShellDrag(
    HWND ownerWindow,
    const std::vector<ShellItemReference>& items,
    const SHDRAGIMAGE* dragImage) {
    Microsoft::WRL::ComPtr<IDataObject> dataObject;
    if (FAILED(CreateShellDragDataObject(
            ownerWindow,
            items,
            dataObject.GetAddressOf())) ||
        dataObject == nullptr) {
        return false;
    }
    return StartShellDragWithDataObject(
        dataObject.Get(), dragImage);
}

bool StartShellDrag(HWND ownerWindow, const std::wstring& path) {
    return StartShellDrag(
        ownerWindow, std::vector<std::wstring>{path});
}

bool StartShellDrag(const std::vector<std::wstring>& paths) {
    return StartShellDrag(nullptr, paths);
}

bool StartShellDrag(const std::wstring& path) {
    return StartShellDrag(nullptr, path);
}
