#include "shell/ShellDropTarget.h"

#include <ShObjIdl.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <atomic>
#include <utility>

namespace {

std::vector<std::wstring> ExtractSupportedDropPaths(IDataObject* dataObject) {
    std::vector<std::wstring> paths;
    if (dataObject == nullptr) {
        return paths;
    }

    Microsoft::WRL::ComPtr<IShellItemArray> items;
    if (FAILED(SHCreateShellItemArrayFromDataObject(
            dataObject,
            IID_PPV_ARGS(items.GetAddressOf()))) || items == nullptr) {
        return paths;
    }

    DWORD count = 0;
    if (FAILED(items->GetCount(&count))) {
        return paths;
    }
    paths.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        if (FAILED(items->GetItemAt(index, item.GetAddressOf())) || item == nullptr) {
            continue;
        }
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
            paths.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return paths;
}

Microsoft::WRL::ComPtr<IUnknown> DataObjectIdentity(IDataObject* dataObject) {
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (dataObject != nullptr) {
        dataObject->QueryInterface(IID_PPV_ARGS(identity.GetAddressOf()));
    }
    return identity;
}

class ScopedWindowDpiAwareness {
public:
    explicit ScopedWindowDpiAwareness(HWND window) {
        const DPI_AWARENESS_CONTEXT windowContext =
            window == nullptr ? nullptr : GetWindowDpiAwarenessContext(window);
        if (windowContext != nullptr) {
            previous_ = SetThreadDpiAwarenessContext(windowContext);
        }
    }

    ~ScopedWindowDpiAwareness() {
        if (previous_ != nullptr) {
            SetThreadDpiAwarenessContext(previous_);
        }
    }

