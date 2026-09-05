#include "shell/ShellDragDrop.h"

#include <Windows.h>
#include <shlobj.h>

#include <atomic>
#include <cstring>
#include <utility>

namespace {

class ShellDataObject final : public IDataObject {
public:
    explicit ShellDataObject(std::wstring path) : path_(std::move(path)) {}

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

        const size_t pathBytes = (path_.size() + 2) * sizeof(wchar_t);
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
        std::memcpy(destination, path_.c_str(), path_.size() * sizeof(wchar_t));
        destination[path_.size()] = L'\0';
        destination[path_.size() + 1] = L'\0';
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
    std::wstring path_;
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

}  // namespace

bool StartShellDrag(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    // Both COM objects start with one creator-owned reference. DoDragDrop may
    // hold temporary references; releasing ours after it returns completes the
    // self-deleting COM lifetime without transferring ownership to a smart pointer.
    auto* dataObject = new ShellDataObject(path);
    auto* dropSource = new ShellDropSource();
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT result = DoDragDrop(
        dataObject,
        dropSource,
        DROPEFFECT_COPY | DROPEFFECT_LINK,
        &effect);
    dataObject->Release();
    dropSource->Release();
    return SUCCEEDED(result) && effect != DROPEFFECT_NONE;
}