    ScopedWindowDpiAwareness(const ScopedWindowDpiAwareness&) = delete;
    ScopedWindowDpiAwareness& operator=(const ScopedWindowDpiAwareness&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

POINT CurrentPhysicalDragPoint(POINTL fallback) {
    POINT point{};
    if (GetPhysicalCursorPos(&point) != FALSE) {
        return point;
    }
    return POINT{fallback.x, fallback.y};
}

class ShellDropTarget final : public IDropTarget {
public:
    ShellDropTarget(
        HWND window,
        ShellDropHandler dropHandler,
        ShellDropEnabledHandler enabledHandler,
        ShellDropPreviewHandler previewHandler)
        : window_(window),
          dropHandler_(std::move(dropHandler)),
          enabledHandler_(std::move(enabledHandler)),
          previewHandler_(std::move(previewHandler)) {
        ScopedWindowDpiAwareness dpiAwareness(window_);
        CoCreateInstance(
            CLSID_DragDropHelper,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dropHelper_.GetAddressOf()));
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
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

    HRESULT STDMETHODCALLTYPE DragEnter(
        IDataObject* dataObject,
        DWORD,
        POINTL point,
        DWORD* effect) override {
        if (effect == nullptr) {
            return E_POINTER;
        }
        ScopedWindowDpiAwareness callbackDpiAwareness(window_);
        if (previewActive_) {
            NotifyPreview(ShellDropPreviewEvent::Leave, lastScreenPoint_);
        }
        ResetDragState();

        const bool canInspect =
            (*effect & DROPEFFECT_MOVE) != 0 &&
            (!enabledHandler_ || enabledHandler_());
        if (canInspect) {
            dragPaths_ = ExtractSupportedDropPaths(dataObject);
            dragDataIdentity_ = DataObjectIdentity(dataObject);
        }
        accepted_ = !dragPaths_.empty();
        *effect = accepted_ ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
        POINT screenPoint = CurrentPhysicalDragPoint(point);
        lastScreenPoint_ = screenPoint;
        if (dropHelper_ != nullptr) {
            ScopedWindowDpiAwareness dpiAwareness(window_);
            dropHelper_->DragEnter(window_, dataObject, &screenPoint, *effect);
        }
        if (accepted_) {
            previewActive_ = true;
            NotifyPreview(ShellDropPreviewEvent::Enter, screenPoint);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
        if (effect == nullptr) {
            return E_POINTER;
        }
        ScopedWindowDpiAwareness callbackDpiAwareness(window_);
        *effect = accepted_ ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
        POINT screenPoint = CurrentPhysicalDragPoint(point);
        lastScreenPoint_ = screenPoint;
        if (dropHelper_ != nullptr) {
            ScopedWindowDpiAwareness dpiAwareness(window_);
            dropHelper_->DragOver(&screenPoint, *effect);
        }
        if (previewActive_) {
            NotifyPreview(ShellDropPreviewEvent::Over, screenPoint);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        ScopedWindowDpiAwareness callbackDpiAwareness(window_);
        if (dropHelper_ != nullptr) {
            ScopedWindowDpiAwareness dpiAwareness(window_);
            dropHelper_->DragLeave();
        }
        if (previewActive_) {
            NotifyPreview(ShellDropPreviewEvent::Leave, lastScreenPoint_);
        }
        ResetDragState();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(
        IDataObject* dataObject,
        DWORD,
        POINTL point,
        DWORD* effect) override {
        if (effect == nullptr) {
            return E_POINTER;
        }
        ScopedWindowDpiAwareness callbackDpiAwareness(window_);
        POINT screenPoint = CurrentPhysicalDragPoint(point);
        lastScreenPoint_ = screenPoint;
        bool canDrop = accepted_ && (!enabledHandler_ || enabledHandler_());
        if (canDrop) {
            const Microsoft::WRL::ComPtr<IUnknown> dropIdentity =
                DataObjectIdentity(dataObject);
            if (dropIdentity == nullptr ||
                dragDataIdentity_ == nullptr ||
                dropIdentity.Get() != dragDataIdentity_.Get()) {
                dragPaths_ = ExtractSupportedDropPaths(dataObject);
                dragDataIdentity_ = dropIdentity;
                canDrop = !dragPaths_.empty();
            }
        }
        DWORD resultEffect = DROPEFFECT_NONE;
        bool handlerAccepted = false;
        if (canDrop && dataObject != nullptr && dropHandler_) {
            handlerAccepted = dropHandler_(dragPaths_, screenPoint);
        }
        if (dropHelper_ != nullptr) {
            ScopedWindowDpiAwareness dpiAwareness(window_);
            if (handlerAccepted) {
                dropHelper_->DragLeave();
                dropHelper_->Show(FALSE);
            } else {
                dropHelper_->Drop(dataObject, &screenPoint, DROPEFFECT_NONE);
            }
        }
        if (!handlerAccepted && previewActive_) {
            NotifyPreview(ShellDropPreviewEvent::Leave, screenPoint);
        }
        *effect = resultEffect;
        ResetDragState();
        return S_OK;
    }

private:
    void NotifyPreview(ShellDropPreviewEvent event, POINT screenPoint) {
        if (previewHandler_) {
            previewHandler_(event, dragPaths_, screenPoint);
        }
    }

    void ResetDragState() {
        accepted_ = false;
        previewActive_ = false;
        dragPaths_.clear();
        dragDataIdentity_.Reset();
    }

    std::atomic<ULONG> references_{1};
    HWND window_ = nullptr;
    ShellDropHandler dropHandler_;
    ShellDropEnabledHandler enabledHandler_;
    ShellDropPreviewHandler previewHandler_;
    Microsoft::WRL::ComPtr<IDropTargetHelper> dropHelper_;
    Microsoft::WRL::ComPtr<IUnknown> dragDataIdentity_;
    std::vector<std::wstring> dragPaths_;
    POINT lastScreenPoint_{};
    bool accepted_ = false;
    bool previewActive_ = false;
};

}  // namespace

bool RegisterShellDropTarget(
    HWND window,
    ShellDropHandler dropHandler,
    ShellDropEnabledHandler enabledHandler,
    ShellDropPreviewHandler previewHandler) {
    if (window == nullptr) {
        return false;
    }
    // RegisterDragDrop takes its own COM reference on success. Release the
    // creator-owned reference in both paths; RevokeDragDrop later releases the
    // registration reference and the object deletes itself at zero.
    auto* target = new ShellDropTarget(
        window,
        std::move(dropHandler),
        std::move(enabledHandler),
        std::move(previewHandler));
    const HRESULT result = RegisterDragDrop(window, target);
    target->Release();
    return SUCCEEDED(result);
}

void UnregisterShellDropTarget(HWND window) {
    if (window != nullptr) {
        RevokeDragDrop(window);
    }
}
