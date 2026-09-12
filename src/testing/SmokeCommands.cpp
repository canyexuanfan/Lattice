#include <Windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <dwmapi.h>
#include <d2d1helper.h>
#include <process.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shlwapi.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <thread>
#include <vector>

#include "app/App.h"
#include "app/resource.h"
#include "app/StartupManager.h"
#include "app/UpdateService.h"
#include "config/ConfigStore.h"
#include "desktop/DesktopScanner.h"
#include "desktop/DesktopLayout.h"
#include "desktop/LegacyStorageMigrator.h"
#include "desktop/DesktopPlacementCoordinator.h"
#include "desktop/CategoryStorageManager.h"
#include "desktop/ManagedShortcutStore.h"
#include "desktop/DesktopSession.h"
#include "organize/AutoOrganizeLayout.h"
#include "organize/AutoOrganizer.h"
#include "organize/AutoOrganizeTransaction.h"
#include "rendering/IconCache.h"
#include "rendering/WallpaperBackdrop.h"
#include "shell/ShellDragDrop.h"
#include "shell/ShellDropTarget.h"
#include "shell/ShellItemReference.h"
#include "ui/InputDialog.h"
#include "ui/DragGhostWindow.h"
#include "ui/AutoOrganizePreviewWindow.h"
#include "ui/DesktopSurfaceWindow.h"
#include "ui/MessageDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/WidgetWindow.h"
#include "testing/SmokeCommands.h"

#pragma comment(lib, "dwmapi.lib")

namespace {

std::string Utf8Text(const std::wstring& value) {
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>((std::max)(required, 0)), char{});
    if (required > 0) {
        WideCharToMultiByte(
            CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
            result.data(), required, nullptr, nullptr);
    }
    return result;
}

HRESULT GetDesktopFolderViewForSmoke(
    Microsoft::WRL::ComPtr<IFolderView>& folderView) {
    folderView.Reset();
    Microsoft::WRL::ComPtr<IShellWindows> shellWindows;
    HRESULT result = CoCreateInstance(
        CLSID_ShellWindows,
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&shellWindows));
    VARIANT location{};
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    VARIANT root{};
    root.vt = VT_EMPTY;
    long desktopWindow = 0;
    Microsoft::WRL::ComPtr<IDispatch> dispatch;
    if (SUCCEEDED(result)) {
        result = shellWindows->FindWindowSW(
            &location,
            &root,
            SWC_DESKTOP,
            &desktopWindow,
            SWFO_NEEDDISPATCH,
            &dispatch);
    }
    Microsoft::WRL::ComPtr<IServiceProvider> serviceProvider;
    if (SUCCEEDED(result)) {
        result = dispatch.As(&serviceProvider);
    }
    Microsoft::WRL::ComPtr<IShellBrowser> shellBrowser;
    if (SUCCEEDED(result)) {
        result = serviceProvider->QueryService(
            SID_STopLevelBrowser,
            IID_PPV_ARGS(&shellBrowser));
    }
    Microsoft::WRL::ComPtr<IShellView> shellView;
    if (SUCCEEDED(result)) {
        result = shellBrowser->QueryActiveShellView(&shellView);
    }
    if (SUCCEEDED(result)) {
        result = shellView.As(&folderView);
    }
    return result;
}

bool CaptureExplorerSelectionForSmoke(
    std::vector<std::wstring>& identities) {
    identities.clear();
    Microsoft::WRL::ComPtr<IFolderView> folderView;
    HRESULT result = GetDesktopFolderViewForSmoke(folderView);
    Microsoft::WRL::ComPtr<IShellFolder> desktopFolder;
    if (SUCCEEDED(result)) {
        result = folderView->GetFolder(IID_PPV_ARGS(&desktopFolder));
    }
    Microsoft::WRL::ComPtr<IEnumIDList> selectedItems;
    if (SUCCEEDED(result)) {
        result = folderView->Items(
            SVGIO_SELECTION,
            IID_PPV_ARGS(&selectedItems));
    }
    while (SUCCEEDED(result) && selectedItems != nullptr) {
        PIDLIST_RELATIVE item = nullptr;
        ULONG fetched = 0;
        result = selectedItems->Next(1, &item, &fetched);
        if (result == S_FALSE || fetched == 0) {
            result = S_OK;
            break;
        }
        if (FAILED(result) || item == nullptr) {
            CoTaskMemFree(item);
            break;
        }
        STRRET parsingName{};
        result = desktopFolder->GetDisplayNameOf(
            item, SHGDN_FORPARSING, &parsingName);
        wchar_t buffer[32768]{};
        if (SUCCEEDED(result)) {
            result = StrRetToBufW(
                &parsingName, item, buffer, ARRAYSIZE(buffer));
        }
        CoTaskMemFree(item);
        if (FAILED(result)) {
            break;
        }
        identities.emplace_back(buffer);
    }
    if (FAILED(result)) {
        identities.clear();
        return false;
    }
    return true;
}

}  // namespace

struct AutoOrganizePreviewWindowSmokeAccess {
    static bool Ready(const AutoOrganizePreviewWindow& window) {
        return window.state_ == AutoOrganizePreviewWindow::ViewState::Ready;
    }

    static bool Scanning(const AutoOrganizePreviewWindow& window) {
        return window.state_ == AutoOrganizePreviewWindow::ViewState::Scanning;
    }

    static size_t HitCount(const AutoOrganizePreviewWindow& window) {
        return window.hitTargets_.size();
    }

    static size_t DecisionCount(const AutoOrganizePreviewWindow& window) {
        return window.plan_.decisions.size();
    }

    static size_t PlacementCount(const AutoOrganizePreviewWindow& window) {
        return window.layoutPlan_.placements.size();
    }

    static size_t SuggestionCount(const AutoOrganizePreviewWindow& window) {
        return static_cast<size_t>(std::count_if(
            window.plan_.decisions.begin(), window.plan_.decisions.end(),
            lattice::organize::IsOwnershipAdjustment));
    }

    static std::wstring SourceNameForItem(
        const AutoOrganizePreviewWindow& window,
        const std::wstring& itemId) {
        const auto found = std::find_if(
            window.plan_.decisions.begin(), window.plan_.decisions.end(),
            [&](const lattice::organize::Decision& decision) {
                return decision.itemId == itemId;
            });
        return found == window.plan_.decisions.end()
            ? std::wstring{} : found->sourceCategoryName;
    }

    static bool DecisionAppearsInAnyGroup(
        const AutoOrganizePreviewWindow& window,
        const std::wstring& itemId) {
        for (const auto& group : window.plan_.groups) {
            const auto decisions = window.DecisionsForGroup(group);
            if (std::any_of(
                    decisions.begin(), decisions.end(),
                    [&](const auto* decision) {
                        return decision->itemId == itemId;
                    })) {
                return true;
            }
        }
        return false;
    }

    static bool RegenerateScreenBounds(
        const AutoOrganizePreviewWindow& window,
        RECT& screenBounds) {
        const auto found = std::find_if(
            window.hitTargets_.begin(), window.hitTargets_.end(),
            [](const AutoOrganizePreviewWindow::HitTarget& hit) {
                return hit.kind == AutoOrganizePreviewWindow::HitKind::Regenerate;
            });
        if (found == window.hitTargets_.end()) return false;
        screenBounds = RECT{
            MulDiv(static_cast<int>(found->bounds.left),
                   static_cast<int>(window.dpi_), 96),
            MulDiv(static_cast<int>(found->bounds.top),
                   static_cast<int>(window.dpi_), 96),
            MulDiv(static_cast<int>(found->bounds.right),
                   static_cast<int>(window.dpi_), 96),
            MulDiv(static_cast<int>(found->bounds.bottom),
                   static_cast<int>(window.dpi_), 96)};
        POINT origin{0, 0};
        if (ClientToScreen(window.hwnd_, &origin) == FALSE) return false;
        OffsetRect(&screenBounds, origin.x, origin.y);
        return true;
    }

    static bool HoverRegenerate(AutoOrganizePreviewWindow& window) {
        const auto found = std::find_if(
            window.hitTargets_.begin(), window.hitTargets_.end(),
            [](const AutoOrganizePreviewWindow::HitTarget& hit) {
                return hit.kind == AutoOrganizePreviewWindow::HitKind::Regenerate;
            });
        if (found == window.hitTargets_.end()) return false;
        const POINT point{
            MulDiv(static_cast<int>((found->bounds.left + found->bounds.right) / 2),
                   static_cast<int>(window.dpi_), 96),
            MulDiv(static_cast<int>((found->bounds.top + found->bounds.bottom) / 2),
                   static_cast<int>(window.dpi_), 96)};
        window.UpdateHover(point);
        UpdateWindow(window.hwnd_);
        return window.IsHovered(AutoOrganizePreviewWindow::HitKind::Regenerate);
    }

    static void RenderNow(AutoOrganizePreviewWindow& window) {
        window.Render();
    }

    static void MarkDesktopChanged(AutoOrganizePreviewWindow& window) {
        window.MarkDesktopChanged();
    }

    static bool Changed(const AutoOrganizePreviewWindow& window) {
        return window.state_ == AutoOrganizePreviewWindow::ViewState::Changed;
    }

    static bool ChangeBlocksApply(const AutoOrganizePreviewWindow& window) {
        return window.desktopChangeBlocksApply_;
    }

    static bool HasFullCardDropTarget(
        const AutoOrganizePreviewWindow& window) {
        return std::any_of(
            window.hitTargets_.begin(), window.hitTargets_.end(),
            [](const AutoOrganizePreviewWindow::HitTarget& hit) {
                return hit.kind == AutoOrganizePreviewWindow::HitKind::Group &&
                    hit.bounds.bottom - hit.bounds.top > 200.0f;
            });
    }

    static bool MoveDecisionToNamedGroup(
        AutoOrganizePreviewWindow& window,
        int decisionIndex,
        const std::wstring& groupName) {
        const auto groups = window.VisibleGroups();
        for (int index = window.groupScrollOffset_;
             index < static_cast<int>(groups.size()); ++index) {
            if (groups[static_cast<std::size_t>(index)]->name != groupName) {
                continue;
            }
            window.MoveDecisionToGroup(
                decisionIndex, index - window.groupScrollOffset_);
            return true;
        }
        return false;
    }

    static void KeepOnDesktop(
        AutoOrganizePreviewWindow& window,
        int decisionIndex) {
        window.KeepDecisionOnDesktop(decisionIndex);
    }

    static std::wstring TargetCategory(
        const AutoOrganizePreviewWindow& window,
        int decisionIndex) {
        return decisionIndex >= 0 &&
            decisionIndex < static_cast<int>(window.plan_.decisions.size())
            ? window.plan_.decisions[static_cast<std::size_t>(decisionIndex)]
                  .targetCategoryName
            : std::wstring{};
    }

    static void ScrollGroups(AutoOrganizePreviewWindow& window) {
        window.ScrollPreview(POINT{700, 700}, -WHEEL_DELTA);
    }

    static int GroupScrollOffset(const AutoOrganizePreviewWindow& window) {
        return window.groupScrollOffset_;
    }

    static void StartScan(AutoOrganizePreviewWindow& window) {
        window.StartScan();
    }

    static bool ActivateApply(AutoOrganizePreviewWindow& window) {
        const auto found = std::find_if(
            window.hitTargets_.begin(), window.hitTargets_.end(),
            [](const AutoOrganizePreviewWindow::HitTarget& hit) {
                return hit.kind == AutoOrganizePreviewWindow::HitKind::Apply;
            });
        if (found == window.hitTargets_.end()) return false;
        window.ActivateHit(*found);
        return window.state_ == AutoOrganizePreviewWindow::ViewState::Applying;
    }

    static void ActivateOverlayAction(AutoOrganizePreviewWindow& window) {
        AutoOrganizePreviewWindow::HitTarget hit;
        hit.kind = AutoOrganizePreviewWindow::HitKind::OverlayAction;
        window.ActivateHit(hit);
    }

    static bool Undoing(const AutoOrganizePreviewWindow& window) {
        return window.state_ == AutoOrganizePreviewWindow::ViewState::Undoing;
    }

    static bool UndoSucceeded(const AutoOrganizePreviewWindow& window) {
        return window.state_ == AutoOrganizePreviewWindow::ViewState::UndoSuccess;
    }
};

struct WidgetWindowSmokeAccess {
    static void WriteRenderState(WidgetWindow& widget, std::wostream& output) {
        output << L"renderCount=" << widget.renderCount_
               << L" lastRenderResult=" << widget.lastRenderResult_ << L"\n";
        auto* target = widget.d2d_.Target();
        output << L"renderTarget=" << target << L" factory=" << widget.d2d_.Factory() << L"\n";
        if (target != nullptr) {
            const auto pixels = target->GetPixelSize();
            const auto size = target->GetSize();
            output << L"targetPixels=" << pixels.width << L"," << pixels.height
                   << L" targetDips=" << size.width << L"," << size.height
                   << L" windowState=" << target->CheckWindowState() << L"\n";
        }
    }

    static bool PrepareWallpaper(WidgetWindow& widget) {
        widget.RefreshWallpaperBackdrop();
        RECT client{};
        return GetClientRect(widget.hwnd_, &client) &&
            widget.wallpaperBackdrop_.CoversPixels(
                client.right, client.bottom);
    }

    static constexpr WallpaperBackdropDrawMode
    WallpaperDrawMode() {
        return WidgetWindow::kWallpaperDrawMode;
    }

    static bool RequestApplicationExit(WidgetWindow& widget) {
        return widget.RequestApplicationExit();
    }

    static bool DropPaths(
        WidgetWindow& widget,
        const std::vector<std::wstring>& paths,
        POINT screenPoint) {
        return widget.AddDroppedPaths(paths, &screenPoint, false);
    }

    static bool QueuePaths(
        WidgetWindow& widget,
        const std::vector<std::wstring>& paths,
        POINT screenPoint,
        bool showError = false) {
        return widget.QueueDroppedPaths(paths, screenPoint, showError);
    }

    static bool QueuePathsWithDesktopPositions(
        WidgetWindow& widget,
        const std::vector<std::wstring>& paths,
        POINT screenPoint,
        const std::vector<std::pair<std::wstring, POINT>>& suppliedPositions,
        bool showError = false) {
        std::vector<WidgetWindow::DesktopDropPosition> desktopPositions;
        desktopPositions.reserve(suppliedPositions.size());
        for (const auto& supplied : suppliedPositions) {
            desktopPositions.push_back(
                WidgetWindow::DesktopDropPosition{supplied.first, supplied.second});
        }
        return widget.QueueDroppedPaths(
            paths,
            screenPoint,
            showError,
            &desktopPositions);
    }

    static bool DropQueued(const WidgetWindow& widget) {
        return widget.shellDropQueued_;
    }

    static void MoveItemToCategory(
        WidgetWindow& widget,
        const std::wstring& itemId,
        const std::wstring& categoryId) {
        widget.MoveItemToCategory(itemId, categoryId);
    }

    static bool MoveItemOut(WidgetWindow& widget, const std::wstring& itemId) {
        return widget.MoveItemOut(itemId, false);
    }

    static bool DropBusy(const WidgetWindow& widget) {
        return widget.IsShellDropBusy();
    }

    static bool DropProjectionActive(const WidgetWindow& widget) {
        return widget.shellDropProjectionActive_;
    }

    static const std::vector<std::wstring>& PendingPaths(
        const WidgetWindow& widget) {
        return widget.pendingShellDropPaths_;
    }

    static int PendingIndex(const WidgetWindow& widget) {
        return widget.pendingShellDropInsertionIndex_;
    }

    static bool PendingShowError(const WidgetWindow& widget) {
        return widget.pendingShellDropShowError_;
    }

    static bool DispatchQueuedDrop(WidgetWindow& widget) {
        MSG message{};
        if (PeekMessageW(
                &message,
                widget.hwnd_,
                kWidgetShellDropCommitMessage,
                kWidgetShellDropCommitMessage,
                PM_REMOVE) == FALSE) {
            return false;
        }
        DispatchMessageW(&message);
        return true;
    }

    static void PreviewPaths(
        WidgetWindow& widget,
        const std::vector<std::wstring>& paths,
        POINT screenPoint,
        bool preloadIcons = true) {
        widget.UpdateShellDropPreview(
            paths,
            screenPoint,
            preloadIcons);
    }

    static std::uint64_t LoadItemsGeneration(const WidgetWindow& widget) {
        return widget.loadItemsGeneration_;
    }

    static HWND Window(const WidgetWindow& widget) {
        return widget.hwnd_;
    }

    static HWND DesktopHost(const WidgetWindow& widget) {
        return widget.desktopHost_;
    }

    static bool IsDesktopHosted(const WidgetWindow& widget) {
        return widget.IsDesktopHosted();
    }

    static bool SetScreenBounds(
        WidgetWindow& widget,
        int x,
        int y,
        int width,
        int height,
        UINT flags) {
        return widget.SetWindowBoundsFromScreen(x, y, width, height, flags);
    }

    static void ToggleCollapsed(WidgetWindow& widget) {
        widget.ToggleCollapsed();
    }

    static bool FlushInteractionSave(WidgetWindow& widget) {
        return widget.FlushPendingInteractionSave();
    }

    static bool InteractionSavePending(const WidgetWindow& widget) {
        return widget.interactionSavePending_;
    }

    static void ReorderSelectedItems(
        WidgetWindow& widget,
        size_t insertionIndex) {
        widget.ReorderSelectedItems(insertionIndex);
    }

    static POINT InsertionScreenPoint(WidgetWindow& widget, size_t index) {
        const RECT cell = widget.iconGrid_.InsertionCellAt(index);
        POINT point{
            widget.DipToPixels((cell.left + cell.right) / 2),
            widget.DipToPixels((cell.top + cell.bottom) / 2),
        };
        ClientToScreen(widget.hwnd_, &point);
        return point;
    }

    static std::vector<std::wstring> CurrentItemIds(const WidgetWindow& widget) {
        std::vector<std::wstring> result;
        result.reserve(widget.currentItems_.size());
        for (const DesktopItem& item : widget.currentItems_) {
            result.push_back(item.id);
        }
        return result;
    }

    static std::vector<std::wstring> SelectedItemIds(
        const WidgetWindow& widget) {
        return widget.SelectedItemIdsInVisibleOrder();
    }

    static int DragInsertionIndex(const WidgetWindow& widget) {
        return widget.dragInsertionIndex_;
    }

    static int ReorderInsertionIndexForPoint(
        const WidgetWindow& widget,
        POINT point) {
        return widget.iconGrid_.ReorderInsertionIndexForPoint(point);
    }

    static int Theme(const WidgetWindow& widget) {
        return widget.theme_;
    }

    static bool SingleClickOpen(const WidgetWindow& widget) {
        return widget.singleClickOpen_;
    }

    static bool UsesLightTheme(const WidgetWindow& widget) {
        return widget.iconGrid_.UsesLightTheme();
    }

    static size_t IconCacheCapacity(const WidgetWindow& widget) {
        return widget.iconCache_.Capacity();
    }

    static void ForceGridThemeForTest(
        WidgetWindow& widget,
        bool lightTheme) {
        widget.iconGrid_.SetLightTheme(lightTheme);
    }

    static void SelectOnly(WidgetWindow& widget, int index) {
        widget.SelectOnly(index);
    }

    static void ToggleSelection(WidgetWindow& widget, int index) {
        widget.ToggleSelection(index);
    }

    static void SelectAll(WidgetWindow& widget) {
        widget.SelectAllItems();
    }

    static void MoveKeyboardSelection(
        WidgetWindow& widget,
        UINT virtualKey) {
        widget.MoveKeyboardSelection(virtualKey);
    }

    static RECT GridBounds(const WidgetWindow& widget) {
        return widget.GridBounds();
    }

    static RECT CellAt(const WidgetWindow& widget, size_t index) {
        return widget.iconGrid_.CellAt(index);
    }

    static void MarqueeSelect(
        WidgetWindow& widget,
        POINT start,
        POINT end,
        bool controlPressed) {
        widget.BeginMarqueeSelection(start, controlPressed);
        widget.UpdateMarqueeSelection(end);
        widget.CompletePointerSelection(false);
    }

    static std::uint64_t GridItemsGeneration(const WidgetWindow& widget) {
        return widget.iconGrid_.ItemsGeneration();
    }

    static size_t LastFallbackDrawCount(const WidgetWindow& widget) {
        return widget.iconGrid_.LastFallbackDrawCount();
    }

    static size_t LastPlaceholderDrawCount(const WidgetWindow& widget) {
        return widget.iconGrid_.LastPlaceholderDrawCount();
    }

    static bool IsIconReady(WidgetWindow& widget, const std::wstring& path) {
        return widget.iconCache_.IsIconReady(widget.d2d_.Target(), path);
    }

    static void ClearIconCache(WidgetWindow& widget) {
        widget.iconCache_.Clear();
    }

    static void PreloadIcon(WidgetWindow& widget, const std::wstring& path) {
        widget.iconCache_.Preload(path);
    }

    static std::wstring LoadedItemIdForPath(
        const WidgetWindow& widget,
        const std::wstring& path) {
        const auto found = std::find_if(
            widget.items_.begin(),
            widget.items_.end(),
            [&](const DesktopItem& item) {
                return CompareStringOrdinal(
                           item.path.c_str(), -1, path.c_str(), -1, TRUE) ==
                       CSTR_EQUAL;
            });
        return found == widget.items_.end() ? L"" : found->id;
    }

    static void AddLoadedItem(
        WidgetWindow& widget,
        const DesktopItem& item) {
        widget.items_.push_back(item);
    }

    static void AliasIcon(
        WidgetWindow& widget,
        const std::wstring& sourcePath,
        const std::wstring& destinationPath) {
        widget.iconCache_.Alias(sourcePath, destinationPath);
    }
};

struct DesktopSurfaceWindowSmokeAccess {
    static constexpr WallpaperBackdropDrawMode
    WallpaperDrawMode() {
        return DesktopSurfaceWindow::kWallpaperDrawMode;
    }

    static IDropTarget* DropTarget(DesktopSurfaceWindow& surface) {
        return surface.desktopDropTarget_.Get();
    }

    static int CellWidth(const DesktopSurfaceWindow& surface) {
        return surface.cellWidth_;
    }

    static int CellHeight(const DesktopSurfaceWindow& surface) {
        return surface.cellHeight_;
    }

    static int IconSize(const DesktopSurfaceWindow& surface) {
        return surface.iconSize_;
    }

    static bool SelectFirstRenameable(
        DesktopSurfaceWindow& surface,
        std::wstring& identity) {
        identity.clear();
        for (const DesktopViewItem& item : surface.visibleItems_) {
            bool canRename = false;
            if (SUCCEEDED(CanRenameDesktopShellItem(
                    ShellItemReference{
                        item.path, item.shellChildPidl},
                    canRename)) && canRename) {
                surface.SelectOnly(item.path);
                identity = item.path;
                return true;
            }
        }
        return false;
    }

    static bool SelectRenameableByIdentity(
        DesktopSurfaceWindow& surface,
        const std::wstring& requestedIdentity) {
        const auto item = std::find_if(
            surface.visibleItems_.begin(),
            surface.visibleItems_.end(),
            [&](const DesktopViewItem& value) {
                return CompareStringOrdinal(
                           value.path.c_str(), -1,
                           requestedIdentity.c_str(), -1,
                           TRUE) == CSTR_EQUAL;
            });
        if (item == surface.visibleItems_.end()) {
            return false;
        }
        bool canRename = false;
        if (FAILED(CanRenameDesktopShellItem(
                ShellItemReference{
                    item->path, item->shellChildPidl},
                canRename)) || !canRename) {
            return false;
        }
        surface.SelectOnly(item->path);
        return true;
    }

    static HWND RenameEditor(
        const DesktopSurfaceWindow& surface) {
        return surface.renameEdit_;
    }

    static std::wstring RenameIdentity(
        const DesktopSurfaceWindow& surface) {
        return surface.renameIdentity_;
    }

    static ShellItemReference ReferenceForIdentity(
        const DesktopSurfaceWindow& surface,
        const std::wstring& identity) {
        const auto item = std::find_if(
            surface.visibleItems_.begin(),
            surface.visibleItems_.end(),
            [&](const DesktopViewItem& value) {
                return CompareStringOrdinal(
                           value.path.c_str(), -1,
                           identity.c_str(), -1,
                           TRUE) == CSTR_EQUAL;
            });
        return item == surface.visibleItems_.end()
            ? ShellItemReference{}
            : ShellItemReference{
                  item->path, item->shellChildPidl};
    }

    static bool ShouldRouteDesktopF2(
        const DesktopSurfaceWindow& surface) {
        return surface.ShouldRouteDesktopF2();
    }

    static int ExplorerSelectionSyncStage(
        const DesktopSurfaceWindow& surface) {
        return surface.lastExplorerSelectionSyncStage_;
    }

    static HRESULT ExplorerSelectionSyncResult(
        const DesktopSurfaceWindow& surface) {
        return surface.lastExplorerSelectionSyncResult_;
    }

    static POINT LabelCenterForIdentity(
        const DesktopSurfaceWindow& surface,
        const std::wstring& identity) {
        const auto item = std::find_if(
            surface.visibleItems_.begin(),
            surface.visibleItems_.end(),
            [&](const DesktopViewItem& value) {
                return CompareStringOrdinal(
                           value.path.c_str(), -1,
                           identity.c_str(), -1,
                           TRUE) == CSTR_EQUAL;
            });
        if (item == surface.visibleItems_.end()) {
            return POINT{-1, -1};
        }
        const RECT label = surface.LabelRect(*item);
        return POINT{
            (label.left + label.right) / 2,
            (label.top + label.bottom) / 2};
    }

    static POINT RenameLabelHitPointForIdentity(
        const DesktopSurfaceWindow& surface,
        const std::wstring& identity) {
        const auto item = std::find_if(
            surface.visibleItems_.begin(),
            surface.visibleItems_.end(),
            [&](const DesktopViewItem& value) {
                return CompareStringOrdinal(
                           value.path.c_str(), -1,
                           identity.c_str(), -1,
                           TRUE) == CSTR_EQUAL;
            });
        if (item == surface.visibleItems_.end()) {
            return POINT{-1, -1};
        }
        const int expectedIndex = static_cast<int>(
            std::distance(surface.visibleItems_.begin(), item));
        const RECT label = surface.LabelRect(*item);
        const RECT interaction = surface.InteractionRect(
            static_cast<size_t>(expectedIndex));
        RECT stableRegion{};
        if (IntersectRect(&stableRegion, &label, &interaction) != FALSE) {
            const POINT candidate{
                (stableRegion.left + stableRegion.right) / 2,
                (stableRegion.top + stableRegion.bottom) / 2};
            if (surface.HitTest(candidate) == expectedIndex) {
                return candidate;
            }
        }
        return POINT{-1, -1};
    }

    static bool UsesPhysicalPixelGeometry(
        DesktopSurfaceWindow& surface) {
        UINT viewDpi = 96;
        if (surface.snapshot_.listViewWindow != nullptr) {
            const UINT reportedDpi = GetDpiForWindow(
                surface.snapshot_.listViewWindow);
            if (reportedDpi != 0) {
                viewDpi = reportedDpi;
            }
        }
        const int expectedIconSize = std::clamp(
            MulDiv(surface.snapshot_.viewIconSize,
                   static_cast<int>(viewDpi), 96),
            16, 512);
        if (surface.d2d_.Target() == nullptr ||
            surface.iconSize_ != expectedIconSize) {
            return false;
        }
        FLOAT dpiX = 0.0f;
        FLOAT dpiY = 0.0f;
        surface.d2d_.Target()->GetDpi(&dpiX, &dpiY);
        const bool anchorsMatch = std::all_of(
            surface.snapshot_.items.begin(),
            surface.snapshot_.items.end(),
            [&](const DesktopViewItem& item) {
                const RECT cell = surface.CellRect(item);
                const int iconLeft = cell.left +
                    (surface.cellWidth_ - surface.iconSize_) / 2;
                return iconLeft == item.screenPoint.x -
                        surface.snapshot_.screenRect.left &&
                    cell.top == item.screenPoint.y -
                        surface.snapshot_.screenRect.top;
            });
        return anchorsMatch &&
            std::abs(dpiX - 96.0f) < 0.01f &&
            std::abs(dpiY - 96.0f) < 0.01f;
    }

    static bool HasShellImageIdentity(
        const DesktopSurfaceWindow& surface) {
        return std::all_of(
            surface.snapshot_.items.begin(),
            surface.snapshot_.items.end(),
            [](const DesktopViewItem& item) {
                return item.systemImageIndex >= 0;
            });
    }

    static size_t MissingShellImageIdentityCount(
        const DesktopSurfaceWindow& surface) {
        return static_cast<size_t>(std::count_if(
            surface.snapshot_.items.begin(),
            surface.snapshot_.items.end(),
            [](const DesktopViewItem& item) {
                return item.systemImageIndex < 0;
            }));
    }

    static bool AllVisibleIconsReady(
        DesktopSurfaceWindow& surface) {
        if (surface.d2d_.Target() == nullptr) {
            return false;
        }
        return std::all_of(
            surface.visibleItems_.begin(),
            surface.visibleItems_.end(),
            [&](const DesktopViewItem& item) {
                return surface.iconCache_.IsIconReady(
                    surface.d2d_.Target(), item.path);
            });
    }

    static std::vector<std::wstring> UnreadyVisibleIconPaths(
        DesktopSurfaceWindow& surface) {
        std::vector<std::wstring> paths;
        if (surface.d2d_.Target() == nullptr) {
            return paths;
        }
        for (const DesktopViewItem& item : surface.visibleItems_) {
            if (!surface.iconCache_.IsIconReady(
                    surface.d2d_.Target(), item.path)) {
                paths.push_back(item.path);
            }
        }
        return paths;
    }

    static size_t ReloadWithRotatedExplorerImageIndices(
        DesktopSurfaceWindow& surface) {
        if (surface.visibleItems_.size() < 2) {
            return 0;
        }
        std::vector<int> indices;
        indices.reserve(surface.visibleItems_.size());
        for (const DesktopViewItem& item : surface.visibleItems_) {
            indices.push_back(item.systemImageIndex);
        }
        size_t changed = 0;
        for (size_t index = 0;
             index < surface.visibleItems_.size(); ++index) {
            int rotated = indices[(index + 1) % indices.size()];
            if (rotated == surface.visibleItems_[index].systemImageIndex) {
                const auto different = std::find_if(
                    indices.begin(),
                    indices.end(),
                    [&](int candidate) {
                        return candidate >= 0 &&
                            candidate !=
                                surface.visibleItems_[index].systemImageIndex;
                    });
                if (different != indices.end()) {
                    rotated = *different;
                }
            }
            if (rotated >= 0 &&
                rotated != surface.visibleItems_[index].systemImageIndex) {
                surface.visibleItems_[index].systemImageIndex = rotated;
                ++changed;
            }
        }
        if (changed == 0) {
            return 0;
        }
        surface.iconCache_.Clear();
        for (const DesktopViewItem& item : surface.visibleItems_) {
            surface.iconCache_.PreloadShellIcon(
                item.path,
                item.systemImageIndex,
                item.overlayIndex,
                surface.iconSize_,
                &item.shellChildPidl,
                surface.snapshot_.viewIconSize);
        }
        return changed;
    }

    static RECT NormalizeMarqueeRect(
        POINT anchor,
        POINT current,
        const RECT& bounds) {
        return DesktopSurfaceWindow::NormalizeMarqueeRect(
            anchor, current, bounds);
    }

    static void ConfigureSelectionFixture(
        DesktopSurfaceWindow& surface,
        const std::vector<DesktopViewItem>& items,
        const std::vector<std::wstring>& selected) {
        surface.snapshot_.screenRect = RECT{0, 0, 300, 220};
        surface.cellWidth_ = 80;
        surface.cellHeight_ = 50;
        surface.iconSize_ = 40;
        surface.visibleItems_ = items;
        surface.selectedIdentities_.clear();
        surface.selectedIdentities_.insert(
            selected.begin(), selected.end());
        surface.selectionBaseline_.clear();
        surface.ResetPointerGesture();
        surface.RebuildInteractionRects();
    }

    static void BeginPointerGesture(
        DesktopSurfaceWindow& surface,
        POINT point,
        bool controlPressed) {
        surface.BeginPointerGesture(point, controlPressed);
    }

    static std::vector<std::wstring> ContinuePointerGesture(
        DesktopSurfaceWindow& surface,
        POINT point) {
        return surface.ContinuePointerGesture(point);
    }

    static void CompletePointerGesture(
        DesktopSurfaceWindow& surface) {
        surface.CompletePointerGesture();
    }

    static void CancelPointerGesture(
        DesktopSurfaceWindow& surface) {
        surface.CancelPointerCapture();
    }

    static bool IsSelected(
        const DesktopSurfaceWindow& surface,
        const std::wstring& identity) {
        return surface.IsSelected(identity);
    }

    static bool IsMarqueeActive(
        const DesktopSurfaceWindow& surface) {
        return surface.pointerGesture_ ==
                DesktopSurfaceWindow::PointerGesture::MarqueeActive &&
            !IsRectEmpty(&surface.marqueeRect_);
    }

    static bool IsMarqueePending(
        const DesktopSurfaceWindow& surface) {
        return surface.pointerGesture_ ==
            DesktopSurfaceWindow::PointerGesture::MarqueePending;
    }

    static bool IsItemPressed(
        const DesktopSurfaceWindow& surface) {
        return surface.pointerGesture_ ==
            DesktopSurfaceWindow::PointerGesture::ItemPressed;
    }

    static bool HasNoPointerGesture(
        const DesktopSurfaceWindow& surface) {
        return surface.pointerGesture_ ==
                DesktopSurfaceWindow::PointerGesture::None &&
            IsRectEmpty(&surface.marqueeRect_);
    }

    static std::vector<std::wstring> SelectedPaths(
        const DesktopSurfaceWindow& surface) {
        return surface.SelectedPathsInVisibleOrder();
    }

    static std::vector<ShellItemReference> SelectedShellItems(
        const DesktopSurfaceWindow& surface) {
        return surface.SelectedShellItemsInVisibleOrder();
    }

    static std::vector<ShellItemReference>
    VisibleShellItems(
        const DesktopSurfaceWindow& surface,
        size_t maximumCount) {
        std::vector<ShellItemReference> result;
        const size_t count = (std::min)(
            maximumCount, surface.visibleItems_.size());
        result.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const DesktopViewItem& item =
                surface.visibleItems_[index];
            result.push_back(ShellItemReference{
                item.path, item.shellChildPidl});
        }
        return result;
    }

    static bool IsBlankPoint(const DesktopSurfaceWindow& surface, POINT point) {
        return surface.HitTest(point) < 0;
    }

    static RECT CellRect(
        const DesktopSurfaceWindow& surface,
        size_t visibleIndex) {
        return visibleIndex < surface.visibleItems_.size()
            ? surface.CellRect(surface.visibleItems_[visibleIndex])
            : RECT{};
    }

    static RECT InteractionRect(
        const DesktopSurfaceWindow& surface,
        size_t visibleIndex) {
        return surface.InteractionRect(visibleIndex);
    }

    static std::optional<RECT> InteractionRectForIdentity(
        const DesktopSurfaceWindow& surface,
        const std::wstring& identity) {
        for (size_t index = 0;
             index < surface.visibleItems_.size(); ++index) {
            if (CompareStringOrdinal(
                    surface.visibleItems_[index].path.c_str(), -1,
                    identity.c_str(), -1, TRUE) == CSTR_EQUAL) {
                return surface.InteractionRect(index);
            }
        }
        return std::nullopt;
    }

    static std::optional<POINT> VisibleScreenPointForIdentity(
        const DesktopSurfaceWindow& surface,
        const std::wstring& identity) {
        const auto item = std::find_if(
            surface.visibleItems_.begin(),
            surface.visibleItems_.end(),
            [&](const DesktopViewItem& value) {
                return CompareStringOrdinal(
                           value.path.c_str(), -1,
                           identity.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
        return item == surface.visibleItems_.end()
            ? std::nullopt
            : std::optional<POINT>(item->screenPoint);
    }

    static RECT ClientBounds(const DesktopSurfaceWindow& surface) {
        return surface.ClientBounds();
    }

    static bool HasNativeListViewQueryAccess(
        const DesktopSurfaceWindow& surface) {
        return surface.listViewQueryReady_;
    }

    static size_t ShellDragStartCount(
        const DesktopSurfaceWindow& surface) {
        return surface.shellDragStartCount_;
    }

    static int InternalDropStageValue(
        const DesktopSurfaceWindow& surface) {
#ifndef NDEBUG
        return static_cast<int>(surface.internalDropStage_);
#else
        (void)surface;
        return -1;
#endif
    }

    static std::vector<DesktopPosition> VisiblePositions(
        const DesktopSurfaceWindow& surface) {
        std::vector<DesktopPosition> result;
        result.reserve(surface.visibleItems_.size());
        for (const DesktopViewItem& item : surface.visibleItems_) {
            result.push_back(DesktopPosition{item.path, item.screenPoint});
        }
        return result;
    }

    static std::optional<POINT> FindExposedHitPointForIdentity(
        const DesktopSurfaceWindow& surface,
        const std::wstring& identity) {
        for (size_t index = 0; index < surface.visibleItems_.size(); ++index) {
            if (CompareStringOrdinal(
                    surface.visibleItems_[index].path.c_str(), -1,
                    identity.c_str(), -1, TRUE) != CSTR_EQUAL) {
                continue;
            }
            const RECT rect = surface.InteractionRect(index);
            for (LONG y = rect.top; y < rect.bottom; y += 2) {
                for (LONG x = rect.left; x < rect.right; x += 2) {
                    const POINT client{x, y};
                    POINT screen = client;
                    if (surface.hwnd_ == nullptr ||
                        ClientToScreen(surface.hwnd_, &screen) == FALSE ||
                        WindowFromPoint(screen) != surface.hwnd_) {
                        continue;
                    }
                    if (surface.HitTest(client) ==
                        static_cast<int>(index)) {
                        return screen;
                    }
                }
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    static bool BuildShellDragImage(
        DesktopSurfaceWindow& surface,
        POINT sourceClientPoint,
        SHDRAGIMAGE& dragImage) {
        return surface.BuildShellDragImage(
            sourceClientPoint, dragImage);
    }

    static void SuppressShellDragForSmoke(
        DesktopSurfaceWindow& surface,
        bool suppress) {
        surface.suppressShellDragForSmoke_ = suppress;
    }

    static std::vector<DesktopPosition> OffsetDragPositions(
        const std::vector<DesktopPosition>& positions,
        POINT source,
        POINT drop) {
        return DesktopSurfaceWindow::OffsetDragPositions(
            positions, source, drop);
    }

    static bool PlanVisibleGridDrop(
        const std::vector<DesktopPosition>& visiblePositions,
        const std::vector<DesktopPosition>& selectedPositions,
        POINT source,
        POINT drop,
        const RECT& screenRect,
        int cellWidth,
        int cellHeight,
        int iconSize,
        std::vector<DesktopPosition>& plannedPositions) {
        return DesktopSurfaceWindow::PlanVisibleGridDrop(
            visiblePositions,
            selectedPositions,
            source,
            drop,
            screenRect,
            cellWidth,
            cellHeight,
            iconSize,
            plannedPositions);
    }

    static std::optional<std::pair<POINT, POINT>>
    FindNativeMarqueeGesture(
        const DesktopSurfaceWindow& surface) {
        const RECT bounds = surface.ClientBounds();
        constexpr size_t kMaxNativeProbes = 64;
        size_t nativeProbes = 0;
        const std::array<POINT, 4> endCandidates{{
            {bounds.left + 8, bounds.top + 8},
            {bounds.right - 8, bounds.top + 8},
            {bounds.left + 8, bounds.bottom - 8},
            {bounds.right - 8, bounds.bottom - 8},
        }};
        for (size_t offset = 0;
             offset < surface.visibleItems_.size(); ++offset) {
            const size_t index =
                surface.visibleItems_.size() - 1 - offset;
            const RECT cell = surface.CellRect(
                surface.visibleItems_[index]);
            const LONG top = (std::max)(cell.top + 1, bounds.top);
            const LONG bottom = (std::min)(
                cell.bottom - 1,
                bounds.bottom - 1);
            const LONG left = (std::max)(cell.left + 1, bounds.left);
            const LONG right = (std::min)(cell.right - 1, bounds.right - 1);
            const LONG stepX = (std::max)(4L, (right - left) / 8);
            const LONG stepY = (std::max)(4L, (bottom - top) / 8);
            for (LONG y = top; y <= bottom; y += stepY) {
                for (LONG x = left; x <= right; x += stepX) {
                    const POINT candidate{x, y};
                    const bool outsideEveryInteraction = std::none_of(
                        surface.visibleItems_.begin(),
                        surface.visibleItems_.end(),
                        [&](const DesktopViewItem& item) {
                            const auto itemIndex = static_cast<size_t>(
                                &item - surface.visibleItems_.data());
                            const RECT interaction =
                                surface.InteractionRect(itemIndex);
                            return PtInRect(&interaction, candidate) != FALSE;
                        });
                    if (!outsideEveryInteraction) {
                        continue;
                    }
                    POINT screenPoint = candidate;
                    if (surface.hwnd_ == nullptr ||
                        ClientToScreen(surface.hwnd_, &screenPoint) == FALSE ||
                        WindowFromPoint(screenPoint) != surface.hwnd_) {
                        continue;
                    }
                    if (nativeProbes >= kMaxNativeProbes) {
                        return std::nullopt;
                    }
                    ++nativeProbes;
                    int hitIndex = -1;
                    if (surface.TryNativeHitTest(
                            candidate, hitIndex) &&
                        hitIndex < 0) {
                        for (const POINT end : endCandidates) {
                            const RECT marquee =
                                DesktopSurfaceWindow::NormalizeMarqueeRect(
                                    candidate, end, bounds);
                            size_t intersectedItems = 0;
                            for (size_t itemIndex = 0;
                                 itemIndex < surface.visibleItems_.size();
                                 ++itemIndex) {
                                const RECT interaction =
                                    surface.InteractionRect(itemIndex);
                                RECT intersection{};
                                if (IntersectRect(
                                        &intersection,
                                        &interaction,
                                        &marquee) != FALSE) {
                                    ++intersectedItems;
                                }
                            }
                            POINT endScreen = end;
                            if (intersectedItems >= 2 &&
                                ClientToScreen(
                                    surface.hwnd_, &endScreen) != FALSE &&
                                WindowFromPoint(endScreen) ==
                                    surface.hwnd_) {
                                return std::pair{candidate, end};
                            }
                        }
                    }
                }
            }
        }
        return std::nullopt;
    }

    static std::optional<POINT> FindBlankPoint(
        const DesktopSurfaceWindow& surface) {
        const RECT bounds = surface.ClientBounds();
        for (LONG y = bounds.bottom - 8;
             y >= bounds.top;
             y -= 8) {
            for (LONG x = bounds.right - 8;
                 x >= bounds.left;
                 x -= 8) {
                const POINT point{x, y};
                if (surface.HitTest(point) < 0) {
                    return point;
                }
            }
        }
        return std::nullopt;
    }

    static void ReconcileSelection(
        DesktopSurfaceWindow& surface,
        const std::vector<DesktopViewItem>& snapshotItems,
        const std::vector<std::wstring>& assigned) {
        surface.snapshot_.items = snapshotItems;
        surface.assignedIdentities_ = assigned;
        surface.RebuildVisibleItems();
    }
};

struct MainWindowSmokeAccess {
    static DesktopSurfaceWindow* DesktopSurface(MainWindow& window) {
        return window.desktopSurface_.get();
    }

    static size_t WidgetCount(const MainWindow& window) {
        return window.widgetWindows_.size();
    }

    static bool LaunchOnStartupSetting(const MainWindow& window) {
        return window.organizerConfig_.settings.launchOnStartup;
    }

    static AppSettings SettingsForDialog(const MainWindow& window) {
        return window.SettingsForDialog();
    }

    static bool SaveSettings(
        MainWindow& window,
        const AppSettings& settings) {
        const bool publicDesktopChanged =
            settings.showPublicDesktopItems !=
            window.organizerConfig_.settings.showPublicDesktopItems;
        window.organizerConfig_.settings = settings;
        if (!window.SaveOrganizerConfig()) {
            return false;
        }
        window.ApplyLiveSettings(publicDesktopChanged);
        return true;
    }

    static bool UsesLightTheme(const MainWindow& window) {
        return window.iconGrid_.UsesLightTheme();
    }

    static bool AllTileGridsUseTheme(
        const MainWindow& window,
        bool lightTheme) {
        return !window.tileViews_.empty() &&
            std::all_of(
                window.tileViews_.begin(),
                window.tileViews_.end(),
                [&](const MainWindow::TileView& tile) {
                    return tile.grid.UsesLightTheme() == lightTheme;
                });
    }

    static size_t IconCacheCapacity(const MainWindow& window) {
        return window.iconCache_.Capacity();
    }

    static void ForceGridThemesForTest(
        MainWindow& window,
        bool lightTheme) {
        window.iconGrid_.SetLightTheme(lightTheme);
        for (MainWindow::TileView& tile : window.tileViews_) {
            tile.grid.SetLightTheme(lightTheme);
        }
    }

    static void LoadDesktopItems(MainWindow& window) {
        window.LoadDesktopItems();
    }

    static bool HasDesktopPath(
        const MainWindow& window,
        const std::wstring& path) {
        return std::any_of(
            window.items_.begin(),
            window.items_.end(),
            [&](const DesktopItem& item) {
                return CompareStringOrdinal(
                           item.path.c_str(), -1,
                           path.c_str(), -1,
                           TRUE) == CSTR_EQUAL;
            });
    }

    static bool HasRegisteredItem(
        const MainWindow& window,
        const std::wstring& itemId) {
        return std::any_of(
            window.organizerConfig_.items.begin(),
            window.organizerConfig_.items.end(),
            [&](const RegisteredItem& item) {
                return item.id == itemId;
            });
    }

    static AutoOrganizePreviewInput AutoOrganizeInput(
        const MainWindow& window) {
        return window.BuildAutoOrganizePreviewInput();
    }

    static AutoOrganizeApplyRequest AutoOrganizeRequest(
        const MainWindow& window,
        const lattice::organize::Plan& plan,
        const lattice::organize::LayoutPlan& layout) {
        return window.BuildAutoOrganizeApplyRequest(plan, layout);
    }

    static HWND ShowAutoOrganizePreview(MainWindow& window) {
        window.ShowAutoOrganizePreview();
        return window.autoOrganizePreview_ == nullptr
            ? nullptr : window.autoOrganizePreview_->Window();
    }

    static void CloseAutoOrganizePreview(MainWindow& window) {
        if (window.autoOrganizePreview_ != nullptr) {
            window.autoOrganizePreview_->Close();
        }
    }

    static void ApplyAutoOrganize(
        MainWindow& window,
        const lattice::organize::Plan& plan,
        const lattice::organize::LayoutPlan& layout) {
        window.ApplyAutoOrganizePlan(plan, layout, nullptr);
    }

    static bool AutoOrganizeIdle(const MainWindow& window) {
        return window.autoOrganizeOperation_ ==
            MainWindow::AutoOrganizeOperation::None;
    }
};

namespace {

std::optional<std::string> ReadFileBytes(const std::wstring& path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

struct StableFileIdentity {
    ULONGLONG volumeSerial = 0;
    FILE_ID_128 fileId{};
};

std::optional<StableFileIdentity> ReadStableFileIdentity(
    const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return std::nullopt;
    }
    HANDLE file = CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            ? FILE_FLAG_BACKUP_SEMANTICS
            : FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    FILE_ID_INFO info{};
    const bool read = GetFileInformationByHandleEx(
        file, FileIdInfo, &info, sizeof(info)) != FALSE;
    CloseHandle(file);
    if (!read) {
        return std::nullopt;
    }
    return StableFileIdentity{info.VolumeSerialNumber, info.FileId};
}

bool SameStableFileIdentity(
    const std::optional<StableFileIdentity>& left,
    const std::optional<StableFileIdentity>& right) {
    return left.has_value() && right.has_value() &&
        left->volumeSerial == right->volumeSerial &&
        std::memcmp(
            &left->fileId, &right->fileId,
            sizeof(FILE_ID_128)) == 0;
}

void AttachParentConsole() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* output = nullptr;
        freopen_s(&output, "CONOUT$", "w", stdout);
        freopen_s(&output, "CONOUT$", "w", stderr);
    }
}

bool IsLatticeWidgetWindow(HWND window, DWORD processId, const wchar_t* title) {
    DWORD candidateProcessId = 0;
    GetWindowThreadProcessId(window, &candidateProcessId);
    if (candidateProcessId != processId) {
        return false;
    }
    wchar_t className[64]{};
    if (GetClassNameW(window, className, ARRAYSIZE(className)) == 0 ||
        wcscmp(className, L"Lattice.WidgetWindow") != 0) {
        return false;
    }
    if (title == nullptr) {
        return true;
    }
    wchar_t windowTitle[256]{};
    GetWindowTextW(window, windowTitle, ARRAYSIZE(windowTitle));
    return wcscmp(windowTitle, title) == 0;
}

struct MainWindowSearchState {
    DWORD processId = 0;
    HWND result = nullptr;
};

BOOL CALLBACK FindMainWindowForProcess(HWND candidate, LPARAM parameter) {
    auto* state = reinterpret_cast<MainWindowSearchState*>(parameter);
    DWORD candidateProcessId = 0;
    GetWindowThreadProcessId(candidate, &candidateProcessId);
    wchar_t className[64]{};
    if (candidateProcessId == state->processId &&
        GetClassNameW(candidate, className, ARRAYSIZE(className)) != 0 &&
        wcscmp(className, L"Lattice.MainWindow") == 0) {
        state->result = candidate;
        return FALSE;
    }
    return TRUE;
}

HWND FindCurrentProcessMainWindow() {
    MainWindowSearchState state{GetCurrentProcessId()};
    EnumWindows(FindMainWindowForProcess, reinterpret_cast<LPARAM>(&state));
    return state.result;
}

struct WidgetWindowSearchState {
    DWORD processId = 0;
    const wchar_t* title = nullptr;
    HWND result = nullptr;
    int count = 0;
};

BOOL CALLBACK FindWidgetChild(HWND candidate, LPARAM parameter) {
    auto* state = reinterpret_cast<WidgetWindowSearchState*>(parameter);
    if (IsLatticeWidgetWindow(candidate, state->processId, state->title)) {
        state->result = candidate;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK FindWidgetTopLevel(HWND candidate, LPARAM parameter) {
    auto* state = reinterpret_cast<WidgetWindowSearchState*>(parameter);
    if (IsLatticeWidgetWindow(candidate, state->processId, state->title)) {
        state->result = candidate;
        return FALSE;
    }
    EnumChildWindows(candidate, FindWidgetChild, parameter);
    return state->result == nullptr;
}

HWND FindLatticeWidgetWindow(const wchar_t* title) {
    WidgetWindowSearchState state{GetCurrentProcessId(), title};
    EnumWindows(FindWidgetTopLevel, reinterpret_cast<LPARAM>(&state));
    return state.result;
}

BOOL CALLBACK CountWidgetChild(HWND candidate, LPARAM parameter) {
    auto* state = reinterpret_cast<WidgetWindowSearchState*>(parameter);
    if (IsLatticeWidgetWindow(candidate, state->processId, nullptr)) {
        ++state->count;
    }
    return TRUE;
}

BOOL CALLBACK CountWidgetTopLevel(HWND candidate, LPARAM parameter) {
    auto* state = reinterpret_cast<WidgetWindowSearchState*>(parameter);
    if (IsLatticeWidgetWindow(candidate, state->processId, nullptr)) {
        ++state->count;
    }
    EnumChildWindows(candidate, CountWidgetChild, parameter);
    return TRUE;
}

int CountLatticeWidgetWindows() {
    WidgetWindowSearchState state{GetCurrentProcessId()};
    EnumWindows(CountWidgetTopLevel, reinterpret_cast<LPARAM>(&state));
    return state.count;
}

bool SetWindowScreenBounds(
    HWND window,
    int x,
    int y,
    int width,
    int height,
    UINT flags) {
    POINT position{x, y};
    if ((GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) != 0 &&
        (flags & SWP_NOMOVE) == 0) {
        HWND parent = GetParent(window);
        if (parent == nullptr || ScreenToClient(parent, &position) == FALSE) {
            return false;
        }
    }
    return SetWindowPos(
               window,
               nullptr,
               position.x,
               position.y,
               width,
               height,
               flags) != FALSE;
}

bool CaptureScreenPixels(
    const RECT& rect,
    std::vector<std::uint32_t>& pixels) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return false;
    }
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = memory == nullptr
        ? nullptr
        : CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = bitmap == nullptr
        ? nullptr
        : SelectObject(memory, bitmap);
    const BOOL copied = previous != nullptr &&
        BitBlt(
            memory,
            0,
            0,
            width,
            height,
            screen,
            rect.left,
            rect.top,
            SRCCOPY | CAPTUREBLT);

    if (previous != nullptr) {
        SelectObject(memory, previous);
        previous = nullptr;
    }

    bool succeeded = false;
    if (copied != FALSE) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        pixels.resize(static_cast<size_t>(width) * height);
        succeeded = GetDIBits(
            memory,
            bitmap,
            0,
            static_cast<UINT>(height),
            pixels.data(),
            &info,
            DIB_RGB_COLORS) == height;
    }

    if (previous != nullptr) {
        SelectObject(memory, previous);
    }
    if (bitmap != nullptr) {
        DeleteObject(bitmap);
    }
    if (memory != nullptr) {
        DeleteDC(memory);
    }
    ReleaseDC(nullptr, screen);
    if (!succeeded) {
        pixels.clear();
    }
    return succeeded;
}

bool SaveCapturedPixelsBmp(
    const RECT& rect,
    const std::vector<std::uint32_t>& pixels,
    const std::filesystem::path& outputPath) {
    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 ||
        pixels.size() != static_cast<size_t>(width) * height) {
        return false;
    }
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER info{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(info);
    fileHeader.bfSize = fileHeader.bfOffBits +
        static_cast<DWORD>(pixels.size() * sizeof(std::uint32_t));
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = -height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(
        reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(
        reinterpret_cast<const char*>(pixels.data()),
        static_cast<std::streamsize>(
            pixels.size() * sizeof(std::uint32_t)));
    output.flush();
    return output.good();
}


bool SaveWindowClientBmp(
    HWND window,
    const std::filesystem::path& outputPath) {
    RECT client{};
    if (window == nullptr ||
        GetClientRect(window, &client) == FALSE) {
        return false;
    }
    const UINT width = static_cast<UINT>(client.right - client.left);
    const UINT height = static_cast<UINT>(client.bottom - client.top);
    if (width == 0 || height == 0) {
        return false;
    }
    HDC windowDc = GetDC(window);
    HDC memoryDc = windowDc == nullptr
        ? nullptr
        : CreateCompatibleDC(windowDc);
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = memoryDc == nullptr
        ? nullptr
        : CreateDIBSection(
            windowDc,
            &bitmapInfo,
            DIB_RGB_COLORS,
            &pixels,
            nullptr,
            0);
    HGDIOBJ previous = bitmap == nullptr
        ? nullptr
        : SelectObject(memoryDc, bitmap);
    constexpr UINT kPrintWindowRenderFullContent = 0x00000002;
    BOOL captured = previous != nullptr
        ? PrintWindow(
            window,
            memoryDc,
            PW_CLIENTONLY | kPrintWindowRenderFullContent)
        : FALSE;
    if (captured == FALSE && previous != nullptr) {
        captured = BitBlt(
            memoryDc,
            0,
            0,
            static_cast<int>(width),
            static_cast<int>(height),
            windowDc,
            0,
            0,
            SRCCOPY | CAPTUREBLT);
    }
    bool saved = false;
    if (captured != FALSE && pixels != nullptr) {
        const DWORD pixelBytes = width * height * 4;
        BITMAPFILEHEADER fileHeader{};
        fileHeader.bfType = 0x4D42;
        fileHeader.bfOffBits =
            sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fileHeader.bfSize = fileHeader.bfOffBits + pixelBytes;
        std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
        if (output) {
            output.write(
                reinterpret_cast<const char*>(&fileHeader),
                sizeof(fileHeader));
            output.write(
                reinterpret_cast<const char*>(&bitmapInfo.bmiHeader),
                sizeof(bitmapInfo.bmiHeader));
            output.write(
                static_cast<const char*>(pixels),
                static_cast<std::streamsize>(pixelBytes));
            output.flush();
            saved = output.good();
        }
    }
    if (previous != nullptr) {
        SelectObject(memoryDc, previous);
    }
    if (bitmap != nullptr) {
        DeleteObject(bitmap);
    }
    if (memoryDc != nullptr) {
        DeleteDC(memoryDc);
    }
    if (windowDc != nullptr) {
        ReleaseDC(window, windowDc);
    }
    return saved;
}

bool CaptureIconPixels(
    HICON icon,
    std::vector<std::uint32_t>& pixels) {
    constexpr int size = 64;
    if (icon == nullptr) {
        return false;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bitmapPixels = nullptr;
    HDC memory = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = memory == nullptr
        ? nullptr
        : CreateDIBSection(
              memory,
              &info,
              DIB_RGB_COLORS,
              &bitmapPixels,
              nullptr,
              0);
    HGDIOBJ previous = bitmap == nullptr
        ? nullptr
        : SelectObject(memory, bitmap);
    bool succeeded = false;
    if (previous != nullptr && bitmapPixels != nullptr) {
        auto* first = static_cast<std::uint32_t*>(bitmapPixels);
        std::fill(first, first + size * size, 0);
        succeeded = DrawIconEx(
            memory,
            0,
            0,
            icon,
            48,
            48,
            0,
            nullptr,
            DI_NORMAL) != FALSE;
        if (succeeded) {
            pixels.assign(first, first + size * size);
        }
    }

    if (previous != nullptr) {
        SelectObject(memory, previous);
    }
    if (bitmap != nullptr) {
        DeleteObject(bitmap);
    }
    if (memory != nullptr) {
        DeleteDC(memory);
    }
    if (!succeeded) {
        pixels.clear();
    }
    return succeeded;
}

HICON LoadSmokeShellIcon(
    const std::wstring& path,
    bool includeOverlay,
    int* overlayIndex) {
    SHFILEINFOW fileInfo{};
    const UINT flags = SHGFI_ICON | SHGFI_SYSICONINDEX |
        SHGFI_ADDOVERLAYS | SHGFI_OVERLAYINDEX;
    if (SHGetFileInfoW(
            path.c_str(),
            0,
            &fileInfo,
            sizeof(fileInfo),
            flags) == 0) {
        return nullptr;
    }

    const int imageIndex = fileInfo.iIcon & 0x00FFFFFF;
    const int resolvedOverlayIndex = (fileInfo.iIcon >> 24) & 0xFF;
    if (overlayIndex != nullptr) {
        *overlayIndex = resolvedOverlayIndex;
    }
    Microsoft::WRL::ComPtr<IImageList> imageList;
    HICON result = nullptr;
    if (SUCCEEDED(SHGetImageList(
            SHIL_EXTRALARGE,
            IID_PPV_ARGS(imageList.GetAddressOf()))) &&
        imageList != nullptr) {
        const UINT imageFlags = ILD_TRANSPARENT |
            (includeOverlay && resolvedOverlayIndex > 0
                ? INDEXTOOVERLAYMASK(resolvedOverlayIndex)
                : 0);
        imageList->GetIcon(imageIndex, imageFlags, &result);
    }
    if (fileInfo.hIcon != nullptr) {
        DestroyIcon(fileInfo.hIcon);
    }
    return result;
}

size_t CountVisiblePixelDifferences(
    const std::vector<std::uint32_t>& left,
    const std::vector<std::uint32_t>& right) {
    if (left.size() != right.size()) {
        return 0;
    }
    size_t differences = 0;
    for (size_t index = 0; index < left.size(); ++index) {
        const std::uint32_t first = left[index];
        const std::uint32_t second = right[index];
        const int blue = std::abs(
            static_cast<int>(first & 0xFF) -
            static_cast<int>(second & 0xFF));
        const int green = std::abs(
            static_cast<int>((first >> 8) & 0xFF) -
            static_cast<int>((second >> 8) & 0xFF));
        const int red = std::abs(
            static_cast<int>((first >> 16) & 0xFF) -
            static_cast<int>((second >> 16) & 0xFF));
        if (red + green + blue >= 12) {
            ++differences;
        }
    }
    return differences;
}

bool IsUncoveredDesktopPoint(POINT point) {
    HWND window = WindowFromPoint(point);
    HWND root = window == nullptr ? nullptr : GetAncestor(window, GA_ROOT);
    wchar_t className[64]{};
    if (root == nullptr ||
        GetClassNameW(root, className, ARRAYSIZE(className)) == 0) {
        return false;
    }
    return wcscmp(className, L"Progman") == 0 ||
           wcscmp(className, L"WorkerW") == 0;
}

bool FindUncoveredDesktopPlacement(
    int widgetWidth,
    int widgetHeight,
    POINT& position,
    RECT& captureRect) {
    std::vector<RECT> workAreas;
    EnumDisplayMonitors(
        nullptr,
        nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info)) {
                reinterpret_cast<std::vector<RECT>*>(parameter)->push_back(
                    info.rcWork);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&workAreas));

    constexpr int patchWidth = 160;
    constexpr int patchHeight = 64;
    constexpr int step = 32;
    for (const RECT& workArea : workAreas) {
        const int maxX = workArea.right - widgetWidth;
        const int maxY = workArea.bottom - widgetHeight;
        for (int y = workArea.top; y <= maxY; y += step) {
            for (int x = workArea.left; x <= maxX; x += step) {
                const RECT patch{
                    x,
                    y,
                    x + patchWidth,
                    y + patchHeight};
                const std::array<POINT, 5> samples{
                    POINT{patch.left + 2, patch.top + 2},
                    POINT{patch.right - 3, patch.top + 2},
                    POINT{patch.left + 2, patch.bottom - 3},
                    POINT{patch.right - 3, patch.bottom - 3},
                    POINT{
                        (patch.left + patch.right) / 2,
                        (patch.top + patch.bottom) / 2}};
                if (std::all_of(
                        samples.begin(),
                        samples.end(),
                        IsUncoveredDesktopPoint)) {
                    position = POINT{x, y};
                    captureRect = patch;
                    return true;
                }
            }
        }
    }
    return false;
}

struct SmokeDesktopPlacementOwnerState {
    bool requestReceived = false;
    bool requestCopied = false;
    int requestCount = 0;
    int configSyncCount = 0;
    int fullRefreshCount = 0;
    HWND captureAtRequest = nullptr;
    DesktopPlacementRequest request;
    std::chrono::steady_clock::time_point releaseStarted{};
    double requestLatencyMilliseconds = -1.0;
    bool collectionRequestCopied = false;
    int collectionRequestCount = 0;
    DesktopCollectionItemRequest collectionRequest;
};

LRESULT CALLBACK SmokeDesktopPlacementOwnerProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    SmokeDesktopPlacementOwnerState* state = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<SmokeDesktopPlacementOwnerState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    } else {
        state = reinterpret_cast<SmokeDesktopPlacementOwnerState*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (message == kDesktopPlacementRequestMessage && state != nullptr) {
        state->requestReceived = true;
        ++state->requestCount;
        state->captureAtRequest = GetCapture();
        state->requestLatencyMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - state->releaseStarted).count();
        const auto* request = reinterpret_cast<const DesktopPlacementRequest*>(lParam);
        if (request != nullptr) {
            try {
                state->request = *request;
                state->requestCopied = true;
            } catch (...) {
                state->requestCopied = false;
            }
        }
        // The owner deliberately accepts the handoff without touching Explorer.
        // Returning nonzero keeps the production fallback path out of this isolated smoke test.
        return 1;
    }
    if (message == kDesktopCollectionRequestMessage && state != nullptr) {
        const auto* request =
            reinterpret_cast<const DesktopCollectionItemRequest*>(lParam);
        ++state->collectionRequestCount;
        if (request != nullptr) {
            state->collectionRequest = *request;
            state->collectionRequestCopied = true;
            return 1;
        }
        return 0;
    }
    if (message == kOrganizerConfigSyncMessage && state != nullptr) {
        ++state->configSyncCount;
        return 0;
    }
    if (message == kOrganizerConfigChangedMessage && state != nullptr) {
        ++state->fullRefreshCount;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

int RunSmokeScan() {
    AttachParentConsole();
    DesktopScanner scanner;
    const auto items = scanner.Scan();
    std::wcout << L"Desktop item count: " << items.size() << L"\n";
    const size_t limit = items.size() < 10 ? items.size() : 10;
    for (size_t index = 0; index < limit; ++index) {
        std::wcout << L"- " << items[index].displayName << L"\n";
    }
    return 0;
}

bool VerifyLegacyBrandMigration() {
    wchar_t originalConfigDirectory[32768]{};
    const DWORD originalLength = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_CONFIG_DIR", originalConfigDirectory, ARRAYSIZE(originalConfigDirectory));
    if (originalLength == 0 || originalLength >= ARRAYSIZE(originalConfigDirectory)) {
        std::wcerr << L"Migration smoke requires an isolated config directory\n";
        return false;
    }

    const std::filesystem::path isolatedConfig(originalConfigDirectory);
    const std::filesystem::path migrationRoot = isolatedConfig.parent_path() / L"brand-migration";
    const std::filesystem::path legacyDirectory = migrationRoot / L"Luno";
    std::error_code fileError;
    std::filesystem::create_directories(legacyDirectory, fileError);
    if (fileError) {
        std::wcerr << L"Migration smoke could not create its legacy directory\n";
        return false;
    }

    SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_CONFIG_DIR", legacyDirectory.c_str());
    AppConfig legacyConfig;
    legacyConfig.window.x = 137;
    legacyConfig.window.y = 249;
    legacyConfig.window.width = 683;
    legacyConfig.window.height = 48;
    legacyConfig.window.normalHeight = 517;
    legacyConfig.window.collapsed = true;
    legacyConfig.window.opacity = 211;
    legacyConfig.currentCategoryId = L"migration-category";

    CategoryConfig category;
    category.id = L"migration-category";
    category.name = L"迁移顺序";
    category.layout.x = 421;
    category.layout.y = 163;
    category.layout.width = 594;
    category.layout.height = 438;
    category.itemIds = {L"migration-item-b", L"migration-item-a"};
    legacyConfig.categories.push_back(category);

    ItemConfig firstItem;
    firstItem.id = L"migration-item-a";
    firstItem.path = L"C:\\Migration\\A.lnk";
    firstItem.displayName = L"A";
    ItemConfig secondItem;
    secondItem.id = L"migration-item-b";
    secondItem.path = L"C:\\Migration\\B.lnk";
    secondItem.displayName = L"B";
    legacyConfig.items = {firstItem, secondItem};

    ConfigStore legacyStore;
    if (!legacyStore.SaveAppConfig(legacyConfig)) {
        std::wcerr << L"Migration smoke could not write the legacy fixture\n";
        return false;
    }
    const std::filesystem::path legacyConfigPath(legacyStore.ConfigPath());
    std::ifstream legacyInput(legacyConfigPath, std::ios::binary);
    const std::string legacyBytes(
        (std::istreambuf_iterator<char>(legacyInput)), std::istreambuf_iterator<char>());
    if (legacyBytes.empty()) {
        std::wcerr << L"Migration smoke legacy fixture is empty\n";
        return false;
    }

    SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_CONFIG_DIR", nullptr);
    SetEnvironmentVariableW(L"LATTICE_ROAMING_DIR", migrationRoot.c_str());
    ConfigStore migratedStore;
    const std::filesystem::path migratedConfigPath(migratedStore.ConfigPath());
    std::ifstream migratedInput(migratedConfigPath, std::ios::binary);
    const std::string migratedBytes(
        (std::istreambuf_iterator<char>(migratedInput)), std::istreambuf_iterator<char>());
    const AppConfig migratedConfig = migratedStore.LoadAppConfig();

    SetEnvironmentVariableW(L"LATTICE_ROAMING_DIR", nullptr);
    SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_CONFIG_DIR", originalConfigDirectory);

    if (legacyBytes != migratedBytes || !std::filesystem::exists(legacyConfigPath) ||
        migratedConfig.window.x != legacyConfig.window.x ||
        migratedConfig.window.y != legacyConfig.window.y ||
        migratedConfig.window.width != legacyConfig.window.width ||
        migratedConfig.window.height != legacyConfig.window.height ||
        migratedConfig.window.normalHeight != legacyConfig.window.normalHeight ||
        migratedConfig.window.collapsed != legacyConfig.window.collapsed ||
        migratedConfig.window.opacity != legacyConfig.window.opacity ||
        migratedConfig.categories.size() != 1 ||
        migratedConfig.categories.front().layout.x != category.layout.x ||
        migratedConfig.categories.front().layout.y != category.layout.y ||
        migratedConfig.categories.front().layout.width != category.layout.width ||
        migratedConfig.categories.front().layout.height != category.layout.height ||
        migratedConfig.categories.front().itemIds != category.itemIds ||
        migratedConfig.items.size() != legacyConfig.items.size()) {
        std::wcerr << L"Luno-to-Lattice migration changed config bytes, widget geometry, or item order\n";
        return false;
    }
    std::wcout << L"Legacy brand migration preserved config bytes and widget ordering\n";
    return true;
}

int RunSmokeConfig() {
    AttachParentConsole();
    wchar_t executablePath[MAX_PATH]{};
    if (GetModuleFileNameW(
            nullptr, executablePath, ARRAYSIZE(executablePath)) == 0) {
        std::wcerr << L"Startup command executable lookup failed\n";
        return 1;
    }
    std::wstring uppercaseExecutable = executablePath;
    std::transform(
        uppercaseExecutable.begin(),
        uppercaseExecutable.end(),
        uppercaseExecutable.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towupper(value)); });
    const std::wstring quotedExecutable =
        L"\"" + std::wstring(executablePath) + L"\"";
    if (!StartupManager::CommandTargetsExecutable(
            quotedExecutable, executablePath) ||
        !StartupManager::CommandTargetsExecutable(
            L"\"" + uppercaseExecutable + L"\"", executablePath) ||
        StartupManager::CommandTargetsExecutable(
            quotedExecutable + L" --hidden", executablePath) ||
        StartupManager::CommandTargetsExecutable(
            L"\"C:\\Other\\Lattice.exe\"", executablePath) ||
        StartupManager::CommandTargetsExecutable(L"", executablePath) ||
        StartupManager::CommandTargetsExecutable(L"   ", executablePath)) {
        std::wcerr << L"Startup command identity parsing failed\n";
        return 1;
    }
    if (!VerifyLegacyBrandMigration()) {
        return 1;
    }
    ConfigStore store;
    WindowConfig config = store.Load();
    config.width = config.width < 320 ? 720 : config.width;
    config.tabSide = 2;
    config.titleOpacity = 180;
    config.showBorder = false;
    if (!store.Save(config)) {
        std::wcerr << L"Config save failed\n";
        return 1;
    }
    const WindowConfig loaded = store.Load();
    if (loaded.tabSide != config.tabSide || loaded.titleOpacity != config.titleOpacity || loaded.showBorder != config.showBorder) {
        std::wcerr << L"Window layout options failed\n";
        return 1;
    }
    std::wcout << L"Config path: " << store.ConfigPath() << L"\n";
    std::wcout << L"Window: " << loaded.x << L"," << loaded.y << L" "
               << loaded.width << L"x" << loaded.height << L"\n";
    AppConfig appConfig = store.LoadAppConfig();
    appConfig.uncategorizedName = L"待整理";
    ItemConfig layoutItem;
    layoutItem.id = L"layout-smoke";
    layoutItem.path = L"C:\\Desktop\\layout-smoke.lnk";
    layoutItem.displayName = L"Layout Smoke";
    layoutItem.originalDesktopPath = layoutItem.path;
    layoutItem.desktopX = 144;
    layoutItem.desktopY = 288;
    layoutItem.hasDesktopPosition = true;
    layoutItem.desktopVisibilityMode = 1;
    layoutItem.desktopVisibilityOriginalFlags = FILE_ATTRIBUTE_SYSTEM;
    layoutItem.desktopVisibilityNewStartValue = 0;
    layoutItem.desktopVisibilityClassicValue = -1;
    appConfig.items.erase(
        std::remove_if(appConfig.items.begin(), appConfig.items.end(), [&](const ItemConfig& item) { return item.id == layoutItem.id; }),
        appConfig.items.end());
    appConfig.items.push_back(layoutItem);
    DesktopPlacementConfig desktopPlacement;
    desktopPlacement.path = layoutItem.originalDesktopPath;
    desktopPlacement.x = 96;
    desktopPlacement.y = 192;
    appConfig.desktopLayout.clear();
    appConfig.desktopLayout.push_back(desktopPlacement);
    DesktopPlacementConfig desktopDisplayPlacement;
    desktopDisplayPlacement.path = layoutItem.originalDesktopPath;
    desktopDisplayPlacement.x = 120;
    desktopDisplayPlacement.y = 240;
    appConfig.desktopDisplayLayout.clear();
    appConfig.desktopDisplayLayout.push_back(
        desktopDisplayPlacement);
    CategoryConfig category;
    category.id = L"smoke-category";
    category.name = L"Smoke";
    category.icon = L"apps";
    category.tileCollapsed = true;
    category.layout.tabSide = 3;
    category.layout.titleOpacity = 160;
    category.layout.showBorder = false;
    category.itemIds.push_back(L"file|smoke");
    appConfig.categories.erase(
        std::remove_if(
            appConfig.categories.begin(),
            appConfig.categories.end(),
            [&](const CategoryConfig& candidate) { return candidate.id == category.id; }),
        appConfig.categories.end());
    appConfig.categories.push_back(category);
    if (!store.SaveAppConfig(appConfig)) {
        std::wcerr << L"App config save failed\n";
        return 1;
    }
    const std::wstring configPath = store.ConfigPath();
    const size_t configSeparator = configPath.find_last_of(L"\\/");
    const std::wstring configDirectory = configSeparator == std::wstring::npos
        ? L"."
        : configPath.substr(0, configSeparator);
    const std::array<std::wstring, 4> noOpConfigPaths{
        configPath,
        configDirectory + L"\\config.backup.ini",
        configDirectory + L"\\config.backup.ini.1",
        configDirectory + L"\\config.backup.ini.2"};
    std::array<std::optional<std::string>, 4> noOpConfigBefore{};
    for (size_t index = 0; index < noOpConfigPaths.size(); ++index) {
        noOpConfigBefore[index] = ReadFileBytes(noOpConfigPaths[index]);
    }
    if (!store.SaveAppConfig(appConfig)) {
        std::wcerr << L"No-op app config save failed\n";
        return 1;
    }
    for (size_t index = 0; index < noOpConfigPaths.size(); ++index) {
        if (ReadFileBytes(noOpConfigPaths[index]) != noOpConfigBefore[index]) {
            std::wcerr << L"No-op app config save rewrote config history\n";
            return 1;
        }
    }
    const AppConfig loadedAppConfig = store.LoadAppConfig();
    const auto loadedLayoutItem = std::find_if(loadedAppConfig.items.begin(), loadedAppConfig.items.end(), [&](const ItemConfig& item) {
        return item.id == layoutItem.id;
    });
    if (loadedAppConfig.uncategorizedName != appConfig.uncategorizedName || loadedLayoutItem == loadedAppConfig.items.end() ||
        loadedLayoutItem->originalDesktopPath != layoutItem.originalDesktopPath || !loadedLayoutItem->hasDesktopPosition ||
        loadedLayoutItem->desktopX != layoutItem.desktopX || loadedLayoutItem->desktopY != layoutItem.desktopY ||
        loadedLayoutItem->desktopVisibilityMode != layoutItem.desktopVisibilityMode ||
        loadedLayoutItem->desktopVisibilityOriginalFlags != layoutItem.desktopVisibilityOriginalFlags ||
        loadedLayoutItem->desktopVisibilityNewStartValue != layoutItem.desktopVisibilityNewStartValue ||
        loadedLayoutItem->desktopVisibilityClassicValue != layoutItem.desktopVisibilityClassicValue ||
        loadedAppConfig.desktopLayout.size() != 1 ||
        loadedAppConfig.desktopLayout.front().path != desktopPlacement.path ||
        loadedAppConfig.desktopLayout.front().x != desktopPlacement.x ||
        loadedAppConfig.desktopLayout.front().y != desktopPlacement.y ||
        loadedAppConfig.desktopDisplayLayout.size() != 1 ||
        loadedAppConfig.desktopDisplayLayout.front().path !=
            desktopDisplayPlacement.path ||
        loadedAppConfig.desktopDisplayLayout.front().x !=
            desktopDisplayPlacement.x ||
        loadedAppConfig.desktopDisplayLayout.front().y !=
            desktopDisplayPlacement.y) {
        std::wcerr << L"Desktop layout config persistence failed\n";
        return 1;
    }
    if (!store.SaveDesktopDisplayPositionsAsync(
            {DesktopPlacementConfig{
                desktopDisplayPlacement.path, 333, 444}}) ||
        !store.SaveDesktopDisplayPositionsAsync(
            {
                DesktopPlacementConfig{
                    desktopDisplayPlacement.path, 777, 888},
                DesktopPlacementConfig{
                    L"desktop-display-second", 555, 666},
            }) ||
        !ConfigStore::DrainPendingWrites(5000)) {
        std::wcerr << L"Desktop display layout async save failed";
        return 1;
    }
    const AppConfig displayUpdatedConfig = store.LoadAppConfig();
    const auto updatedDisplay = std::find_if(
        displayUpdatedConfig.desktopDisplayLayout.begin(),
        displayUpdatedConfig.desktopDisplayLayout.end(),
        [&](const DesktopPlacementConfig& value) {
            return value.path == desktopDisplayPlacement.path;
        });
    const auto secondDisplay = std::find_if(
        displayUpdatedConfig.desktopDisplayLayout.begin(),
        displayUpdatedConfig.desktopDisplayLayout.end(),
        [](const DesktopPlacementConfig& value) {
            return value.path == L"desktop-display-second";
        });
    if (updatedDisplay ==
            displayUpdatedConfig.desktopDisplayLayout.end() ||
        updatedDisplay->x != 777 || updatedDisplay->y != 888 ||
        secondDisplay ==
            displayUpdatedConfig.desktopDisplayLayout.end() ||
        secondDisplay->x != 555 || secondDisplay->y != 666 ||
        displayUpdatedConfig.desktopLayout.size() != 1 ||
        displayUpdatedConfig.desktopLayout.front().x !=
            desktopPlacement.x ||
        displayUpdatedConfig.desktopLayout.front().y !=
            desktopPlacement.y) {
        std::wcerr << L"Desktop display layout merge changed native layout";
        return 1;
     }

    const std::wstring firstRenamedIdentity =
        L"C:\\Desktop\\layout-smoke-middle.lnk";
    const std::wstring finalRenamedIdentity =
        L"C:\\Desktop\\layout-smoke-final.lnk";
    std::vector<std::wstring> expectedItemOrder;
    expectedItemOrder.reserve(displayUpdatedConfig.items.size());
    for (const ItemConfig& item : displayUpdatedConfig.items) {
        expectedItemOrder.push_back(item.id);
    }
    std::vector<std::wstring> expectedCategoryOrder;
    expectedCategoryOrder.reserve(
        displayUpdatedConfig.categories.size());
    for (const CategoryConfig& candidate :
         displayUpdatedConfig.categories) {
        expectedCategoryOrder.push_back(candidate.id);
    }
    const std::vector<std::wstring> expectedUncategorizedOrder =
        displayUpdatedConfig.uncategorizedItemIds;
    std::vector<std::vector<std::wstring>> expectedCategoryItemOrder;
    expectedCategoryItemOrder.reserve(
        displayUpdatedConfig.categories.size());
    for (const CategoryConfig& candidate :
         displayUpdatedConfig.categories) {
        expectedCategoryItemOrder.push_back(candidate.itemIds);
    }
    if (!store.SaveDesktopDisplayPositionsAsync(
            {DesktopPlacementConfig{
                layoutItem.path, 901, 902}}) ||
        !store.SaveShellRenameAsync(
            layoutItem.path,
            firstRenamedIdentity,
            L"Layout Smoke Renamed") ||
        !store.SaveDesktopDisplayPositionsAsync(
            {DesktopPlacementConfig{
                firstRenamedIdentity, 903, 904}}) ||
        !store.SaveShellRenameAsync(
            firstRenamedIdentity,
            finalRenamedIdentity,
            L"Layout Smoke Final")) {
        std::wcerr << L"Shell rename async enqueue failed\n";
        return 1;
    }
    const auto verifyRenamedConfig =
        [&](const AppConfig& candidate) {
            const auto identityCount = [](
                const std::vector<DesktopPlacementConfig>& positions,
                const std::wstring& identity) {
                return std::count_if(
                    positions.begin(), positions.end(),
                    [&](const DesktopPlacementConfig& position) {
                        return CompareStringOrdinal(
                                   position.path.c_str(), -1,
                                   identity.c_str(), -1,
                                   TRUE) == CSTR_EQUAL;
                    });
            };
            const auto itemIdentityCount = [](
                const std::vector<ItemConfig>& items,
                const std::wstring& identity) {
                return std::count_if(
                    items.begin(), items.end(),
                    [&](const ItemConfig& item) {
                        return CompareStringOrdinal(
                                   item.path.c_str(), -1,
                                   identity.c_str(), -1,
                                   TRUE) == CSTR_EQUAL &&
                            CompareStringOrdinal(
                                   item.originalDesktopPath.c_str(), -1,
                                   identity.c_str(), -1,
                                   TRUE) == CSTR_EQUAL;
                    });
            };
            const auto renamed = std::find_if(
                candidate.items.begin(), candidate.items.end(),
                [&](const ItemConfig& item) {
                    return item.id == layoutItem.id;
                });
            const auto nativePosition = std::find_if(
                candidate.desktopLayout.begin(),
                candidate.desktopLayout.end(),
                [&](const DesktopPlacementConfig& position) {
                    return position.path == finalRenamedIdentity;
                });
            const auto displayPosition = std::find_if(
                candidate.desktopDisplayLayout.begin(),
                candidate.desktopDisplayLayout.end(),
                [&](const DesktopPlacementConfig& position) {
                    return position.path == finalRenamedIdentity;
                });
            std::vector<std::wstring> itemOrder;
            itemOrder.reserve(candidate.items.size());
            for (const ItemConfig& item : candidate.items) {
                itemOrder.push_back(item.id);
            }
            std::vector<std::wstring> categoryOrder;
            categoryOrder.reserve(candidate.categories.size());
            std::vector<std::vector<std::wstring>> categoryItemOrder;
            categoryItemOrder.reserve(candidate.categories.size());
            for (const CategoryConfig& category : candidate.categories) {
                categoryOrder.push_back(category.id);
                categoryItemOrder.push_back(category.itemIds);
            }
            return renamed != candidate.items.end() &&
                renamed->path == finalRenamedIdentity &&
                renamed->originalDesktopPath == finalRenamedIdentity &&
                renamed->displayName == L"Layout Smoke Final" &&
                nativePosition != candidate.desktopLayout.end() &&
                nativePosition->x == desktopPlacement.x &&
                nativePosition->y == desktopPlacement.y &&
                displayPosition != candidate.desktopDisplayLayout.end() &&
                displayPosition->x == 903 &&
                displayPosition->y == 904 &&
                identityCount(candidate.desktopLayout,
                    layoutItem.path) == 0 &&
                identityCount(candidate.desktopLayout,
                    firstRenamedIdentity) == 0 &&
                identityCount(candidate.desktopLayout,
                    finalRenamedIdentity) == 1 &&
                identityCount(candidate.desktopDisplayLayout,
                    layoutItem.path) == 0 &&
                identityCount(candidate.desktopDisplayLayout,
                    firstRenamedIdentity) == 0 &&
                identityCount(candidate.desktopDisplayLayout,
                    finalRenamedIdentity) == 1 &&
                itemIdentityCount(candidate.items,
                    layoutItem.path) == 0 &&
                itemIdentityCount(candidate.items,
                    firstRenamedIdentity) == 0 &&
                itemIdentityCount(candidate.items,
                    finalRenamedIdentity) == 1 &&
                itemOrder == expectedItemOrder &&
                categoryOrder == expectedCategoryOrder &&
                candidate.uncategorizedItemIds ==
                    expectedUncategorizedOrder &&
                categoryItemOrder == expectedCategoryItemOrder;
        };
    if (!verifyRenamedConfig(store.LoadAppConfig()) ||
        !ConfigStore::DrainPendingWrites(5000) ||
        !verifyRenamedConfig(store.LoadAppConfig())) {
        std::wcerr << L"Shell rename identity migration failed\n";
        return 1;
    }

    const auto savedCategory = std::find_if(
        loadedAppConfig.categories.begin(),
        loadedAppConfig.categories.end(),
        [&](const CategoryConfig& candidate) { return candidate.id == category.id; });
    if (savedCategory == loadedAppConfig.categories.end() || !savedCategory->tileCollapsed) {
        std::wcerr << L"Tile collapse state failed\n";
        return 1;
    }
    const std::wstring categoryPath = configDirectory + L"\\smoke-category.ini";
    if (!store.ExportCategoryConfig(category, categoryPath)) {
        std::wcerr << L"Category export failed\n";
        return 1;
    }
    CategoryConfig imported;
    if (!store.ImportCategoryConfig(categoryPath, imported) || imported.name != category.name || imported.icon != category.icon ||
        imported.tileCollapsed != category.tileCollapsed ||
        imported.layout.tabSide != category.layout.tabSide || imported.layout.titleOpacity != category.layout.titleOpacity ||
        imported.layout.showBorder != category.layout.showBorder) {
        std::wcerr << L"Category import failed\n";
        return 1;
    }
    WindowConfig profile = loaded;
    profile.viewMode = 1;
    profile.tabSide = 3;
    if (!store.SaveLayoutProfile(profile)) {
        std::wcerr << L"Layout profile save failed\n";
        return 1;
    }
    WindowConfig loadedProfile;
    if (!store.LoadLayoutProfile(loadedProfile) || loadedProfile.viewMode != 1 || loadedProfile.tabSide != 3) {
        std::wcerr << L"Layout profile load failed\n";
        return 1;
    }

    const DWORD fixtureRootRequired = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
    if (fixtureRootRequired == 0) {
        std::wcerr << L"Desktop filesystem consistency fixture root is missing\n";
        return 1;
    }
    std::wstring fixtureRootValue(fixtureRootRequired, L'\0');
    const DWORD fixtureRootCopied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        fixtureRootValue.data(), fixtureRootRequired);
    if (fixtureRootCopied == 0 || fixtureRootCopied >= fixtureRootRequired) {
        std::wcerr << L"Desktop filesystem consistency fixture root is invalid\n";
        return 1;
    }
    fixtureRootValue.resize(fixtureRootCopied);
    const std::filesystem::path consistencyRoot =
        std::filesystem::path(fixtureRootValue) /
        L"desktop-filesystem-consistency";
    const std::filesystem::path desktopRoot =
        consistencyRoot / L"Desktop";
    const std::filesystem::path publicRoot =
        consistencyRoot / L"PublicDesktop";
    const std::filesystem::path outsideRoot =
        consistencyRoot / L"Outside";
    std::error_code consistencyError;
    std::filesystem::create_directories(desktopRoot, consistencyError);
    std::filesystem::create_directories(publicRoot, consistencyError);
    std::filesystem::create_directories(outsideRoot, consistencyError);
    if (consistencyError ||
        !SetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_DESKTOP_DIR",
            desktopRoot.c_str()) ||
        !SetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_PUBLIC_DESKTOP_DIR",
            publicRoot.c_str())) {
        std::wcerr << L"Desktop filesystem consistency fixture setup failed\n";
        return 1;
    }

    const std::filesystem::path existingPath =
        desktopRoot / L"delete-while-running.txt";
    const std::filesystem::path addedBeforeStartPath =
        desktopRoot / L"added-before-start.txt";
    const std::filesystem::path addedWhileRunningPath =
        desktopRoot / L"added-while-running.txt";
    const std::filesystem::path deletedBeforeStartPath =
        desktopRoot / L"deleted-before-start.txt";
    const std::filesystem::path unavailableParentPath =
        desktopRoot / L"unavailable-parent" / L"keep-reference.txt";
    const std::filesystem::path outsideMissingPath =
        outsideRoot / L"keep-reference.txt";
    wchar_t dataRootBuffer[32768]{};
    const DWORD dataRootLength = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_DATA_DIR",
        dataRootBuffer,
        ARRAYSIZE(dataRootBuffer));
    if (dataRootLength == 0 || dataRootLength >= ARRAYSIZE(dataRootBuffer)) {
        std::wcerr << L"Desktop filesystem consistency data root is invalid\n";
        return 1;
    }
    const std::filesystem::path managedMissingPath =
        std::filesystem::path(dataRootBuffer) /
        L"ManagedShortcuts" / L"legacy-missing.txt";
    {
        std::ofstream existing(existingPath, std::ios::binary);
        std::ofstream addedBeforeStart(
            addedBeforeStartPath, std::ios::binary);
        existing << "existing";
        addedBeforeStart << "added-before-start";
        if (!existing || !addedBeforeStart) {
            std::wcerr << L"Desktop filesystem consistency fixture files failed\n";
            return 1;
        }
    }

    constexpr wchar_t kDeletedBeforeStartId[] =
        L"consistency-deleted-before-start";
    constexpr wchar_t kDeletedWhileRunningId[] =
        L"consistency-deleted-while-running";
    constexpr wchar_t kOutsideReferenceId[] =
        L"consistency-outside-reference";
    constexpr wchar_t kUnavailableReferenceId[] =
        L"consistency-unavailable-reference";
    constexpr wchar_t kManagedReferenceId[] =
        L"consistency-managed-reference";
    const auto makeItem = [](
        const std::wstring& id,
        const std::filesystem::path& path,
        const std::wstring& displayName) {
        ItemConfig item;
        item.id = id;
        item.path = path.wstring();
        item.originalDesktopPath = item.path;
        item.displayName = displayName;
        return item;
    };
    AppConfig consistencyConfig;
    consistencyConfig.currentCategoryId = L"consistency-category";
    consistencyConfig.items = {
        makeItem(
            kDeletedBeforeStartId,
            deletedBeforeStartPath,
            L"Deleted Before Start"),
        makeItem(
            kDeletedWhileRunningId,
            existingPath,
            L"Deleted While Running"),
        makeItem(
            kOutsideReferenceId,
            outsideMissingPath,
            L"Outside Reference"),
        makeItem(
            kUnavailableReferenceId,
            unavailableParentPath,
            L"Unavailable Reference"),
        makeItem(
            kManagedReferenceId,
            managedMissingPath,
            L"Managed Reference")};
    consistencyConfig.uncategorizedItemIds = {
        kOutsideReferenceId,
        kUnavailableReferenceId,
        kManagedReferenceId};
    CategoryConfig consistencyCategory;
    consistencyCategory.id = consistencyConfig.currentCategoryId;
    consistencyCategory.name = L"Consistency";
    consistencyCategory.itemIds = {
        kDeletedBeforeStartId,
        kDeletedWhileRunningId};
    consistencyConfig.categories.push_back(consistencyCategory);
    for (const ItemConfig& item : consistencyConfig.items) {
        consistencyConfig.desktopLayout.push_back(
            DesktopPlacementConfig{item.path, 100, 200});
        consistencyConfig.desktopDisplayLayout.push_back(
            DesktopPlacementConfig{item.path, 300, 400});
    }
    if (!store.SaveAppConfig(consistencyConfig)) {
        std::wcerr << L"Desktop filesystem consistency config setup failed\n";
        return 1;
    }

    const auto containsItemId = [](
        const AppConfig& candidate,
        const std::wstring& itemId) {
        return std::any_of(
            candidate.items.begin(),
            candidate.items.end(),
            [&](const ItemConfig& item) {
                return item.id == itemId;
            });
    };
    const auto containsMembership = [](
        const AppConfig& candidate,
        const std::wstring& itemId) {
        if (std::find(
                candidate.uncategorizedItemIds.begin(),
                candidate.uncategorizedItemIds.end(),
                itemId) != candidate.uncategorizedItemIds.end()) {
            return true;
        }
        return std::any_of(
            candidate.categories.begin(),
            candidate.categories.end(),
            [&](const CategoryConfig& candidateCategory) {
                return std::find(
                           candidateCategory.itemIds.begin(),
                           candidateCategory.itemIds.end(),
                           itemId) != candidateCategory.itemIds.end();
            });
    };
    const auto containsLayoutIdentity = [](
        const std::vector<DesktopPlacementConfig>& layout,
        const std::filesystem::path& path) {
        return std::any_of(
            layout.begin(),
            layout.end(),
            [&](const DesktopPlacementConfig& position) {
                return CompareStringOrdinal(
                           position.path.c_str(), -1,
                           path.c_str(), -1,
                           TRUE) == CSTR_EQUAL;
            });
    };
    const auto verifyRemoved = [&](
        const AppConfig& candidate,
        const std::wstring& itemId,
        const std::filesystem::path& path) {
        return !containsItemId(candidate, itemId) &&
            !containsMembership(candidate, itemId) &&
            !containsLayoutIdentity(candidate.desktopLayout, path) &&
            !containsLayoutIdentity(candidate.desktopDisplayLayout, path);
    };

    MainWindow consistencyWindow(
        GetModuleHandleW(nullptr), {}, false);
    MainWindowSmokeAccess::LoadDesktopItems(consistencyWindow);
    const AppConfig afterStartupRefresh = store.LoadAppConfig();
    if (!verifyRemoved(
            afterStartupRefresh,
            kDeletedBeforeStartId,
            deletedBeforeStartPath) ||
        MainWindowSmokeAccess::HasRegisteredItem(
            consistencyWindow, kDeletedBeforeStartId) ||
        MainWindowSmokeAccess::HasDesktopPath(
            consistencyWindow, deletedBeforeStartPath.wstring()) ||
        !MainWindowSmokeAccess::HasRegisteredItem(
            consistencyWindow, kDeletedWhileRunningId) ||
        !MainWindowSmokeAccess::HasDesktopPath(
            consistencyWindow, existingPath.wstring()) ||
        !MainWindowSmokeAccess::HasDesktopPath(
            consistencyWindow, addedBeforeStartPath.wstring()) ||
        !containsItemId(afterStartupRefresh, kOutsideReferenceId) ||
        !containsItemId(afterStartupRefresh, kUnavailableReferenceId) ||
        !containsItemId(afterStartupRefresh, kManagedReferenceId)) {
        std::wcerr << L"Desktop startup filesystem reconciliation failed\n";
        return 1;
    }

    {
        std::ofstream addedWhileRunning(
            addedWhileRunningPath, std::ios::binary);
        addedWhileRunning << "added-while-running";
        if (!addedWhileRunning) {
            std::wcerr << L"Desktop runtime addition fixture failed\n";
            return 1;
        }
    }
    MainWindowSmokeAccess::LoadDesktopItems(consistencyWindow);
    if (!MainWindowSmokeAccess::HasDesktopPath(
            consistencyWindow, addedWhileRunningPath.wstring())) {
        std::wcerr << L"Desktop runtime addition was not discovered\n";
        return 1;
    }

    if (!store.SaveInteractionStateAsync(
            consistencyCategory.id,
            consistencyCategory.layout,
            consistencyCategory.itemIds,
            true) ||
        !store.SaveDesktopDisplayPositionsAsync(
            {DesktopPlacementConfig{
                existingPath.wstring(), 777, 888}})) {
        std::wcerr << L"Desktop filesystem consistency pending-state setup failed\n";
        return 1;
    }
    if (!std::filesystem::remove(existingPath, consistencyError) ||
        consistencyError) {
        std::wcerr << L"Desktop runtime deletion fixture failed\n";
        return 1;
    }
    MainWindowSmokeAccess::LoadDesktopItems(consistencyWindow);
    const AppConfig pendingDeleteConfig = store.LoadAppConfig();
    if (!verifyRemoved(
            pendingDeleteConfig,
            kDeletedWhileRunningId,
            existingPath) ||
        MainWindowSmokeAccess::HasRegisteredItem(
            consistencyWindow, kDeletedWhileRunningId) ||
        MainWindowSmokeAccess::HasDesktopPath(
            consistencyWindow, existingPath.wstring()) ||
        !MainWindowSmokeAccess::HasDesktopPath(
            consistencyWindow, addedBeforeStartPath.wstring()) ||
        !MainWindowSmokeAccess::HasDesktopPath(
            consistencyWindow, addedWhileRunningPath.wstring())) {
        std::wcerr << L"Desktop runtime deletion reconciliation failed\n";
        return 1;
    }
    if (!ConfigStore::DrainPendingWrites(5000)) {
        std::wcerr << L"Desktop filesystem consistency write drain failed\n";
        return 1;
    }
    const AppConfig persistedConsistencyConfig = store.LoadAppConfig();
    if (!verifyRemoved(
            persistedConsistencyConfig,
            kDeletedBeforeStartId,
            deletedBeforeStartPath) ||
        !verifyRemoved(
            persistedConsistencyConfig,
            kDeletedWhileRunningId,
            existingPath) ||
        !containsItemId(persistedConsistencyConfig, kOutsideReferenceId) ||
        !containsItemId(
            persistedConsistencyConfig, kUnavailableReferenceId) ||
        !containsItemId(persistedConsistencyConfig, kManagedReferenceId) ||
        !std::filesystem::exists(addedBeforeStartPath) ||
        !std::filesystem::exists(addedWhileRunningPath)) {
        std::wcerr << L"Desktop filesystem consistency persistence failed\n";
        return 1;
    }
    std::wcout << L"Desktop filesystem additions and confirmed deletions converged\n";
    return 0;
}

int RunSmokeDesktopLayout() {
    AttachParentConsole();
    DesktopLayout layout;
    DesktopViewSnapshot snapshot;
    std::wstring errorMessage;
    const std::filesystem::path diagnosticPath =
        std::filesystem::path(ConfigStore{}.ConfigPath()).parent_path() /
        L"smoke-layout-diagnostics.txt";
    std::ofstream diagnostics(diagnosticPath, std::ios::trunc);
    const auto started = std::chrono::steady_clock::now();
    if (!layout.CaptureViewSnapshot(snapshot, errorMessage)) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        diagnostics << "STATUS=FAIL\nELAPSED_MS=" << elapsed << "\n";
        diagnostics.flush();
        std::wcerr << L"Desktop layout snapshot failed: " << errorMessage << L"\n";
        return 1;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    const bool valid = snapshot.desktopHost != nullptr &&
        snapshot.shellViewWindow != nullptr &&
        snapshot.listViewWindow != nullptr &&
        IsWindow(snapshot.desktopHost) != FALSE &&
        IsWindow(snapshot.shellViewWindow) != FALSE &&
        IsWindow(snapshot.listViewWindow) != FALSE &&
        snapshot.screenRect.right > snapshot.screenRect.left &&
        snapshot.screenRect.bottom > snapshot.screenRect.top &&
        !snapshot.items.empty() &&
        std::any_of(
            snapshot.items.begin(),
            snapshot.items.end(),
            [](const DesktopViewItem& item) {
                return item.viewIndex >= 0 && item.systemImageIndex >= 0;
            });
    diagnostics << "STATUS=" << (valid ? "PASS" : "FAIL") << "\n"
                << "ELAPSED_MS=" << elapsed << "\n"
                << "ITEM_COUNT=" << snapshot.items.size() << "\n"
                << "VIEW_FLAGS=" << snapshot.viewFlags << "\n"
                << "VIEW_ICON_SIZE=" << snapshot.viewIconSize << "\n";
    diagnostics.flush();
    if (!valid) {
        std::wcerr << L"Desktop layout snapshot returned incomplete Shell identity\n";
        return 1;
    }
    std::wcout << L"Desktop layout snapshot items: " << snapshot.items.size()
               << L", elapsed: " << elapsed << L" ms\n";
    return 0;
}

int RunSmokeShellNewMenu() {
    AttachParentConsole();
    const std::filesystem::path configPath(ConfigStore{}.ConfigPath());
    const std::filesystem::path directory = configPath.parent_path() / L"shell-new-smoke";
    std::error_code fileError;
    std::filesystem::create_directories(directory, fileError);
    if (fileError) {
        std::wcerr << L"Shell New test directory failed\n";
        return 1;
    }

    Microsoft::WRL::ComPtr<IShellItem> folderItem;
    Microsoft::WRL::ComPtr<IShellFolder> folder;
    Microsoft::WRL::ComPtr<IContextMenu> contextMenu;
    HMENU menu = CreatePopupMenu();
    bool found = false;
    int newItemCount = 0;
    if (SUCCEEDED(SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&folderItem))) &&
        SUCCEEDED(folderItem->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&folder))) &&
        SUCCEEDED(folder->CreateViewObject(nullptr, IID_PPV_ARGS(&contextMenu))) &&
        SUCCEEDED(contextMenu->QueryContextMenu(menu, 0, 0x5000, 0x5FFF, CMF_NORMAL | CMF_EXPLORE))) {
        const int count = GetMenuItemCount(menu);
        for (int index = 0; index < count; ++index) {
            wchar_t label[128]{};
            MENUITEMINFOW info{};
            info.cbSize = sizeof(info);
            info.fMask = MIIM_STRING | MIIM_SUBMENU;
            info.dwTypeData = label;
            info.cch = ARRAYSIZE(label);
            if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info) || info.hSubMenu == nullptr) {
                continue;
            }
            std::wstring normalized(label);
            normalized.erase(std::remove(normalized.begin(), normalized.end(), L'&'), normalized.end());
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t value) {
                return static_cast<wchar_t>(std::towlower(value));
            });
            if (normalized.find(L"新建") != std::wstring::npos || normalized == L"new" || normalized.rfind(L"new ", 0) == 0) {
                found = true;
                newItemCount = GetMenuItemCount(info.hSubMenu);
                break;
            }
        }
    }
    DestroyMenu(menu);
    contextMenu.Reset();
    folder.Reset();
    folderItem.Reset();
    std::filesystem::remove(directory, fileError);
    if (!found || newItemCount <= 0) {
        std::wcerr << L"Windows Shell New submenu unavailable\n";
        return 1;
    }
    std::wcout << L"Windows Shell New items: " << newItemCount << L"\n";
    return 0;
}

int RunOptionalDesktopShellItemRename() {
    wchar_t sourceBuffer[32768]{};
    const DWORD sourceLength = GetEnvironmentVariableW(
        L"LATTICE_SMOKE_DESKTOP_RENAME_PATH",
        sourceBuffer,
        ARRAYSIZE(sourceBuffer));
    if (sourceLength == 0) {
        return 0;
    }
    if (sourceLength >= ARRAYSIZE(sourceBuffer)) {
        std::wcerr << L"Desktop Shell-item rename path is invalid\n";
        return 10;
    }

    const std::filesystem::path sourcePath(sourceBuffer);
    if (CompareStringOrdinal(
            sourcePath.extension().c_str(), -1,
            L".lnk", -1, TRUE) != CSTR_EQUAL) {
        std::wcerr << L"Desktop Shell-item rename fixture must be a .lnk\n";
        return 11;
    }
    std::error_code fileError;
    if (!std::filesystem::exists(sourcePath, fileError) || fileError) {
        std::wcerr << L"Desktop Shell-item rename fixture does not exist\n";
        return 12;
    }

    ShellItemReference originalReference;
    bool canRename = false;
    const HRESULT referenceResult =
        CreateDesktopShellItemReference(
            sourcePath.wstring(), originalReference);
    const HRESULT attributesResult = SUCCEEDED(referenceResult)
        ? CanRenameDesktopShellItem(originalReference, canRename)
        : referenceResult;
    if (FAILED(attributesResult) || !canRename ||
        originalReference.desktopChildPidl.empty()) {
        std::wcerr << L"Desktop Shell-item rename fixture is not renameable\n";
        return 13;
    }

    const std::wstring originalDisplayName =
        sourcePath.stem().wstring();
    const std::wstring temporaryDisplayName =
        originalDisplayName + L"-Lattice-Smoke-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    const std::filesystem::path targetPath =
        sourcePath.parent_path() /
        (temporaryDisplayName + sourcePath.extension().wstring());
    fileError.clear();
    if (std::filesystem::exists(targetPath, fileError) || fileError) {
        std::wcerr << L"Desktop Shell-item temporary endpoint already exists\n";
        return 14;
    }

    DesktopShellRenameResult renamed;
    const HRESULT renameResult = RenameDesktopShellItem(
        nullptr,
        originalReference,
        temporaryDisplayName,
        renamed);
    fileError.clear();
    const bool sourceExistsAfterRename =
        std::filesystem::exists(sourcePath, fileError) && !fileError;
    fileError.clear();
    const bool targetExistsAfterRename =
        std::filesystem::exists(targetPath, fileError) && !fileError;
    const bool committed =
        renamed.disposition == ShellRenameDisposition::Renamed ||
        !sourceExistsAfterRename || targetExistsAfterRename;

    int validationResult = 0;
    ShellItemReference freshTarget;
    bool freshTargetCanRename = false;
    if (FAILED(renameResult) ||
        renamed.disposition != ShellRenameDisposition::Renamed ||
        renamed.item.desktopChildPidl.empty() ||
        CompareStringOrdinal(
            renamed.item.path.c_str(), -1,
            targetPath.wstring().c_str(), -1,
            TRUE) != CSTR_EQUAL ||
        CompareStringOrdinal(
            renamed.displayName.c_str(), -1,
            temporaryDisplayName.c_str(), -1,
            TRUE) != CSTR_EQUAL ||
        sourceExistsAfterRename || !targetExistsAfterRename ||
        FAILED(CreateDesktopShellItemReference(
            targetPath.wstring(), freshTarget)) ||
        freshTarget.desktopChildPidl.empty() ||
        FAILED(CanRenameDesktopShellItem(
            freshTarget, freshTargetCanRename)) ||
        !freshTargetCanRename) {
        validationResult = 15;
    }

    if (committed) {
        ShellItemReference restoreReference = freshTarget;
        if (restoreReference.desktopChildPidl.empty()) {
            if (!renamed.item.desktopChildPidl.empty()) {
                restoreReference = renamed.item;
            } else {
                CreateDesktopShellItemReference(
                    targetPath.wstring(), restoreReference);
            }
        }
        DesktopShellRenameResult restored;
        const HRESULT restoreResult =
            restoreReference.desktopChildPidl.empty()
            ? E_FAIL
            : RenameDesktopShellItem(
                nullptr,
                restoreReference,
                originalDisplayName,
                restored);
        fileError.clear();
        const bool sourceRestored =
            std::filesystem::exists(sourcePath, fileError) && !fileError;
        fileError.clear();
        const bool targetRemoved =
            !std::filesystem::exists(targetPath, fileError) && !fileError;
        ShellItemReference freshRestored;
        bool restoredCanRename = false;
        const HRESULT freshRestoreResult = sourceRestored
            ? CreateDesktopShellItemReference(
                sourcePath.wstring(), freshRestored)
            : E_FAIL;
        if (!sourceRestored || !targetRemoved ||
            FAILED(freshRestoreResult) ||
            freshRestored.desktopChildPidl.empty() ||
            FAILED(CanRenameDesktopShellItem(
                freshRestored, restoredCanRename)) ||
            !restoredCanRename ||
            restored.disposition != ShellRenameDisposition::Renamed ||
            CompareStringOrdinal(
                restored.item.path.c_str(), -1,
                sourcePath.wstring().c_str(), -1,
                TRUE) != CSTR_EQUAL ||
            CompareStringOrdinal(
                restored.displayName.c_str(), -1,
                originalDisplayName.c_str(), -1,
                TRUE) != CSTR_EQUAL) {
            std::wcerr << L"Desktop Shell-item rename restore failed: 0x"
                       << std::hex
                       << static_cast<unsigned long>(restoreResult)
                       << L"\n";
            return 16;
        }
    }
    if (validationResult != 0) {
        std::wcerr << L"Desktop Shell-item rename validation failed: 0x"
                   << std::hex
                   << static_cast<unsigned long>(renameResult)
                   << L", post-commit=0x"
                   << static_cast<unsigned long>(renamed.postCommitError)
                   << L"\n";
        return validationResult;
    }
    std::wcout << L"Desktop Shell-item rename and restore passed\n";
    return 0;
}

int RunSmokeShellRename() {
    AttachParentConsole();
    const int desktopItemResult = RunOptionalDesktopShellItemRename();
    if (desktopItemResult != 0) {
        return desktopItemResult;
    }
    wchar_t sourceBuffer[32768]{};
    wchar_t targetNameBuffer[1024]{};
    wchar_t expectedLeafBuffer[1024]{};
    wchar_t restoreNameBuffer[1024]{};
    const DWORD sourceLength = GetEnvironmentVariableW(
        L"LATTICE_SMOKE_SHELL_RENAME_PATH",
        sourceBuffer,
        ARRAYSIZE(sourceBuffer));
    const DWORD targetLength = GetEnvironmentVariableW(
        L"LATTICE_SMOKE_SHELL_RENAME_TARGET",
        targetNameBuffer,
        ARRAYSIZE(targetNameBuffer));
    const DWORD expectedLeafLength = GetEnvironmentVariableW(
        L"LATTICE_SMOKE_SHELL_RENAME_EXPECTED_LEAF",
        expectedLeafBuffer,
        ARRAYSIZE(expectedLeafBuffer));
    const DWORD restoreNameLength = GetEnvironmentVariableW(
        L"LATTICE_SMOKE_SHELL_RENAME_RESTORE_NAME",
        restoreNameBuffer,
        ARRAYSIZE(restoreNameBuffer));
    if (sourceLength == 0 || sourceLength >= ARRAYSIZE(sourceBuffer) ||
        targetLength == 0 || targetLength >= ARRAYSIZE(targetNameBuffer) ||
        expectedLeafLength >= ARRAYSIZE(expectedLeafBuffer) ||
        restoreNameLength >= ARRAYSIZE(restoreNameBuffer)) {
        std::wcerr << L"Explicit desktop rename smoke path and target are required\n";
        return 1;
    }
    const std::filesystem::path sourcePath(sourceBuffer);
    const wchar_t* expectedLeaf = expectedLeafLength == 0
        ? targetNameBuffer
        : expectedLeafBuffer;
    const std::filesystem::path targetPath =
        sourcePath.parent_path() / expectedLeaf;
    if (!std::filesystem::exists(sourcePath) ||
        std::filesystem::exists(targetPath)) {
        std::wcerr << L"Desktop rename smoke endpoints are invalid\n";
        return 2;
    }

    bool canRename = false;
    const HRESULT attributesResult = CanRenameShellPath(
        sourcePath.wstring(), canRename);
    ShellPathRenameResult renamed;
    const HRESULT renameResult = SUCCEEDED(attributesResult) && canRename
        ? RenameShellPath(
            nullptr, sourcePath.wstring(), targetNameBuffer, renamed)
        : attributesResult;
    if (FAILED(renameResult) || !std::filesystem::exists(targetPath) ||
        std::filesystem::exists(sourcePath) ||
        CompareStringOrdinal(
            renamed.parsingName.c_str(), -1,
            targetPath.wstring().c_str(), -1, TRUE) != CSTR_EQUAL) {
        std::wcerr << L"Desktop Shell rename failed: 0x"
                   << std::hex << static_cast<unsigned long>(renameResult)
                   << L"\n";
        return 3;
    }

    ShellPathRenameResult restored;
    const std::wstring restoreName = restoreNameLength == 0
        ? sourcePath.filename().wstring()
        : std::wstring(restoreNameBuffer);
    const HRESULT restoreResult = RenameShellPath(
        nullptr,
        renamed.parsingName,
        restoreName,
        restored);
    if (FAILED(restoreResult) || !std::filesystem::exists(sourcePath) ||
        std::filesystem::exists(targetPath) ||
        CompareStringOrdinal(
            restored.parsingName.c_str(), -1,
            sourcePath.wstring().c_str(), -1, TRUE) != CSTR_EQUAL) {
        std::wcerr << L"Desktop Shell rename restore failed: 0x"
                   << std::hex << static_cast<unsigned long>(restoreResult)
                   << L"\n";
        return 4;
    }
    std::wcout << L"Desktop Shell rename and restore passed: "
               << restored.displayName << L"\n";
    return 0;
}

int RunSmokeManagedItems() {
    AttachParentConsole();
    const DWORD required = GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
    if (required == 0) {
        std::wcerr << L"Explicit managed item smoke directory is required\n";
        return 1;
    }
    std::wstring baseValue(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        baseValue.data(),
        required);
    if (copied == 0 || copied >= required) {
        std::wcerr << L"Managed item smoke directory is invalid\n";
        return 1;
    }
    baseValue.resize(copied);
    const std::filesystem::path testBase = std::filesystem::absolute(baseValue).lexically_normal();
    const std::filesystem::path testRoot =
        testBase / (L"run-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path dataDirectory = testRoot / L"Data";
    const std::filesystem::path configDirectory = testRoot / L"Config";
    const std::filesystem::path desktopDirectory = testRoot / L"Desktop";
    const std::filesystem::path publicDesktopDirectory = testRoot / L"PublicDesktop";
    std::error_code fileError;
    if (!std::filesystem::create_directories(desktopDirectory / L"资料文件夹", fileError) || fileError ||
        !std::filesystem::create_directories(publicDesktopDirectory / L"公共资料文件夹", fileError) || fileError) {
        std::wcerr << L"Managed item smoke directory setup failed\n";
        return 1;
    }
    {
        std::ofstream file(desktopDirectory / L"说明.txt", std::ios::binary);
        file << "desktop file";
    }
    {
        std::ofstream file(desktopDirectory / L"资料文件夹" / L"内容.md", std::ios::binary);
        file << "folder child";
    }
    {
        std::ofstream file(desktopDirectory / L"测试快捷方式.lnk", std::ios::binary);
        file << "shortcut fixture";
    }
    {
        std::ofstream file(desktopDirectory / L"测试网址.url", std::ios::binary);
        file << "url fixture";
    }
    {
        std::ofstream file(publicDesktopDirectory / L"公共快捷方式.lnk", std::ios::binary);
        file << "public shortcut fixture";
    }
    {
        std::ofstream file(publicDesktopDirectory / L"公共资料文件夹" / L"公共内容.md", std::ios::binary);
        file << "public folder child";
    }

    ManagedShortcutStore store(
        dataDirectory.wstring(),
        desktopDirectory.wstring(),
        publicDesktopDirectory.wstring());
    const std::filesystem::path sourceFile = desktopDirectory / L"说明.txt";
    const std::filesystem::path sourceFolder = desktopDirectory / L"资料文件夹";
    const std::filesystem::path sourceShortcut =
        desktopDirectory / L"测试快捷方式.lnk";
    const std::filesystem::path sourceUrl = desktopDirectory / L"测试网址.url";
    const std::filesystem::path publicShortcut = publicDesktopDirectory / L"公共快捷方式.lnk";
    const std::filesystem::path publicFolder = publicDesktopDirectory / L"公共资料文件夹";
    const std::filesystem::path publicBatchA = publicDesktopDirectory / L"公共批量一.lnk";
    const std::filesystem::path publicBatchB = publicDesktopDirectory / L"公共批量二.lnk";
    const std::filesystem::path externalFile = testRoot / L"外部引用.txt";
    {
        std::ofstream file(externalFile, std::ios::binary);
        file << "external reference";
    }
    const auto fail = [&](const std::wstring& message) {
        std::wcerr << message << L"\n";
        std::error_code cleanupError;
        std::filesystem::remove_all(testRoot, cleanupError);
        return 1;
    };

    if (!SetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_CONFIG_DIR",
            configDirectory.c_str()) ||
        !SetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_DATA_DIR",
            dataDirectory.c_str()) ||
        !SetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_DESKTOP_DIR",
            desktopDirectory.c_str()) ||
        !SetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_PUBLIC_DESKTOP_DIR",
            publicDesktopDirectory.c_str())) {
        return fail(L"Original-path collection environment setup failed");
    }

    const auto userIdentityBefore = ReadStableFileIdentity(sourceFile);
    const auto publicIdentityBefore = ReadStableFileIdentity(publicFolder);
    const auto userBytesBefore = ReadFileBytes(sourceFile.wstring());
    const auto publicBytesBefore = ReadFileBytes(
        (publicFolder / L"公共内容.md").wstring());
    const auto countTopLevel = [](const std::filesystem::path& directory) {
        size_t count = 0;
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end;
             !error && it != end; it.increment(error)) {
            ++count;
        }
        return error ? static_cast<size_t>(-1) : count;
    };
    const size_t userCountBefore = countTopLevel(desktopDirectory);
    const size_t publicCountBefore = countTopLevel(publicDesktopDirectory);
    ConfigStore metadataConfigStore;
    AppConfig metadataConfig;
    CategoryConfig metadataCategory;
    metadataCategory.id = L"original-path-category";
    metadataCategory.name = L"原路径收纳";
    metadataConfig.categories.push_back(metadataCategory);
    if (!metadataConfigStore.SaveAppConfig(metadataConfig)) {
        return fail(L"Original-path collection config setup failed");
    }
    DesktopCollectionItemRequest userRequest;
    userRequest.categoryId = metadataCategory.id;
    userRequest.path = sourceFile.wstring();
    userRequest.insertionIndex = 0;
    const DesktopCollectionItemResult userCollected =
        CommitDesktopCollectionItemTransaction(userRequest);
    DesktopCollectionItemRequest publicRequest;
    publicRequest.categoryId = metadataCategory.id;
    publicRequest.path = publicFolder.wstring();
    publicRequest.insertionIndex = 1;
    const DesktopCollectionItemResult publicCollected =
        CommitDesktopCollectionItemTransaction(publicRequest);
    const AppConfig restartedConfig = metadataConfigStore.LoadAppConfig();
    const auto restartedCategory = std::find_if(
        restartedConfig.categories.begin(),
        restartedConfig.categories.end(),
        [&](const CategoryConfig& value) {
            return value.id == metadataCategory.id;
        });
    const bool noManagedPayload =
        !std::filesystem::exists(dataDirectory / L"ManagedShortcuts");
    if (!userCollected.succeeded || !publicCollected.succeeded ||
        userCollected.destinationPath != sourceFile.wstring() ||
        publicCollected.destinationPath != publicFolder.wstring() ||
        restartedCategory == restartedConfig.categories.end() ||
        restartedCategory->itemIds.size() != 2 ||
        restartedCategory->itemIds[0] != userCollected.itemId ||
        restartedCategory->itemIds[1] != publicCollected.itemId ||
        !SameStableFileIdentity(
            userIdentityBefore, ReadStableFileIdentity(sourceFile)) ||
        !SameStableFileIdentity(
            publicIdentityBefore, ReadStableFileIdentity(publicFolder)) ||
        ReadFileBytes(sourceFile.wstring()) != userBytesBefore ||
        ReadFileBytes((publicFolder / L"公共内容.md").wstring()) !=
            publicBytesBefore ||
        countTopLevel(desktopDirectory) != userCountBefore ||
        countTopLevel(publicDesktopDirectory) != publicCountBefore ||
        !noManagedPayload) {
        return fail(
            L"Daily collection changed an original path, File ID, content, count, order, or ManagedShortcuts state");
    }

    DesktopPlacementRequest userMoveOut;
    userMoveOut.path = sourceFile.wstring();
    userMoveOut.commitMoveOut = true;
    userMoveOut.itemId = userCollected.itemId;
    userMoveOut.sourcePath = sourceFile.wstring();
    std::wstring unchangedDesktopPath;
    std::wstring metadataError;
    if (!CommitDesktopMoveOutTransaction(
            userMoveOut, unchangedDesktopPath, metadataError) ||
        unchangedDesktopPath != sourceFile.wstring() ||
        !SameStableFileIdentity(
            userIdentityBefore, ReadStableFileIdentity(sourceFile)) ||
        std::filesystem::exists(dataDirectory / L"ManagedShortcuts")) {
        return fail(
            L"Daily move-out changed the original item or created ManagedShortcuts: " +
            metadataError);
    }

    if (!store.IsSupportedDesktopItem(sourceFile.wstring()) ||
        !store.IsSupportedDesktopItem(sourceFolder.wstring()) ||
        store.IsSupportedShortcut(sourceFile.wstring()) ||
        store.RequiresManagedStorage(sourceFile.wstring()) ||
        store.RequiresManagedStorage(sourceFolder.wstring()) ||
        store.RequiresManagedStorage(sourceShortcut.wstring()) ||
        store.RequiresManagedStorage(sourceUrl.wstring()) ||
        store.RequiresManagedStorage(publicShortcut.wstring()) ||
        store.RequiresManagedStorage(publicFolder.wstring()) ||
        store.RequiresManagedStorage(externalFile.wstring()) ||
        store.IsSupportedDesktopItem(externalFile.wstring())) {
        return fail(L"Manual desktop item type acceptance failed");
    }

    const DWORD originalAttributes = GetFileAttributesW(sourceFile.c_str());
    ItemConfig visibilityItem;
    visibilityItem.path = sourceFile.wstring();
    visibilityItem.desktopVisibilityMode = 1;
    visibilityItem.desktopVisibilityOriginalFlags = static_cast<int>(
        originalAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
    std::wstring visibilityError;
    if (SetFileAttributesW(
            sourceFile.c_str(),
            originalAttributes | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) == FALSE ||
        !store.PrepareForManagedStorage(visibilityItem, visibilityError) ||
        (GetFileAttributesW(sourceFile.c_str()) & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) !=
            (originalAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ||
        visibilityItem.desktopVisibilityMode != 0 ||
        visibilityItem.desktopVisibilityOriginalFlags != 0 ||
        visibilityItem.desktopVisibilityNewStartValue != -1 ||
        visibilityItem.desktopVisibilityClassicValue != -1) {
        return fail(L"Legacy desktop visibility restore failed: " + visibilityError);
    }

    std::wstring managedFile;
    std::wstring moveError;
    if (!store.MoveIntoCategory(
            L"smoke-file",
            sourceFile.wstring(),
            L"smoke-category",
            [](const std::wstring&) { return true; },
            managedFile,
            moveError) ||
        std::filesystem::exists(sourceFile) ||
        !std::filesystem::exists(managedFile) ||
        !store.RequiresManagedStorage(managedFile)) {
        return fail(L"Ordinary file collection failed: " + moveError);
    }
    std::wstring restoredFile;
    if (!store.MoveToOriginalDesktop(
            L"smoke-file",
            managedFile,
            sourceFile.wstring(),
            [](const std::wstring&) { return true; },
            restoredFile,
            moveError) ||
        restoredFile != sourceFile.wstring() ||
        !std::filesystem::exists(sourceFile)) {
        return fail(L"Ordinary file restore failed: " + moveError);
    }

    std::wstring managedDuplicate;
    if (!store.MoveIntoCategory(
            L"smoke-duplicate",
            sourceFile.wstring(),
            L"smoke-category",
            [](const std::wstring&) { return true; },
            managedDuplicate,
            moveError)) {
        return fail(L"Duplicate reconciliation setup failed: " + moveError);
    }
    {
        std::ofstream duplicate(sourceFile, std::ios::binary);
        duplicate << "desktop file";
    }
    bool duplicatePersisted = false;
    std::wstring reconciledFile;
    if (!store.MoveToOriginalDesktop(
            L"smoke-duplicate",
            managedDuplicate,
            sourceFile.wstring(),
            [&](const std::wstring& path) {
                duplicatePersisted = path == sourceFile.wstring();
                return duplicatePersisted;
            },
            reconciledFile,
            moveError) ||
        !duplicatePersisted ||
        reconciledFile != sourceFile.wstring() ||
        std::filesystem::exists(managedDuplicate) ||
        !std::filesystem::exists(sourceFile)) {
        return fail(L"Identical desktop duplicate reconciliation failed: " + moveError);
    }

    std::wstring managedConflict;
    if (!store.MoveIntoCategory(
            L"smoke-conflict",
            sourceFile.wstring(),
            L"smoke-category",
            [](const std::wstring&) { return true; },
            managedConflict,
            moveError)) {
        return fail(L"Desktop conflict setup failed: " + moveError);
    }
    {
        std::ofstream conflict(sourceFile, std::ios::binary);
        conflict << "different desktop content";
    }
    std::wstring conflictDestination;
    if (store.MoveToOriginalDesktop(
            L"smoke-conflict",
            managedConflict,
            sourceFile.wstring(),
            [](const std::wstring&) { return true; },
            conflictDestination,
            moveError) ||
        !std::filesystem::exists(managedConflict) ||
        !std::filesystem::exists(sourceFile)) {
        return fail(L"Different desktop duplicate was not preserved safely");
    }
    std::wstring rollbackDestination;
    std::wstring rollbackPersistPath;
    if (store.MoveToOriginalDesktop(
            L"smoke-conflict",
            managedConflict,
            sourceFile.wstring(),
            [&](const std::wstring& path) {
                rollbackPersistPath = path;
                return false;
            },
            rollbackDestination,
            moveError,
            nullptr,
            true,
            true) ||
        rollbackPersistPath.empty() ||
        rollbackPersistPath == sourceFile.wstring() ||
        !std::filesystem::exists(managedConflict) ||
        !std::filesystem::exists(sourceFile) ||
        std::filesystem::exists(rollbackPersistPath) ||
        std::filesystem::exists(
            dataDirectory / L"ManagedShortcuts" / L"move-journal.bin")) {
        return fail(
            L"Explicit unique-name conflict rollback did not preserve both endpoints");
    }
    bool uniqueConflictPersisted = false;
    if (!store.MoveToOriginalDesktop(
            L"smoke-conflict",
            managedConflict,
            sourceFile.wstring(),
            [&](const std::wstring& path) {
                uniqueConflictPersisted =
                    path != sourceFile.wstring() &&
                    std::filesystem::path(path).parent_path() ==
                        sourceFile.parent_path();
                return uniqueConflictPersisted;
            },
            conflictDestination,
            moveError,
            nullptr,
            true,
            true) ||
        !uniqueConflictPersisted ||
        conflictDestination == sourceFile.wstring() ||
        std::filesystem::path(conflictDestination).filename().wstring() !=
            L"说明 (2).txt" ||
        std::filesystem::exists(managedConflict) ||
        !std::filesystem::exists(sourceFile) ||
        !std::filesystem::exists(conflictDestination) ||
        ReadFileBytes(conflictDestination) != userBytesBefore ||
        ReadFileBytes(sourceFile.wstring()) == userBytesBefore ||
        std::filesystem::exists(
            dataDirectory / L"ManagedShortcuts" / L"move-journal.bin")) {
        return fail(
            L"Explicit unique-name conflict release failed: " + moveError);
    }

    std::wstring managedPublicShortcut;
    if (!store.MoveIntoCategory(
            L"smoke-public-desktop",
            publicShortcut.wstring(),
            L"smoke-category",
            [](const std::wstring&) { return true; },
            managedPublicShortcut,
            moveError)) {
        return fail(L"Public Desktop shortcut collection failed: " + moveError);
    }
    bool publicRestorePersisted = false;
    std::wstring publicRestoreDestination;
    if (!store.MoveToOriginalDesktop(
            L"smoke-public-desktop",
            managedPublicShortcut,
            publicShortcut.wstring(),
            [&](const std::wstring& path) {
                publicRestorePersisted = path == publicShortcut.wstring();
                return publicRestorePersisted;
            },
            publicRestoreDestination,
            moveError) ||
        !publicRestorePersisted ||
        publicRestoreDestination != publicShortcut.wstring() ||
        std::filesystem::exists(managedPublicShortcut) ||
        !std::filesystem::exists(publicShortcut) ||
        std::filesystem::exists(desktopDirectory / publicShortcut.filename())) {
        return fail(L"Public Desktop exact restore failed: " + moveError);
    }

    std::wstring managedPublicFolder;
    if (!store.MoveIntoCategory(
            L"smoke-public-folder",
            publicFolder.wstring(),
            L"smoke-category",
            [](const std::wstring&) { return true; },
            managedPublicFolder,
            moveError) ||
        std::filesystem::exists(publicFolder) ||
        !std::filesystem::exists(std::filesystem::path(managedPublicFolder) / L"公共内容.md")) {
        return fail(L"Public Desktop folder collection failed: " + moveError);
    }
    std::wstring restoredPublicFolder;
    if (!store.MoveToOriginalDesktop(
            L"smoke-public-folder",
            managedPublicFolder,
            publicFolder.wstring(),
            [](const std::wstring&) { return true; },
            restoredPublicFolder,
            moveError) ||
        restoredPublicFolder != publicFolder.wstring() ||
        !std::filesystem::exists(publicFolder / L"公共内容.md")) {
        return fail(L"Public Desktop folder exact restore failed: " + moveError);
    }

    {
        std::ofstream first(publicBatchA, std::ios::binary);
        std::ofstream second(publicBatchB, std::ios::binary);
        first << "public batch a";
        second << "public batch b";
    }
    std::wstring managedPublicBatchA;
    std::wstring managedPublicBatchB;
    if (!store.MoveIntoCategory(
            L"smoke-public-batch-a",
            publicBatchA.wstring(),
            L"smoke-category",
            [](const std::wstring&) { return true; },
            managedPublicBatchA,
            moveError) ||
        !store.MoveIntoCategory(
            L"smoke-public-batch-b",
            publicBatchB.wstring(),
            L"smoke-category",
            [](const std::wstring&) { return true; },
            managedPublicBatchB,
            moveError)) {
        return fail(L"Public Desktop batch setup failed: " + moveError);
    }
    HWND batchOwner = CreateWindowExW(
        0, L"STATIC", L"Lattice batch smoke owner", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (batchOwner == nullptr) {
        return fail(L"Public Desktop batch owner creation failed");
    }
    int batchPersistCalls = 0;
    std::vector<std::pair<std::wstring, std::wstring>> batchDestinations;
    const bool batchMoved = store.MoveToOriginalDesktopBatch(
        {
            {L"smoke-public-batch-a", managedPublicBatchA,
             (desktopDirectory / publicBatchA.filename()).wstring(), true},
            {L"smoke-public-batch-b", managedPublicBatchB, publicBatchB.wstring(), true},
        },
        [&](const std::vector<std::pair<std::wstring, std::wstring>>& moved) {
            ++batchPersistCalls;
            return moved.size() == 2;
        },
        batchDestinations,
        moveError,
        batchOwner);
    DestroyWindow(batchOwner);
    if (!batchMoved || batchPersistCalls != 1 || batchDestinations.size() != 2 ||
        std::filesystem::exists(managedPublicBatchA) ||
        std::filesystem::exists(managedPublicBatchB) ||
        !std::filesystem::exists(desktopDirectory / publicBatchA.filename()) ||
        std::filesystem::exists(publicBatchA) ||
        !std::filesystem::exists(publicBatchB) ||
        std::filesystem::exists(dataDirectory / L"ManagedShortcuts" / L"move-journal.bin")) {
        return fail(L"Public Desktop batch transaction failed: " + moveError);
    }

    std::wstring managedFolder;
    if (!store.MoveIntoCategory(
            L"smoke-folder",
            sourceFolder.wstring(),
            L"smoke-category",
            [](const std::wstring&) { return true; },
            managedFolder,
            moveError) ||
        std::filesystem::exists(sourceFolder) ||
        !std::filesystem::exists(std::filesystem::path(managedFolder) / L"内容.md")) {
        return fail(L"Folder collection failed: " + moveError);
    }
    std::wstring restoredFolder;
    if (!store.MoveToOriginalDesktop(
            L"smoke-folder",
            managedFolder,
            sourceFolder.wstring(),
            [](const std::wstring&) { return true; },
            restoredFolder,
            moveError) ||
        restoredFolder != sourceFolder.wstring() ||
        !std::filesystem::exists(sourceFolder / L"内容.md")) {
        return fail(L"Folder restore failed: " + moveError);
    }

    std::filesystem::remove_all(testRoot, fileError);
    if (fileError) {
        std::wcerr << L"Managed item smoke cleanup failed\n";
        return 1;
    }
    std::wcout << L"Ordinary, Public Desktop, and explicit unique-conflict transactions passed\n";
    return 0;
}

int RunSmokeShortcutOverlay(HINSTANCE instance) {
    AttachParentConsole();
    wchar_t notifyValue[2]{};
    if (GetEnvironmentVariableW(
            L"LATTICE_SMOKE_NOTIFY_SHORTCUT_OVERLAY",
            notifyValue,
            ARRAYSIZE(notifyValue)) == 1 &&
        notifyValue[0] == L'1') {
        SHChangeNotify(
            SHCNE_ASSOCCHANGED,
            SHCNF_IDLIST,
            nullptr,
            nullptr);
    }
    const DWORD required = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        nullptr,
        0);
    if (required == 0) {
        std::wcerr << L"Explicit shortcut overlay smoke directory is required\n";
        return 1;
    }
    std::wstring baseValue(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        baseValue.data(),
        required);
    if (copied == 0 || copied >= required) {
        std::wcerr << L"Shortcut overlay smoke directory is invalid\n";
        return 1;
    }
    baseValue.resize(copied);

    std::error_code fileError;
    const std::filesystem::path testRoot =
        std::filesystem::absolute(baseValue).lexically_normal() /
        (L"shortcut-overlay-" + std::to_wstring(GetCurrentProcessId()));
    if (!std::filesystem::create_directories(testRoot, fileError) || fileError) {
        std::wcerr << L"Shortcut overlay smoke directory setup failed\n";
        return 1;
    }

    wchar_t executablePath[MAX_PATH]{};
    if (GetModuleFileNameW(instance, executablePath, ARRAYSIZE(executablePath)) == 0) {
        std::wcerr << L"Unable to resolve smoke executable path\n";
        return 1;
    }
    wchar_t systemDirectory[MAX_PATH]{};
    const UINT systemDirectoryLength = GetSystemDirectoryW(
        systemDirectory,
        ARRAYSIZE(systemDirectory));
    if (systemDirectoryLength == 0 ||
        systemDirectoryLength >= ARRAYSIZE(systemDirectory)) {
        std::wcerr << L"Unable to resolve the Shell icon fixture library\n";
        return 1;
    }
    const std::filesystem::path iconLibrary =
        std::filesystem::path(systemDirectory) / L"SHELL32.dll";
    const std::filesystem::path shortcutPath = testRoot / L"Lattice smoke.lnk";
    Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
    Microsoft::WRL::ComPtr<IPersistFile> persistFile;
    if (FAILED(CoCreateInstance(
            CLSID_ShellLink,
            nullptr,
            CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(shellLink.GetAddressOf()))) ||
        shellLink == nullptr ||
        FAILED(shellLink->SetPath(executablePath)) ||
        FAILED(shellLink->SetIconLocation(iconLibrary.c_str(), 141)) ||
        FAILED(shellLink.As(&persistFile)) ||
        persistFile == nullptr ||
        FAILED(persistFile->Save(shortcutPath.c_str(), TRUE))) {
        std::wcerr << L"Unable to create shortcut overlay fixture\n";
        return 1;
    }

    int overlayIndex = 0;
    HICON baseIcon = LoadSmokeShellIcon(shortcutPath.wstring(), false, nullptr);
    HICON expectedIcon = LoadSmokeShellIcon(
        shortcutPath.wstring(),
        true,
        &overlayIndex);
    if (baseIcon == nullptr || expectedIcon == nullptr || overlayIndex <= 0) {
        if (baseIcon != nullptr) {
            DestroyIcon(baseIcon);
        }
        if (expectedIcon != nullptr) {
            DestroyIcon(expectedIcon);
        }
        std::wcerr << L"Windows Shell did not expose a shortcut overlay\n";
        return 1;
    }

    int ordinaryOverlayIndex = -1;
    HICON ordinaryIcon = LoadSmokeShellIcon(
        executablePath,
        true,
        &ordinaryOverlayIndex);
    if (ordinaryIcon == nullptr || ordinaryOverlayIndex != 0) {
        if (ordinaryIcon != nullptr) {
            DestroyIcon(ordinaryIcon);
        }
        DestroyIcon(baseIcon);
        DestroyIcon(expectedIcon);
        std::wcerr << L"Ordinary files must not receive a shortcut overlay\n";
        return 1;
    }
    DestroyIcon(ordinaryIcon);

    IconCache iconCache;
    iconCache.Preload(shortcutPath.wstring());
    HICON cachedIcon = nullptr;
    const ULONGLONG deadline = GetTickCount64() + 3000;
    while (cachedIcon == nullptr && GetTickCount64() < deadline) {
        Sleep(10);
        cachedIcon = iconCache.CopyReadyIconForDrag(shortcutPath.wstring());
    }

    IconCache desktopSizedIconCache;
    desktopSizedIconCache.PreloadShellIcon(
        shortcutPath.wstring(),
        -1,
        overlayIndex,
        72,
        nullptr,
        48);
    HICON desktopSizedIcon = nullptr;
    const ULONGLONG desktopSizedDeadline = GetTickCount64() + 3000;
    while (desktopSizedIcon == nullptr &&
           GetTickCount64() < desktopSizedDeadline) {
        Sleep(10);
        desktopSizedIcon =
            desktopSizedIconCache.CopyReadyIconForDrag(shortcutPath.wstring());
    }

    std::vector<std::uint32_t> basePixels;
    std::vector<std::uint32_t> expectedPixels;
    std::vector<std::uint32_t> cachedPixels;
    std::vector<std::uint32_t> desktopSizedPixels;
    const bool captured =
        CaptureIconPixels(baseIcon, basePixels) &&
        CaptureIconPixels(expectedIcon, expectedPixels) &&
        CaptureIconPixels(cachedIcon, cachedPixels) &&
        CaptureIconPixels(desktopSizedIcon, desktopSizedPixels);
    DestroyIcon(baseIcon);
    DestroyIcon(expectedIcon);
    if (cachedIcon != nullptr) {
        DestroyIcon(cachedIcon);
    }
    if (desktopSizedIcon != nullptr) {
        DestroyIcon(desktopSizedIcon);
    }
    if (!captured ||
        basePixels == expectedPixels ||
        cachedPixels != expectedPixels) {
        std::wcerr << L"Lattice shortcut icon is not the Shell-composited icon\n";
        return 1;
    }
    if (desktopSizedPixels != expectedPixels) {
        std::wcerr
            << L"Logical 48px desktop request selected a non-Explorer Shell image list at 150% DPI\n";
        return 2;
    }

    HICON resourceIcon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_SHORTCUT_OVERLAY),
        IMAGE_ICON,
        24,
        24,
        LR_DEFAULTCOLOR));
    if (resourceIcon == nullptr) {
        std::wcerr << L"Shortcut overlay resource 102 is unavailable\n";
        return 1;
    }
    DestroyIcon(resourceIcon);

    std::filesystem::remove_all(testRoot, fileError);
    if (fileError) {
        std::wcerr << L"Unable to clean shortcut overlay smoke fixture\n";
        return 1;
    }
    std::wcout << L"Shortcut overlay index: " << overlayIndex << L"\n";
    std::wcout << L"Ordinary file overlay index: " << ordinaryOverlayIndex << L"\n";
    std::wcout << L"Shortcut overlay shell composition: PASS\n";
    return 0;
}

int RunSmokeCategoryStorage() {
    AttachParentConsole();
    const DWORD required = GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
    if (required == 0) {
        std::wcerr << L"Explicit category storage smoke directory is required\n";
        return 1;
    }
    std::wstring baseValue(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        baseValue.data(),
        required);
    if (copied == 0 || copied >= required) {
        std::wcerr << L"Category storage smoke directory is invalid\n";
        return 1;
    }
    baseValue.resize(copied);

    const std::filesystem::path testRoot =
        std::filesystem::absolute(baseValue).lexically_normal() /
        (L"category-storage-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path configDirectory = testRoot / L"Config";
    const std::filesystem::path dataDirectory = testRoot / L"Data";
    const std::filesystem::path desktopDirectory = testRoot / L"Desktop";
    const std::filesystem::path managedDirectory = dataDirectory / L"ManagedShortcuts";
    const std::wstring categoryId = L"cat-legacy-storage";
    std::error_code fileError;
    std::filesystem::create_directories(configDirectory, fileError);
    std::filesystem::create_directories(desktopDirectory, fileError);
    std::filesystem::create_directories(managedDirectory / L"uncategorized", fileError);
    std::filesystem::create_directories(managedDirectory / categoryId, fileError);
    if (fileError) {
        std::wcerr << L"Category storage smoke setup failed\n";
        return 1;
    }
    {
        std::ofstream file(managedDirectory / L"uncategorized" / L"豆包.lnk", std::ios::binary);
        file << "uncategorized item";
    }
    {
        std::ofstream file(managedDirectory / categoryId / L"代码.txt", std::ios::binary);
        file << "category item";
    }

    const auto fail = [&](const std::wstring& message) {
        std::wcerr << message << L"\n";
        std::error_code cleanupError;
        std::filesystem::remove_all(testRoot, cleanupError);
        return 1;
    };
    if (!SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_CONFIG_DIR", configDirectory.c_str())) {
        return fail(L"Category storage config override failed");
    }

    ConfigStore configStore;
    AppConfig config;
    config.uncategorizedName = L"AI";
    config.uncategorizedStorageFolder = L"uncategorized";
    ItemConfig uncategorizedItem;
    uncategorizedItem.id = L"uncategorized-item";
    uncategorizedItem.path = (managedDirectory / L"uncategorized" / L"豆包.lnk").wstring();
    config.items.push_back(uncategorizedItem);
    config.uncategorizedItemIds.push_back(uncategorizedItem.id);
    ItemConfig categoryItem;
    categoryItem.id = L"category-item";
    categoryItem.path = (managedDirectory / categoryId / L"代码.txt").wstring();
    config.items.push_back(categoryItem);
    CategoryConfig category;
    category.id = categoryId;
    category.name = L"AI编程";
    category.storageFolder = categoryId;
    category.itemIds.push_back(categoryItem.id);
    config.categories.push_back(category);
    if (!configStore.SaveAppConfig(config)) {
        return fail(L"Legacy category storage config save failed");
    }

    CategoryStorageManager storageManager(configStore);
    std::wstring errorMessage;
    if (!storageManager.SynchronizeAll(errorMessage)) {
        return fail(L"Category metadata validation failed: " + errorMessage);
    }
    if (!storageManager.Rename(categoryId, L"CON / 编程工具", errorMessage)) {
        return fail(L"Category metadata rename failed: " + errorMessage);
    }
    const AppConfig renamed = configStore.LoadAppConfig();
    const auto renamedCategory = std::find_if(
        renamed.categories.begin(),
        renamed.categories.end(),
        [&](const CategoryConfig& value) { return value.id == categoryId; });
    const auto renamedItem = std::find_if(
        renamed.items.begin(),
        renamed.items.end(),
        [&](const ItemConfig& value) { return value.id == categoryItem.id; });
    if (renamedCategory == renamed.categories.end() ||
        renamedCategory->name != L"CON / 编程工具" ||
        renamedCategory->storageFolder != categoryId ||
        renamedItem == renamed.items.end() ||
        renamedItem->path != categoryItem.path ||
        !std::filesystem::exists(categoryItem.path) ||
        !std::filesystem::exists(uncategorizedItem.path)) {
        return fail(L"Category rename changed legacy storage paths or item identity");
    }
    if (storageManager.CanUseName(categoryId, L"AI", errorMessage) ||
        !storageManager.CanUseName(categoryId, L"CON", errorMessage)) {
        return fail(L"Category display-name validation still follows folder rules");
    }
    if (!storageManager.RemoveEmpty(categoryId, errorMessage) ||
        !std::filesystem::exists(categoryItem.path)) {
        return fail(L"Category removal validation touched a legacy source: " + errorMessage);
    }

    const std::filesystem::path preservedConfigPath =
        configDirectory / L"config.ini";
    const std::optional<std::string> preservedConfigBytes =
        ReadFileBytes(preservedConfigPath);
    if (!preservedConfigBytes.has_value()) {
        return fail(L"Category metadata no-op snapshot failed");
    }
    if (!storageManager.SynchronizeAll(errorMessage)) {
        return fail(L"Category metadata no-op validation failed: " + errorMessage);
    }
    const std::optional<std::string> after =
        ReadFileBytes(preservedConfigPath);
    if (!after.has_value() || *after != *preservedConfigBytes) {
        return fail(L"Category metadata validation rewrote config history");
    }

    std::filesystem::remove_all(testRoot, fileError);
    if (fileError) {
        std::wcerr << L"Category storage smoke cleanup failed\n";
        return 1;
    }
    std::wcout << L"Category metadata-only rename and validation passed\n";
    return 0;
}

int RunSmokeWidgetAlignment(HINSTANCE instance) {
    AttachParentConsole();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD required = GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
    if (required == 0) {
        std::wcerr << L"Explicit widget alignment smoke directory is required\n";
        return 1;
    }
    std::wstring baseValue(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        baseValue.data(),
        required);
    if (copied == 0 || copied >= required) {
        std::wcerr << L"Widget alignment smoke directory is invalid\n";
        return 1;
    }
    baseValue.resize(copied);
    const std::filesystem::path testRoot =
        std::filesystem::absolute(baseValue).lexically_normal() /
        (L"widget-alignment-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path configDirectory = testRoot / L"Config";
    const std::filesystem::path dataDirectory = testRoot / L"Data";
    const std::filesystem::path desktopDirectory = testRoot / L"Desktop";
    std::error_code fileError;
    std::filesystem::create_directories(configDirectory, fileError);
    std::filesystem::create_directories(dataDirectory, fileError);
    std::filesystem::create_directories(desktopDirectory, fileError);
    if (fileError ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_CONFIG_DIR", configDirectory.c_str()) ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DATA_DIR", dataDirectory.c_str()) ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DESKTOP_DIR", desktopDirectory.c_str())) {
        std::filesystem::remove_all(testRoot, fileError);
        std::wcerr << L"Widget alignment smoke setup failed\n";
        return 1;
    }

    const auto fail = [&](const std::wstring& message, int exitCode = 1) {
        std::wcerr << message << L"\n";
        ConfigStore::DrainPendingWrites(5000);
        std::error_code cleanupError;
        std::filesystem::remove_all(testRoot, cleanupError);
        return exitCode;
    };
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitorInfo)) {
        return fail(L"Widget alignment monitor lookup failed", 12);
    }
    const int firstX = monitorInfo.rcWork.left + 137;
    const int firstY = monitorInfo.rcWork.top + 123;
    const int secondX = firstX + 500;
    const int secondY = firstY + 40;
    AppConfig config;
    config.settings.startHidden = false;
    CategoryConfig firstCategory;
    firstCategory.id = L"alignment-first";
    firstCategory.name = L"对齐测试一";
    firstCategory.storageFolder = firstCategory.name;
    firstCategory.layout.x = firstX;
    firstCategory.layout.y = firstY;
    firstCategory.layout.width = 300;
    firstCategory.layout.height = 240;
    firstCategory.layout.normalHeight = 240;
    firstCategory.layout.collapsed = false;
    firstCategory.layout.locked = false;
    CategoryConfig secondCategory = firstCategory;
    secondCategory.id = L"alignment-second";
    secondCategory.name = L"对齐测试二";
    secondCategory.storageFolder = secondCategory.name;
    secondCategory.layout.x = secondX;
    secondCategory.layout.y = secondY;
    config.categories = {firstCategory, secondCategory};
    ConfigStore configStore;
    if (!configStore.SaveAppConfig(config)) {
        return fail(L"Widget alignment config save failed", 13);
    }

    WidgetWindow first(instance, nullptr, firstCategory.id, 0);
    WidgetWindow second(instance, nullptr, secondCategory.id, 0);
    if (!first.Create() || !second.Create()) {
        first.Close();
        second.Close();
        return fail(L"Widget alignment window creation failed", 14);
    }
    first.Show(SW_SHOWNOACTIVATE);
    second.Show(SW_SHOWNOACTIVATE);
    HWND firstWindow = WidgetWindowSmokeAccess::Window(first);
    HWND secondWindow = WidgetWindowSmokeAccess::Window(second);
    if (firstWindow == nullptr || secondWindow == nullptr) {
        first.Close();
        second.Close();
        return fail(L"Widget alignment windows not found", 15);
    }
    if (!IsWindowVisible(firstWindow) || !IsWindowVisible(secondWindow)) {
        first.Close();
        second.Close();
        return fail(L"Widget alignment windows are not visible", 23);
    }
    if (CountLatticeWidgetWindows() < 2) {
        first.Close();
        second.Close();
        return fail(L"Widget alignment window enumeration found fewer than two windows", 24);
    }
    RECT firstRect{};
    RECT secondRect{};
    GetWindowRect(firstWindow, &firstRect);
    GetWindowRect(secondWindow, &secondRect);
    const int firstWindowWidth = firstRect.right - firstRect.left;
    const int firstWindowHeight = firstRect.bottom - firstRect.top;
    const int secondWindowWidth = secondRect.right - secondRect.left;
    const int secondWindowHeight = secondRect.bottom - secondRect.top;
    SetWindowScreenBounds(
        firstWindow,
        monitorInfo.rcWork.left + 40,
        monitorInfo.rcWork.top + 80,
        firstWindowWidth,
        firstWindowHeight,
        SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowScreenBounds(
        secondWindow,
        monitorInfo.rcWork.right - secondWindowWidth - 40,
        monitorInfo.rcWork.top + 120,
        secondWindowWidth,
        secondWindowHeight,
        SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(firstWindow, &firstRect);
    GetWindowRect(secondWindow, &secondRect);
    const int dpi = static_cast<int>(std::max<UINT>(96, GetDpiForWindow(firstWindow)));
    const int alignmentDelta = std::max(1, MulDiv(3, dpi, 96));
    const int outsideAdjacencyThreshold = MulDiv(2, dpi, 96) + 1;

    RECT sizingRect = firstRect;
    sizingRect.right = secondRect.left - alignmentDelta;
    const int minimumWidth = MulDiv(260, dpi, 96);
    const int alignmentThreshold = MulDiv(4, dpi, 96);
    if (sizingRect.right - sizingRect.left < minimumWidth) {
        first.Close();
        second.Close();
        return fail(L"Resize smoke proposal is narrower than the widget minimum", 25);
    }
    if (std::abs(secondRect.left - sizingRect.right) > alignmentThreshold) {
        first.Close();
        second.Close();
        return fail(L"Resize smoke proposal is outside the alignment threshold", 26);
    }
    const LRESULT sizingResult = SendMessageW(
        firstWindow,
        WM_SIZING,
        WMSZ_RIGHT,
        reinterpret_cast<LPARAM>(&sizingRect));
    const int snappedSizingRight = reinterpret_cast<volatile RECT*>(&sizingRect)->right;
    bool verticalGuideVisible = false;
    struct GuideState {
        int expectedX;
        bool* visible;
    } guideState{secondRect.left - 1, &verticalGuideVisible};
    EnumWindows([](HWND candidate, LPARAM parameter) -> BOOL {
        wchar_t className[64]{};
        GetClassNameW(candidate, className, ARRAYSIZE(className));
        if (wcscmp(className, L"Lattice.AlignmentGuide") != 0 || !IsWindowVisible(candidate)) {
            return TRUE;
        }
        RECT rect{};
        GetWindowRect(candidate, &rect);
        auto* state = reinterpret_cast<GuideState*>(parameter);
        if (rect.left == state->expectedX && rect.right - rect.left == 2) {
            *state->visible = true;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&guideState));
    if (snappedSizingRight != secondRect.left) {
        first.Close();
        second.Close();
        if (sizingResult != TRUE) {
            return fail(L"Resize sizing message was not handled", 21);
        }
        if (snappedSizingRight == secondRect.left - alignmentDelta) {
            return fail(L"Resize sizing rectangle was handled but remained unchanged", 22);
        }
        return fail(L"Resize edge snapped to an unexpected coordinate", 16);
    }
    if (!verticalGuideVisible) {
        first.Close();
        second.Close();
        return fail(L"Resize guide line failed", 20);
    }
    SendMessageW(firstWindow, WM_EXITSIZEMOVE, 0, 0);

    const int firstWidth = firstRect.right - firstRect.left;
    const int firstHeight = firstRect.bottom - firstRect.top;
    RECT weakAdjacencyRect{
        secondRect.right + outsideAdjacencyThreshold,
        secondRect.top,
        secondRect.right + outsideAdjacencyThreshold + firstWidth,
        secondRect.top + firstHeight};
    const int unsnappedLeft = weakAdjacencyRect.left;
    SendMessageW(firstWindow, WM_MOVING, 0, reinterpret_cast<LPARAM>(&weakAdjacencyRect));
    const int weakAdjacencyLeft = reinterpret_cast<volatile RECT*>(&weakAdjacencyRect)->left;
    if (weakAdjacencyLeft != unsnappedLeft) {
        first.Close();
        second.Close();
        return fail(L"Adjacent edge snap is still too strong", 17);
    }
    SendMessageW(firstWindow, WM_EXITSIZEMOVE, 0, 0);

    RECT alignedWithGap{
        secondRect.left + alignmentDelta,
        secondRect.bottom + 24,
        secondRect.left + alignmentDelta + firstWidth,
        secondRect.bottom + 24 + firstHeight};
    SendMessageW(firstWindow, WM_MOVING, 0, reinterpret_cast<LPARAM>(&alignedWithGap));
    const volatile RECT* alignedResult = reinterpret_cast<volatile RECT*>(&alignedWithGap);
    if (alignedResult->left != secondRect.left || alignedResult->top != secondRect.bottom + 24) {
        first.Close();
        second.Close();
        return fail(L"Independent edge alignment with a gap failed", 18);
    }
    SendMessageW(firstWindow, WM_EXITSIZEMOVE, 0, 0);
    first.Close();
    second.Close();
    if (!ConfigStore::DrainPendingWrites(5000)) {
        return fail(L"Widget alignment async persistence did not drain", 27);
    }
    std::filesystem::remove_all(testRoot, fileError);
    if (fileError) {
        std::wcerr << L"Widget alignment smoke cleanup failed\n";
        return 1;
    }
    std::wcout << L"Resize snap, guide line, weak adjacency, and independent edge alignment passed\n";
    return 0;
}

int RunSmokeWidgetDesktopLayer(HINSTANCE instance) {
    AttachParentConsole();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD required = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
    if (required == 0) {
        std::wcerr << L"Explicit widget desktop layer smoke directory is required\n";
        return 1;
    }
    std::wstring baseValue(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        baseValue.data(),
        required);
    if (copied == 0 || copied >= required) {
        std::wcerr << L"Widget desktop layer smoke directory is invalid\n";
        return 1;
    }
    baseValue.resize(copied);

    const std::filesystem::path testRoot =
        std::filesystem::absolute(baseValue).lexically_normal() /
        (L"widget-desktop-layer-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const std::filesystem::path configRoot = testRoot / L"Config";
    const std::filesystem::path dataRoot = testRoot / L"Data";
    const std::filesystem::path desktopRoot = testRoot / L"Desktop";
    std::error_code fileError;
    for (const std::filesystem::path& directory :
         std::array<std::filesystem::path, 3>{configRoot, dataRoot, desktopRoot}) {
        std::filesystem::create_directories(directory, fileError);
        if (fileError) {
            std::filesystem::remove_all(testRoot, fileError);
            std::wcerr << L"Widget desktop layer directory setup failed\n";
            return 60;
        }
    }
    if (!SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_CONFIG_DIR", configRoot.c_str()) ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DATA_DIR", dataRoot.c_str()) ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DESKTOP_DIR", desktopRoot.c_str())) {
        std::filesystem::remove_all(testRoot, fileError);
        std::wcerr << L"Widget desktop layer environment setup failed\n";
        return 60;
    }

    constexpr int targetWidth = 320;
    constexpr int targetHeight = 240;
    POINT targetPosition{};
    RECT captureRect{};
    if (!FindUncoveredDesktopPlacement(
            targetWidth,
            targetHeight,
            targetPosition,
            captureRect)) {
        std::filesystem::remove_all(testRoot, fileError);
        std::wcerr << L"No uncovered desktop area was available for the pixel test\n";
        return 61;
    }
    const int targetX = targetPosition.x;
    const int targetY = targetPosition.y;
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = MonitorFromPoint(
        targetPosition, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        std::filesystem::remove_all(testRoot, fileError);
        std::wcerr << L"Widget desktop layer monitor lookup failed\n";
        return 61;
    }
    AppConfig config;
    config.settings.showPublicDesktopItems = false;
    CategoryConfig category;
    category.id = L"desktop-layer-category";
    category.name = L"桌面层级测试";
    category.storageFolder = category.name;
    category.layout.x = targetX;
    category.layout.y = targetY;
    category.layout.width = targetWidth;
    category.layout.height = targetHeight;
    category.layout.normalHeight = targetHeight;
    category.layout.monitorId = monitorInfo.szDevice;
    config.categories.push_back(category);
    CategoryConfig siblingCategory = category;
    siblingCategory.id = L"desktop-layer-sibling";
    siblingCategory.name = L"桌面层级重叠测试";
    siblingCategory.storageFolder = siblingCategory.name;
    siblingCategory.layout.x = targetX + 48;
    // Keep the sibling overlap probe inside the same 160x64 desktop patch
    // that FindUncoveredDesktopPlacement validated above.
    siblingCategory.layout.y = targetY + 32;
    siblingCategory.layout.width = 220;
    siblingCategory.layout.height = 32;
    siblingCategory.layout.normalHeight = 180;
    siblingCategory.layout.collapsed = true;
    config.categories.push_back(siblingCategory);
    ConfigStore configStore;
    if (!configStore.SaveAppConfig(config)) {
        std::filesystem::remove_all(testRoot, fileError);
        std::wcerr << L"Widget desktop layer config save failed\n";
        return 62;
    }
    constexpr wchar_t kCoverClassName[] = L"Lattice.SmokeNormalCoverWindow";
    WNDCLASSEXW coverClass{};
    coverClass.cbSize = sizeof(coverClass);
    coverClass.lpfnWndProc = DefWindowProcW;
    coverClass.hInstance = instance;
    coverClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    coverClass.lpszClassName = kCoverClassName;
    if (RegisterClassExW(&coverClass) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::filesystem::remove_all(testRoot, fileError);
        std::wcerr << L"Widget desktop layer cover class registration failed\n";
        return 63;
    }
    HWND coverWindow = CreateWindowExW(
        0,
        kCoverClassName,
        L"Lattice normal application cover",
        WS_POPUP,
        targetX,
        targetY,
        targetWidth,
        targetHeight,
        nullptr,
        nullptr,
        instance,
        nullptr);
    std::unique_ptr<WidgetWindow> widget;
    std::unique_ptr<WidgetWindow> siblingWidget;
    const auto cleanup = [&]() {
        if (siblingWidget != nullptr) {
            siblingWidget->Close();
            siblingWidget.reset();
        }
        if (widget != nullptr) {
            widget->Close();
            widget.reset();
        }
        if (coverWindow != nullptr && IsWindow(coverWindow) != FALSE) {
            DestroyWindow(coverWindow);
        }
        std::error_code cleanupError;
        std::filesystem::remove_all(testRoot, cleanupError);
    };
    const auto fail = [&](const wchar_t* message, int exitCode) {
        std::ofstream diagnostic(
            std::filesystem::path(baseValue) /
                L"widget-desktop-layer-failure.txt",
            std::ios::trunc);
        diagnostic << "EXIT_CODE=" << exitCode << "\n"
                   << "MESSAGE=" << Utf8Text(message) << "\n";
        diagnostic.flush();
        std::wcerr << message << L"\n";
        cleanup();
        return exitCode;
    };
    if (coverWindow == nullptr ||
        (GetWindowLongPtrW(coverWindow, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0 ||
        !SetWindowPos(
            coverWindow,
            HWND_TOP,
            targetX,
            targetY,
            targetWidth,
            targetHeight,
            SWP_SHOWWINDOW)) {
        return fail(L"Normal cover window setup failed", 64);
    }
    SetForegroundWindow(coverWindow);
    UpdateWindow(coverWindow);

    widget = std::make_unique<WidgetWindow>(
        instance, nullptr, category.id, 0);
    if (!widget->Create()) {
        return fail(L"Desktop-hosted widget creation failed", 65);
    }
    widget->Show(SW_SHOWNOACTIVATE);
    const HWND widgetWindow = WidgetWindowSmokeAccess::Window(*widget);
    const HWND desktopHost = WidgetWindowSmokeAccess::DesktopHost(*widget);
    wchar_t hostClass[64]{};
    if (widgetWindow == nullptr ||
        desktopHost == nullptr ||
        !WidgetWindowSmokeAccess::IsDesktopHosted(*widget) ||
        GetWindow(widgetWindow, GW_OWNER) != desktopHost ||
        GetAncestor(desktopHost, GA_ROOT) != desktopHost ||
        GetClassNameW(desktopHost, hostClass, ARRAYSIZE(hostClass)) == 0 ||
        (wcscmp(hostClass, L"Progman") != 0 &&
         wcscmp(hostClass, L"WorkerW") != 0) ||
        (GetWindowLongPtrW(widgetWindow, GWL_STYLE) & WS_POPUP) == 0 ||
        (GetWindowLongPtrW(widgetWindow, GWL_STYLE) & WS_CHILD) != 0 ||
        (GetWindowLongPtrW(widgetWindow, GWL_EXSTYLE) & WS_EX_LAYERED) != 0 ||
        (GetWindowLongPtrW(widgetWindow, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0 ||
        FindLatticeWidgetWindow(category.name.c_str()) != widgetWindow) {
        return fail(L"Widget was not attached to the Explorer desktop host", 66);
    }

    RECT widgetRect{};
    if (!GetWindowRect(widgetWindow, &widgetRect) ||
        widgetRect.left != targetX ||
        widgetRect.top != targetY ||
        widgetRect.right - widgetRect.left != targetWidth ||
        widgetRect.bottom - widgetRect.top != targetHeight) {
        return fail(L"Desktop-hosted widget screen coordinates changed", 67);
    }
    // Keep every z-order assertion inside the same desktop-only patch that
    // FindUncoveredDesktopPlacement validated and CaptureScreenPixels samples.
    // The rest of the widget may legitimately sit below another application's
    // transparent or partially overlapping window.
    POINT overlapPoint{
        (captureRect.left + captureRect.right) / 2,
        (captureRect.top + captureRect.bottom) / 2};
    HWND overlapWindow = WindowFromPoint(overlapPoint);
    if (overlapWindow == nullptr ||
        GetAncestor(overlapWindow, GA_ROOT) != coverWindow) {
        return fail(L"New widget covered an already open normal application", 68);
    }
    widget->SetVisible(false);
    widget->SetVisible(true);
    overlapWindow = WindowFromPoint(overlapPoint);
    if (overlapWindow == nullptr ||
        GetAncestor(overlapWindow, GA_ROOT) != coverWindow) {
        return fail(L"Showing the widget raised it above a normal application", 69);
    }

    const auto settleFrame = [](DWORD milliseconds) {
        const ULONGLONG end = GetTickCount64() + milliseconds;
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
        } while (GetTickCount64() < end);
        DwmFlush();
    };
    ShowWindow(coverWindow, SW_HIDE);
    widget->SetVisible(false);
    settleFrame(300);
    DwmFlush();
    std::vector<std::uint32_t> desktopPixels;
    if (!CaptureScreenPixels(captureRect, desktopPixels)) {
        return fail(L"Desktop pixels could not be captured with the widget hidden", 70);
    }
    widget->SetVisible(true);
    // The synchronous show paint can be submitted while DWM still has the
    // old covered/hidden state. Request the sampled frame after that state
    // transition, rather than only waiting after the early paint.
    settleFrame(300);
    RedrawWindow(
        widgetWindow,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME);
    DwmFlush();
    std::vector<std::uint32_t> widgetPixels;
    if (!CaptureScreenPixels(captureRect, widgetPixels)) {
        return fail(L"Desktop pixels could not be captured with the widget visible", 71);
    }
    const size_t visiblePixelDifferences =
        CountVisiblePixelDifferences(desktopPixels, widgetPixels);
    std::wcout << L"Desktop visible pixel differences: "
               << visiblePixelDifferences << L"/" << widgetPixels.size()
               << L"\n";
    if (visiblePixelDifferences < widgetPixels.size() / 100) {
        const auto outputRoot = std::filesystem::path(baseValue);
        std::wofstream diagnostic(outputRoot / L"widget-pixels.txt");
        diagnostic << L"widget=" << widgetWindow << L" pid=" << GetCurrentProcessId()
                   << L" rect=" << widgetRect.left << L"," << widgetRect.top
                   << L"," << widgetRect.right << L"," << widgetRect.bottom << L"\n";
        const POINT probe{captureRect.left + 4, captureRect.top + 4};
        HWND hit = WindowFromPoint(probe);
        wchar_t hitClass[128]{};
        DWORD hitPid = 0;
        GetClassNameW(hit, hitClass, ARRAYSIZE(hitClass));
        GetWindowThreadProcessId(hit, &hitPid);
        diagnostic << L"hit=" << hit << L" class=" << hitClass << L" pid=" << hitPid
                   << L" visible=" << IsWindowVisible(widgetWindow) << L"\n";
        DWORD cloaked = 0;
        const HRESULT cloakResult = DwmGetWindowAttribute(
            widgetWindow, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        diagnostic << L"cloakResult=" << cloakResult << L" cloaked=" << cloaked << L"\n";
        WidgetWindowSmokeAccess::WriteRenderState(*widget, diagnostic);
        const LONG width = captureRect.right - captureRect.left;
        const LONG height = captureRect.bottom - captureRect.top;
        BITMAPFILEHEADER fileHeader{};
        BITMAPINFOHEADER info{};
        fileHeader.bfType = 0x4D42;
        fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(info);
        fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(widgetPixels.size() * 4);
        info.biSize = sizeof(info);
        info.biWidth = width;
        info.biHeight = -height;
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        std::ofstream bitmap(outputRoot / L"widget-pixels.bmp", std::ios::binary);
        bitmap.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
        bitmap.write(reinterpret_cast<const char*>(&info), sizeof(info));
        bitmap.write(reinterpret_cast<const char*>(widgetPixels.data()),
            static_cast<std::streamsize>(widgetPixels.size() * 4));
        return fail(L"Desktop-hosted widget did not produce visible screen pixels", 71);
    }
    const HWND visibleWindow = WindowFromPoint(overlapPoint);
    if (visibleWindow == nullptr ||
        GetAncestor(visibleWindow, GA_ROOT) != widgetWindow) {
        const HWND visibleRoot = visibleWindow == nullptr
            ? nullptr
            : GetAncestor(visibleWindow, GA_ROOT);
        wchar_t visibleClass[128]{};
        wchar_t rootClass[128]{};
        DWORD visiblePid = 0;
        DWORD rootPid = 0;
        if (visibleWindow != nullptr) {
            GetClassNameW(visibleWindow, visibleClass, ARRAYSIZE(visibleClass));
            GetWindowThreadProcessId(visibleWindow, &visiblePid);
        }
        if (visibleRoot != nullptr) {
            GetClassNameW(visibleRoot, rootClass, ARRAYSIZE(rootClass));
            GetWindowThreadProcessId(visibleRoot, &rootPid);
        }
        std::wcerr << L"WindowFromPoint diagnostics: point=("
                   << overlapPoint.x << L"," << overlapPoint.y
                   << L") hit=" << visibleWindow
                   << L" class=" << visibleClass
                   << L" pid=" << visiblePid
                   << L" root=" << visibleRoot
                   << L" rootClass=" << rootClass
                   << L" rootPid=" << rootPid
                   << L" widget=" << widgetWindow
                   << L" desktopHost=" << desktopHost << L"\n";
        return fail(L"Widget was not visible above the desktop root", 72);
    }

    // Compare a background-only strip below the title glyphs, above the
    // collapsed bottom border. Read actual screen pixels, not PrintWindow.
    if (!WidgetWindowSmokeAccess::PrepareWallpaper(*widget)) {
        return fail(L"Collapse pixel probe requires a loaded full-height wallpaper", 78);
    }
    const LONG headerHeight = MulDiv(32, GetDpiForWindow(widgetWindow), 96);
    const RECT titleStrip{
        (std::max)(widgetRect.left + 60, captureRect.left + 2),
        widgetRect.top + headerHeight - 6,
        (std::min)(widgetRect.right - 60, captureRect.right - 2),
        widgetRect.top + headerHeight - 4};
    const auto captureTitle = [&](std::vector<std::uint32_t>& pixels) {
        RedrawWindow(widgetWindow, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        settleFrame(100);
        for (LONG x = titleStrip.left; x < titleStrip.right; ++x) {
            const HWND hit = WindowFromPoint(POINT{x, titleStrip.top});
            if (hit != widgetWindow) {
                const HWND root = hit == nullptr
                    ? nullptr
                    : GetAncestor(hit, GA_ROOT);
                wchar_t hitClass[128]{};
                wchar_t rootClass[128]{};
                DWORD hitPid = 0;
                DWORD rootPid = 0;
                if (hit != nullptr) {
                    GetClassNameW(hit, hitClass, ARRAYSIZE(hitClass));
                    GetWindowThreadProcessId(hit, &hitPid);
                }
                if (root != nullptr) {
                    GetClassNameW(root, rootClass, ARRAYSIZE(rootClass));
                    GetWindowThreadProcessId(root, &rootPid);
                }
                std::wofstream diagnostic(
                    std::filesystem::path(baseValue) /
                        L"widget-desktop-layer-obstruction.txt");
                diagnostic << L"POINT=" << x << L"," << titleStrip.top
                           << L"\nHIT=" << hit
                           << L"\nHIT_CLASS=" << hitClass
                           << L"\nHIT_PID=" << hitPid
                           << L"\nROOT=" << root
                           << L"\nROOT_CLASS=" << rootClass
                           << L"\nROOT_PID=" << rootPid
                           << L"\nWIDGET=" << widgetWindow << L"\n";
                diagnostic.flush();
                return false;
            }
        }
        return CaptureScreenPixels(titleStrip, pixels);
    };
    std::vector<std::uint32_t> expandedTitle;
    std::vector<std::uint32_t> collapsedTitle;
    std::vector<std::uint32_t> restoredTitle;
    if (!captureTitle(expandedTitle)) {
        return fail(L"Expanded title pixel probe is obscured or unavailable", 78);
    }
    WidgetWindowSmokeAccess::ToggleCollapsed(*widget);
    if (!captureTitle(collapsedTitle)) {
        return fail(L"Collapsed title pixel probe is obscured or unavailable", 78);
    }
    WidgetWindowSmokeAccess::ToggleCollapsed(*widget);
    if (!captureTitle(restoredTitle) || expandedTitle != collapsedTitle ||
        expandedTitle != restoredTitle) {
        const auto differentPixels = [](
            const std::vector<std::uint32_t>& left,
            const std::vector<std::uint32_t>& right) {
            const size_t common = (std::min)(left.size(), right.size());
            size_t different = left.size() > right.size()
                ? left.size() - right.size()
                : right.size() - left.size();
            for (size_t index = 0; index < common; ++index) {
                if (left[index] != right[index]) ++different;
            }
            return different;
        };
        std::ofstream diagnostic(
            std::filesystem::path(baseValue) /
                L"widget-desktop-layer-pixels.txt",
            std::ios::trunc);
        diagnostic << "EXPANDED_COLLAPSED_DIFF="
                   << differentPixels(expandedTitle, collapsedTitle) << "\n"
                   << "EXPANDED_RESTORED_DIFF="
                   << differentPixels(expandedTitle, restoredTitle) << "\n"
                   << "EXPANDED_COUNT=" << expandedTitle.size() << "\n"
                   << "COLLAPSED_COUNT=" << collapsedTitle.size() << "\n"
                   << "RESTORED_COUNT=" << restoredTitle.size() << "\n";
        diagnostic.flush();
        return fail(L"Collapse changed the real title background pixels", 78);
    }
    std::wcout << L"Collapse title background pixel equality passed: "
               << expandedTitle.size() << L" pixels across three states\n";

    siblingWidget = std::make_unique<WidgetWindow>(
        instance, nullptr, siblingCategory.id, 0);
    if (!siblingWidget->Create()) {
        return fail(L"Overlapping sibling widget creation failed", 75);
    }
    siblingWidget->Show(SW_SHOWNOACTIVATE);
    const HWND siblingWindow = WidgetWindowSmokeAccess::Window(*siblingWidget);
    POINT siblingOverlapPoint{
        siblingCategory.layout.x + siblingCategory.layout.width / 2,
        siblingCategory.layout.y + 16};
    if (siblingWindow == nullptr ||
        !SetWindowPos(
            widgetWindow,
            siblingWindow,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                SWP_NOOWNERZORDER) ||
        GetAncestor(WindowFromPoint(siblingOverlapPoint), GA_ROOT) !=
            siblingWindow) {
        return fail(L"Overlap precondition did not place the target widget underneath", 75);
    }
    WidgetWindowSmokeAccess::ToggleCollapsed(*widget);
    if (!SetWindowPos(
            coverWindow,
            HWND_TOP,
            targetX,
            targetY,
            targetWidth,
            targetHeight,
            SWP_SHOWWINDOW | SWP_NOACTIVATE)) {
        return fail(L"Normal cover could not be restored for overlap testing", 76);
    }
    WidgetWindowSmokeAccess::ToggleCollapsed(*widget);
    HWND promotedOverlapWindow = WindowFromPoint(siblingOverlapPoint);
    if (promotedOverlapWindow == nullptr ||
        GetAncestor(promotedOverlapWindow, GA_ROOT) != coverWindow) {
        return fail(L"Expanding a widget raised it above a normal application", 76);
    }
    ShowWindow(coverWindow, SW_HIDE);
    promotedOverlapWindow = WindowFromPoint(siblingOverlapPoint);
    if (promotedOverlapWindow == nullptr ||
        GetAncestor(promotedOverlapWindow, GA_ROOT) != widgetWindow) {
        return fail(L"Expanded widget did not cover an overlapping sibling widget", 77);
    }
    siblingWidget->Close();
    siblingWidget.reset();

    constexpr int moveX = 41;
    constexpr int moveY = 29;
    if (!WidgetWindowSmokeAccess::SetScreenBounds(
            *widget,
            targetX + moveX,
            targetY + moveY,
            targetWidth,
            targetHeight,
            SWP_NOZORDER | SWP_NOACTIVATE) ||
        !GetWindowRect(widgetWindow, &widgetRect) ||
        widgetRect.left != targetX + moveX ||
        widgetRect.top != targetY + moveY ||
        SendMessageW(widgetWindow, WM_MOUSEACTIVATE, 0, 0) != MA_ACTIVATE) {
        return fail(L"Desktop-hosted widget coordinate or activation behavior failed", 73);
    }

    if (!WidgetWindowSmokeAccess::FlushInteractionSave(*widget) ||
        !ConfigStore::DrainPendingWrites(5000)) {
        return fail(L"Widget desktop layer pending state did not drain before close snapshot", 74);
    }
    const std::optional<std::string> configBeforeClose =
        ReadFileBytes(configStore.ConfigPath());
    if (!configBeforeClose.has_value()) {
        return fail(L"Widget desktop layer close snapshot failed", 74);
    }
    widget->Close();
    widget.reset();
    const std::optional<std::string> configAfterWindowClose =
        ReadFileBytes(configStore.ConfigPath());
    if (!configAfterWindowClose.has_value() ||
        *configAfterWindowClose != *configBeforeClose) {
        return fail(L"Closing a widget rewrote persisted user state", 74);
    }

    cleanup();
    std::wcout << L"Desktop host, normal-window coverage, sibling expansion Z-order, screen coordinates, and close preservation passed\n";
    return 0;
}

int RunSmokeDesktopIconFidelity(HINSTANCE instance) {
    AttachParentConsole();
    SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD outputRequired = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
    if (outputRequired == 0) {
        std::wcerr << L"Desktop icon fidelity output directory missing\n";
        return 136;
    }
    std::wstring outputRoot(outputRequired, L'\0');
    const DWORD outputCopied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        outputRoot.data(),
        outputRequired);
    if (outputCopied == 0 || outputCopied >= outputRequired) {
        std::wcerr << L"Desktop icon fidelity output directory invalid\n";
        return 136;
    }
    outputRoot.resize(outputCopied);
    DesktopLayout layout;
    DesktopViewSnapshot before;
    std::vector<DesktopPosition> positionsBefore;
    std::wstring errorMessage;
    if (!layout.CaptureViewSnapshot(before, errorMessage)) {
        std::ofstream diagnostic(
            std::filesystem::path(outputRoot) /
                L"desktop-icon-fidelity-error.txt",
            std::ios::trunc);
        diagnostic << "STAGE=CaptureViewSnapshot\n"
                   << "ERROR=" << Utf8Text(errorMessage) << "\n";
        std::wcerr << errorMessage << L"\n";
        return 130;
    }
    if (!layout.CaptureAllPositions(positionsBefore, errorMessage)) {
        std::ofstream diagnostic(
            std::filesystem::path(outputRoot) /
                L"desktop-icon-fidelity-error.txt",
            std::ios::trunc);
        diagnostic << "STAGE=CaptureAllPositions\n"
                   << "ERROR=" << Utf8Text(errorMessage) << "\n";
        std::wcerr << errorMessage << L"\n";
        return 130;
    }
    const std::filesystem::path nativeCapture =
        std::filesystem::path(outputRoot) / L"desktop-native.bmp";
    const std::filesystem::path latticeCapture =
        std::filesystem::path(outputRoot) / L"desktop-lattice.bmp";
    const std::filesystem::path rotatedIndexCapture =
        std::filesystem::path(outputRoot) /
            L"desktop-lattice-rotated-view-index.bmp";
    const bool nativeCaptureAvailable =
        SaveWindowClientBmp(before.listViewWindow, nativeCapture);
    DesktopSurfaceWindow surface(instance);
    if (!surface.Create({}, errorMessage)) {
        std::wcerr << errorMessage << L"\n";
        return 131;
    }
    {
        std::ofstream metrics(
            std::filesystem::path(outputRoot) /
                L"desktop-icon-metrics.txt",
            std::ios::trunc);
        metrics
            << "VIEW_ICON_LOGICAL=" << before.viewIconSize << "\n"
            << "LISTVIEW_DPI="
            << GetDpiForWindow(before.listViewWindow) << "\n"
            << "SURFACE_ICON_PHYSICAL="
            << DesktopSurfaceWindowSmokeAccess::IconSize(surface) << "\n"
            << "CELL_WIDTH="
            << DesktopSurfaceWindowSmokeAccess::CellWidth(surface) << "\n"
            << "CELL_HEIGHT="
            << DesktopSurfaceWindowSmokeAccess::CellHeight(surface) << "\n";
        for (size_t index = 0;
             index < std::min<size_t>(before.items.size(), 24);
             ++index) {
            metrics
                << "ITEM_" << index << "="
                << before.items[index].screenPoint.x << ","
                << before.items[index].screenPoint.y << ","
                << before.items[index].systemImageIndex << ","
                << before.items[index].overlayIndex << ","
                << Utf8Text(before.items[index].displayName) << "\n";
        }
    }
    if (!DesktopSurfaceWindowSmokeAccess::UsesPhysicalPixelGeometry(
            surface) ||
        !DesktopSurfaceWindowSmokeAccess::HasShellImageIdentity(
            surface)) {
        const size_t missing =
            DesktopSurfaceWindowSmokeAccess::MissingShellImageIdentityCount(
                surface);
        surface.Close();
        std::wcerr
            << L"Desktop icon fidelity geometry/identity failed: missing="
            << missing << L"\n";
        return 132;
    }
    surface.Show();
    const ULONGLONG iconDeadline = GetTickCount64() + 5000;
    while (!DesktopSurfaceWindowSmokeAccess::AllVisibleIconsReady(
               surface) &&
           GetTickCount64() < iconDeadline) {
        MSG message{};
        while (PeekMessageW(
                &message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(
            0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    if (!DesktopSurfaceWindowSmokeAccess::AllVisibleIconsReady(
            surface)) {
        const auto unready =
            DesktopSurfaceWindowSmokeAccess::UnreadyVisibleIconPaths(
                surface);
        std::ofstream diagnostic(
            std::filesystem::path(outputRoot) /
                L"desktop-icon-fidelity-error.txt",
            std::ios::trunc);
        diagnostic << "STAGE=WaitInitialIcons\n";
        for (const std::wstring& path : unready) {
            diagnostic << "UNREADY=" << Utf8Text(path) << "\n";
        }
        surface.Close();
        std::wcerr
            << L"Desktop icon fidelity left a Shell item on a placeholder\n";
        return 133;
    }
    RedrawWindow(
        surface.Window(),
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
    if (!SaveWindowClientBmp(
            surface.Window(), latticeCapture)) {
        surface.Close();
        std::wcerr << L"Could not capture Lattice desktop surface\n";
        return 138;
    }
    const std::optional<std::string> initialLatticePixels =
        ReadFileBytes(latticeCapture.wstring());
    const size_t rotatedIndexCount =
        DesktopSurfaceWindowSmokeAccess::
            ReloadWithRotatedExplorerImageIndices(surface);
    if (!initialLatticePixels.has_value() || rotatedIndexCount == 0) {
        surface.Close();
        std::wcerr
            << L"Desktop icon fidelity could not perturb Explorer view indices\n";
        return 139;
    }
    const ULONGLONG reloadDeadline = GetTickCount64() + 5000;
    while (!DesktopSurfaceWindowSmokeAccess::AllVisibleIconsReady(
               surface) &&
           GetTickCount64() < reloadDeadline) {
        MSG message{};
        while (PeekMessageW(
                &message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(
            0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    RedrawWindow(
        surface.Window(),
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
    const bool reloadedIconsReady =
        DesktopSurfaceWindowSmokeAccess::AllVisibleIconsReady(surface);
    const bool reloadedCaptureSaved = reloadedIconsReady &&
        SaveWindowClientBmp(
            surface.Window(), rotatedIndexCapture);
    const std::optional<std::string> reloadedLatticePixels =
        reloadedCaptureSaved
            ? ReadFileBytes(rotatedIndexCapture.wstring())
            : std::nullopt;
    if (!reloadedLatticePixels.has_value() ||
        *reloadedLatticePixels != *initialLatticePixels) {
        surface.Close();
        std::wcerr
            << L"Desktop icons changed when Explorer view indices were rotated\n";
        return 139;
    }
    surface.Close();
    DesktopViewSnapshot after;
    std::vector<DesktopPosition> positionsAfter;
    if (!layout.CaptureViewSnapshot(after, errorMessage) ||
        !layout.CaptureAllPositions(positionsAfter, errorMessage) ||
        before.viewFlags != after.viewFlags ||
        positionsBefore.size() != positionsAfter.size()) {
        std::wcerr
            << L"Desktop icon fidelity changed Explorer snapshot\n";
        return 134;
    }
    const bool positionsStable = std::all_of(
        positionsBefore.begin(),
        positionsBefore.end(),
        [&](const DesktopPosition& expected) {
            return std::any_of(
                positionsAfter.begin(),
                positionsAfter.end(),
                [&](const DesktopPosition& actual) {
                    return CompareStringOrdinal(
                               expected.path.c_str(), -1,
                               actual.path.c_str(), -1, TRUE) ==
                            CSTR_EQUAL &&
                        expected.point.x == actual.point.x &&
                        expected.point.y == actual.point.y;
                });
        });
    if (!positionsStable) {
        std::wcerr
            << L"Desktop icon fidelity changed Explorer coordinates\n";
        return 135;
    }
    std::wcout
        << L"Desktop icon fidelity passed: items="
        << before.items.size()
        << L", view-icon-px=" << before.viewIconSize
        << L", desktop-dpi="
        << GetDpiForWindow(before.listViewWindow)
        << L", native-capture="
        << (nativeCaptureAvailable ? 1 : 0)
        << L", rotated-view-indices="
        << rotatedIndexCount
        << L"\n";
    return 0;
}

bool DesktopContextMenuHasCanonicalRename(
    HWND ownerWindow,
    const ShellItemReference& item) {
    Microsoft::WRL::ComPtr<IContextMenu> contextMenu;
    if (FAILED(CreateDesktopShellSelectionObject(
            ownerWindow,
            {item},
            IID_IContextMenu,
            reinterpret_cast<void**>(
                contextMenu.GetAddressOf()))) ||
        contextMenu == nullptr) {
        return false;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return false;
    }
    constexpr UINT kFirstCommand = 1;
    constexpr UINT kLastCommand = 0x7FFF;
    const HRESULT queryResult = contextMenu->QueryContextMenu(
        menu,
        0,
        kFirstCommand,
        kLastCommand,
        CMF_NORMAL | CMF_CANRENAME);
    bool found = false;
    if (SUCCEEDED(queryResult)) {
        const UINT commandCount = (std::min)(
            static_cast<UINT>(HRESULT_CODE(queryResult)),
            kLastCommand - kFirstCommand + 1);
        for (UINT offset = 0; offset < commandCount; ++offset) {
            wchar_t verb[128]{};
            if (SUCCEEDED(contextMenu->GetCommandString(
                    offset,
                    GCS_VERBW,
                    nullptr,
                    reinterpret_cast<LPSTR>(verb),
                    ARRAYSIZE(verb) - 1)) &&
                CompareStringOrdinal(
                    verb, -1, L"rename", -1, TRUE) ==
                    CSTR_EQUAL) {
                found = true;
                break;
            }
            char ansiVerb[128]{};
            if (SUCCEEDED(contextMenu->GetCommandString(
                    offset,
                    GCS_VERBA,
                    nullptr,
                    ansiVerb,
                    ARRAYSIZE(ansiVerb) - 1)) &&
                lstrcmpiA(ansiVerb, "rename") == 0) {
                found = true;
                break;
            }
        }
    }
    DestroyMenu(menu);
    return found;
}

int RunSmokeDesktopDisplayTakeover(HINSTANCE instance) {
    AttachParentConsole();
    SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t desktopRenameBuffer[32768]{};
    const DWORD desktopRenameLength = GetEnvironmentVariableW(
        L"LATTICE_SMOKE_DESKTOP_RENAME_PATH",
        desktopRenameBuffer,
        ARRAYSIZE(desktopRenameBuffer));
    if (desktopRenameLength >= ARRAYSIZE(desktopRenameBuffer)) {
        std::wcerr << L"Desktop takeover rename path is invalid\n";
        return 207;
    }
    const bool hasDesktopRenameFixture = desktopRenameLength != 0;
    const std::wstring desktopRenamePath = hasDesktopRenameFixture
        ? std::wstring(desktopRenameBuffer, desktopRenameLength)
        : std::wstring{};
    if (hasDesktopRenameFixture &&
        CompareStringOrdinal(
            std::filesystem::path(desktopRenamePath).extension().c_str(),
            -1, L".lnk", -1, TRUE) != CSTR_EQUAL) {
        std::wcerr << L"Desktop takeover rename fixture must be a .lnk\n";
        return 207;
    }
    DesktopLayout layout;
    DesktopViewSnapshot before;
    std::wstring errorMessage;
    if (!layout.CaptureViewSnapshot(before, errorMessage)) {
        const DWORD required = GetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
        if (required != 0) {
            std::wstring root(required, L'\0');
            const DWORD copied = GetEnvironmentVariableW(
                L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
                root.data(), required);
            if (copied != 0 && copied < required) {
                root.resize(copied);
                std::ofstream diagnostic(
                    std::filesystem::path(root) /
                        L"desktop-takeover-capture-error.txt",
                    std::ios::trunc);
                diagnostic << Utf8Text(errorMessage) << "\n";
            }
        }
        std::wcerr << errorMessage << L"\n";
        return 101;
    }
    std::vector<DesktopPosition> positionsBefore;
    if (!layout.CaptureAllPositions(
            positionsBefore, errorMessage)) {
        std::wcerr << errorMessage << L"\n";
        return 102;
    }
    std::vector<std::wstring> assigned;
    POINT assignedProbe{};
    const auto assignedItem = std::find_if(
        before.items.begin(), before.items.end(),
        [&](const DesktopViewItem& item) {
            return !hasDesktopRenameFixture ||
                CompareStringOrdinal(
                    item.path.c_str(), -1,
                    desktopRenamePath.c_str(), -1,
                    TRUE) != CSTR_EQUAL;
        });
    if (assignedItem != before.items.end()) {
        const DesktopViewItem& item = *assignedItem;
        assigned.push_back(item.path);
        assignedProbe = POINT{
            item.screenPoint.x + 12,
            item.screenPoint.y + 12};
    } else if (!before.items.empty()) {
        assignedProbe = POINT{
            before.items.front().screenPoint.x + 12,
            before.items.front().screenPoint.y + 12};
    }
    DesktopSurfaceWindow surface(instance);
    if (!surface.Create(assigned, errorMessage)) {
        std::wcerr << errorMessage << L"\n";
        return 103;
    }
    const HWND surfaceWindow = surface.Window();
    const DesktopViewSnapshot created = surface.Snapshot();
    const LONG_PTR style =
        GetWindowLongPtrW(surfaceWindow, GWL_STYLE);
    const LONG_PTR exStyle =
        GetWindowLongPtrW(surfaceWindow, GWL_EXSTYLE);
    wchar_t hostClass[64]{};
    if (!surface.IsDesktopHosted() ||
        surfaceWindow == nullptr ||
        created.desktopHost == nullptr ||
        GetClassNameW(
            created.desktopHost,
            hostClass,
            ARRAYSIZE(hostClass)) == 0 ||
        (wcscmp(hostClass, L"Progman") != 0 &&
         wcscmp(hostClass, L"WorkerW") != 0) ||
        (style & WS_POPUP) == 0 ||
        (style & WS_CHILD) != 0 ||
        GetAncestor(surfaceWindow, GA_PARENT) !=
            created.desktopHost ||
        (exStyle & WS_EX_LAYERED) != 0 ||
        (exStyle & WS_EX_TOPMOST) != 0 ||
        surface.VisibleItemCount() + assigned.size() !=
            before.items.size()) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover host or unique partition invariant failed\n";
        return 104;
    }
    if (!DesktopSurfaceWindowSmokeAccess::UsesPhysicalPixelGeometry(
            surface) ||
        !DesktopSurfaceWindowSmokeAccess::HasShellImageIdentity(
            surface)) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover DPI geometry or Shell image identity invariant failed\n";
        return 114;
    }
    surface.Show();
    const ULONGLONG deadline = GetTickCount64() + 1200;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(
                &message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(
            0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    const ULONGLONG iconDeadline = GetTickCount64() + 3000;
    while (!DesktopSurfaceWindowSmokeAccess::AllVisibleIconsReady(
               surface) &&
           GetTickCount64() < iconDeadline) {
        MSG message{};
        while (PeekMessageW(
                &message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(
            0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    if (!DesktopSurfaceWindowSmokeAccess::AllVisibleIconsReady(
            surface)) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover left a visible Shell item on a placeholder icon\n";
        return 115;
    }
    const auto pumpMessagesFor = [](DWORD milliseconds) {
        const ULONGLONG deadline = GetTickCount64() + milliseconds;
        while (GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(
                    &message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjects(
                0, nullptr, FALSE, 16, QS_ALLINPUT);
        }
    };
    const HWND originalForeground = GetForegroundWindow();
    struct ForegroundRestorer {
        HWND window = nullptr;
        ~ForegroundRestorer() {
            if (window != nullptr && IsWindow(window) != FALSE) {
                SetForegroundWindow(window);
            }
        }
    } foregroundRestorer{originalForeground};
    const auto withForegroundInputQueue = [&](auto&& action) {
        const HWND foreground = GetForegroundWindow();
        const DWORD currentThread = GetCurrentThreadId();
        const DWORD foregroundThread = foreground == nullptr
            ? 0
            : GetWindowThreadProcessId(foreground, nullptr);
        const bool needsAttachment = foregroundThread != 0 &&
            foregroundThread != currentThread;
        HANDLE foregroundThreadHandle = needsAttachment
            ? OpenThread(SYNCHRONIZE, FALSE, foregroundThread)
            : nullptr;
        if (needsAttachment &&
            AttachThreadInput(
                currentThread, foregroundThread, TRUE) == FALSE) {
            if (foregroundThreadHandle != nullptr) {
                CloseHandle(foregroundThreadHandle);
            }
            return false;
        }
        action();
        if (needsAttachment &&
            AttachThreadInput(
                currentThread, foregroundThread, FALSE) == FALSE) {
            const bool foregroundThreadExited =
                foregroundThreadHandle != nullptr &&
                WaitForSingleObject(foregroundThreadHandle, 0) ==
                    WAIT_OBJECT_0;
            if (!foregroundThreadExited &&
                AttachThreadInput(
                    currentThread, foregroundThread, FALSE) == FALSE) {
                if (foregroundThreadHandle != nullptr) {
                    CloseHandle(foregroundThreadHandle);
                }
                return false;
            }
        }
        if (foregroundThreadHandle != nullptr) {
            CloseHandle(foregroundThreadHandle);
        }
        return true;
    };
    std::wstring renameIdentity;
    const bool renameSelected = hasDesktopRenameFixture
        ? DesktopSurfaceWindowSmokeAccess::SelectRenameableByIdentity(
            surface, desktopRenamePath)
        : DesktopSurfaceWindowSmokeAccess::SelectFirstRenameable(
            surface, renameIdentity);
    if (hasDesktopRenameFixture && renameSelected) {
        renameIdentity = desktopRenamePath;
    }
    if (!renameSelected) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover has no renameable Shell item\n";
        return 200;
    }
    const ShellItemReference renameReference =
        DesktopSurfaceWindowSmokeAccess::ReferenceForIdentity(
            surface, renameIdentity);
    if (!DesktopContextMenuHasCanonicalRename(
            surfaceWindow, renameReference)) {
        surface.Close();
        std::wcerr
            << L"Desktop Shell menu did not expose canonical rename\n";
        return 201;
    }
    // This branch directly exercises the editor UI. Unlike the separate
    // real-input F2 smoke, SendMessage does not grant foreground activation,
    // so temporarily share the existing foreground input queue while the
    // activatable editor is created and focused.
    const bool f2InputQueueRestored = withForegroundInputQueue([&]() {
        SendMessageW(surfaceWindow, WM_KEYDOWN, VK_F2, 0);
        pumpMessagesFor(80);
    });
    if (!f2InputQueueRestored) {
        surface.Close();
        std::wcerr
            << L"Desktop F2 smoke could not restore the foreground input queue\n";
        return 217;
    }
    HWND renameEditor =
        DesktopSurfaceWindowSmokeAccess::RenameEditor(surface);
    if (renameEditor == nullptr ||
        IsWindowVisible(renameEditor) == FALSE) {
        surface.Close();
        std::wcerr
            << L"Desktop F2 did not show the rename editor\n";
        return 202;
    }
    GUITHREADINFO guiThreadInfo{};
    guiThreadInfo.cbSize = sizeof(guiThreadInfo);
    const DWORD editorThread = GetWindowThreadProcessId(
        renameEditor, nullptr);
    if (GetFocus() != renameEditor || editorThread == 0 ||
        GetGUIThreadInfo(editorThread, &guiThreadInfo) == FALSE ||
        guiThreadInfo.hwndFocus != renameEditor) {
        surface.Close();
        std::wcerr << L"Desktop rename editor did not receive real focus\n";
        return 208;
    }
    const int renameTextLength = GetWindowTextLengthW(renameEditor);
    std::wstring originalEditText(
        static_cast<size_t>(renameTextLength) + 1, L'\0');
    const int copiedRenameText = GetWindowTextW(
        renameEditor,
        originalEditText.data(),
        static_cast<int>(originalEditText.size()));
    originalEditText.resize(
        static_cast<size_t>((std::max)(copiedRenameText, 0)));
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    SendMessageW(
        renameEditor,
        EM_GETSEL,
        reinterpret_cast<WPARAM>(&selectionStart),
        reinterpret_cast<LPARAM>(&selectionEnd));
    const DWORD fileAttributes = GetFileAttributesW(
        renameIdentity.c_str());
    const bool isDirectory = fileAttributes != INVALID_FILE_ATTRIBUTES &&
        (fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const std::wstring physicalExtension =
        std::filesystem::path(renameIdentity).extension().wstring();
    const std::wstring physicalFileName =
        std::filesystem::path(renameIdentity).filename().wstring();
    const bool visibleTextHasPhysicalExtension =
        !isDirectory && !physicalExtension.empty() &&
        CompareStringOrdinal(
            originalEditText.c_str(), -1,
            physicalFileName.c_str(), -1,
            TRUE) == CSTR_EQUAL;
    const DWORD expectedSelectionEnd = static_cast<DWORD>(
        visibleTextHasPhysicalExtension
        ? originalEditText.size() - physicalExtension.size()
        : originalEditText.size());
    if (originalEditText.empty() || selectionStart != 0 ||
        selectionEnd != expectedSelectionEnd) {
        surface.Close();
        std::wcerr
            << L"Desktop rename editor extension selection is incorrect\n";
        return 209;
    }
    SendMessageW(renameEditor, WM_KEYDOWN, VK_ESCAPE, 0);
    pumpMessagesFor(80);
    if (DesktopSurfaceWindowSmokeAccess::RenameEditor(surface) !=
            nullptr) {
        surface.Close();
        std::wcerr
            << L"Desktop rename editor did not cancel on Escape\n";
        return 203;
    }
    pumpMessagesFor(GetDoubleClickTime() + 100);
    const POINT labelPoint =
        DesktopSurfaceWindowSmokeAccess::RenameLabelHitPointForIdentity(
            surface, renameIdentity);
    if (labelPoint.x < 0 || labelPoint.y < 0) {
        surface.Close();
        std::wcerr
            << L"Desktop rename label geometry is unavailable\n";
        return 204;
    }
    const bool clickInputQueueRestored = withForegroundInputQueue([&]() {
        SendMessageW(
            surfaceWindow, WM_LBUTTONDOWN, MK_LBUTTON,
            MAKELPARAM(labelPoint.x, labelPoint.y));
        SendMessageW(
            surfaceWindow, WM_LBUTTONUP, 0,
            MAKELPARAM(labelPoint.x, labelPoint.y));
        pumpMessagesFor(80);
    });
    const bool clickTimerInputQueueRestored =
        clickInputQueueRestored && withForegroundInputQueue([&]() {
        pumpMessagesFor(GetDoubleClickTime() + 100);
    });
    if (!clickInputQueueRestored || !clickTimerInputQueueRestored) {
        surface.Close();
        std::wcerr
            << L"Desktop slow-click smoke could not restore the foreground input queue\n";
        return 218;
    }
    renameEditor =
        DesktopSurfaceWindowSmokeAccess::RenameEditor(surface);
    if (renameEditor == nullptr ||
        IsWindowVisible(renameEditor) == FALSE) {
        surface.Close();
        std::wcerr
            << L"Desktop slow label click did not show the rename editor\n";
        return 205;
    }
    if (!hasDesktopRenameFixture) {
        SendMessageW(renameEditor, WM_KEYDOWN, VK_ESCAPE, 0);
        pumpMessagesFor(80);
        if (DesktopSurfaceWindowSmokeAccess::RenameEditor(surface) !=
                nullptr) {
            surface.Close();
            std::wcerr
                << L"Desktop label-click rename did not cancel cleanly\n";
            return 206;
        }
    } else {
        bool renameCommitCaptured = false;
        std::wstring committedPreviousIdentity;
        std::wstring committedNewIdentity;
        std::wstring committedDisplayName;
        surface.SetRenameCommitHandler(
            [&](const std::wstring& previousIdentity,
                const std::wstring& newIdentity,
                const std::wstring& newDisplayName) {
                renameCommitCaptured = true;
                committedPreviousIdentity = previousIdentity;
                committedNewIdentity = newIdentity;
                committedDisplayName = newDisplayName;
                return true;
            });
        const std::filesystem::path sourcePath(renameIdentity);
        const std::wstring temporaryStem =
            sourcePath.stem().wstring() +
            L"-Lattice-UI-Smoke-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
        const std::wstring requestedDisplayName = temporaryStem;
        const std::filesystem::path targetPath =
            sourcePath.parent_path() /
            (temporaryStem + physicalExtension);
        std::error_code renameFileError;
        if (std::filesystem::exists(targetPath, renameFileError) ||
            renameFileError) {
            SendMessageW(renameEditor, WM_KEYDOWN, VK_ESCAPE, 0);
            pumpMessagesFor(80);
            surface.Close();
            std::wcerr
                << L"Desktop UI rename temporary endpoint already exists\n";
            return 210;
        }

        int commitValidationResult = 0;
        if (SetWindowTextW(
                renameEditor,
                requestedDisplayName.c_str()) == FALSE) {
            commitValidationResult = 211;
        } else {
            SendMessageW(renameEditor, WM_KEYDOWN, VK_RETURN, 0);
            pumpMessagesFor(160);
            if (DesktopSurfaceWindowSmokeAccess::RenameEditor(surface) !=
                    nullptr) {
                commitValidationResult = 212;
            }
        }

        renameFileError.clear();
        const bool sourceExistsAfterCommit =
            std::filesystem::exists(sourcePath, renameFileError) &&
            !renameFileError;
        renameFileError.clear();
        const bool targetExistsAfterCommit =
            std::filesystem::exists(targetPath, renameFileError) &&
            !renameFileError;
        const bool renameCommitted = renameCommitCaptured ||
            !sourceExistsAfterCommit || targetExistsAfterCommit;
        ShellItemReference freshRenamedReference;
        bool freshRenamedCanRename = false;
        if (commitValidationResult == 0) {
            freshRenamedReference =
                DesktopSurfaceWindowSmokeAccess::ReferenceForIdentity(
                    surface, committedNewIdentity);
            if (!renameCommitCaptured ||
                CompareStringOrdinal(
                    committedPreviousIdentity.c_str(), -1,
                    renameIdentity.c_str(), -1,
                    TRUE) != CSTR_EQUAL ||
                CompareStringOrdinal(
                    committedNewIdentity.c_str(), -1,
                    targetPath.wstring().c_str(), -1,
                    TRUE) != CSTR_EQUAL ||
                CompareStringOrdinal(
                    committedDisplayName.c_str(), -1,
                    requestedDisplayName.c_str(), -1,
                    TRUE) != CSTR_EQUAL ||
                sourceExistsAfterCommit || !targetExistsAfterCommit ||
                freshRenamedReference.desktopChildPidl.empty() ||
                CompareStringOrdinal(
                    freshRenamedReference.path.c_str(), -1,
                    targetPath.wstring().c_str(), -1,
                    TRUE) != CSTR_EQUAL ||
                FAILED(CanRenameDesktopShellItem(
                    freshRenamedReference,
                    freshRenamedCanRename)) ||
                !freshRenamedCanRename) {
                commitValidationResult = 213;
            }
        }

        if (renameCommitted) {
            ShellItemReference restoreReference =
                freshRenamedReference;
            if (restoreReference.desktopChildPidl.empty()) {
                CreateDesktopShellItemReference(
                    targetPath.wstring(), restoreReference);
            }
            DesktopShellRenameResult restored;
            const HRESULT restoreResult =
                restoreReference.desktopChildPidl.empty()
                ? E_FAIL
                : RenameDesktopShellItem(
                    nullptr,
                    restoreReference,
                    sourcePath.stem().wstring(),
                    restored);
            renameFileError.clear();
            const bool sourceRestored =
                std::filesystem::exists(sourcePath, renameFileError) &&
                !renameFileError;
            renameFileError.clear();
            const bool targetRemoved =
                !std::filesystem::exists(targetPath, renameFileError) &&
                !renameFileError;
            ShellItemReference freshRestoredReference;
            bool freshRestoredCanRename = false;
            const HRESULT freshRestoreResult = sourceRestored
                ? CreateDesktopShellItemReference(
                    sourcePath.wstring(), freshRestoredReference)
                : E_FAIL;
            if (!sourceRestored || !targetRemoved ||
                FAILED(freshRestoreResult) ||
                freshRestoredReference.desktopChildPidl.empty() ||
                FAILED(CanRenameDesktopShellItem(
                    freshRestoredReference,
                    freshRestoredCanRename)) ||
                !freshRestoredCanRename ||
                restored.disposition !=
                    ShellRenameDisposition::Renamed ||
                CompareStringOrdinal(
                    restored.item.path.c_str(), -1,
                    sourcePath.wstring().c_str(), -1,
                    TRUE) != CSTR_EQUAL) {
                surface.Close();
                std::wcerr << L"Desktop UI rename restore failed: 0x"
                           << std::hex
                           << static_cast<unsigned long>(restoreResult)
                           << L"\n";
                return 214;
            }
            if (!surface.Refresh(errorMessage)) {
                surface.Close();
                std::wcerr
                    << L"Desktop UI rename refresh after restore failed: "
                    << errorMessage << L"\n";
                return 215;
            }
            const ShellItemReference refreshedReference =
                DesktopSurfaceWindowSmokeAccess::ReferenceForIdentity(
                    surface, renameIdentity);
            if (refreshedReference.desktopChildPidl.empty() ||
                CompareStringOrdinal(
                    refreshedReference.path.c_str(), -1,
                    sourcePath.wstring().c_str(), -1,
                    TRUE) != CSTR_EQUAL ||
                DesktopSurfaceWindowSmokeAccess::ReferenceForIdentity(
                    surface,
                    targetPath.wstring()).desktopChildPidl.empty() ==
                    false) {
                surface.Close();
                std::wcerr
                    << L"Desktop UI rename refresh kept a stale identity\n";
                return 216;
            }
        } else {
            HWND pendingEditor =
                DesktopSurfaceWindowSmokeAccess::RenameEditor(surface);
            if (pendingEditor != nullptr) {
                SendMessageW(
                    pendingEditor, WM_KEYDOWN, VK_ESCAPE, 0);
                pumpMessagesFor(80);
            }
        }
        if (commitValidationResult != 0) {
            surface.Close();
            std::wcerr
                << L"Desktop UI rename commit validation failed\n";
            return commitValidationResult;
        }
        surface.Close();
        return 0;
    }
    const std::vector<ShellItemReference> shellItems =
        DesktopSurfaceWindowSmokeAccess::VisibleShellItems(
            surface, 2);
    if (shellItems.size() != 2 ||
        std::any_of(
            shellItems.begin(), shellItems.end(),
            [](const ShellItemReference& item) {
                return item.path.empty() ||
                    item.desktopChildPidl.empty();
            })) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover snapshot did not retain two Shell child identities\n";
        return 116;
    }
    Microsoft::WRL::ComPtr<IDataObject> shellSelection;
    const std::vector<std::wstring> shellSelectionPaths{
        shellItems[0].path, shellItems[1].path};
    const std::vector<std::wstring> extractedSelection =
        SUCCEEDED(CreateDesktopShellSelectionObject(
            surfaceWindow,
            shellItems,
            IID_PPV_ARGS(shellSelection.GetAddressOf()))) &&
            shellSelection != nullptr
        ? ExtractShellDropPaths(shellSelection.Get())
        : std::vector<std::wstring>{};
    const bool selectionIdentityMatches =
        extractedSelection.size() ==
            shellSelectionPaths.size() &&
        std::equal(
            extractedSelection.begin(),
            extractedSelection.end(),
            shellSelectionPaths.begin(),
            [](const std::wstring& left,
               const std::wstring& right) {
                return CompareStringOrdinal(
                           left.c_str(), -1,
                           right.c_str(), -1,
                           TRUE) == CSTR_EQUAL;
            });
    if (!selectionIdentityMatches) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover multi-selection lost native Shell identity or order\n";
        return 117;
    }

    const std::optional<POINT> blankPoint =
        DesktopSurfaceWindowSmokeAccess::FindBlankPoint(
            surface);
    if (!blankPoint.has_value()) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover could not find a blank marquee probe point\n";
        return 118;
    }
    SendMessageW(
        surfaceWindow,
        WM_LBUTTONDOWN,
        MK_LBUTTON,
        MAKELPARAM(blankPoint->x, blankPoint->y));
    if (GetCapture() != surfaceWindow ||
        !DesktopSurfaceWindowSmokeAccess::IsMarqueePending(
            surface)) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover did not capture a blank marquee press\n";
        return 119;
    }
    SendMessageW(
        surfaceWindow,
        WM_MOUSEMOVE,
        0,
        MAKELPARAM(blankPoint->x, blankPoint->y));
    if (GetCapture() == surfaceWindow ||
        !DesktopSurfaceWindowSmokeAccess::HasNoPointerGesture(
            surface)) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover did not self-heal a missing left-button state\n";
        return 120;
    }
    SendMessageW(
        surfaceWindow,
        WM_LBUTTONDOWN,
        MK_LBUTTON,
        MAKELPARAM(blankPoint->x, blankPoint->y));
    if (GetCapture() != surfaceWindow) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover could not reacquire marquee capture\n";
        return 121;
    }
    SendMessageW(surfaceWindow, WM_CANCELMODE, 0, 0);
    if (GetCapture() == surfaceWindow ||
        !DesktopSurfaceWindowSmokeAccess::HasNoPointerGesture(
            surface)) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover cancel mode left transient capture behind\n";
        return 122;
    }
    // Exercise Windows input routing, not just direct WM_* dispatch. The
    // start point must be inside the old full cell but native Explorer blank.
    POINT savedCursor{};
    POINT inputStart{};
    POINT inputEnd{};
    const std::optional<std::pair<POINT, POINT>> nativeGesture =
        DesktopSurfaceWindowSmokeAccess::FindNativeMarqueeGesture(
            surface);
    if (!DesktopSurfaceWindowSmokeAccess::HasNativeListViewQueryAccess(
            surface)) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover could not open native ListView query access\n";
        return 126;
    }
    if (!nativeGesture.has_value()) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover could not find a native marquee geometry\n";
        return 127;
    }
    inputStart = nativeGesture->first;
    inputEnd = nativeGesture->second;
    ClientToScreen(surfaceWindow, &inputStart);
    ClientToScreen(surfaceWindow, &inputEnd);
    if (WindowFromPoint(inputStart) != surfaceWindow ||
        WindowFromPoint(inputEnd) != surfaceWindow) {
        surface.Close();
        std::wcerr
            << L"Native marquee geometry was not routed to the takeover surface\n";
        return 128;
    }
    if (!GetCursorPos(&savedCursor) ||
        ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
          GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) |
          GetAsyncKeyState(VK_MENU)) & 0x8000) != 0) {
        surface.Close();
        std::wcerr
            << L"Native input precondition found a held mouse button or modifier\n";
        return 129;
    }
    const size_t shellDragCountBefore =
        DesktopSurfaceWindowSmokeAccess::ShellDragStartCount(
            surface);
    DesktopSurfaceWindowSmokeAccess::SuppressShellDragForSmoke(
        surface, true);
    const auto pumpInput = [&]() {
        const ULONGLONG end = GetTickCount64() + 100;
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        } while (GetTickCount64() < end);
    };
    const auto injectMouse = [&](POINT point, DWORD flags) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = MulDiv(point.x - GetSystemMetrics(SM_XVIRTUALSCREEN),
            65535, (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1));
        input.mi.dy = MulDiv(point.y - GetSystemMetrics(SM_YVIRTUALSCREEN),
            65535, (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1));
        input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
            MOUSEEVENTF_MOVE | flags;
        const bool sent = SendInput(1, &input, sizeof(input)) == 1;
        pumpInput();
        return sent;
    };
    const bool movedToStart = injectMouse(inputStart, 0);
    const bool routedToSurface =
        movedToStart && WindowFromPoint(inputStart) == surfaceWindow;
    const bool pressedAtStart =
        routedToSurface &&
        injectMouse(inputStart, MOUSEEVENTF_LEFTDOWN);
    const bool capturedAtStart =
        pressedAtStart && GetCapture() == surfaceWindow;
    const bool movedToEnd =
        capturedAtStart && injectMouse(inputEnd, 0);
    const bool marqueeActive =
        movedToEnd &&
        DesktopSurfaceWindowSmokeAccess::IsMarqueeActive(surface);
    const size_t selectedDuringMarquee =
        DesktopSurfaceWindowSmokeAccess::SelectedPaths(surface).size();
    // Always release our press even if capture or a previous assertion failed.
    const bool released = injectMouse(inputEnd, MOUSEEVENTF_LEFTUP);
    const bool captureReleased = GetCapture() != surfaceWindow;
    const bool pointerGestureFinished =
        DesktopSurfaceWindowSmokeAccess::HasNoPointerGesture(surface);
    const size_t selectedAfterRelease =
        DesktopSurfaceWindowSmokeAccess::SelectedPaths(surface).size();
    const size_t shellDragCountAfter =
        DesktopSurfaceWindowSmokeAccess::ShellDragStartCount(surface);
    const bool nativeInputPassed =
        movedToStart && routedToSurface && pressedAtStart &&
        capturedAtStart && movedToEnd && marqueeActive &&
        selectedDuringMarquee >= 2 && released && captureReleased &&
        pointerGestureFinished && selectedAfterRelease >= 2 &&
        shellDragCountAfter == shellDragCountBefore;
    injectMouse(savedCursor, 0);
    DesktopSurfaceWindowSmokeAccess::SuppressShellDragForSmoke(
        surface, false);
    if (!nativeInputPassed) {
        const DWORD diagnosticRootLength = GetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
        if (diagnosticRootLength > 0) {
            std::wstring diagnosticRoot(
                diagnosticRootLength, L'\0');
            const DWORD copied = GetEnvironmentVariableW(
                L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
                diagnosticRoot.data(),
                diagnosticRootLength);
            if (copied > 0 && copied < diagnosticRootLength) {
                diagnosticRoot.resize(copied);
                std::ofstream diagnostic(
                    std::filesystem::path(diagnosticRoot) /
                        L"native-marquee-result.txt",
                    std::ios::trunc);
                diagnostic
                    << "movedStart=" << movedToStart
                    << " routed=" << routedToSurface
                    << " pressed=" << pressedAtStart
                    << " captured=" << capturedAtStart
                    << " movedEnd=" << movedToEnd
                    << " marquee=" << marqueeActive
                    << " selectedDuring=" << selectedDuringMarquee
                    << " released=" << released
                    << " captureReleased=" << captureReleased
                    << " gestureFinished=" << pointerGestureFinished
                    << " selectedAfter=" << selectedAfterRelease
                    << " dragBefore=" << shellDragCountBefore
                    << " dragAfter=" << shellDragCountAfter
                    << " start=" << inputStart.x << ',' << inputStart.y
                    << " end=" << inputEnd.x << ',' << inputEnd.y
                    << '\n';
            }
        }
        std::wcerr
            << L"Native cell-gap marquee failed: movedStart="
            << movedToStart << L", routed=" << routedToSurface
            << L", pressed=" << pressedAtStart
            << L", captured=" << capturedAtStart
            << L", movedEnd=" << movedToEnd
            << L", marquee=" << marqueeActive
            << L", selectedDuring=" << selectedDuringMarquee
            << L", released=" << released
            << L", captureReleased=" << captureReleased
            << L", gestureFinished=" << pointerGestureFinished
            << L", selectedAfter=" << selectedAfterRelease
            << L", dragBefore=" << shellDragCountBefore
            << L", dragAfter=" << shellDragCountAfter << L"\n";
        surface.Close();
        return 124;
    }

    const auto writeTakeoverDiagnostics =
        [&](HWND hitWindow, const wchar_t* phase) {
            const DWORD required = GetEnvironmentVariableW(
                L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
                nullptr,
                0);
            if (required == 0) {
                return;
            }
            std::wstring root(required, L'\0');
            const DWORD copied = GetEnvironmentVariableW(
                L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
                root.data(),
                required);
            if (copied == 0 || copied >= required) {
                return;
            }
            root.resize(copied);
            std::wofstream diagnostic(
                std::filesystem::path(root) /
                    L"desktop-takeover-diagnostics.txt",
                std::ios::app);
            const auto dumpWindow =
                [&](const wchar_t* label, HWND window) {
                    wchar_t className[128]{};
                    RECT rect{};
                    DWORD processId = 0;
                    if (window != nullptr) {
                        GetClassNameW(
                            window,
                            className,
                            ARRAYSIZE(className));
                        GetWindowRect(window, &rect);
                        GetWindowThreadProcessId(
                            window, &processId);
                    }
                    diagnostic
                        << label << L" hwnd="
                        << reinterpret_cast<UINT_PTR>(window)
                        << L" class=" << className
                        << L" pid=" << processId
                        << L" parent="
                        << reinterpret_cast<UINT_PTR>(
                            window == nullptr
                                ? nullptr
                                : GetParent(window))
                        << L" owner="
                        << reinterpret_cast<UINT_PTR>(
                            window == nullptr
                                ? nullptr
                                : GetWindow(window, GW_OWNER))
                        << L" root="
                        << reinterpret_cast<UINT_PTR>(
                            window == nullptr
                                ? nullptr
                                : GetAncestor(window, GA_ROOT))
                        << L" style="
                        << (window == nullptr
                                ? 0
                                : GetWindowLongPtrW(
                                      window, GWL_STYLE))
                        << L" exstyle="
                        << (window == nullptr
                                ? 0
                                : GetWindowLongPtrW(
                                      window, GWL_EXSTYLE))
                        << L" visible="
                        << (window != nullptr &&
                            IsWindowVisible(window) != FALSE)
                        << L" enabled="
                        << (window != nullptr &&
                            IsWindowEnabled(window) != FALSE)
                        << L" rect="
                        << rect.left << L"," << rect.top
                        << L"," << rect.right << L","
                        << rect.bottom << L"\n";
                };
            diagnostic << L"phase=" << phase
                       << L" probe=" << assignedProbe.x
                       << L"," << assignedProbe.y << L"\n";
            dumpWindow(L"surface", surfaceWindow);
            dumpWindow(L"desktop-host", created.desktopHost);
            dumpWindow(L"defview", created.shellViewWindow);
            dumpWindow(L"listview", created.listViewWindow);
            dumpWindow(L"hit", hitWindow);
            dumpWindow(
                L"hit-ga-parent",
                hitWindow == nullptr
                    ? nullptr
                    : GetAncestor(hitWindow, GA_PARENT));
            dumpWindow(
                L"hit-ga-root",
                hitWindow == nullptr
                    ? nullptr
                    : GetAncestor(hitWindow, GA_ROOT));
            dumpWindow(
                L"hit-ga-rootowner",
                hitWindow == nullptr
                    ? nullptr
                    : GetAncestor(hitWindow, GA_ROOTOWNER));
            dumpWindow(
                L"hit-gw-owner",
                hitWindow == nullptr
                    ? nullptr
                    : GetWindow(hitWindow, GW_OWNER));
            dumpWindow(
                L"hit-z-prev",
                hitWindow == nullptr
                    ? nullptr
                    : GetWindow(hitWindow, GW_HWNDPREV));
            dumpWindow(
                L"hit-z-next",
                hitWindow == nullptr
                    ? nullptr
                    : GetWindow(hitWindow, GW_HWNDNEXT));
            int zIndex = 0;
            for (HWND child = GetWindow(
             created.desktopHost, GW_CHILD);
                 child != nullptr;
                 child = GetWindow(child, GW_HWNDNEXT)) {
                diagnostic << L"zindex=" << zIndex++ << L" ";
                dumpWindow(L"child", child);
            }
            diagnostic.flush();
        };
    bool surfaceBeforeDefView = false;
    bool sawSurface = false;
    bool sawDefView = false;
    for (HWND child = GetWindow(created.desktopHost, GW_CHILD);
         child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (child == surfaceWindow) {
            sawSurface = true;
            surfaceBeforeDefView = !sawDefView;
        } else if (child == created.shellViewWindow) {
            sawDefView = true;
        }
    }
    if (!sawSurface || !sawDefView || !surfaceBeforeDefView) {
        writeTakeoverDiagnostics(
            WindowFromPoint(assignedProbe),
            L"desktop-host-z-order-failed");
        surface.Close();
        std::wcerr
            << L"Desktop takeover was not ahead of Explorer DefView\n";
        return 105;
    }
    if (!assigned.empty()) {
        const HWND takeoverHit = WindowFromPoint(assignedProbe);
        const HWND takeoverRoot = takeoverHit == nullptr
            ? nullptr
            : GetAncestor(takeoverHit, GA_ROOT);
        const bool normalApplicationAlreadyCovers =
            takeoverHit != nullptr &&
            takeoverRoot != created.desktopHost;
        const bool externalDesktopLayerCovers =
            takeoverHit != nullptr &&
            takeoverRoot == created.desktopHost &&
            takeoverHit != surfaceWindow &&
            GetAncestor(takeoverHit, GA_PARENT) ==
                created.desktopHost;
        if (takeoverHit != surfaceWindow &&
            !normalApplicationAlreadyCovers &&
            !externalDesktopLayerCovers) {
            writeTakeoverDiagnostics(
                takeoverHit, L"explorer-occlusion-failed");
            surface.Close();
            std::wcerr
                << L"Desktop takeover did not occlude the Explorer icon layer\n";
            return 105;
        }
        if (externalDesktopLayerCovers) {
            writeTakeoverDiagnostics(
                takeoverHit, L"external-desktop-layer-observed");
        }
        const HWND previousForeground = GetForegroundWindow();
        HWND normalCover = normalApplicationAlreadyCovers
            ? nullptr
            : CreateWindowExW(
            0,
            L"STATIC",
            L"Lattice takeover normal-window probe",
            WS_OVERLAPPEDWINDOW,
            assignedProbe.x - 8,
            assignedProbe.y - 8,
            96,
            96,
            nullptr,
            nullptr,
            instance,
            nullptr);
        if (!normalApplicationAlreadyCovers &&
            normalCover != nullptr) {
            ShowWindow(normalCover, SW_SHOWNORMAL);
            SetWindowPos(
                normalCover, HWND_TOP,
                assignedProbe.x - 8,
                assignedProbe.y - 8,
                96, 96, SWP_SHOWWINDOW);
            SetForegroundWindow(normalCover);
            UpdateWindow(normalCover);
            const ULONGLONG coverDeadline = GetTickCount64() + 500;
            while (GetTickCount64() < coverDeadline) {
                MSG message{};
                while (PeekMessageW(
                        &message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (GetAncestor(
                        WindowFromPoint(assignedProbe),
                        GA_ROOT) == normalCover) {
                    break;
                }
                MsgWaitForMultipleObjects(
                    0, nullptr, FALSE, 16, QS_ALLINPUT);
            }
        }
        if (!normalApplicationAlreadyCovers &&
            (normalCover == nullptr ||
             GetAncestor(
                 WindowFromPoint(assignedProbe),
                 GA_ROOT) != normalCover)) {
            writeTakeoverDiagnostics(
                WindowFromPoint(assignedProbe),
                L"normal-window-coverage-failed");
            if (normalCover != nullptr) {
                DestroyWindow(normalCover);
            }
            if (previousForeground != nullptr &&
                IsWindow(previousForeground) != FALSE) {
                SetForegroundWindow(previousForeground);
            }
            surface.Close();
            std::wcerr
                << L"Desktop takeover covered a normal application window\n";
            return 108;
        }
        if (normalCover != nullptr) {
            DestroyWindow(normalCover);
            if (previousForeground != nullptr &&
                IsWindow(previousForeground) != FALSE) {
                SetForegroundWindow(previousForeground);
            }
        }
    }
    if (IsWindowVisible(created.listViewWindow) == FALSE ||
        IsWindowEnabled(created.listViewWindow) == FALSE) {
        surface.Close();
        std::wcerr
            << L"Desktop takeover changed Explorer list-view state\n";
        return 109;
    }
    surface.Close();

    DesktopViewSnapshot after;
    std::vector<DesktopPosition> positionsAfter;
    if (!layout.CaptureViewSnapshot(after, errorMessage) ||
        !layout.CaptureAllPositions(
            positionsAfter, errorMessage) ||
        after.viewFlags != before.viewFlags ||
        after.items.size() != before.items.size() ||
        positionsAfter.size() != positionsBefore.size()) {
        std::wcerr
            << L"Desktop takeover close did not preserve Explorer snapshot\n";
        return 106;
    }
    const bool sameDesktopIdentities = std::all_of(
        before.items.begin(), before.items.end(),
        [&](const DesktopViewItem& expected) {
            return std::any_of(
                after.items.begin(), after.items.end(),
                [&](const DesktopViewItem& actual) {
                    return CompareStringOrdinal(
                               expected.path.c_str(), -1,
                               actual.path.c_str(), -1,
                               TRUE) == CSTR_EQUAL;
                });
        });
    if (!sameDesktopIdentities) {
        std::wcerr
            << L"Desktop takeover input changed the desktop item set\n";
        return 125;
    }
    const auto findPosition =
        [&](const DesktopPosition& expected) {
            return std::find_if(
                positionsAfter.begin(),
                positionsAfter.end(),
                [&](const DesktopPosition& actual) {
                    return CompareStringOrdinal(
                               expected.path.c_str(),
                               -1,
                               actual.path.c_str(),
                               -1,
                               TRUE) == CSTR_EQUAL &&
                        expected.point.x == actual.point.x &&
                        expected.point.y == actual.point.y;
                }) != positionsAfter.end();
        };
    if (!std::all_of(
            positionsBefore.begin(),
            positionsBefore.end(),
            findPosition)) {
        std::wcerr
            << L"Desktop takeover changed Explorer icon coordinates\n";
        return 107;
    }
    std::wcout
        << L"Desktop display takeover PoC passed: items="
        << before.items.size()
        << L", visible="
        << (before.items.size() - assigned.size())
        << L", flags=" << before.viewFlags << L"\n";
    return 0;
}

HWND FindShellPocListView();

class RemoteListViewQuery final {
public:
    ~RemoteListViewQuery() {
        if (process_ != nullptr && remoteBuffer_ != nullptr) {
            VirtualFreeEx(process_, remoteBuffer_, 0, MEM_RELEASE);
        }
        if (process_ != nullptr) {
            CloseHandle(process_);
        }
    }

    bool Initialize(HWND listView) {
        listView_ = listView;
        DWORD processId = 0;
        GetWindowThreadProcessId(listView_, &processId);
        if (processId == 0) return false;
        if (processId == GetCurrentProcessId()) return true;
        process_ = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION |
                PROCESS_VM_READ | PROCESS_VM_WRITE,
            FALSE,
            processId);
        if (process_ == nullptr) return false;
        constexpr SIZE_T kBufferSize =
            sizeof(LVHITTESTINFO) > sizeof(RECT)
                ? sizeof(LVHITTESTINFO)
                : sizeof(RECT);
        remoteBuffer_ = VirtualAllocEx(
            process_, nullptr, kBufferSize,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        return remoteBuffer_ != nullptr;
    }

    bool HitTest(POINT point, LVHITTESTINFO& hit) const {
        hit = {};
        hit.pt = point;
        hit.iItem = -1;
        LRESULT messageResult = -1;
        return Query(LVM_HITTEST, 0, &hit, sizeof(hit), messageResult);
    }

    bool ItemRect(int itemIndex, int rectKind, RECT& rect) const {
        rect = {};
        rect.left = rectKind;
        LRESULT messageResult = FALSE;
        return Query(
                   LVM_GETITEMRECT,
                   static_cast<WPARAM>(itemIndex),
                   &rect,
                   sizeof(rect),
                   messageResult) &&
            messageResult != FALSE;
    }

    bool ItemPosition(int itemIndex, POINT& point) const {
        point = {};
        LRESULT messageResult = FALSE;
        return Query(
                   LVM_GETITEMPOSITION,
                   static_cast<WPARAM>(itemIndex),
                   &point,
                   sizeof(point),
                   messageResult) &&
            messageResult != FALSE;
    }

private:
    bool Query(
        UINT message,
        WPARAM wParam,
        void* localBuffer,
        SIZE_T bufferSize,
        LRESULT& messageResult) const {
        if (listView_ == nullptr || localBuffer == nullptr ||
            bufferSize == 0 || IsWindow(listView_) == FALSE) {
            return false;
        }
        void* messageBuffer = localBuffer;
        if (process_ != nullptr) {
            SIZE_T written = 0;
            if (WriteProcessMemory(
                    process_, remoteBuffer_, localBuffer, bufferSize,
                    &written) == FALSE ||
                written != bufferSize) {
                return false;
            }
            messageBuffer = remoteBuffer_;
        }
        DWORD_PTR rawResult = 0;
        if (SendMessageTimeoutW(
                listView_, message, wParam,
                reinterpret_cast<LPARAM>(messageBuffer),
                SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
                1000, &rawResult) == 0) {
            return false;
        }
        if (process_ != nullptr) {
            SIZE_T read = 0;
            if (ReadProcessMemory(
                    process_, remoteBuffer_, localBuffer, bufferSize,
                    &read) == FALSE ||
                read != bufferSize) {
                return false;
            }
        }
        messageResult = static_cast<LRESULT>(rawResult);
        return true;
    }

    HWND listView_ = nullptr;
    HANDLE process_ = nullptr;
    void* remoteBuffer_ = nullptr;
};

int RunSmokeDesktopNativeDragOracle() {
    AttachParentConsole();
    SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto environmentValue = [](const wchar_t* name) {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) return std::wstring{};
        std::wstring value(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            name, value.data(), required);
        if (copied == 0 || copied >= required) return std::wstring{};
        value.resize(copied);
        return value;
    };
    const std::wstring markerA =
        environmentValue(L"LATTICE_INTERNAL_DRAG_MARKER_A");
    const std::wstring markerB =
        environmentValue(L"LATTICE_INTERNAL_DRAG_MARKER_B");
    const std::filesystem::path resultRoot = environmentValue(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR");
    std::ofstream result(
        resultRoot / L"desktop-native-drag-oracle-result.txt",
        std::ios::trunc);
    const auto markerIsValid = [](const std::wstring& path) {
        return !path.empty() &&
            std::filesystem::path(path).filename().wstring().rfind(
                L".lattice-internal-drag-", 0) == 0 &&
            GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    if (resultRoot.empty() || !markerIsValid(markerA) ||
        !markerIsValid(markerB)) {
        result << "INPUT_REJECTED=1\nSTATUS=FAIL\n";
        return 221;
    }

    DesktopLayout layout;
    std::vector<DesktopPosition> positionsBefore;
    DWORD flagsBefore = 0;
    std::wstring errorMessage;
    if (!layout.CaptureAllPositions(positionsBefore, errorMessage) ||
        !layout.CaptureViewFlags(flagsBefore, errorMessage)) {
        result << "SNAPSHOT_FAILED=1\nERROR="
               << Utf8Text(errorMessage) << "\nSTATUS=FAIL\n";
        return 222;
    }
    const auto lookupPosition = [](
        const std::vector<DesktopPosition>& positions,
        const std::wstring& path) -> std::optional<POINT> {
        const auto found = std::find_if(
            positions.begin(), positions.end(),
            [&](const DesktopPosition& position) {
                return CompareStringOrdinal(
                    position.path.c_str(), -1,
                    path.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
        return found == positions.end()
            ? std::nullopt
            : std::optional<POINT>(found->point);
    };
    const auto beforeA = lookupPosition(positionsBefore, markerA);
    const auto beforeB = lookupPosition(positionsBefore, markerB);
    const HWND listView = FindShellPocListView();
    if (!beforeA.has_value() || !beforeB.has_value() ||
        listView == nullptr || IsWindow(listView) == FALSE) {
        result << "MARKERS_OR_LISTVIEW_MISSING=1\nSTATUS=FAIL\n";
        return 223;
    }

    RemoteListViewQuery query;
    if (!query.Initialize(listView)) {
        result << "LISTVIEW_QUERY_FAILED=1\nSTATUS=FAIL\n";
        return 224;
    }
    DWORD_PTR itemCountResult = 0;
    if (SendMessageTimeoutW(
            listView, LVM_GETITEMCOUNT, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
            1000, &itemCountResult) == 0) {
        result << "ITEM_COUNT_QUERY_FAILED=1\nSTATUS=FAIL\n";
        return 224;
    }
    const int itemCount = static_cast<int>(itemCountResult);
    int markerAIndex = -1;
    int markerBIndex = -1;
    for (int index = 0; index < itemCount; ++index) {
        POINT position{};
        if (!query.ItemPosition(index, position)) continue;
        if (position.x == beforeA->x && position.y == beforeA->y) {
            markerAIndex = index;
        }
        if (position.x == beforeB->x && position.y == beforeB->y) {
            markerBIndex = index;
        }
    }
    if (markerAIndex < 0 || markerBIndex < 0 ||
        markerAIndex == markerBIndex) {
        result << "MARKER_INDEX_MATCH_FAILED=1\nSTATUS=FAIL\n";
        return 223;
    }
    RECT iconRectA{};
    if (!query.ItemRect(markerAIndex, LVIR_ICON, iconRectA)) {
        result << "LISTVIEW_QUERY_FAILED=1\nSTATUS=FAIL\n";
        return 224;
    }
    POINT dragStart{
        (iconRectA.left + iconRectA.right) / 2,
        (iconRectA.top + iconRectA.bottom) / 2};
    LVHITTESTINFO startHit{};
    if (!query.HitTest(dragStart, startHit) ||
        startHit.iItem != markerAIndex) {
        result << "START_HIT_FAILED=1\nSTATUS=FAIL\n";
        return 225;
    }

    DWORD_PTR spacingResult = 0;
    if (SendMessageTimeoutW(
            listView, LVM_GETITEMSPACING, FALSE, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
            1000, &spacingResult) == 0) {
        result << "SPACING_QUERY_FAILED=1\nSTATUS=FAIL\n";
        return 226;
    }
    const int cellWidth = LOWORD(spacingResult);
    const int cellHeight = HIWORD(spacingResult);
    const POINT gridDelta{
        beforeB->x - beforeA->x,
        beforeB->y - beforeA->y};
    std::optional<POINT> dragEnd;
    const int maximumOffset = (std::max)(4, cellWidth / 2 - 2);
    for (int offset = 4; offset <= maximumOffset; offset += 2) {
        const std::array<POINT, 4> candidates{{
            {dragStart.x + gridDelta.x + offset,
             dragStart.y + gridDelta.y},
            {dragStart.x + gridDelta.x - offset,
             dragStart.y + gridDelta.y},
            {dragStart.x + gridDelta.x,
             dragStart.y + gridDelta.y + offset},
            {dragStart.x + gridDelta.x,
             dragStart.y + gridDelta.y - offset},
        }};
        for (const POINT candidate : candidates) {
            LVHITTESTINFO hit{};
            if (query.HitTest(candidate, hit) && hit.iItem < 0) {
                dragEnd = candidate;
                break;
            }
        }
        if (dragEnd.has_value()) break;
    }
    if (!dragEnd.has_value()) {
        result << "OCCUPIED_CELL_GAP_NOT_FOUND=1\n"
               << "CELL_WIDTH=" << cellWidth << "\n"
               << "CELL_HEIGHT=" << cellHeight << "\n"
               << "STATUS=FAIL\n";
        return 227;
    }

    POINT dragStartScreen = dragStart;
    POINT dragEndScreen = *dragEnd;
    if (ClientToScreen(listView, &dragStartScreen) == FALSE ||
        ClientToScreen(listView, &dragEndScreen) == FALSE ||
        WindowFromPoint(dragStartScreen) != listView ||
        WindowFromPoint(dragEndScreen) != listView) {
        result << "NATIVE_ROUTE_NOT_EXPOSED=1\nSTATUS=FAIL\n";
        return 228;
    }
    if (((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
          GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) |
          GetAsyncKeyState(VK_MENU)) & 0x8000) != 0) {
        result << "INPUT_BUSY=1\nSTATUS=FAIL\n";
        return 229;
    }

    const int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualWidth =
        (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    const int virtualHeight =
        (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    const auto sendMouse = [&](POINT screenPoint, DWORD flags) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = MulDiv(
            screenPoint.x - virtualLeft, 65535, virtualWidth);
        input.mi.dy = MulDiv(
            screenPoint.y - virtualTop, 65535, virtualHeight);
        input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE |
            MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE | flags;
        return SendInput(1, &input, sizeof(input)) == 1;
    };
    POINT savedCursor{};
    GetCursorPos(&savedCursor);
    bool inputSucceeded = sendMouse(dragStartScreen, 0);
    Sleep(50);
    inputSucceeded =
        sendMouse(dragStartScreen, MOUSEEVENTF_LEFTDOWN) &&
        inputSucceeded;
    Sleep(80);
    inputSucceeded = sendMouse(dragEndScreen, 0) && inputSucceeded;
    Sleep(180);
    inputSucceeded =
        sendMouse(dragEndScreen, MOUSEEVENTF_LEFTUP) &&
        inputSucceeded;
    Sleep(500);
    sendMouse(savedCursor, 0);

    std::vector<DesktopPosition> positionsAfter;
    DWORD flagsAfter = 0;
    const bool capturedAfter =
        layout.CaptureAllPositions(positionsAfter, errorMessage) &&
        layout.CaptureViewFlags(flagsAfter, errorMessage);
    size_t changedCount = 0;
    if (capturedAfter) {
        for (const DesktopPosition& position : positionsBefore) {
            const auto current = lookupPosition(positionsAfter, position.path);
            if (!current.has_value() ||
                current->x != position.point.x ||
                current->y != position.point.y) {
                ++changedCount;
                result << "CHANGED_IDENTITY="
                       << Utf8Text(position.path) << "\n";
            }
        }
    }
    const auto afterA = lookupPosition(positionsAfter, markerA);
    const auto afterB = lookupPosition(positionsAfter, markerB);
    const bool markerMoved = beforeA.has_value() && afterA.has_value() &&
        (beforeA->x != afterA->x || beforeA->y != afterA->y);

    std::wstring restoreError;
    const bool restoreIssued = layout.RestorePositions(
        positionsBefore, restoreError);
    Sleep(160);
    std::vector<DesktopPosition> restoredOnce;
    std::vector<DesktopPosition> restoredTwice;
    const bool restoredCaptured =
        layout.CaptureAllPositions(restoredOnce, restoreError) &&
        (Sleep(100), layout.CaptureAllPositions(restoredTwice, restoreError));
    const auto exactMap = [&](const std::vector<DesktopPosition>& current) {
        if (current.size() != positionsBefore.size()) return false;
        return std::all_of(
            positionsBefore.begin(), positionsBefore.end(),
            [&](const DesktopPosition& position) {
                const auto value = lookupPosition(current, position.path);
                return value.has_value() &&
                    value->x == position.point.x &&
                    value->y == position.point.y;
            });
    };
    const bool restored = restoreIssued && restoredCaptured &&
        exactMap(restoredOnce) && exactMap(restoredTwice);

    result << "INPUT_SUCCEEDED=" << inputSucceeded << "\n"
           << "CAPTURED_AFTER=" << capturedAfter << "\n"
           << "FLAGS_BEFORE=" << flagsBefore << "\n"
           << "FLAGS_AFTER=" << flagsAfter << "\n"
           << "CELL_WIDTH=" << cellWidth << "\n"
           << "CELL_HEIGHT=" << cellHeight << "\n"
           << "REQUESTED_OFFSET_X="
           << (dragEnd->x - dragStart.x - gridDelta.x) << "\n"
           << "REQUESTED_OFFSET_Y="
           << (dragEnd->y - dragStart.y - gridDelta.y) << "\n"
           << "CHANGED_COUNT=" << changedCount << "\n"
           << "MARKER_A_MOVED=" << markerMoved << "\n";
    if (beforeA.has_value() && afterA.has_value()) {
        result << "MARKER_A_BEFORE=" << beforeA->x << "," << beforeA->y
               << "\nMARKER_A_AFTER=" << afterA->x << "," << afterA->y
               << "\n";
    }
    if (beforeB.has_value() && afterB.has_value()) {
        result << "MARKER_B_BEFORE=" << beforeB->x << "," << beforeB->y
               << "\nMARKER_B_AFTER=" << afterB->x << "," << afterB->y
               << "\n";
    }
    result << "RESTORED_TWICE=" << restored << "\n";
    const bool passed = inputSucceeded && capturedAfter && markerMoved &&
        flagsAfter == flagsBefore && restored;
    result << "STATUS=" << (passed ? "PASS" : "FAIL") << "\n";
    if (!passed) {
        std::wcerr << L"Native Explorer drag oracle failed: "
                   << errorMessage << L" / " << restoreError << L"\n";
        return 230;
    }
    return 0;
}

int RunSmokeDesktopInternalDrag(HINSTANCE instance) {
    AttachParentConsole();
    SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto environmentValue = [](const wchar_t* name) {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) return std::wstring{};
        std::wstring value(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            name, value.data(), required);
        if (copied == 0 || copied >= required) return std::wstring{};
        value.resize(copied);
        return value;
    };
    const std::wstring markerA =
        environmentValue(L"LATTICE_INTERNAL_DRAG_MARKER_A");
    const std::wstring markerB =
        environmentValue(L"LATTICE_INTERNAL_DRAG_MARKER_B");
    const std::filesystem::path resultRoot = environmentValue(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR");
    std::ofstream result(
        resultRoot / L"desktop-internal-drag-result.txt",
        std::ios::trunc);
    const auto markerIsValid = [](const std::wstring& path) {
        return !path.empty() &&
            std::filesystem::path(path).filename().wstring().rfind(
                L".lattice-internal-drag-", 0) == 0 &&
            GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    if (resultRoot.empty() || !markerIsValid(markerA) ||
        !markerIsValid(markerB) ||
        CompareStringOrdinal(
            markerA.c_str(), -1, markerB.c_str(), -1, TRUE) ==
                CSTR_EQUAL) {
        result << "INPUT_REJECTED=1\nSTATUS=FAIL\n";
        return 201;
    }

    DesktopLayout layout;
    std::vector<DesktopPosition> positionsBefore;
    DWORD flagsBefore = 0;
    std::wstring errorMessage;
    if (!layout.CaptureAllPositions(positionsBefore, errorMessage) ||
        !layout.CaptureViewFlags(flagsBefore, errorMessage)) {
        result << "SNAPSHOT_FAILED=1\n";
        result << "ERROR=" << Utf8Text(errorMessage) << "\n";
        result << "POSITIONS=" << positionsBefore.size() << "\n";
        result << "STATUS=FAIL\n";
        return 202;
    }
    DesktopSurfaceWindow probeSurface(instance);
    if (!probeSurface.Create({}, errorMessage)) {
        result << "SURFACE_CREATE_FAILED=1\nSTATUS=FAIL\n";
        return 204;
    }
    DesktopViewSnapshot before = probeSurface.Snapshot();
    before.viewFlags = flagsBefore;
    const auto findItem = [&](const std::wstring& path) {
        return std::find_if(
            before.items.begin(), before.items.end(),
            [&](const DesktopViewItem& item) {
                return CompareStringOrdinal(
                    item.path.c_str(), -1, path.c_str(), -1, TRUE) ==
                    CSTR_EQUAL;
            });
    };
    auto itemA = findItem(markerA);
    auto itemB = findItem(markerB);
    if (itemA == before.items.end() || itemB == before.items.end()) {
        result << "MARKERS_NOT_VISIBLE=1\nSTATUS=FAIL\n";
        return 203;
    }
    const auto identityA = ReadStableFileIdentity(markerA);
    const auto identityB = ReadStableFileIdentity(markerB);
    const auto bytesA = ReadFileBytes(markerA);
    const auto bytesB = ReadFileBytes(markerB);
    const std::filesystem::path desktopRoot =
        std::filesystem::path(markerA).parent_path();
    const auto directoryEntries = [&]() {
        std::vector<std::wstring> names;
        std::error_code error;
        for (std::filesystem::directory_iterator it(desktopRoot, error), end;
             !error && it != end; it.increment(error)) {
            names.push_back(it->path().filename().wstring());
        }
        if (error) names.clear();
        std::sort(names.begin(), names.end());
        return names;
    };
    const std::vector<std::wstring> entriesBefore = directoryEntries();

    std::vector<std::wstring> assigned;
    assigned.reserve(before.items.size() - 2);
    for (const DesktopViewItem& item : before.items) {
        if (CompareStringOrdinal(
                item.path.c_str(), -1, markerA.c_str(), -1, TRUE) !=
                CSTR_EQUAL &&
            CompareStringOrdinal(
                item.path.c_str(), -1, markerB.c_str(), -1, TRUE) !=
                CSTR_EQUAL) {
            assigned.push_back(item.path);
        }
    }

    probeSurface.Show();
    const auto pumpProbe = [](DWORD milliseconds) {
        const ULONGLONG deadline = GetTickCount64() + milliseconds;
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjects(
                0, nullptr, FALSE, 8, QS_ALLINPUT);
        } while (GetTickCount64() < deadline);
    };
    pumpProbe(250);
    const int fixtureCellWidth =
        DesktopSurfaceWindowSmokeAccess::CellWidth(probeSurface);
    const int fixtureCellHeight =
        DesktopSurfaceWindowSmokeAccess::CellHeight(probeSurface);
    const int fixtureIconSize =
        DesktopSurfaceWindowSmokeAccess::IconSize(probeSurface);
    const int fixtureColumns = (std::max)(
        1, static_cast<int>(
            (before.screenRect.right - before.screenRect.left) /
            (std::max)(1, fixtureCellWidth)));
    const int fixtureRows = (std::max)(
        1, static_cast<int>(
            (before.screenRect.bottom - before.screenRect.top) /
            (std::max)(1, fixtureCellHeight)));
    std::vector<POINT> freeExposedSlots;
    for (int row = -fixtureRows; row <= fixtureRows; ++row) {
        for (int column = -fixtureColumns;
             column <= fixtureColumns; ++column) {
            const POINT slot{
                itemA->screenPoint.x + column * fixtureCellWidth,
                itemA->screenPoint.y + row * fixtureCellHeight};
            if (slot.x < before.screenRect.left ||
                slot.y < before.screenRect.top ||
                slot.x + fixtureIconSize > before.screenRect.right ||
                slot.y + fixtureCellHeight > before.screenRect.bottom) {
                continue;
            }
            bool occupied = false;
            for (const DesktopViewItem& item : before.items) {
                if (CompareStringOrdinal(
                        item.path.c_str(), -1, markerA.c_str(), -1, TRUE) ==
                            CSTR_EQUAL ||
                    CompareStringOrdinal(
                        item.path.c_str(), -1, markerB.c_str(), -1, TRUE) ==
                            CSTR_EQUAL) {
                    continue;
                }
                if (std::abs(item.screenPoint.x - slot.x) <
                        fixtureCellWidth / 2 &&
                    std::abs(item.screenPoint.y - slot.y) <
                        fixtureCellHeight / 2) {
                    occupied = true;
                    break;
                }
            }
            const POINT hitPoint{
                slot.x + fixtureIconSize / 2,
                slot.y + fixtureIconSize / 2};
            if (!occupied &&
                WindowFromPoint(hitPoint) == probeSurface.Window()) {
                freeExposedSlots.push_back(slot);
            }
        }
    }
    const auto samePoint = [](POINT left, POINT right) {
        return left.x == right.x && left.y == right.y;
    };
    const auto containsSlot = [&](POINT target) {
        return std::any_of(
            freeExposedSlots.begin(), freeExposedSlots.end(),
            [&](POINT value) { return samePoint(value, target); });
    };
    POINT fixtureSourceA{};
    POINT fixtureSourceB{};
    POINT fixtureTargetA{};
    bool fixtureFound = false;
    const std::array<POINT, 2> pairShapes{{
        POINT{fixtureCellWidth, 0},
        POINT{0, fixtureCellHeight}}};
    for (const POINT shape : pairShapes) {
        for (const POINT source : freeExposedSlots) {
            const POINT sourceB{
                source.x + shape.x, source.y + shape.y};
            if (!containsSlot(sourceB)) continue;
            for (const POINT target : freeExposedSlots) {
                const POINT targetB{
                    target.x + shape.x, target.y + shape.y};
                if (!containsSlot(targetB) ||
                    samePoint(source, target) ||
                    samePoint(source, targetB) ||
                    samePoint(sourceB, target) ||
                    samePoint(sourceB, targetB)) {
                    continue;
                }
                fixtureSourceA = source;
                fixtureSourceB = sourceB;
                fixtureTargetA = target;
                fixtureFound = true;
                break;
            }
            if (fixtureFound) break;
        }
        if (fixtureFound) break;
    }
    probeSurface.Close();
    pumpProbe(80);
    if (!fixtureFound) {
        result << "EXPOSED_FIXTURE_SLOTS_FAILED=1\n";
        result << "FREE_EXPOSED_SLOTS=" << freeExposedSlots.size() << "\n";
        result << "STATUS=FAIL\n";
        return 209;
    }
    std::vector<DesktopPosition> confirmedFixture;
    if (!layout.PositionScreenItemsOnce(
            {{markerA, fixtureSourceA}, {markerB, fixtureSourceB}},
            confirmedFixture, errorMessage) ||
        confirmedFixture.size() != 2) {
        result << "FIXTURE_POSITION_FAILED=1\nSTATUS=FAIL\n";
        return 210;
    }
    positionsBefore.clear();
    if (!layout.CaptureAllPositions(positionsBefore, errorMessage) ||
        !layout.CaptureViewFlags(before.viewFlags, errorMessage)) {
        result << "PREPARED_SNAPSHOT_FAILED=1\nSTATUS=FAIL\n";
        return 211;
    }
    for (DesktopViewItem& item : before.items) {
        const auto prepared = std::find_if(
            positionsBefore.begin(), positionsBefore.end(),
            [&](const DesktopPosition& position) {
                return CompareStringOrdinal(
                           position.path.c_str(), -1,
                           item.path.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
        if (prepared == positionsBefore.end()) {
            result << "PREPARED_IDENTITY_MISSING=1\nSTATUS=FAIL\n";
            return 211;
        }
        item.viewPoint = prepared->point;
        item.screenPoint = prepared->point;
        if (ClientToScreen(
                before.listViewWindow,
                &item.screenPoint) == FALSE) {
            result << "PREPARED_POINT_MAP_FAILED=1\nSTATUS=FAIL\n";
            return 211;
        }
    }
    itemA = findItem(markerA);
    itemB = findItem(markerB);
    if (itemA == before.items.end() || itemB == before.items.end()) {
        result << "PREPARED_MARKERS_NOT_VISIBLE=1\nSTATUS=FAIL\n";
        return 212;
    }

    DesktopSurfaceWindow surface(instance);
    if (!surface.Create(assigned, errorMessage)) {
        result << "SURFACE_CREATE_FAILED=1\nSTATUS=FAIL\n";
        return 204;
    }
    ConfigStore displayStore;
    std::vector<DesktopPosition> committedViewPositions;
    surface.SetDisplayPositionCommitHandler(
        [&](const std::vector<DesktopPosition>& positions) {
            committedViewPositions = positions;
            std::vector<DesktopPlacementConfig> placements;
            placements.reserve(positions.size());
            for (const DesktopPosition& position : positions) {
                placements.push_back(DesktopPlacementConfig{
                    position.path,
                    position.point.x,
                    position.point.y});
            }
            return displayStore.SaveDesktopDisplayPositionsAsync(
                placements);
        });
    surface.Show();
    const auto pumpFor = [](DWORD milliseconds) {
        const ULONGLONG deadline = GetTickCount64() + milliseconds;
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjects(
                0, nullptr, FALSE, 8, QS_ALLINPUT);
        } while (GetTickCount64() < deadline);
    };
    pumpFor(350);
    const HWND surfaceWindow = surface.Window();
    const auto rectA =
        DesktopSurfaceWindowSmokeAccess::InteractionRectForIdentity(
            surface, markerA);
    const auto rectB =
        DesktopSurfaceWindowSmokeAccess::InteractionRectForIdentity(
            surface, markerB);
    if (surface.VisibleItemCount() != 2 || !rectA.has_value() ||
        !rectB.has_value()) {
        surface.Close();
        result << "MARKER_PARTITION_FAILED=1\nSTATUS=FAIL\n";
        return 205;
    }
    if (((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
          GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) |
          GetAsyncKeyState(VK_MENU)) & 0x8000) != 0) {
        surface.Close();
        result << "INPUT_BUSY=1\nSTATUS=FAIL\n";
        return 206;
    }

    const RECT bounds =
        DesktopSurfaceWindowSmokeAccess::ClientBounds(surface);
    const std::array<std::pair<POINT, POINT>, 4> marqueeCandidates{{
        {{bounds.left + 2, bounds.top + 2},
         {bounds.right - 2, bounds.bottom - 2}},
        {{bounds.right - 2, bounds.bottom - 2},
         {bounds.left + 2, bounds.top + 2}},
        {{bounds.right - 2, bounds.top + 2},
         {bounds.left + 2, bounds.bottom - 2}},
        {{bounds.left + 2, bounds.bottom - 2},
         {bounds.right - 2, bounds.top + 2}},
    }};
    std::optional<std::pair<POINT, POINT>> marquee;
    for (const auto& candidate : marqueeCandidates) {
        POINT startScreen = candidate.first;
        ClientToScreen(surfaceWindow, &startScreen);
        if (DesktopSurfaceWindowSmokeAccess::IsBlankPoint(
                surface, candidate.first) &&
            WindowFromPoint(startScreen) == surfaceWindow) {
            marquee = candidate;
            break;
        }
    }
    if (!marquee.has_value()) {
        surface.Close();
        result << "MARQUEE_POINTS_FAILED=1\nSTATUS=FAIL\n";
        return 207;
    }

    const int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualWidth =
        (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    const int virtualHeight =
        (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    const auto sendMouse = [&](POINT screenPoint, DWORD flags) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = MulDiv(
            screenPoint.x - virtualLeft, 65535, virtualWidth);
        input.mi.dy = MulDiv(
            screenPoint.y - virtualTop, 65535, virtualHeight);
        input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE |
            MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE | flags;
        return SendInput(1, &input, sizeof(input)) == 1;
    };
    const auto sendEscape = []() {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_ESCAPE;
        const bool keyDown = SendInput(1, &input, sizeof(input)) == 1;
        Sleep(80);
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        const bool keyUp = SendInput(1, &input, sizeof(input)) == 1;
        return keyDown && keyUp;
    };
    POINT savedCursor{};
    GetCursorPos(&savedCursor);
    POINT marqueeStart = marquee->first;
    POINT marqueeEnd = marquee->second;
    ClientToScreen(surfaceWindow, &marqueeStart);
    ClientToScreen(surfaceWindow, &marqueeEnd);
    bool marqueePassed = sendMouse(marqueeStart, 0);
    pumpFor(30);
    marqueePassed = marqueePassed &&
        sendMouse(marqueeStart, MOUSEEVENTF_LEFTDOWN);
    pumpFor(30);
    marqueePassed = marqueePassed && sendMouse(marqueeEnd, 0);
    pumpFor(60);
    const bool marqueeInputSequenceSucceeded = marqueePassed;
    const bool marqueeReleased =
        sendMouse(marqueeEnd, MOUSEEVENTF_LEFTUP);
    pumpFor(80);
    const std::vector<std::wstring> selected =
        DesktopSurfaceWindowSmokeAccess::SelectedPaths(surface);
    const bool marqueeSelectedA =
        std::any_of(selected.begin(), selected.end(),
            [&](const std::wstring& value) {
                return CompareStringOrdinal(
                    value.c_str(), -1, markerA.c_str(), -1, TRUE) ==
                    CSTR_EQUAL;
            });
    const bool marqueeSelectedB =
        std::any_of(selected.begin(), selected.end(),
            [&](const std::wstring& value) {
                return CompareStringOrdinal(
                    value.c_str(), -1, markerB.c_str(), -1, TRUE) ==
                    CSTR_EQUAL;
            });
    marqueePassed = marqueeInputSequenceSucceeded && marqueeReleased &&
        selected.size() == 2 && marqueeSelectedA && marqueeSelectedB;
    if (!marqueePassed) {
        sendMouse(marqueeEnd, MOUSEEVENTF_LEFTUP);
        sendMouse(savedCursor, 0);
        surface.Close();
        result << "REAL_MARQUEE_FAILED=1\n";
        result << "REAL_MARQUEE_INPUT_SEQUENCE_SUCCEEDED="
               << marqueeInputSequenceSucceeded << "\n";
        result << "REAL_MARQUEE_RELEASED=" << marqueeReleased << "\n";
        result << "REAL_MARQUEE_SELECTED_COUNT=" << selected.size() << "\n";
        result << "REAL_MARQUEE_SELECTED_A=" << marqueeSelectedA << "\n";
        result << "REAL_MARQUEE_SELECTED_B=" << marqueeSelectedB << "\n";
        result << "STATUS=FAIL\n";
        return 208;
    }

    POINT dragStart{
        (rectA->left + rectA->right) / 2,
        (rectA->top + rectA->bottom) / 2};
    ClientToScreen(surfaceWindow, &dragStart);
    const POINT dragDelta{
        fixtureTargetA.x - fixtureSourceA.x,
        fixtureTargetA.y - fixtureSourceA.y};
    const POINT dragEnd{
        dragStart.x + dragDelta.x,
        dragStart.y + dragDelta.y};
    if (WindowFromPoint(dragStart) != surfaceWindow ||
        WindowFromPoint(dragEnd) != surfaceWindow) {
        sendMouse(savedCursor, 0);
        surface.Close();
        result << "FIXTURE_ROUTE_CHANGED=1\nSTATUS=FAIL\n";
        return 213;
    }

    const ULONGLONG iconReadyDeadline = GetTickCount64() + 2500;
    while (!DesktopSurfaceWindowSmokeAccess::AllVisibleIconsReady(surface) &&
           GetTickCount64() < iconReadyDeadline) {
        pumpFor(16);
    }
    POINT dragStartClient = dragStart;
    ScreenToClient(surfaceWindow, &dragStartClient);
    SHDRAGIMAGE expectedDragImage{};
    const bool dragCompositeReady =
        DesktopSurfaceWindowSmokeAccess::BuildShellDragImage(
            surface, dragStartClient, expectedDragImage);
    RECT expectedGhostRect{};
    if (dragCompositeReady) {
        expectedGhostRect = RECT{
            dragEnd.x - expectedDragImage.ptOffset.x,
            dragEnd.y - expectedDragImage.ptOffset.y,
            dragEnd.x - expectedDragImage.ptOffset.x +
                expectedDragImage.sizeDragImage.cx,
            dragEnd.y - expectedDragImage.ptOffset.y +
                expectedDragImage.sizeDragImage.cy};
        IntersectRect(
            &expectedGhostRect,
            &expectedGhostRect,
            &before.screenRect);
    }
    if (expectedDragImage.hbmpDragImage != nullptr) {
        DeleteObject(expectedDragImage.hbmpDragImage);
        expectedDragImage.hbmpDragImage = nullptr;
    }
    if (!dragCompositeReady || IsRectEmpty(&expectedGhostRect)) {
        sendMouse(savedCursor, 0);
        surface.Close();
        result << "DRAG_COMPOSITE_NOT_READY=1\nSTATUS=FAIL\n";
        return 214;
    }

    // Establish the reference frame with the cursor already at the future
    // drag hotspot. GDI screen capture does not normally include the cursor,
    // but keeping its position identical also avoids compositor/cursor noise.
    bool baselineReady = sendMouse(dragEnd, 0);
    pumpFor(80);
    DwmFlush();
    std::vector<std::uint32_t> feedbackBaseline;
    baselineReady = baselineReady && CaptureScreenPixels(
        before.screenRect, feedbackBaseline);
    const size_t shellDragBefore =
        DesktopSurfaceWindowSmokeAccess::ShellDragStartCount(surface);
    std::atomic<bool> inputSucceeded{true};
    std::atomic<bool> inputFinished{false};
    constexpr size_t kFeedbackFrameCount = 5;
    std::array<std::vector<std::uint32_t>, kFeedbackFrameCount>
        feedbackFrames;
    std::array<bool, kFeedbackFrameCount> feedbackFrameCaptured{};
    std::thread injector([&]() {
        bool ok = sendMouse(dragStart, 0);
        Sleep(30);
        ok = sendMouse(dragStart, MOUSEEVENTF_LEFTDOWN) && ok;
        Sleep(40);
        ok = sendMouse(dragEnd, 0) && ok;
        Sleep(60);
        for (size_t index = 0; index < kFeedbackFrameCount; ++index) {
            DwmFlush();
            feedbackFrameCaptured[index] = CaptureScreenPixels(
                before.screenRect, feedbackFrames[index]);
            Sleep(24);
        }
        ok = sendMouse(dragEnd, MOUSEEVENTF_LEFTUP) && ok;
        inputSucceeded.store(ok);
        inputFinished.store(true);
    });
    const ULONGLONG inputDeadline = GetTickCount64() + 5000;
    while (!inputFinished.load() && GetTickCount64() < inputDeadline) {
        pumpFor(16);
    }
    injector.join();
    pumpFor(250);
    sendMouse(savedCursor, 0);
    const HRESULT multiDragImageInitializationResult =
        LastShellDragImageInitializationResultForTesting();

    const LONG captureWidth =
        before.screenRect.right - before.screenRect.left;
    const size_t capturePixelCount = feedbackBaseline.size();
    size_t capturedFeedbackFrames = 0;
    size_t maximumGhostPixelChanges = 0;
    size_t maximumBlackTransitions = 0;
    for (size_t frameIndex = 0;
         frameIndex < kFeedbackFrameCount; ++frameIndex) {
        if (!feedbackFrameCaptured[frameIndex] ||
            feedbackFrames[frameIndex].size() != capturePixelCount) {
            continue;
        }
        ++capturedFeedbackFrames;
        size_t ghostChanges = 0;
        size_t blackTransitions = 0;
        for (LONG y = before.screenRect.top;
             y < before.screenRect.bottom; ++y) {
            const size_t row = static_cast<size_t>(
                y - before.screenRect.top) * captureWidth;
            for (LONG x = before.screenRect.left;
                 x < before.screenRect.right; ++x) {
                const size_t pixelIndex = row + static_cast<size_t>(
                    x - before.screenRect.left);
                const std::uint32_t first = feedbackBaseline[pixelIndex];
                const std::uint32_t second =
                    feedbackFrames[frameIndex][pixelIndex];
                const int firstBlue = first & 0xFF;
                const int firstGreen = (first >> 8) & 0xFF;
                const int firstRed = (first >> 16) & 0xFF;
                const int secondBlue = second & 0xFF;
                const int secondGreen = (second >> 8) & 0xFF;
                const int secondRed = (second >> 16) & 0xFF;
                const int channelDifference =
                    std::abs(firstRed - secondRed) +
                    std::abs(firstGreen - secondGreen) +
                    std::abs(firstBlue - secondBlue);
                if (x >= expectedGhostRect.left &&
                    x < expectedGhostRect.right &&
                    y >= expectedGhostRect.top &&
                    y < expectedGhostRect.bottom &&
                    channelDifference >= 12) {
                    ++ghostChanges;
                }
                if (firstRed + firstGreen + firstBlue >= 150 &&
                    secondRed + secondGreen + secondBlue <= 24) {
                    ++blackTransitions;
                }
            }
        }
        maximumGhostPixelChanges = (std::max)(
            maximumGhostPixelChanges, ghostChanges);
        maximumBlackTransitions = (std::max)(
            maximumBlackTransitions, blackTransitions);
    }
    const size_t ghostRectArea = static_cast<size_t>(
        expectedGhostRect.right - expectedGhostRect.left) *
        static_cast<size_t>(
            expectedGhostRect.bottom - expectedGhostRect.top);
    const size_t minimumGhostChanges = (std::max)(
        static_cast<size_t>(200), ghostRectArea / 100);
    const bool dragGhostVisible = baselineReady &&
        capturedFeedbackFrames == kFeedbackFrameCount &&
        maximumGhostPixelChanges >= minimumGhostChanges;
    const bool noBlackFrame = baselineReady &&
        capturedFeedbackFrames == kFeedbackFrameCount &&
        maximumBlackTransitions < capturePixelCount / 20;
    if (!dragGhostVisible || !noBlackFrame) {
        SaveCapturedPixelsBmp(
            before.screenRect,
            feedbackBaseline,
            resultRoot / L"desktop-drag-feedback-baseline.bmp");
        for (size_t index = 0; index < kFeedbackFrameCount; ++index) {
            if (feedbackFrameCaptured[index]) {
                SaveCapturedPixelsBmp(
                    before.screenRect,
                    feedbackFrames[index],
                    resultRoot /
                        (L"desktop-drag-feedback-held-" +
                         std::to_wstring(index) + L".bmp"));
            }
        }
    }

    const bool multiShellDragStarted =
        DesktopSurfaceWindowSmokeAccess::ShellDragStartCount(surface) ==
        shellDragBefore + 1;
    const auto singleRectA =
        DesktopSurfaceWindowSmokeAccess::InteractionRectForIdentity(
            surface, markerA);
    if (!singleRectA.has_value()) {
        sendMouse(savedCursor, 0);
        surface.Close();
        result << "SINGLE_RECT_MISSING=1\nSTATUS=FAIL\n";
        return 215;
    }
    POINT singleStartClient{
        (singleRectA->left + singleRectA->right) / 2,
        (singleRectA->top + singleRectA->bottom) / 2};
    POINT singleStart = singleStartClient;
    ClientToScreen(surfaceWindow, &singleStart);
    bool singleSelectionReady = sendMouse(singleStart, 0);
    pumpFor(30);
    singleSelectionReady = singleSelectionReady &&
        sendMouse(singleStart, MOUSEEVENTF_LEFTDOWN);
    pumpFor(30);
    singleSelectionReady = singleSelectionReady &&
        sendMouse(singleStart, MOUSEEVENTF_LEFTUP);
    pumpFor(80);
    const std::vector<std::wstring> singleSelected =
        DesktopSurfaceWindowSmokeAccess::SelectedPaths(surface);
    singleSelectionReady = singleSelectionReady &&
        singleSelected.size() == 1 &&
        CompareStringOrdinal(
            singleSelected.front().c_str(), -1,
            markerA.c_str(), -1, TRUE) == CSTR_EQUAL;
    if (!singleSelectionReady) {
        sendMouse(savedCursor, 0);
        surface.Close();
        result << "SINGLE_SELECTION_FAILED=1\nSTATUS=FAIL\n";
        return 216;
    }
    pumpFor(GetDoubleClickTime() + 100);

    const LONG horizontalTravel =
        singleStart.x + 120 < before.screenRect.right - 4 ? 120 : -120;
    const LONG verticalTravel =
        singleStart.y + 96 < before.screenRect.bottom - 4 ? 96 : -96;
    const POINT singleEnd{
        singleStart.x + horizontalTravel,
        singleStart.y + verticalTravel};
    const LONG thresholdTravelX = (std::max)(
        12L,
        static_cast<LONG>(GetSystemMetrics(SM_CXDRAG) + 2));
    const LONG thresholdTravelY = (std::max)(
        12L,
        static_cast<LONG>(GetSystemMetrics(SM_CYDRAG) + 2));
    const POINT singleThresholdPoint{
        singleStart.x + (horizontalTravel > 0
            ? thresholdTravelX
            : -thresholdTravelX),
        singleStart.y + (verticalTravel > 0
            ? thresholdTravelY
            : -thresholdTravelY)};
    if (WindowFromPoint(singleStart) != surfaceWindow ||
        WindowFromPoint(singleThresholdPoint) != surfaceWindow ||
        WindowFromPoint(singleEnd) != surfaceWindow) {
        sendMouse(savedCursor, 0);
        surface.Close();
        result << "SINGLE_ROUTE_CHANGED=1\nSTATUS=FAIL\n";
        return 217;
    }
    SHDRAGIMAGE singleDragImage{};
    const bool singleCompositeReady =
        DesktopSurfaceWindowSmokeAccess::BuildShellDragImage(
            surface, singleStartClient, singleDragImage);
    RECT expectedSingleGhostRect{};
    if (singleCompositeReady) {
        expectedSingleGhostRect = RECT{
            singleEnd.x - singleDragImage.ptOffset.x,
            singleEnd.y - singleDragImage.ptOffset.y,
            singleEnd.x - singleDragImage.ptOffset.x +
                singleDragImage.sizeDragImage.cx,
            singleEnd.y - singleDragImage.ptOffset.y +
                singleDragImage.sizeDragImage.cy};
        IntersectRect(
            &expectedSingleGhostRect,
            &expectedSingleGhostRect,
            &before.screenRect);
    }
    if (singleDragImage.hbmpDragImage != nullptr) {
        DeleteObject(singleDragImage.hbmpDragImage);
        singleDragImage.hbmpDragImage = nullptr;
    }
    if (!singleCompositeReady || IsRectEmpty(&expectedSingleGhostRect)) {
        sendMouse(savedCursor, 0);
        surface.Close();
        result << "SINGLE_COMPOSITE_NOT_READY=1\nSTATUS=FAIL\n";
        return 218;
    }

    const auto cancelBeforeA =
        DesktopSurfaceWindowSmokeAccess::VisibleScreenPointForIdentity(
            surface, markerA);
    const auto cancelBeforeB =
        DesktopSurfaceWindowSmokeAccess::VisibleScreenPointForIdentity(
            surface, markerB);
    bool singleBaselineReady = sendMouse(singleEnd, 0);
    pumpFor(80);
    DwmFlush();
    std::vector<std::uint32_t> singleFeedbackBaseline;
    singleBaselineReady = singleBaselineReady && CaptureScreenPixels(
        before.screenRect, singleFeedbackBaseline);
    const size_t singleShellDragBefore =
        DesktopSurfaceWindowSmokeAccess::ShellDragStartCount(surface);
    std::atomic<bool> singleInputSucceeded{true};
    std::atomic<bool> singleInputFinished{false};
    std::array<std::vector<std::uint32_t>, kFeedbackFrameCount>
        singleFeedbackFrames;
    std::array<bool, kFeedbackFrameCount>
        singleFeedbackFrameCaptured{};
    std::thread singleInjector([&]() {
        bool ok = sendMouse(singleStart, 0);
        Sleep(30);
        ok = sendMouse(singleStart, MOUSEEVENTF_LEFTDOWN) && ok;
        Sleep(40);
        ok = sendMouse(singleThresholdPoint, 0) && ok;
        Sleep(50);
        ok = sendMouse(singleEnd, 0) && ok;
        Sleep(60);
        for (size_t index = 0; index < kFeedbackFrameCount; ++index) {
            DwmFlush();
            singleFeedbackFrameCaptured[index] = CaptureScreenPixels(
                before.screenRect, singleFeedbackFrames[index]);
            Sleep(24);
        }
        ok = sendEscape() && ok;
        Sleep(40);
        ok = sendMouse(singleEnd, MOUSEEVENTF_LEFTUP) && ok;
        singleInputSucceeded.store(ok);
        singleInputFinished.store(true);
    });
    const ULONGLONG singleInputDeadline = GetTickCount64() + 5000;
    while (!singleInputFinished.load() &&
           GetTickCount64() < singleInputDeadline) {
        pumpFor(16);
    }
    singleInjector.join();
    pumpFor(250);
    sendMouse(savedCursor, 0);
    const HRESULT singleDragImageInitializationResult =
        LastShellDragImageInitializationResultForTesting();

    size_t singleCapturedFeedbackFrames = 0;
    size_t maximumSingleGhostPixelChanges = 0;
    size_t maximumSingleBlackTransitions = 0;
    for (size_t frameIndex = 0;
         frameIndex < kFeedbackFrameCount; ++frameIndex) {
        if (!singleFeedbackFrameCaptured[frameIndex] ||
            singleFeedbackBaseline.size() != capturePixelCount ||
            singleFeedbackFrames[frameIndex].size() != capturePixelCount) {
            continue;
        }
        ++singleCapturedFeedbackFrames;
        size_t ghostChanges = 0;
        size_t blackTransitions = 0;
        for (LONG y = before.screenRect.top;
             y < before.screenRect.bottom; ++y) {
            const size_t row = static_cast<size_t>(
                y - before.screenRect.top) * captureWidth;
            for (LONG x = before.screenRect.left;
                 x < before.screenRect.right; ++x) {
                const size_t pixelIndex = row + static_cast<size_t>(
                    x - before.screenRect.left);
                const std::uint32_t first =
                    singleFeedbackBaseline[pixelIndex];
                const std::uint32_t second =
                    singleFeedbackFrames[frameIndex][pixelIndex];
                const int firstBlue = first & 0xFF;
                const int firstGreen = (first >> 8) & 0xFF;
                const int firstRed = (first >> 16) & 0xFF;
                const int secondBlue = second & 0xFF;
                const int secondGreen = (second >> 8) & 0xFF;
                const int secondRed = (second >> 16) & 0xFF;
                const int channelDifference =
                    std::abs(firstRed - secondRed) +
                    std::abs(firstGreen - secondGreen) +
                    std::abs(firstBlue - secondBlue);
                if (x >= expectedSingleGhostRect.left &&
                    x < expectedSingleGhostRect.right &&
                    y >= expectedSingleGhostRect.top &&
                    y < expectedSingleGhostRect.bottom &&
                    channelDifference >= 12) {
                    ++ghostChanges;
                }
                if (firstRed + firstGreen + firstBlue >= 150 &&
                    secondRed + secondGreen + secondBlue <= 24) {
                    ++blackTransitions;
                }
            }
        }
        maximumSingleGhostPixelChanges = (std::max)(
            maximumSingleGhostPixelChanges, ghostChanges);
        maximumSingleBlackTransitions = (std::max)(
            maximumSingleBlackTransitions, blackTransitions);
    }
    const size_t singleGhostRectArea = static_cast<size_t>(
        expectedSingleGhostRect.right - expectedSingleGhostRect.left) *
        static_cast<size_t>(
            expectedSingleGhostRect.bottom - expectedSingleGhostRect.top);
    const size_t minimumSingleGhostChanges = (std::max)(
        static_cast<size_t>(80), singleGhostRectArea / 100);
    const bool singleDragGhostVisible = singleBaselineReady &&
        singleCapturedFeedbackFrames == kFeedbackFrameCount &&
        maximumSingleGhostPixelChanges >= minimumSingleGhostChanges;
    const bool singleNoBlackFrame = singleBaselineReady &&
        singleCapturedFeedbackFrames == kFeedbackFrameCount &&
        maximumSingleBlackTransitions < capturePixelCount / 20;
    const auto cancelAfterA =
        DesktopSurfaceWindowSmokeAccess::VisibleScreenPointForIdentity(
            surface, markerA);
    const auto cancelAfterB =
        DesktopSurfaceWindowSmokeAccess::VisibleScreenPointForIdentity(
            surface, markerB);
    const size_t singleShellDragAfter =
        DesktopSurfaceWindowSmokeAccess::ShellDragStartCount(surface);
    const bool singleCancelStable =
        cancelBeforeA.has_value() && cancelAfterA.has_value() &&
        cancelBeforeB.has_value() && cancelAfterB.has_value() &&
        cancelBeforeA->x == cancelAfterA->x &&
        cancelBeforeA->y == cancelAfterA->y &&
        cancelBeforeB->x == cancelAfterB->x &&
        cancelBeforeB->y == cancelAfterB->y;
    const bool singleCancelPassed = singleInputSucceeded.load() &&
        singleShellDragAfter == singleShellDragBefore + 1 &&
        singleDragGhostVisible && singleNoBlackFrame && singleCancelStable;
    if (!singleDragGhostVisible || !singleNoBlackFrame) {
        SaveCapturedPixelsBmp(
            before.screenRect,
            singleFeedbackBaseline,
            resultRoot / L"desktop-single-drag-feedback-baseline.bmp");
        for (size_t index = 0; index < kFeedbackFrameCount; ++index) {
            if (singleFeedbackFrameCaptured[index]) {
                SaveCapturedPixelsBmp(
                    before.screenRect,
                    singleFeedbackFrames[index],
                    resultRoot /
                        (L"desktop-single-drag-feedback-held-" +
                         std::to_wstring(index) + L".bmp"));
            }
        }
    }

    const auto displayedAfterA =
        DesktopSurfaceWindowSmokeAccess::VisibleScreenPointForIdentity(
            surface, markerA);
    const auto displayedAfterB =
        DesktopSurfaceWindowSmokeAccess::VisibleScreenPointForIdentity(
            surface, markerB);
    const bool displayPersisted =
        ConfigStore::DrainPendingWrites(5000);
    const AppConfig persistedDisplayConfig =
        displayStore.LoadAppConfig();

    DesktopViewSnapshot after;
    std::vector<DesktopPosition> positionsAfter;
    DWORD flagsAfter = 0;
    const bool capturedAfter =
        layout.CaptureViewSnapshot(after, errorMessage) &&
        layout.CaptureAllPositions(positionsAfter, errorMessage) &&
        layout.CaptureViewFlags(flagsAfter, errorMessage);
    surface.Close();

    const auto findPosition = [](
        const std::vector<DesktopPosition>& positions,
        const std::wstring& path) {
        return std::find_if(
            positions.begin(), positions.end(),
            [&](const DesktopPosition& position) {
                return CompareStringOrdinal(
                    position.path.c_str(), -1, path.c_str(), -1, TRUE) ==
                    CSTR_EQUAL;
            });
    };
    const auto beforeA = findPosition(positionsBefore, markerA);
    const auto beforeB = findPosition(positionsBefore, markerB);
    bool underlyingPositionsStable = capturedAfter &&
        positionsBefore.size() == positionsAfter.size();
    for (const DesktopPosition& position : positionsBefore) {
        const auto current = findPosition(positionsAfter, position.path);
        if (current == positionsAfter.end() ||
            current->point.x != position.point.x ||
            current->point.y != position.point.y) {
            underlyingPositionsStable = false;
            break;
        }
    }
    const bool groupMoved = beforeA != positionsBefore.end() &&
        beforeB != positionsBefore.end() &&
        displayedAfterA.has_value() && displayedAfterB.has_value() &&
        (displayedAfterA->x != beforeA->point.x ||
         displayedAfterA->y != beforeA->point.y) &&
        displayedAfterA->x - beforeA->point.x ==
            displayedAfterB->x - beforeB->point.x &&
        displayedAfterA->y - beforeA->point.y ==
            displayedAfterB->y - beforeB->point.y;
    const auto findDisplayPlacement = [](
        const AppConfig& config,
        const std::wstring& identity) {
        return std::find_if(
            config.desktopDisplayLayout.begin(),
            config.desktopDisplayLayout.end(),
            [&](const DesktopPlacementConfig& value) {
                return CompareStringOrdinal(
                           value.path.c_str(), -1,
                           identity.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
    };
    const auto persistedA = findDisplayPlacement(
        persistedDisplayConfig, markerA);
    const auto persistedB = findDisplayPlacement(
        persistedDisplayConfig, markerB);
    const bool persistedCoordinatesValid = displayPersisted &&
        committedViewPositions.size() == 2 &&
        persistedA !=
            persistedDisplayConfig.desktopDisplayLayout.end() &&
        persistedB !=
            persistedDisplayConfig.desktopDisplayLayout.end();
    std::vector<DesktopPosition> persistedPositions;
    persistedPositions.reserve(
        persistedDisplayConfig.desktopDisplayLayout.size());
    for (const DesktopPlacementConfig& placement :
         persistedDisplayConfig.desktopDisplayLayout) {
        persistedPositions.push_back(DesktopPosition{
            placement.path,
            POINT{placement.x, placement.y}});
    }
    bool restartDisplayStable = false;
    DesktopSurfaceWindow restartSurface(instance);
    if (persistedCoordinatesValid &&
        restartSurface.Create(assigned, errorMessage)) {
        restartSurface.UpdateDisplayPositions(persistedPositions);
        restartSurface.Show();
        pumpFor(250);
        const auto restartedA =
            DesktopSurfaceWindowSmokeAccess::VisibleScreenPointForIdentity(
                restartSurface, markerA);
        const auto restartedB =
            DesktopSurfaceWindowSmokeAccess::VisibleScreenPointForIdentity(
                restartSurface, markerB);
        restartDisplayStable =
            restartedA.has_value() && restartedB.has_value() &&
            displayedAfterA.has_value() && displayedAfterB.has_value() &&
            restartedA->x == displayedAfterA->x &&
            restartedA->y == displayedAfterA->y &&
            restartedB->x == displayedAfterB->x &&
            restartedB->y == displayedAfterB->y;
        restartSurface.Close();
        pumpFor(80);
    }
    const auto snapshotIdentities = [](const DesktopViewSnapshot& snapshot) {
        std::vector<std::wstring> identities;
        identities.reserve(snapshot.items.size());
        for (const DesktopViewItem& item : snapshot.items) {
            std::wstring identity = item.path;
            std::transform(
                identity.begin(), identity.end(), identity.begin(),
                [](wchar_t value) {
                    return static_cast<wchar_t>(std::towlower(value));
                });
            identities.push_back(std::move(identity));
        }
        std::sort(identities.begin(), identities.end());
        return identities;
    };
    const bool shellItemsStable = capturedAfter &&
        snapshotIdentities(before) == snapshotIdentities(after);
    const bool endpointsStable =
        SameStableFileIdentity(identityA, ReadStableFileIdentity(markerA)) &&
        SameStableFileIdentity(identityB, ReadStableFileIdentity(markerB)) &&
        bytesA == ReadFileBytes(markerA) &&
        bytesB == ReadFileBytes(markerB) &&
        entriesBefore == directoryEntries() && shellItemsStable;
    const bool dragPassed = inputSucceeded.load() && capturedAfter &&
        dragCompositeReady && dragGhostVisible && noBlackFrame &&
        multiShellDragStarted && singleCancelPassed &&
        flagsAfter == before.viewFlags && groupMoved &&
        underlyingPositionsStable && endpointsStable &&
        persistedCoordinatesValid && restartDisplayStable;

    Sleep(120);
    std::vector<DesktopPosition> stableOnce;
    std::vector<DesktopPosition> stableTwice;
    const bool stabilityCaptured =
        layout.CaptureAllPositions(stableOnce, errorMessage) &&
        (Sleep(80), layout.CaptureAllPositions(
            stableTwice, errorMessage));
    const auto samePositions = [](const auto& left, const auto& right) {
        if (left.size() != right.size()) return false;
        return std::all_of(left.begin(), left.end(),
            [&](const DesktopPosition& expected) {
                const auto actual = std::find_if(
                    right.begin(), right.end(),
                    [&](const DesktopPosition& candidate) {
                        return CompareStringOrdinal(
                            candidate.path.c_str(), -1,
                            expected.path.c_str(), -1, TRUE) ==
                                CSTR_EQUAL;
                    });
                return actual != right.end() &&
                    actual->point.x == expected.point.x &&
                    actual->point.y == expected.point.y;
            });
    };
    const bool nativeStableTwice =
        stabilityCaptured &&
        samePositions(positionsBefore, stableOnce) &&
        samePositions(stableOnce, stableTwice);
    const bool passed = dragPassed && nativeStableTwice;
    result << "REAL_MARQUEE_SELECTED_TWO=" << marqueePassed << "\n";
    result << "INTERNAL_DRAG_STARTED="
           << multiShellDragStarted << "\n";
    result << "DRAG_COMPOSITE_READY=" << dragCompositeReady << "\n";
    result << "DRAG_IMAGE_INIT_HRESULT="
           << multiDragImageInitializationResult << "\n";
    result << "DRAG_FEEDBACK_FRAMES=" << capturedFeedbackFrames << "\n";
    result << "DRAG_GHOST_VISIBLE=" << dragGhostVisible << "\n";
    result << "DRAG_GHOST_CHANGED_PIXELS="
           << maximumGhostPixelChanges << "\n";
    result << "DRAG_GHOST_MINIMUM_PIXELS="
           << minimumGhostChanges << "\n";
    result << "NO_BLACK_FRAME=" << noBlackFrame << "\n";
    result << "MAX_BLACK_TRANSITION_PIXELS="
           << maximumBlackTransitions << "\n";
    result << "SINGLE_SELECTION_READY=" << singleSelectionReady << "\n";
    result << "SINGLE_DRAG_COMPOSITE_READY="
           << singleCompositeReady << "\n";
    result << "SINGLE_DRAG_IMAGE_INIT_HRESULT="
           << singleDragImageInitializationResult << "\n";
    result << "SINGLE_DRAG_FEEDBACK_FRAMES="
           << singleCapturedFeedbackFrames << "\n";
    result << "SINGLE_DRAG_GHOST_VISIBLE="
           << singleDragGhostVisible << "\n";
    result << "SINGLE_DRAG_GHOST_CHANGED_PIXELS="
           << maximumSingleGhostPixelChanges << "\n";
    result << "SINGLE_DRAG_GHOST_MINIMUM_PIXELS="
           << minimumSingleGhostChanges << "\n";
    result << "SINGLE_NO_BLACK_FRAME=" << singleNoBlackFrame << "\n";
    result << "SINGLE_MAX_BLACK_TRANSITION_PIXELS="
           << maximumSingleBlackTransitions << "\n";
    result << "SINGLE_ESC_CANCEL_POSITION_STABLE="
           << singleCancelStable << "\n";
    result << "SINGLE_INPUT_SUCCEEDED="
           << singleInputSucceeded.load() << "\n";
    result << "SINGLE_DRAG_STARTED="
           << (singleShellDragAfter == singleShellDragBefore + 1) << "\n";
    result << "SINGLE_DRAG_COUNT_BEFORE="
           << singleShellDragBefore << "\n";
    result << "SINGLE_DRAG_COUNT_AFTER="
           << singleShellDragAfter << "\n";
    result << "SINGLE_ESC_CANCEL_PASSED="
           << singleCancelPassed << "\n";
    result << "GROUP_MOVED_WITH_RELATIVE_GEOMETRY=" << groupMoved << "\n";
    result << "NO_NEW_FILES_OR_SHORTCUTS=" << endpointsStable << "\n";
    result << "SHELL_IDENTITY_SET_STABLE=" << shellItemsStable << "\n";
    result << "EXPLORER_POSITIONS_FLAGS_UNCHANGED="
           << (underlyingPositionsStable &&
               flagsAfter == before.viewFlags) << "\n";
    result << "DISPLAY_LAYOUT_PERSISTED=" <<
        persistedCoordinatesValid << "\n";
    result << "RESTART_DISPLAY_LAYOUT_STABLE=" <<
        restartDisplayStable << "\n";
    result << "NATIVE_LAYOUT_STABLE_TWICE=" <<
        nativeStableTwice << "\n";
    result << "STATUS=" << (passed ? "PASS" : "FAIL") << "\n";
    return passed ? 0 : 210;
}

int RunSmokeDesktopCurrentConfigDrag(HINSTANCE instance) {
    AttachParentConsole();
    SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto environmentValue = [](const wchar_t* name) {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) return std::wstring{};
        std::wstring value(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(
            name, value.data(), required);
        if (copied == 0 || copied >= required) return std::wstring{};
        value.resize(copied);
        return value;
    };
    const std::filesystem::path configDirectory =
        environmentValue(L"DESKTOP_ORGANIZER_CONFIG_DIR");
    const std::filesystem::path dataDirectory =
        environmentValue(L"DESKTOP_ORGANIZER_DATA_DIR");
    const std::filesystem::path resultDirectory =
        environmentValue(L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR");
    const std::wstring instanceSuffix =
        environmentValue(L"DESKTOP_ORGANIZER_INSTANCE_SUFFIX");
    wchar_t moduleBuffer[32768]{};
    const DWORD moduleLength = GetModuleFileNameW(
        nullptr, moduleBuffer, ARRAYSIZE(moduleBuffer));
    std::error_code pathError;
    const std::filesystem::path modulePath =
        moduleLength > 0 && moduleLength < ARRAYSIZE(moduleBuffer)
        ? std::filesystem::weakly_canonical(
              std::filesystem::path(
                  std::wstring(moduleBuffer, moduleLength)),
              pathError)
        : std::filesystem::path{};
    const std::filesystem::path projectRoot =
        modulePath.empty()
        ? std::filesystem::path{}
        : modulePath.parent_path().parent_path().parent_path();
    const auto canonical = [&](const std::filesystem::path& path) {
        std::error_code error;
        const auto value = std::filesystem::weakly_canonical(path, error);
        if (error) pathError = error;
        return value;
    };
    const std::filesystem::path canonicalConfig =
        canonical(configDirectory);
    const std::filesystem::path canonicalData =
        canonical(dataDirectory);
    const std::filesystem::path canonicalResults =
        canonical(resultDirectory);
    const std::filesystem::path runRoot =
        canonicalResults.parent_path();
    const std::filesystem::path expectedRunsRoot =
        canonical(projectRoot / L".workspace" / L"runs");
    const auto isDirectoryWithoutReparsePoint =
        [](const std::filesystem::path& path) {
            const DWORD attributes =
                GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        };
    ConfigStore isolatedStore;
    const std::filesystem::path actualConfigPath =
        canonical(isolatedStore.ConfigPath());
    const bool isolated = !pathError &&
        !projectRoot.empty() &&
        runRoot.parent_path() == expectedRunsRoot &&
        canonicalConfig.parent_path() == runRoot &&
        canonicalData.parent_path() == runRoot &&
        canonicalResults.parent_path() == runRoot &&
        canonicalConfig.filename() == L"Config" &&
        canonicalData.filename() == L"Data" &&
        canonicalResults.filename() == L"Results" &&
        actualConfigPath == canonicalConfig / L"config.ini" &&
        instanceSuffix.rfind(L"desktop-internal-", 0) == 0 &&
        isDirectoryWithoutReparsePoint(expectedRunsRoot) &&
        isDirectoryWithoutReparsePoint(runRoot) &&
        isDirectoryWithoutReparsePoint(canonicalConfig) &&
        isDirectoryWithoutReparsePoint(canonicalData) &&
        isDirectoryWithoutReparsePoint(canonicalResults);
    if (!isolated) {
        std::wcerr
            << L"Current-config drag smoke rejected non-isolated paths\n";
        return 231;
    }
    std::ofstream result(
        canonicalResults / L"desktop-current-config-drag-result.txt",
        std::ios::trunc);
    const std::wstring marker =
        environmentValue(L"LATTICE_INTERNAL_DRAG_MARKER_A");
    const bool markerValid = !marker.empty() &&
        std::filesystem::path(marker).filename().wstring().rfind(
            L".lattice-internal-drag-", 0) == 0 &&
        GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!markerValid) {
        result << "ISOLATION_VALID=1\n"
               << "INPUT_REJECTED=1\nSTATUS=FAIL\n";
        return 232;
    }

    DesktopLayout layout;
    std::vector<DesktopPosition> explorerBefore;
    DWORD flagsBefore = 0;
    std::wstring errorMessage;
    if (!layout.CaptureAllPositions(explorerBefore, errorMessage) ||
        !layout.CaptureViewFlags(flagsBefore, errorMessage)) {
        result << "ISOLATION_VALID=1\n"
               << "SNAPSHOT_FAILED=1\nSTATUS=FAIL\n";
        return 233;
    }

    App app(instance);
    const bool initialized = app.Initialize(SW_SHOWNOACTIVATE);
    const HWND mainWindow = FindCurrentProcessMainWindow();
    auto* main = mainWindow == nullptr
        ? nullptr
        : reinterpret_cast<MainWindow*>(
              GetWindowLongPtrW(mainWindow, GWLP_USERDATA));
    DesktopSurfaceWindow* surface = main == nullptr
        ? nullptr
        : MainWindowSmokeAccess::DesktopSurface(*main);
    const size_t widgetCount = main == nullptr
        ? 0
        : MainWindowSmokeAccess::WidgetCount(*main);
    if (!initialized || main == nullptr || surface == nullptr ||
        surface->Window() == nullptr) {
        result << "ISOLATION_VALID=1\n"
               << "APP_INITIALIZED=" << initialized << "\n"
               << "MAIN_WINDOW_READY=" << (main != nullptr) << "\n"
               << "SURFACE_READY=" << (surface != nullptr) << "\n"
               << "WIDGET_COUNT=" << widgetCount << "\n"
               << "STATUS=FAIL\n";
        if (mainWindow != nullptr) {
            DestroyWindow(mainWindow);
            app.Run();
        }
        return 234;
    }

    const auto pumpFor = [](DWORD milliseconds) {
        const ULONGLONG deadline = GetTickCount64() + milliseconds;
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjects(
                0, nullptr, FALSE, 8, QS_ALLINPUT);
        } while (GetTickCount64() < deadline);
    };
    pumpFor(500);
    const std::vector<DesktopPosition> visibleBefore =
        DesktopSurfaceWindowSmokeAccess::VisiblePositions(*surface);
    std::wstring dragIdentity;
    std::wstring secondDragIdentity;
    auto dragBefore = visibleBefore.end();
    auto secondDragBefore = visibleBefore.end();
    std::optional<POINT> dragStart;
    std::optional<POINT> secondDragPoint;
    for (auto candidate = visibleBefore.begin();
         candidate != visibleBefore.end(); ++candidate) {
        const auto exposed =
            DesktopSurfaceWindowSmokeAccess::FindExposedHitPointForIdentity(
                *surface, candidate->path);
        if (!exposed.has_value()) {
            continue;
        }
        dragIdentity = candidate->path;
        dragBefore = candidate;
        dragStart = exposed;
        break;
    }
    if (dragBefore != visibleBefore.end()) {
        for (auto candidate = visibleBefore.begin();
             candidate != visibleBefore.end(); ++candidate) {
            if (CompareStringOrdinal(
                    candidate->path.c_str(), -1,
                    dragIdentity.c_str(), -1, TRUE) == CSTR_EQUAL) {
                continue;
            }
            const auto exposed =
                DesktopSurfaceWindowSmokeAccess::
                    FindExposedHitPointForIdentity(
                        *surface, candidate->path);
            if (!exposed.has_value()) {
                continue;
            }
            secondDragIdentity = candidate->path;
            secondDragBefore = candidate;
            secondDragPoint = exposed;
            break;
        }
    }
    if (dragBefore == visibleBefore.end() || !dragStart.has_value() ||
        secondDragBefore == visibleBefore.end() ||
        !secondDragPoint.has_value()) {
        result << "ISOLATION_VALID=1\n"
               << "APP_INITIALIZED=1\n"
               << "WIDGET_COUNT=" << widgetCount << "\n"
               << "VISIBLE_COUNT=" << visibleBefore.size() << "\n"
               << "EXPOSED_MULTI_SOURCE_FOUND=0\n"
               << "STATUS=FAIL\n";
        DestroyWindow(mainWindow);
        app.Run();
        return 235;
    }

    const int cellWidth =
        DesktopSurfaceWindowSmokeAccess::CellWidth(*surface);
    const int cellHeight =
        DesktopSurfaceWindowSmokeAccess::CellHeight(*surface);
    const int iconSize =
        DesktopSurfaceWindowSmokeAccess::IconSize(*surface);
    const DesktopViewSnapshot snapshot = surface->Snapshot();
    const POINT gridOrigin = dragBefore->point;
    const LONG horizontalInset = (cellWidth - iconSize) / 2;
    const LONG minimumAnchorX =
        snapshot.screenRect.left + horizontalInset;
    const LONG maximumAnchorX =
        snapshot.screenRect.right - cellWidth + horizontalInset;
    const LONG minimumAnchorY = snapshot.screenRect.top;
    const LONG maximumAnchorY =
        snapshot.screenRect.bottom - cellHeight;
    const int minimumColumn = static_cast<int>(std::ceil(
        static_cast<double>(minimumAnchorX - gridOrigin.x) /
        static_cast<double>(cellWidth)));
    const int maximumColumn = static_cast<int>(std::floor(
        static_cast<double>(maximumAnchorX - gridOrigin.x) /
        static_cast<double>(cellWidth)));
    const int minimumRow = static_cast<int>(std::ceil(
        static_cast<double>(minimumAnchorY - gridOrigin.y) /
        static_cast<double>(cellHeight)));
    const int maximumRow = static_cast<int>(std::floor(
        static_cast<double>(maximumAnchorY - gridOrigin.y) /
        static_cast<double>(cellHeight)));
    std::set<std::pair<int, int>> occupiedCells;
    size_t duplicateCells = 0;
    size_t outOfBoundsCells = 0;
    for (const DesktopPosition& position : visibleBefore) {
        if (CompareStringOrdinal(
                position.path.c_str(), -1,
                dragIdentity.c_str(), -1, TRUE) == CSTR_EQUAL) {
            continue;
        }
        const std::pair<int, int> cell{
            static_cast<int>(std::lround(
                static_cast<double>(position.point.x - gridOrigin.x) /
                static_cast<double>(cellWidth))),
            static_cast<int>(std::lround(
                static_cast<double>(position.point.y - gridOrigin.y) /
                static_cast<double>(cellHeight)))};
        if (cell.first < minimumColumn ||
            cell.first > maximumColumn ||
            cell.second < minimumRow ||
            cell.second > maximumRow) {
            ++outOfBoundsCells;
        }
        if (!occupiedCells.insert(cell).second) {
            ++duplicateCells;
        }
    }
    POINT dragEnd{};
    POINT plannedMarker{};
    POINT plannedSecond{};
    bool routePlanned = false;
    size_t exposedTargets = 0;
    size_t acceptedPlans = 0;
    const LONG stepX = (std::max)(8, cellWidth / 2);
    const LONG stepY = (std::max)(8, cellHeight / 2);
    for (LONG y = snapshot.screenRect.top + 4;
         y < snapshot.screenRect.bottom - 4 && !routePlanned;
         y += stepY) {
        for (LONG x = snapshot.screenRect.left + 4;
             x < snapshot.screenRect.right - 4; x += stepX) {
            const POINT candidate{x, y};
            if (WindowFromPoint(candidate) != surface->Window()) {
                continue;
            }
            ++exposedTargets;
            std::vector<DesktopPosition> planned;
            if (!DesktopSurfaceWindowSmokeAccess::PlanVisibleGridDrop(
                    visibleBefore,
                    {{dragIdentity, dragBefore->point},
                     {secondDragIdentity, secondDragBefore->point}},
                    *dragStart,
                    candidate,
                    snapshot.screenRect,
                    cellWidth,
                    cellHeight,
                    iconSize,
                    planned)) {
                continue;
            }
            ++acceptedPlans;
            const auto plannedItem = std::find_if(
                planned.begin(), planned.end(),
                [&](const DesktopPosition& position) {
                    return CompareStringOrdinal(
                               position.path.c_str(), -1,
                               dragIdentity.c_str(), -1,
                               TRUE) == CSTR_EQUAL;
                });
            const auto plannedSecondItem = std::find_if(
                planned.begin(), planned.end(),
                [&](const DesktopPosition& position) {
                    return CompareStringOrdinal(
                               position.path.c_str(), -1,
                               secondDragIdentity.c_str(), -1,
                               TRUE) == CSTR_EQUAL;
                });
            if (plannedItem == planned.end() ||
                plannedSecondItem == planned.end()) {
                continue;
            }
            const LONG firstDeltaX =
                plannedItem->point.x - dragBefore->point.x;
            const LONG firstDeltaY =
                plannedItem->point.y - dragBefore->point.y;
            const LONG secondDeltaX =
                plannedSecondItem->point.x - secondDragBefore->point.x;
            const LONG secondDeltaY =
                plannedSecondItem->point.y - secondDragBefore->point.y;
            if ((firstDeltaX == 0 && firstDeltaY == 0) ||
                firstDeltaX != secondDeltaX ||
                firstDeltaY != secondDeltaY) {
                continue;
            }
            dragEnd = candidate;
            plannedMarker = plannedItem->point;
            plannedSecond = plannedSecondItem->point;
            routePlanned = true;
            break;
        }
    }
    if (!routePlanned) {
        result << "ISOLATION_VALID=1\n"
               << "APP_INITIALIZED=1\n"
               << "WIDGET_COUNT=" << widgetCount << "\n"
               << "VISIBLE_COUNT=" << visibleBefore.size() << "\n"
               << "EXPOSED_TARGETS=" << exposedTargets << "\n"
               << "ACCEPTED_PLANS=" << acceptedPlans << "\n"
               << "GRID_DUPLICATE_CELLS=" << duplicateCells << "\n"
               << "GRID_OUT_OF_BOUNDS=" << outOfBoundsCells << "\n"
               << "ROUTE_PLANNED=0\nSTATUS=FAIL\n";
        DestroyWindow(mainWindow);
        app.Run();
        return 236;
    }

    const int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualWidth =
        (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    const int virtualHeight =
        (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    const auto sendMouse = [&](POINT screenPoint, DWORD flags) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = MulDiv(
            screenPoint.x - virtualLeft, 65535, virtualWidth);
        input.mi.dy = MulDiv(
            screenPoint.y - virtualTop, 65535, virtualHeight);
        input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE |
            MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE | flags;
        return SendInput(1, &input, sizeof(input)) == 1;
    };
    std::wstring focusIdentity;
    std::optional<POINT> focusPoint;
    std::vector<std::wstring> explorerSelectionBeforeClick;
    const bool capturedExplorerSelectionBeforeClick =
        CaptureExplorerSelectionForSmoke(
            explorerSelectionBeforeClick);
    for (const DesktopPosition& candidate : visibleBefore) {
        const bool alreadySelectedByExplorer = std::any_of(
            explorerSelectionBeforeClick.begin(),
            explorerSelectionBeforeClick.end(),
            [&](const std::wstring& identity) {
                return CompareStringOrdinal(
                           identity.c_str(), -1,
                           candidate.path.c_str(), -1,
                           TRUE) == CSTR_EQUAL;
            });
        if (alreadySelectedByExplorer) {
            continue;
        }
        const auto exposed =
            DesktopSurfaceWindowSmokeAccess::FindExposedHitPointForIdentity(
                *surface, candidate.path);
        if (!exposed.has_value()) {
            continue;
        }
        bool canRename = false;
        const ShellItemReference reference =
            DesktopSurfaceWindowSmokeAccess::ReferenceForIdentity(
                *surface, candidate.path);
        if (SUCCEEDED(CanRenameDesktopShellItem(reference, canRename)) &&
            canRename) {
            focusIdentity = candidate.path;
            focusPoint = exposed;
            break;
        }
    }
    POINT savedCursor{};
    GetCursorPos(&savedCursor);
    bool focusClickInjected = false;
    bool foregroundFocusUnchangedOnMouseDown = false;
    bool explorerViewFocusedOnMouseDown = false;
    bool clickSelectionMatches = false;
    bool selectedShellIdentityMatches = false;
    bool explorerSelectionMatchesClick = false;
    int explorerSelectionSyncStage = 0;
    HRESULT explorerSelectionSyncResult = E_PENDING;
    bool desktopF2RouteReady = false;
    bool f2IdentityMatches = false;
    bool f2ForegroundRouteMatches = false;
    bool renameEditorClosedByRealEscape = false;
    if (focusPoint.has_value()) {
        focusClickInjected = sendMouse(*focusPoint, 0);
        pumpFor(40);
        const HWND foregroundBeforeMouseDown = GetForegroundWindow();
        GUITHREADINFO beforeMouseDownInformation{};
        beforeMouseDownInformation.cbSize =
            sizeof(beforeMouseDownInformation);
        const bool capturedBeforeMouseDown =
            GetGUIThreadInfo(0, &beforeMouseDownInformation) != FALSE;
        focusClickInjected =
            sendMouse(*focusPoint, MOUSEEVENTF_LEFTDOWN) &&
            focusClickInjected;
        pumpFor(60);
        const std::vector<std::wstring> selectedOnMouseDown =
            DesktopSurfaceWindowSmokeAccess::SelectedPaths(*surface);
        GUITHREADINFO afterMouseDownInformation{};
        afterMouseDownInformation.cbSize =
            sizeof(afterMouseDownInformation);
        foregroundFocusUnchangedOnMouseDown =
            capturedBeforeMouseDown &&
            GetGUIThreadInfo(0, &afterMouseDownInformation) != FALSE &&
            GetForegroundWindow() == foregroundBeforeMouseDown &&
            afterMouseDownInformation.hwndActive ==
                beforeMouseDownInformation.hwndActive &&
            afterMouseDownInformation.hwndFocus ==
                beforeMouseDownInformation.hwndFocus;
        explorerViewFocusedOnMouseDown =
            afterMouseDownInformation.hwndFocus ==
                snapshot.listViewWindow ||
            (snapshot.listViewWindow != nullptr &&
             afterMouseDownInformation.hwndFocus != nullptr &&
             IsChild(
                 snapshot.listViewWindow,
                 afterMouseDownInformation.hwndFocus) != FALSE);
        clickSelectionMatches = selectedOnMouseDown.size() == 1 &&
            CompareStringOrdinal(
                selectedOnMouseDown.front().c_str(), -1,
                focusIdentity.c_str(), -1, TRUE) == CSTR_EQUAL;
        const ShellItemReference selectedReference =
            DesktopSurfaceWindowSmokeAccess::ReferenceForIdentity(
                *surface, focusIdentity);
        std::wstring resolvedSelectedIdentity;
        selectedShellIdentityMatches =
            SUCCEEDED(ResolveDesktopShellItemReferenceParsingName(
                selectedReference, resolvedSelectedIdentity)) &&
            CompareStringOrdinal(
                resolvedSelectedIdentity.c_str(), -1,
                focusIdentity.c_str(), -1, TRUE) == CSTR_EQUAL;
        explorerSelectionSyncStage =
            DesktopSurfaceWindowSmokeAccess::ExplorerSelectionSyncStage(
                *surface);
        explorerSelectionSyncResult =
            DesktopSurfaceWindowSmokeAccess::ExplorerSelectionSyncResult(
                *surface);
        focusClickInjected =
            sendMouse(*focusPoint, MOUSEEVENTF_LEFTUP) &&
            focusClickInjected;
        const ULONGLONG selectionDeadline = GetTickCount64() + 250;
        do {
            std::vector<std::wstring> explorerSelectionAfterClick;
            explorerSelectionMatchesClick =
                capturedExplorerSelectionBeforeClick &&
                CaptureExplorerSelectionForSmoke(
                    explorerSelectionAfterClick) &&
                explorerSelectionAfterClick.size() == 1 &&
                CompareStringOrdinal(
                    explorerSelectionAfterClick.front().c_str(), -1,
                    focusIdentity.c_str(), -1, TRUE) == CSTR_EQUAL;
            if (explorerSelectionMatchesClick) {
                break;
            }
            pumpFor(20);
        } while (GetTickCount64() < selectionDeadline);

        desktopF2RouteReady =
            DesktopSurfaceWindowSmokeAccess::ShouldRouteDesktopF2(
                *surface);

        INPUT f2Inputs[2]{};
        f2Inputs[0].type = INPUT_KEYBOARD;
        f2Inputs[0].ki.wVk = VK_F2;
        f2Inputs[1].type = INPUT_KEYBOARD;
        f2Inputs[1].ki.wVk = VK_F2;
        f2Inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        const bool f2Injected =
            SendInput(ARRAYSIZE(f2Inputs), f2Inputs, sizeof(INPUT)) ==
            ARRAYSIZE(f2Inputs);
        pumpFor(120);
        const HWND renameEditor =
            DesktopSurfaceWindowSmokeAccess::RenameEditor(*surface);
        GUITHREADINFO renameKeyboardInformation{};
        renameKeyboardInformation.cbSize =
            sizeof(renameKeyboardInformation);
        f2ForegroundRouteMatches = renameEditor != nullptr &&
            GetForegroundWindow() == renameEditor &&
            GetGUIThreadInfo(0, &renameKeyboardInformation) != FALSE &&
            renameKeyboardInformation.hwndFocus == renameEditor;
        f2IdentityMatches = f2Injected &&
            selectedShellIdentityMatches &&
            renameEditor != nullptr &&
            CompareStringOrdinal(
                DesktopSurfaceWindowSmokeAccess::RenameIdentity(
                    *surface).c_str(), -1,
                focusIdentity.c_str(), -1, TRUE) == CSTR_EQUAL;
        if (renameEditor != nullptr) {
            INPUT escapeInputs[2]{};
            escapeInputs[0].type = INPUT_KEYBOARD;
            escapeInputs[0].ki.wVk = VK_ESCAPE;
            escapeInputs[1].type = INPUT_KEYBOARD;
            escapeInputs[1].ki.wVk = VK_ESCAPE;
            escapeInputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(
                ARRAYSIZE(escapeInputs),
                escapeInputs,
                sizeof(INPUT));
            pumpFor(80);
            renameEditorClosedByRealEscape =
                DesktopSurfaceWindowSmokeAccess::RenameEditor(
                    *surface) == nullptr;
        }
    }
    const auto sameIdentitySet = [](std::vector<std::wstring> left,
                                    std::vector<std::wstring> right) {
        const auto normalize = [](std::vector<std::wstring>& identities) {
            for (std::wstring& identity : identities) {
                std::transform(
                    identity.begin(), identity.end(), identity.begin(),
                    [](wchar_t value) {
                        return static_cast<wchar_t>(std::towlower(value));
                    });
            }
            std::sort(identities.begin(), identities.end());
        };
        normalize(left);
        normalize(right);
        return left == right;
    };
    bool multiSelectionInputSucceeded = sendMouse(*dragStart, 0);
    pumpFor(40);
    multiSelectionInputSucceeded =
        sendMouse(*dragStart, MOUSEEVENTF_LEFTDOWN) &&
        multiSelectionInputSucceeded;
    pumpFor(40);
    multiSelectionInputSucceeded =
        sendMouse(*dragStart, MOUSEEVENTF_LEFTUP) &&
        multiSelectionInputSucceeded;
    pumpFor(80);
    INPUT controlInput{};
    controlInput.type = INPUT_KEYBOARD;
    controlInput.ki.wVk = VK_CONTROL;
    multiSelectionInputSucceeded =
        SendInput(1, &controlInput, sizeof(controlInput)) == 1 &&
        multiSelectionInputSucceeded;
    pumpFor(30);
    multiSelectionInputSucceeded =
        sendMouse(*secondDragPoint, 0) && multiSelectionInputSucceeded;
    pumpFor(30);
    multiSelectionInputSucceeded =
        sendMouse(*secondDragPoint, MOUSEEVENTF_LEFTDOWN) &&
        multiSelectionInputSucceeded;
    pumpFor(40);
    multiSelectionInputSucceeded =
        sendMouse(*secondDragPoint, MOUSEEVENTF_LEFTUP) &&
        multiSelectionInputSucceeded;
    controlInput.ki.dwFlags = KEYEVENTF_KEYUP;
    multiSelectionInputSucceeded =
        SendInput(1, &controlInput, sizeof(controlInput)) == 1 &&
        multiSelectionInputSucceeded;
    pumpFor(GetDoubleClickTime() + 40);

    const std::vector<std::wstring> expectedMultiSelection{
        dragIdentity, secondDragIdentity};
    const std::vector<std::wstring> latticeMultiSelection =
        DesktopSurfaceWindowSmokeAccess::SelectedPaths(*surface);
    std::vector<std::wstring> explorerMultiSelection;
    const bool latticeMultiSelectionMatches = sameIdentitySet(
        latticeMultiSelection, expectedMultiSelection);
    const bool explorerMultiSelectionMatches =
        CaptureExplorerSelectionForSmoke(explorerMultiSelection) &&
        sameIdentitySet(explorerMultiSelection, expectedMultiSelection);
    std::vector<std::wstring> shellMultiSelection;
    for (const ShellItemReference& reference :
         DesktopSurfaceWindowSmokeAccess::SelectedShellItems(*surface)) {
        std::wstring resolvedIdentity;
        if (FAILED(ResolveDesktopShellItemReferenceParsingName(
                reference, resolvedIdentity))) {
            shellMultiSelection.clear();
            break;
        }
        shellMultiSelection.push_back(std::move(resolvedIdentity));
    }
    const bool shellMultiSelectionMatches = sameIdentitySet(
        shellMultiSelection, expectedMultiSelection);
    if (!multiSelectionInputSucceeded ||
        !latticeMultiSelectionMatches ||
        !explorerMultiSelectionMatches ||
        !shellMultiSelectionMatches) {
        sendMouse(savedCursor, 0);
        result << "ISOLATION_VALID=1\n"
               << "MULTI_SELECTION_INPUT_SUCCEEDED="
               << multiSelectionInputSucceeded << "\n"
               << "LATTICE_MULTI_SELECTION_MATCH="
               << latticeMultiSelectionMatches << "\n"
               << "EXPLORER_MULTI_SELECTION_MATCH="
               << explorerMultiSelectionMatches << "\n"
               << "SHELL_MULTI_SELECTION_MATCH="
               << shellMultiSelectionMatches << "\n"
               << "STATUS=FAIL\n";
        DestroyWindow(mainWindow);
        return 239;
    }
    const LONG thresholdX = (std::max)(
        12L, static_cast<LONG>(GetSystemMetrics(SM_CXDRAG) + 2));
    const LONG thresholdY = (std::max)(
        12L, static_cast<LONG>(GetSystemMetrics(SM_CYDRAG) + 2));
    const auto thresholdDelta = [](LONG delta, LONG threshold) {
        return delta == 0 ? 0L : (delta > 0 ? threshold : -threshold);
    };
    const POINT threshold{
        dragStart->x + thresholdDelta(
            dragEnd.x - dragStart->x, thresholdX),
        dragStart->y + thresholdDelta(
            dragEnd.y - dragStart->y, thresholdY)};
    if (WindowFromPoint(threshold) != surface->Window()) {
        result << "ISOLATION_VALID=1\n"
               << "THRESHOLD_ROUTE_CHANGED=1\nSTATUS=FAIL\n";
        DestroyWindow(mainWindow);
        app.Run();
        return 237;
    }

    const size_t dragStartsBefore =
        DesktopSurfaceWindowSmokeAccess::ShellDragStartCount(*surface);
    std::atomic<bool> inputSucceeded{false};
    std::atomic<bool> inputFinished{false};
    std::thread injector([&]() {
        bool ok = sendMouse(*dragStart, 0);
        Sleep(40);
        ok = sendMouse(*dragStart, MOUSEEVENTF_LEFTDOWN) && ok;
        Sleep(50);
        ok = sendMouse(threshold, 0) && ok;
        Sleep(70);
        ok = sendMouse(dragEnd, 0) && ok;
        Sleep(100);
        ok = sendMouse(dragEnd, MOUSEEVENTF_LEFTUP) && ok;
        inputSucceeded.store(ok);
        inputFinished.store(true);
    });
    const ULONGLONG inputDeadline = GetTickCount64() + 5000;
    while (!inputFinished.load() &&
           GetTickCount64() < inputDeadline) {
        pumpFor(16);
    }
    injector.join();
    pumpFor(180);
    const auto immediate =
        DesktopSurfaceWindowSmokeAccess::VisibleScreenPointForIdentity(
            *surface, dragIdentity);
    const auto immediateSecond =
        DesktopSurfaceWindowSmokeAccess::VisibleScreenPointForIdentity(
            *surface, secondDragIdentity);
    const int dropStage =
        DesktopSurfaceWindowSmokeAccess::InternalDropStageValue(*surface);
    const size_t dragStartsAfter =
        DesktopSurfaceWindowSmokeAccess::ShellDragStartCount(*surface);

    const bool surfaceRefreshed =
        surface->Refresh(errorMessage, false);
    pumpFor(80);
    const auto afterRefresh =
        DesktopSurfaceWindowSmokeAccess::VisibleScreenPointForIdentity(
            *surface, dragIdentity);
    const auto afterRefreshSecond =
        DesktopSurfaceWindowSmokeAccess::VisibleScreenPointForIdentity(
            *surface, secondDragIdentity);
    sendMouse(savedCursor, 0);

    const bool writesDrained = ConfigStore::DrainPendingWrites(5000);
    const AppConfig persisted = isolatedStore.LoadAppConfig();
    const auto persistedDraggedItem = std::find_if(
        persisted.desktopDisplayLayout.begin(),
        persisted.desktopDisplayLayout.end(),
        [&](const DesktopPlacementConfig& placement) {
            return CompareStringOrdinal(
                       placement.path.c_str(), -1,
                dragIdentity.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
    const auto persistedSecondDraggedItem = std::find_if(
        persisted.desktopDisplayLayout.begin(),
        persisted.desktopDisplayLayout.end(),
        [&](const DesktopPlacementConfig& placement) {
            return CompareStringOrdinal(
                       placement.path.c_str(), -1,
                       secondDragIdentity.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
    POINT plannedView = plannedMarker;
    POINT plannedSecondView = plannedSecond;
    const bool plannedViewMapped =
        snapshot.listViewWindow != nullptr &&
        ScreenToClient(snapshot.listViewWindow, &plannedView) != FALSE &&
        ScreenToClient(snapshot.listViewWindow, &plannedSecondView) != FALSE;
    const bool persistedMatches = writesDrained && plannedViewMapped &&
        persistedDraggedItem != persisted.desktopDisplayLayout.end() &&
        persistedDraggedItem->x == plannedView.x &&
        persistedDraggedItem->y == plannedView.y &&
        persistedSecondDraggedItem !=
            persisted.desktopDisplayLayout.end() &&
        persistedSecondDraggedItem->x == plannedSecondView.x &&
        persistedSecondDraggedItem->y == plannedSecondView.y;
    const bool movedImmediate = immediate.has_value() &&
        immediate->x == plannedMarker.x &&
        immediate->y == plannedMarker.y &&
        immediateSecond.has_value() &&
        immediateSecond->x == plannedSecond.x &&
        immediateSecond->y == plannedSecond.y &&
        immediateSecond->x - immediate->x ==
            secondDragBefore->point.x - dragBefore->point.x &&
        immediateSecond->y - immediate->y ==
            secondDragBefore->point.y - dragBefore->point.y;
    const bool movedAfterRefresh = afterRefresh.has_value() &&
        afterRefresh->x == plannedMarker.x &&
        afterRefresh->y == plannedMarker.y &&
        afterRefreshSecond.has_value() &&
        afterRefreshSecond->x == plannedSecond.x &&
        afterRefreshSecond->y == plannedSecond.y &&
        afterRefreshSecond->x - afterRefresh->x ==
            secondDragBefore->point.x - dragBefore->point.x &&
        afterRefreshSecond->y - afterRefresh->y ==
            secondDragBefore->point.y - dragBefore->point.y;

    std::vector<DesktopPosition> explorerAfter;
    DWORD flagsAfter = 0;
    const bool capturedAfter =
        layout.CaptureAllPositions(explorerAfter, errorMessage) &&
        layout.CaptureViewFlags(flagsAfter, errorMessage);
    const auto sameExplorerPositions = [](const auto& left, const auto& right) {
        if (left.size() != right.size()) return false;
        return std::all_of(
            left.begin(), left.end(),
            [&](const DesktopPosition& expected) {
                const auto actual = std::find_if(
                    right.begin(), right.end(),
                    [&](const DesktopPosition& value) {
                        return CompareStringOrdinal(
                                   value.path.c_str(), -1,
                                   expected.path.c_str(), -1,
                                   TRUE) == CSTR_EQUAL;
                    });
                return actual != right.end() &&
                    actual->point.x == expected.point.x &&
                    actual->point.y == expected.point.y;
            });
    };
    const bool explorerStable = capturedAfter &&
        flagsAfter == flagsBefore &&
        sameExplorerPositions(explorerBefore, explorerAfter);
    const bool dragStarted =
        dragStartsAfter == dragStartsBefore + 1;
    const bool passed = widgetCount > 0 &&
        focusPoint.has_value() && focusClickInjected &&
        explorerViewFocusedOnMouseDown && clickSelectionMatches &&
        selectedShellIdentityMatches && explorerSelectionMatchesClick &&
        desktopF2RouteReady &&
        f2IdentityMatches && f2ForegroundRouteMatches &&
        renameEditorClosedByRealEscape &&
        multiSelectionInputSucceeded &&
        latticeMultiSelectionMatches &&
        explorerMultiSelectionMatches &&
        shellMultiSelectionMatches &&
        inputSucceeded.load() && dragStarted && dropStage == 8 &&
        movedImmediate && surfaceRefreshed && movedAfterRefresh &&
        persistedMatches && explorerStable;
    result << "ISOLATION_VALID=1\n"
           << "APP_INITIALIZED=1\n"
           << "WIDGET_COUNT=" << widgetCount << "\n"
           << "VISIBLE_COUNT=" << visibleBefore.size() << "\n"
           << "ROUTE_PLANNED=1\n"
           << "FOCUS_TARGET_FOUND=" << focusPoint.has_value() << "\n"
           << "FOCUS_CLICK_INJECTED=" << focusClickInjected << "\n"
           << "FOREGROUND_FOCUS_UNCHANGED_ON_MOUSE_DOWN="
           << foregroundFocusUnchangedOnMouseDown << "\n"
           << "EXPLORER_VIEW_FOCUSED_ON_MOUSE_DOWN="
           << explorerViewFocusedOnMouseDown << "\n"
           << "CLICK_SELECTION_MATCH=" << clickSelectionMatches << "\n"
           << "SELECTED_SHELL_IDENTITY_MATCH="
           << selectedShellIdentityMatches << "\n"
           << "EXPLORER_SELECTION_MATCH="
           << explorerSelectionMatchesClick << "\n"
           << "EXPLORER_SELECTION_SYNC_STAGE="
           << explorerSelectionSyncStage << "\n"
           << "EXPLORER_SELECTION_SYNC_HRESULT="
           << static_cast<long long>(explorerSelectionSyncResult) << "\n"
           << "DESKTOP_F2_ROUTE_READY=" << desktopF2RouteReady << "\n"
           << "F2_IDENTITY_MATCH=" << f2IdentityMatches << "\n"
           << "F2_FOREGROUND_ROUTE_MATCH="
           << f2ForegroundRouteMatches << "\n"
           << "RENAME_EDITOR_CLOSED_BY_REAL_ESCAPE="
           << renameEditorClosedByRealEscape << "\n"
           << "MULTI_SELECTION_INPUT_SUCCEEDED="
           << multiSelectionInputSucceeded << "\n"
           << "LATTICE_MULTI_SELECTION_MATCH="
           << latticeMultiSelectionMatches << "\n"
           << "EXPLORER_MULTI_SELECTION_MATCH="
           << explorerMultiSelectionMatches << "\n"
           << "SHELL_MULTI_SELECTION_MATCH="
           << shellMultiSelectionMatches << "\n"
           << "INPUT_SUCCEEDED=" << inputSucceeded.load() << "\n"
           << "DRAG_STARTED=" << dragStarted << "\n"
           << "DROP_STAGE=" << dropStage << "\n"
           << "MOVED_IMMEDIATE=" << movedImmediate << "\n"
           << "SURFACE_REFRESHED_WITH_CACHED_WALLPAPER="
           << surfaceRefreshed << "\n"
           << "MOVED_AFTER_REFRESH=" << movedAfterRefresh << "\n"
           << "GROUP_RELATIVE_GEOMETRY_PRESERVED="
           << (movedImmediate && movedAfterRefresh) << "\n"
           << "DISPLAY_LAYOUT_PERSISTED=" << persistedMatches << "\n"
           << "EXPLORER_POSITIONS_FLAGS_UNCHANGED="
           << explorerStable << "\n"
           << "STATUS=" << (passed ? "PASS" : "FAIL") << "\n";
    DestroyWindow(mainWindow);
    return passed ? 0 : 238;
}

int RunSmokeWidgetInteraction(HINSTANCE instance) {
    AttachParentConsole();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD required = GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
    if (required == 0) {
        std::wcerr << L"Explicit widget interaction smoke directory is required\n";
        return 1;
    }
    std::wstring baseValue(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        baseValue.data(),
        required);
    if (copied == 0 || copied >= required) {
        std::wcerr << L"Widget interaction smoke directory is invalid\n";
        return 1;
    }
    baseValue.resize(copied);
    const std::filesystem::path testRoot =
        std::filesystem::absolute(baseValue).lexically_normal() /
        (L"widget-interaction-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path dataRoot = testRoot / L"Data";
    const std::filesystem::path desktopRoot = testRoot / L"Desktop";
    std::error_code fileError;
    std::filesystem::create_directories(dataRoot, fileError);
    std::filesystem::create_directories(desktopRoot, fileError);
    if (fileError) {
        std::wcerr << L"Widget interaction directories failed\n";
        return 1;
    }
    const std::filesystem::path uncategorizedPath = desktopRoot / L"未分类.txt";
    const std::filesystem::path desktopOnlyPath = desktopRoot / L"真实桌面.txt";
    const std::filesystem::path firstPath = desktopRoot / L"第一项.txt";
    const std::filesystem::path secondPath = desktopRoot / L"第二项.txt";
    const std::filesystem::path thirdPath = desktopRoot / L"第三项.txt";
    const std::filesystem::path fourthPath = desktopRoot / L"第四项.txt";
    {
        std::ofstream file(uncategorizedPath, std::ios::binary);
        file << "uncategorized";
    }
    {
        std::ofstream file(desktopOnlyPath, std::ios::binary);
        file << "desktop";
    }
    {
        std::ofstream file(firstPath, std::ios::binary);
        file << "first";
    }
    {
        std::ofstream file(secondPath, std::ios::binary);
        file << "second";
    }
    {
        std::ofstream file(thirdPath, std::ios::binary);
        file << "third";
    }
    {
        std::ofstream file(fourthPath, std::ios::binary);
        file << "fourth";
    }
    SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DATA_DIR", dataRoot.c_str());
    SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DESKTOP_DIR", desktopRoot.c_str());

    const auto cleanup = [&]() {
        std::error_code cleanupError;
        std::filesystem::remove_all(testRoot, cleanupError);
    };
    const auto fail = [&](const std::wstring& message, int code) {
        std::wcerr << message << L"\n";
        cleanup();
        return code;
    };

    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        return fail(L"Widget interaction monitor lookup failed", 31);
    }
    AppConfig config;
    config.settings.lastVisible = true;
    config.settings.singleClickOpen = false;
    const bool actualStartup = StartupManager{}.IsEnabled();
    config.settings.launchOnStartup = !actualStartup;
    config.window.x = monitorInfo.rcWork.left + 110;
    config.window.y = monitorInfo.rcWork.top + 130;
    config.window.width = 390;
    config.window.height = 360;
    config.window.normalHeight = 360;
    config.window.viewMode = 1;
    config.window.locked = false;
    config.window.collapsed = false;
    config.window.monitorId = monitorInfo.szDevice;
    config.uncategorizedName = L"未分类";
    config.uncategorizedStorageFolder = L"未分类";
    config.uncategorizedItemIds = {L"widget-uncategorized"};
    ItemConfig uncategorizedItem{L"widget-uncategorized", uncategorizedPath.wstring(), L"未分类项"};
    uncategorizedItem.originalDesktopPath = uncategorizedPath.wstring();
    config.items.push_back(std::move(uncategorizedItem));
    ItemConfig firstItem{L"widget-first", firstPath.wstring(), L"第一项"};
    firstItem.originalDesktopPath = firstPath.wstring();
    config.items.push_back(std::move(firstItem));
    ItemConfig secondItem{L"widget-second", secondPath.wstring(), L"第二项"};
    secondItem.originalDesktopPath = secondPath.wstring();
    config.items.push_back(std::move(secondItem));
    ItemConfig thirdItem{L"widget-third", thirdPath.wstring(), L"第三项"};
    thirdItem.originalDesktopPath = thirdPath.wstring();
    config.items.push_back(std::move(thirdItem));
    ItemConfig fourthItem{L"widget-fourth", fourthPath.wstring(), L"第四项"};
    fourthItem.originalDesktopPath = fourthPath.wstring();
    config.items.push_back(std::move(fourthItem));
    for (int index = 0; index < 219; ++index) {
        config.desktopLayout.push_back(DesktopPlacementConfig{
            (desktopRoot / (L"交互性能规模填充-" + std::to_wstring(index) +
                L"-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ-0123456789.txt")).wstring(),
            index * 11,
            index * 7});
    }
    CategoryConfig category;
    category.id = L"widget-category";
    category.name = L"交互测试";
    category.storageFolder = L"交互测试";
    category.itemIds = {
        L"widget-first", L"widget-second", L"widget-third", L"widget-fourth"};
    category.layout = config.window;
    category.layout.x = monitorInfo.rcWork.left + 560;
    category.layout.y = monitorInfo.rcWork.top + 170;
    config.categories.push_back(category);
    ConfigStore configStore;
    if (!configStore.SaveAppConfig(config)) {
        return fail(L"Widget interaction config save failed", 32);
    }

    App app(instance);
    if (!app.InitializeForIsolatedSmoke(SW_SHOWNOACTIVATE)) {
        return fail(L"Widget interaction app initialization failed", 33);
    }
    HWND mainWindow = nullptr;
    HWND uncategorizedWindow = nullptr;
    HWND categoryWindow = nullptr;
    const ULONGLONG windowDiscoveryDeadline = GetTickCount64() + 1000;
    do {
        mainWindow = FindCurrentProcessMainWindow();
        uncategorizedWindow = FindLatticeWidgetWindow(L"未分类");
        categoryWindow = FindLatticeWidgetWindow(L"交互测试");
        if (mainWindow != nullptr &&
            uncategorizedWindow != nullptr &&
            categoryWindow != nullptr) {
            break;
        }
        MSG message{};
        while (PeekMessageW(
                &message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(
            0, nullptr, FALSE, 16, QS_ALLINPUT);
    } while (GetTickCount64() < windowDiscoveryDeadline);
    if (mainWindow == nullptr || uncategorizedWindow == nullptr || categoryWindow == nullptr) {
        std::wcerr << L"Widget discovery main=" << mainWindow
                   << L" uncategorized=" << uncategorizedWindow
                   << L" category=" << categoryWindow
                   << L" count=" << CountLatticeWidgetWindows()
                   << L"\n";
        if (mainWindow != nullptr) {
            DestroyWindow(mainWindow);
            app.Run();
        }
        return fail(L"Widget interaction windows not found", 34);
    }
    auto* uncategorizedWidget = reinterpret_cast<WidgetWindow*>(
        GetWindowLongPtrW(uncategorizedWindow, GWLP_USERDATA));
    auto* categoryWidget = reinterpret_cast<WidgetWindow*>(
        GetWindowLongPtrW(categoryWindow, GWLP_USERDATA));
    auto* mainState = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(mainWindow, GWLP_USERDATA));
    if (uncategorizedWidget == nullptr ||
        categoryWidget == nullptr || mainState == nullptr ||
        !categoryWidget->HasShellDropTarget()) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget OLE shell drop target was not registered", 44);
    }
    AutoOrganizePreviewInput integrationInput =
        MainWindowSmokeAccess::AutoOrganizeInput(*mainState);
    const size_t uncategorizedOwnershipCount = static_cast<size_t>(
        std::count_if(
            integrationInput.snapshot.items.begin(),
            integrationInput.snapshot.items.end(),
            [](const lattice::organize::ItemSnapshot& item) {
                return item.sourceCategoryId == L"uncategorized";
            }));
    const size_t categoryOwnershipCount = static_cast<size_t>(
        std::count_if(
            integrationInput.snapshot.items.begin(),
            integrationInput.snapshot.items.end(),
            [](const lattice::organize::ItemSnapshot& item) {
                return item.sourceCategoryId == L"widget-category";
            }));
    const size_t desktopOwnershipCount = static_cast<size_t>(
        std::count_if(
            integrationInput.snapshot.items.begin(),
            integrationInput.snapshot.items.end(),
            [](const lattice::organize::ItemSnapshot& item) {
                return item.sourceCategoryId.empty();
            }));
    const bool hasDuplicateIdentity = std::any_of(
        integrationInput.snapshot.items.begin(),
        integrationInput.snapshot.items.end(),
        [&](const lattice::organize::ItemSnapshot& item) {
            return std::count_if(
                       integrationInput.snapshot.items.begin(),
                       integrationInput.snapshot.items.end(),
                       [&](const lattice::organize::ItemSnapshot& candidate) {
                           return CompareStringOrdinal(
                                      item.parsingIdentity.c_str(), -1,
                                      candidate.parsingIdentity.c_str(), -1,
                                      TRUE) == CSTR_EQUAL;
                       }) != 1;
        });
    if (integrationInput.snapshot.items.size() != 6 ||
        uncategorizedOwnershipCount != 1 ||
        categoryOwnershipCount != 4 ||
        desktopOwnershipCount != 1 ||
        hasDuplicateIdentity ||
        std::any_of(
            integrationInput.snapshot.items.begin(),
            integrationInput.snapshot.items.end(),
            [](const lattice::organize::ItemSnapshot& item) {
                return item.sourceCategoryId == L"uncategorized" &&
                    item.sourceCategoryName != L"未分类";
            })) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(
            L"Auto-organize production snapshot duplicated a physical desktop identity or lost configured ownership",
            253);
    }
    const auto integrationItem = std::find_if(
        integrationInput.snapshot.items.begin(),
        integrationInput.snapshot.items.end(),
        [](const lattice::organize::ItemSnapshot& item) {
            return item.sourceCategoryId == L"uncategorized";
        });
    if (integrationItem == integrationInput.snapshot.items.end()) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Auto-organize production snapshot omitted a live item", 251);
    }
    const lattice::organize::Plan canonicalPlan =
        lattice::organize::BuildPlan(integrationInput.snapshot);
    const auto canonicalUncategorized = std::find_if(
        canonicalPlan.decisions.begin(), canonicalPlan.decisions.end(),
        [&](const lattice::organize::Decision& decision) {
            return decision.itemId == integrationItem->id;
        });
    const auto canonicalDesktop = std::find_if(
        canonicalPlan.decisions.begin(), canonicalPlan.decisions.end(),
        [&](const lattice::organize::Decision& decision) {
            return CompareStringOrdinal(
                       decision.parsingIdentity.c_str(), -1,
                       desktopOnlyPath.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
    if (canonicalUncategorized == canonicalPlan.decisions.end() ||
        lattice::organize::IsOwnershipAdjustment(*canonicalUncategorized) ||
        canonicalUncategorized->targetCategoryId != L"uncategorized" ||
        canonicalUncategorized->sourceCategoryName != L"未分类" ||
        canonicalDesktop == canonicalPlan.decisions.end() ||
        !canonicalDesktop->sourceCategoryId.empty()) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(
            L"Auto-organize production plan diverged from configured grid membership",
            254);
    }
    lattice::organize::Plan integrationPlan;
    integrationPlan.id = L"widget-integration-plan";
    integrationPlan.baseConfigRevision = integrationInput.snapshot.configRevision;
    lattice::organize::Decision integrationDecision;
    integrationDecision.itemId = integrationItem->id;
    integrationDecision.parsingIdentity = integrationItem->parsingIdentity;
    integrationDecision.sourceCategoryId = integrationItem->sourceCategoryId;
    integrationDecision.sourceIndex = integrationItem->sourceIndex;
    integrationDecision.monitorId = integrationItem->monitorId;
    integrationDecision.targetCategoryId = L"widget-category";
    integrationDecision.targetCategoryName = L"交互测试";
    integrationDecision.targetIsExistingCategory = true;
    integrationDecision.selected = true;
    integrationPlan.decisions.push_back(std::move(integrationDecision));
    lattice::organize::LayoutPlan integrationLayout;
    integrationLayout.monitorContextSignature =
        lattice::organize::MonitorContextSignature(
            integrationInput.layoutContext.monitors);
    const AutoOrganizeApplyRequest integrationRequest =
        MainWindowSmokeAccess::AutoOrganizeRequest(
            *mainState, integrationPlan, integrationLayout);
    if (integrationRequest.moves.size() != 1 ||
        integrationRequest.moves.front().item.id != integrationItem->id ||
        integrationRequest.moves.front().expectedSourceCategoryId !=
            L"uncategorized" ||
        integrationRequest.moves.front().expectedSourceIndex != 0 ||
        integrationRequest.moves.front().targetCategoryId !=
            L"widget-category") {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Auto-organize production request lost identity or membership", 252);
    }
    const HWND firstPreview =
        MainWindowSmokeAccess::ShowAutoOrganizePreview(*mainState);
    const HWND secondPreview =
        MainWindowSmokeAccess::ShowAutoOrganizePreview(*mainState);
    if (firstPreview == nullptr || firstPreview != secondPreview ||
        (GetWindowLongPtrW(firstPreview, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
        MainWindowSmokeAccess::CloseAutoOrganizePreview(*mainState);
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Auto-organize entries did not share one ordinary preview", 253);
    }
    MainWindowSmokeAccess::CloseAutoOrganizePreview(*mainState);
    lattice::organize::Plan windowFailurePlan = integrationPlan;
    windowFailurePlan.decisions.front().targetCategoryId = L"auto-window-failure";
    windowFailurePlan.decisions.front().targetCategoryName = L"故障注入候选";
    windowFailurePlan.decisions.front().targetIsExistingCategory = false;
    lattice::organize::GroupPlan windowFailureGroup;
    windowFailureGroup.id = L"auto-window-failure";
    windowFailureGroup.name = L"故障注入候选";
    windowFailureGroup.monitorId = integrationItem->monitorId;
    windowFailureGroup.createNewCategory = true;
    windowFailureGroup.itemIds = {integrationItem->id};
    windowFailurePlan.groups.push_back(std::move(windowFailureGroup));
    lattice::organize::LayoutPlan windowFailureLayout = integrationLayout;
    const auto failureMonitor = std::find_if(
        integrationInput.layoutContext.monitors.begin(),
        integrationInput.layoutContext.monitors.end(),
        [&](const lattice::organize::MonitorLayout& monitor) {
            return monitor.id == integrationItem->monitorId;
        });
    if (failureMonitor == integrationInput.layoutContext.monitors.end()) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Auto-organize failure fixture monitor was unavailable", 259);
    }
    lattice::organize::WidgetPlacement failurePlacement;
    failurePlacement.groupId = L"auto-window-failure";
    failurePlacement.monitorId = failureMonitor->id;
    failurePlacement.bounds = {
        failureMonitor->workArea.left + 20,
        failureMonitor->workArea.top + 20,
        failureMonitor->workArea.left + 410,
        failureMonitor->workArea.top + 380};
    failurePlacement.placed = true;
    windowFailureLayout.placements.push_back(std::move(failurePlacement));
    const size_t widgetsBeforeFailure =
        MainWindowSmokeAccess::WidgetCount(*mainState);
    SetEnvironmentVariableW(
        L"LATTICE_SMOKE_FAIL_AUTO_ORGANIZE_WINDOW_PREPARE", L"1");
    MainWindowSmokeAccess::ApplyAutoOrganize(
        *mainState, windowFailurePlan, windowFailureLayout);
    const ULONGLONG rollbackDeadline = GetTickCount64() + 5000;
    while (!MainWindowSmokeAccess::AutoOrganizeIdle(*mainState) &&
           GetTickCount64() < rollbackDeadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    SetEnvironmentVariableW(
        L"LATTICE_SMOKE_FAIL_AUTO_ORGANIZE_WINDOW_PREPARE", nullptr);
    const AppConfig afterWindowFailure = configStore.LoadAppConfig();
    const auto restoredUncategorized = std::find(
        afterWindowFailure.uncategorizedItemIds.begin(),
        afterWindowFailure.uncategorizedItemIds.end(), integrationItem->id);
    if (!MainWindowSmokeAccess::AutoOrganizeIdle(*mainState) ||
        !afterWindowFailure.autoOrganizeUndoHistory.empty() ||
        restoredUncategorized == afterWindowFailure.uncategorizedItemIds.end() ||
        std::any_of(
            afterWindowFailure.categories.begin(),
            afterWindowFailure.categories.end(),
            [](const CategoryConfig& category) {
                return category.id == L"auto-window-failure";
            }) ||
        MainWindowSmokeAccess::WidgetCount(*mainState) != widgetsBeforeFailure) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Auto-organize window preparation failure did not roll back", 260);
    }
    if (MainWindowSmokeAccess::LaunchOnStartupSetting(*mainState) !=
            actualStartup ||
        MainWindowSmokeAccess::SettingsForDialog(*mainState)
                .launchOnStartup != actualStartup) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(
            L"Settings startup state did not reconcile to the real Run command",
            67);
    }
    AppSettings refreshedSettings =
        MainWindowSmokeAccess::SettingsForDialog(*mainState);
    refreshedSettings.theme = 1;
    refreshedSettings.singleClickOpen = true;
    refreshedSettings.iconCacheSize = 333;
    if (!MainWindowSmokeAccess::SaveSettings(
            *mainState, refreshedSettings)) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Settings refresh save failed", 68);
    }
    const auto pumpUntil = [&](const std::function<bool()>& predicate) {
        const ULONGLONG deadline = GetTickCount64() + 1000;
        do {
            MSG message{};
            while (PeekMessageW(
                    &message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (predicate()) {
                return true;
            }
            MsgWaitForMultipleObjects(
                0, nullptr, FALSE, 16, QS_ALLINPUT);
        } while (GetTickCount64() < deadline);
        return predicate();
    };
    if (!pumpUntil([&]() {
            return WidgetWindowSmokeAccess::Theme(*categoryWidget) ==
                       refreshedSettings.theme &&
                WidgetWindowSmokeAccess::Theme(*uncategorizedWidget) ==
                       refreshedSettings.theme &&
                WidgetWindowSmokeAccess::SingleClickOpen(*categoryWidget) &&
                WidgetWindowSmokeAccess::SingleClickOpen(*uncategorizedWidget) &&
                WidgetWindowSmokeAccess::UsesLightTheme(*categoryWidget) &&
                WidgetWindowSmokeAccess::UsesLightTheme(*uncategorizedWidget) &&
                MainWindowSmokeAccess::UsesLightTheme(*mainState) &&
                MainWindowSmokeAccess::AllTileGridsUseTheme(*mainState, true) &&
                WidgetWindowSmokeAccess::IconCacheCapacity(*categoryWidget) == 333 &&
                WidgetWindowSmokeAccess::IconCacheCapacity(*uncategorizedWidget) == 333 &&
                MainWindowSmokeAccess::IconCacheCapacity(*mainState) == 333;
        })) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(
            L"Saved theme, single-click, or icon-cache setting did not reach every live view",
            69);
    }
    const AppConfig settingsOnDisk = configStore.LoadAppConfig();
    if (settingsOnDisk.settings.theme != refreshedSettings.theme ||
        !settingsOnDisk.settings.singleClickOpen ||
        settingsOnDisk.settings.iconCacheSize != 333 ||
        settingsOnDisk.settings.launchOnStartup != actualStartup) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Saved settings did not persist their production values", 70);
    }
    refreshedSettings.theme = 0;
    refreshedSettings.singleClickOpen = false;
    if (!MainWindowSmokeAccess::SaveSettings(
            *mainState, refreshedSettings) ||
        !pumpUntil([&]() {
            return !WidgetWindowSmokeAccess::SingleClickOpen(*categoryWidget) &&
                !WidgetWindowSmokeAccess::SingleClickOpen(*uncategorizedWidget) &&
                !WidgetWindowSmokeAccess::UsesLightTheme(*categoryWidget) &&
                !WidgetWindowSmokeAccess::UsesLightTheme(*uncategorizedWidget) &&
                !MainWindowSmokeAccess::UsesLightTheme(*mainState) &&
                MainWindowSmokeAccess::AllTileGridsUseTheme(*mainState, false);
        })) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget single-click setting did not restore", 71);
    }

    refreshedSettings.theme = 2;
    if (!MainWindowSmokeAccess::SaveSettings(
            *mainState, refreshedSettings) ||
        !pumpUntil([&]() {
            return WidgetWindowSmokeAccess::Theme(*categoryWidget) == 2 &&
                WidgetWindowSmokeAccess::Theme(*uncategorizedWidget) == 2;
        })) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Follow-system theme setting did not reach every live view", 72);
    }
    const bool expectedSystemLightTheme =
        MainWindowSmokeAccess::UsesLightTheme(*mainState);
    MainWindowSmokeAccess::ForceGridThemesForTest(
        *mainState, !expectedSystemLightTheme);
    WidgetWindowSmokeAccess::ForceGridThemeForTest(
        *categoryWidget, !expectedSystemLightTheme);
    WidgetWindowSmokeAccess::ForceGridThemeForTest(
        *uncategorizedWidget, !expectedSystemLightTheme);
    SendMessageW(mainWindow, WM_THEMECHANGED, 0, 0);
    SendMessageW(categoryWindow, WM_THEMECHANGED, 0, 0);
    SendMessageW(uncategorizedWindow, WM_THEMECHANGED, 0, 0);
    if (MainWindowSmokeAccess::UsesLightTheme(*mainState) !=
            expectedSystemLightTheme ||
        !MainWindowSmokeAccess::AllTileGridsUseTheme(
            *mainState, expectedSystemLightTheme) ||
        WidgetWindowSmokeAccess::UsesLightTheme(*categoryWidget) !=
            expectedSystemLightTheme ||
        WidgetWindowSmokeAccess::UsesLightTheme(*uncategorizedWidget) !=
            expectedSystemLightTheme) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"System theme change did not refresh every cached icon grid", 73);
    }

    const LONG_PTR categoryExStyle = GetWindowLongPtrW(
        categoryWindow, GWL_EXSTYLE);
    if ((categoryExStyle & WS_EX_TOOLWINDOW) == 0 ||
        (categoryExStyle & WS_EX_NOACTIVATE) != 0 ||
        (categoryExStyle & WS_EX_TOPMOST) != 0 ||
        SendMessageW(categoryWindow, WM_MOUSEACTIVATE, 0, 0) !=
            MA_ACTIVATE ||
        (SendMessageW(categoryWindow, WM_GETDLGCODE, 0, 0) &
            (DLGC_WANTARROWS | DLGC_WANTALLKEYS)) !=
            (DLGC_WANTARROWS | DLGC_WANTALLKEYS)) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(
            L"Widget keyboard activation contract was not enabled safely",
            61);
    }
    WidgetWindowSmokeAccess::SelectOnly(*categoryWidget, 0);
    if (WidgetWindowSmokeAccess::SelectedItemIds(*categoryWidget) !=
            std::vector<std::wstring>{L"widget-first"}) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget single selection failed", 62);
    }
    WidgetWindowSmokeAccess::ToggleSelection(*categoryWidget, 1);
    if (WidgetWindowSmokeAccess::SelectedItemIds(*categoryWidget) !=
            std::vector<std::wstring>{L"widget-first", L"widget-second"}) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget Ctrl-style selection toggle failed", 63);
    }
    const RECT selectionGrid =
        WidgetWindowSmokeAccess::GridBounds(*categoryWidget);
    const RECT firstCell =
        WidgetWindowSmokeAccess::CellAt(*categoryWidget, 0);
    WidgetWindowSmokeAccess::MarqueeSelect(
        *categoryWidget,
        POINT{selectionGrid.right - 1, selectionGrid.bottom - 1},
        POINT{firstCell.left, firstCell.top},
        false);
    if (WidgetWindowSmokeAccess::SelectedItemIds(*categoryWidget) !=
            std::vector<std::wstring>{
                L"widget-first", L"widget-second", L"widget-third", L"widget-fourth"}) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget blank-area marquee selection failed", 64);
    }
    WidgetWindowSmokeAccess::SelectOnly(*categoryWidget, 0);
    WidgetWindowSmokeAccess::MarqueeSelect(
        *categoryWidget,
        POINT{
            firstCell.right - 1,
            firstCell.bottom - 1},
        POINT{firstCell.left + 1, firstCell.top + 1},
        true);
    if (!WidgetWindowSmokeAccess::SelectedItemIds(
            *categoryWidget).empty()) {
        std::wcerr << L"Widget Ctrl marquee remaining selection:";
        for (const std::wstring& itemId :
             WidgetWindowSmokeAccess::SelectedItemIds(*categoryWidget)) {
            std::wcerr << L" " << itemId;
        }
        std::wcerr << L"\n";
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget Ctrl marquee reverse selection failed", 65);
    }
    WidgetWindowSmokeAccess::SelectAll(*categoryWidget);
    WidgetWindowSmokeAccess::MoveKeyboardSelection(
        *categoryWidget, VK_RIGHT);
    if (WidgetWindowSmokeAccess::SelectedItemIds(*categoryWidget) !=
            std::vector<std::wstring>{L"widget-second"}) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget keyboard selection navigation failed", 66);
    }
    WidgetWindowSmokeAccess::SelectOnly(*categoryWidget, 0);

    const int uncategorizedX = monitorInfo.rcWork.left + 170;
    const int uncategorizedY = monitorInfo.rcWork.top + 240;
    const int categoryX = monitorInfo.rcWork.left + 650;
    const int categoryY = monitorInfo.rcWork.top + 280;
    const int categoryDpi = static_cast<int>(std::max<UINT>(96, GetDpiForWindow(categoryWindow)));
    const auto dip = [&](int value) { return MulDiv(value, categoryDpi, 96); };
    const LPARAM collapsePoint = MAKELPARAM(dip(12), dip(16));
    POINT collapseScreenPoint{dip(12), dip(16)};
    ClientToScreen(categoryWindow, &collapseScreenPoint);
    if (SendMessageW(
            categoryWindow,
            WM_NCHITTEST,
            0,
            MAKELPARAM(collapseScreenPoint.x, collapseScreenPoint.y)) != HTCLIENT) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget collapse point was not a client button", 46);
    }
    RECT expandedRect{};
    GetWindowRect(categoryWindow, &expandedRect);
    const auto hitTestClientPixel = [&](int x, int y) {
        POINT screenPoint{x, y};
        ClientToScreen(categoryWindow, &screenPoint);
        return SendMessageW(
            categoryWindow,
            WM_NCHITTEST,
            0,
            MAKELPARAM(screenPoint.x, screenPoint.y));
    };
    RECT expandedClient{};
    GetClientRect(categoryWindow, &expandedClient);
    const int headerHeight = dip(32);
    const int lowerEdgeY = std::min(
        static_cast<int>(expandedClient.bottom) - dip(8),
        headerHeight + dip(24));
    const LRESULT topCenterHit = hitTestClientPixel(expandedClient.right / 2, 1);
    const LRESULT topLeftHit = hitTestClientPixel(1, 1);
    const LRESULT topRightHit = hitTestClientPixel(expandedClient.right - 2, 1);
    const LRESULT lowerLeftHit = hitTestClientPixel(1, lowerEdgeY);
    const LRESULT lowerRightHit = hitTestClientPixel(expandedClient.right - 2, lowerEdgeY);
    const LRESULT bottomCenterHit =
        hitTestClientPixel(expandedClient.right / 2, expandedClient.bottom - 2);
    const LRESULT bottomLeftHit = hitTestClientPixel(1, expandedClient.bottom - 2);
    const LRESULT bottomRightHit =
        hitTestClientPixel(expandedClient.right - 2, expandedClient.bottom - 2);
    if (topCenterHit != HTCAPTION ||
        topLeftHit != HTCLIENT ||
        topRightHit != HTCAPTION ||
        lowerLeftHit != HTLEFT ||
        lowerRightHit != HTRIGHT ||
        bottomCenterHit != HTBOTTOM ||
        bottomLeftHit != HTBOTTOMLEFT ||
        bottomRightHit != HTBOTTOMRIGHT) {
        DestroyWindow(mainWindow);
        std::wcerr << L"Widget header or lower resize hit-test matrix failed\n";
        cleanup();
        return 51;
    }
    const auto collapseStart = std::chrono::steady_clock::now();
    SendMessageW(categoryWindow, WM_LBUTTONDOWN, MK_LBUTTON, collapsePoint);
    SendMessageW(categoryWindow, WM_LBUTTONUP, 0, collapsePoint);
    const auto collapseMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - collapseStart).count();
    RECT collapsedRect{};
    GetWindowRect(categoryWindow, &collapsedRect);
    if (collapsedRect.bottom - collapsedRect.top != dip(32)) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget collapsed header did not match the 32 DIP DeskGo height", 47);
    }
    RECT collapsedClient{};
    GetClientRect(categoryWindow, &collapsedClient);
    const LRESULT collapsedBottomRightHit = hitTestClientPixel(
        collapsedClient.right - 2,
        collapsedClient.bottom - 2);
    if (collapsedBottomRightHit != HTCAPTION) {
        DestroyWindow(mainWindow);
        std::wcerr << L"Collapsed widget title blank did not remain draggable without resize\n";
        cleanup();
        return 52;
    }
    const auto expandStart = std::chrono::steady_clock::now();
    SendMessageW(categoryWindow, WM_LBUTTONDOWN, MK_LBUTTON, collapsePoint);
    SendMessageW(categoryWindow, WM_LBUTTONUP, 0, collapsePoint);
    const auto expandMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - expandStart).count();
    RECT restoredExpandedRect{};
    GetWindowRect(categoryWindow, &restoredExpandedRect);
    if (restoredExpandedRect.bottom - restoredExpandedRect.top != expandedRect.bottom - expandedRect.top) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget did not restore its expanded height after header verification", 48);
    }
    const LPARAM lockPoint = MAKELPARAM(dip(36), dip(22));
    POINT lockScreenPoint{dip(36), dip(22)};
    ClientToScreen(categoryWindow, &lockScreenPoint);
    if (SendMessageW(
            categoryWindow,
            WM_NCHITTEST,
            0,
            MAKELPARAM(lockScreenPoint.x, lockScreenPoint.y)) != HTCLIENT) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget lock point was not a client button", 35);
    }
    SendMessageW(categoryWindow, WM_LBUTTONDOWN, MK_LBUTTON, lockPoint);
    SendMessageW(categoryWindow, WM_LBUTTONUP, 0, lockPoint);
    if (!WidgetWindowSmokeAccess::FlushInteractionSave(
            *categoryWidget)) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget deferred lock persistence failed", 36);
    }
    AppConfig lockedConfig = configStore.LoadAppConfig();
    const auto lockedCategory = std::find_if(
        lockedConfig.categories.begin(),
        lockedConfig.categories.end(),
        [](const CategoryConfig& value) { return value.id == L"widget-category"; });
    if (lockedCategory == lockedConfig.categories.end() || !lockedCategory->layout.locked) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget lock click did not persist locked state", 36);
    }
    if (hitTestClientPixel(expandedClient.right - 2, lowerEdgeY) != HTCLIENT ||
        hitTestClientPixel(
            expandedClient.right / 2,
            expandedClient.bottom - 2) != HTCLIENT) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Locked widget still exposed a resize hit target", 53);
    }
    const RECT reorderGrid =
        WidgetWindowSmokeAccess::GridBounds(*categoryWidget);
    const RECT reorderFirstCell =
        WidgetWindowSmokeAccess::CellAt(*categoryWidget, 0);
    const RECT reorderLastCell =
        WidgetWindowSmokeAccess::CellAt(*categoryWidget, 3);
    const POINT sourceDipPoint{
        (reorderFirstCell.left + reorderFirstCell.right) / 2,
        (reorderFirstCell.top + reorderFirstCell.bottom) / 2};
    const POINT blankAppendDipPoint{
        (reorderLastCell.left + reorderLastCell.right) / 2,
        std::min(
            reorderGrid.bottom - 4,
            reorderLastCell.bottom + 4)};
    const LPARAM sourcePoint = MAKELPARAM(
        dip(sourceDipPoint.x), dip(sourceDipPoint.y));
    const LPARAM targetPoint = MAKELPARAM(
        dip(blankAppendDipPoint.x), dip(blankAppendDipPoint.y));
    SendMessageW(categoryWindow, WM_MOUSEMOVE, 0, sourcePoint);
    const auto dragPrepareStart = std::chrono::steady_clock::now();
    SendMessageW(categoryWindow, WM_LBUTTONDOWN, MK_LBUTTON, sourcePoint);
    const auto dragPrepareMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - dragPrepareStart).count();
    const auto dragStart = std::chrono::steady_clock::now();
    SendMessageW(categoryWindow, WM_MOUSEMOVE, MK_LBUTTON, targetPoint);
    const auto dragStartMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - dragStart).count();
    if (WidgetWindowSmokeAccess::DragInsertionIndex(*categoryWidget) != 3) {
        {
            std::ofstream diagnostic(
                std::filesystem::path(baseValue) /
                    L"widget-append-diagnostic.txt",
                std::ios::binary | std::ios::trunc);
            diagnostic << "grid=" << reorderGrid.left << ","
                       << reorderGrid.top << "," << reorderGrid.right
                       << "," << reorderGrid.bottom << "\n"
                       << "source=" << sourceDipPoint.x << ","
                       << sourceDipPoint.y << "\n"
                       << "blank=" << blankAppendDipPoint.x << ","
                       << blankAppendDipPoint.y << "\n"
                       << "raw="
                       << WidgetWindowSmokeAccess::ReorderInsertionIndexForPoint(
                              *categoryWidget, blankAppendDipPoint)
                       << "\nnormalized="
                       << WidgetWindowSmokeAccess::DragInsertionIndex(*categoryWidget)
                       << "\nselectedCount="
                       << WidgetWindowSmokeAccess::SelectedItemIds(*categoryWidget).size()
                       << "\nitemCount="
                       << WidgetWindowSmokeAccess::CurrentItemIds(*categoryWidget).size()
                       << "\n";
        }
        std::wcerr << L"Widget append diagnostic grid="
                   << reorderGrid.left << L"," << reorderGrid.top << L","
                   << reorderGrid.right << L"," << reorderGrid.bottom
                   << L" source=" << sourceDipPoint.x << L"," << sourceDipPoint.y
                   << L" blank=" << blankAppendDipPoint.x << L"," << blankAppendDipPoint.y
                   << L" raw="
                   << WidgetWindowSmokeAccess::ReorderInsertionIndexForPoint(
                          *categoryWidget, blankAppendDipPoint)
                   << L" normalized="
                   << WidgetWindowSmokeAccess::DragInsertionIndex(*categoryWidget)
                   << L" selected=";
        for (const std::wstring& itemId :
             WidgetWindowSmokeAccess::SelectedItemIds(*categoryWidget)) {
            std::wcerr << itemId << L";";
        }
        std::wcerr << L" order=";
        for (const std::wstring& itemId :
             WidgetWindowSmokeAccess::CurrentItemIds(*categoryWidget)) {
            std::wcerr << itemId << L";";
        }
        std::wcerr << L"\n";
        SendMessageW(categoryWindow, WM_LBUTTONUP, 0, targetPoint);
        DestroyWindow(mainWindow);
        app.Run();
        return fail(
            L"Widget single-item blank-area append preview was not normalized",
            72);
    }
    HWND ghostWindow = FindWindowW(L"Lattice.DragGhostWindow", nullptr);
    if (ghostWindow == nullptr || !IsWindowVisible(ghostWindow) ||
        (GetWindowLongPtrW(ghostWindow, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
        SendMessageW(categoryWindow, WM_LBUTTONUP, 0, targetPoint);
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget drag ghost was not visible and topmost", 37);
    }
    const auto reorderStart = std::chrono::steady_clock::now();
    SendMessageW(categoryWindow, WM_LBUTTONUP, 0, targetPoint);
    const auto reorderMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - reorderStart).count();
    if (WidgetWindowSmokeAccess::CurrentItemIds(*categoryWidget) !=
            std::vector<std::wstring>{
                L"widget-second", L"widget-third", L"widget-fourth", L"widget-first"} ||
        WidgetWindowSmokeAccess::SelectedItemIds(*categoryWidget) !=
            std::vector<std::wstring>{L"widget-first"}) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(
            L"Widget single-item blank-area append did not update immediately",
            73);
    }

    WidgetWindowSmokeAccess::SelectOnly(*categoryWidget, 0);
    WidgetWindowSmokeAccess::ToggleSelection(*categoryWidget, 1);
    const RECT multiSourceCell =
        WidgetWindowSmokeAccess::CellAt(*categoryWidget, 0);
    const POINT multiSourceDipPoint{
        (multiSourceCell.left + multiSourceCell.right) / 2,
        (multiSourceCell.top + multiSourceCell.bottom) / 2};
    const LPARAM multiSourcePoint = MAKELPARAM(
        dip(multiSourceDipPoint.x), dip(multiSourceDipPoint.y));
    SendMessageW(categoryWindow, WM_MOUSEMOVE, 0, multiSourcePoint);
    SendMessageW(categoryWindow, WM_LBUTTONDOWN, MK_LBUTTON, multiSourcePoint);
    SendMessageW(categoryWindow, WM_MOUSEMOVE, MK_LBUTTON, targetPoint);
    if (WidgetWindowSmokeAccess::DragInsertionIndex(*categoryWidget) != 2) {
        SendMessageW(categoryWindow, WM_LBUTTONUP, 0, targetPoint);
        DestroyWindow(mainWindow);
        app.Run();
        return fail(
            L"Widget multi-select blank-area append preview was not normalized",
            74);
    }
    ghostWindow = FindWindowW(L"Lattice.DragGhostWindow", nullptr);
    if (ghostWindow == nullptr || !IsWindowVisible(ghostWindow)) {
        SendMessageW(categoryWindow, WM_LBUTTONUP, 0, targetPoint);
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget multi-select drag ghost was not visible", 75);
    }
    const auto multiReorderStart = std::chrono::steady_clock::now();
    SendMessageW(categoryWindow, WM_LBUTTONUP, 0, targetPoint);
    const auto multiReorderMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - multiReorderStart).count();
    if (WidgetWindowSmokeAccess::CurrentItemIds(*categoryWidget) !=
            std::vector<std::wstring>{
                L"widget-fourth", L"widget-first", L"widget-second", L"widget-third"} ||
        WidgetWindowSmokeAccess::SelectedItemIds(*categoryWidget) !=
            std::vector<std::wstring>{L"widget-second", L"widget-third"}) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(
            L"Widget multi-select blank-area append did not preserve group order and selection",
            76);
    }
    const auto persistStart = std::chrono::steady_clock::now();
    if (!WidgetWindowSmokeAccess::FlushInteractionSave(
            *categoryWidget)) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget deferred reorder persistence failed", 38);
    }
    const auto persistMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - persistStart).count();
    const auto configLoadStart = std::chrono::steady_clock::now();
    AppConfig reorderedConfig = configStore.LoadAppConfig();
    const auto configLoadMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - configLoadStart).count();
    const auto reorderedCategory = std::find_if(
        reorderedConfig.categories.begin(),
        reorderedConfig.categories.end(),
        [](const CategoryConfig& value) { return value.id == L"widget-category"; });
    if (reorderedCategory == reorderedConfig.categories.end() ||
        reorderedCategory->itemIds != std::vector<std::wstring>{
            L"widget-fourth", L"widget-first", L"widget-second", L"widget-third"}) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget internal drag did not persist reordered items", 38);
    }
    const std::optional<std::string> configBeforeNoOpReorder =
        ReadFileBytes(configStore.ConfigPath());
    WidgetWindowSmokeAccess::SelectOnly(*categoryWidget, 0);
    WidgetWindowSmokeAccess::ReorderSelectedItems(*categoryWidget, 0);
    if (!configBeforeNoOpReorder.has_value() ||
        WidgetWindowSmokeAccess::InteractionSavePending(*categoryWidget) ||
        ReadFileBytes(configStore.ConfigPath()) != configBeforeNoOpReorder) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget no-op reorder queued or wrote configuration", 77);
    }
    SendMessageW(categoryWindow, WM_LBUTTONDOWN, MK_LBUTTON, lockPoint);
    SendMessageW(categoryWindow, WM_LBUTTONUP, 0, lockPoint);
    if (!WidgetWindowSmokeAccess::FlushInteractionSave(
            *categoryWidget)) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget deferred unlock persistence failed", 45);
    }
    const AppConfig unlockedConfig = configStore.LoadAppConfig();
    const auto unlockedCategory = std::find_if(
        unlockedConfig.categories.begin(),
        unlockedConfig.categories.end(),
        [](const CategoryConfig& value) { return value.id == L"widget-category"; });
    if (unlockedCategory == unlockedConfig.categories.end() || unlockedCategory->layout.locked) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget unlock click did not persist unlocked state", 45);
    }

    SetWindowScreenBounds(uncategorizedWindow, uncategorizedX, uncategorizedY, 0, 0,
                          SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
    SendMessageW(uncategorizedWindow, WM_EXITSIZEMOVE, 0, 0);
    SetWindowScreenBounds(categoryWindow, categoryX, categoryY, 0, 0,
                          SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
    SendMessageW(categoryWindow, WM_EXITSIZEMOVE, 0, 0);
    if (uncategorizedWidget == nullptr ||
        !WidgetWindowSmokeAccess::FlushInteractionSave(*uncategorizedWidget) ||
        !WidgetWindowSmokeAccess::FlushInteractionSave(
            *categoryWidget)) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget deferred position persistence failed", 43);
    }
    std::wcout << L"WIDGET_INTERACTION_TIMING_MS collapse=" << collapseMilliseconds
               << L" expand=" << expandMilliseconds
               << L" dragPrepare=" << dragPrepareMilliseconds
               << L" dragStart=" << dragStartMilliseconds
               << L" reorder=" << std::max(
                      reorderMilliseconds, multiReorderMilliseconds)
               << L" persist=" << persistMilliseconds
               << L" configLoad=" << configLoadMilliseconds << L"\n";
    {
        std::ofstream timingFile(
            std::filesystem::path(baseValue) / L"widget-interaction-timing.txt",
            std::ios::binary | std::ios::trunc);
        timingFile << "collapse=" << collapseMilliseconds << "\n"
                   << "expand=" << expandMilliseconds << "\n"
                   << "dragPrepare=" << dragPrepareMilliseconds << "\n"
                   << "dragStart=" << dragStartMilliseconds << "\n"
                   << "reorder=" << std::max(
                          reorderMilliseconds, multiReorderMilliseconds) << "\n"
                   << "persist=" << persistMilliseconds << "\n"
                   << "configLoad=" << configLoadMilliseconds << "\n";
    }
    if (collapseMilliseconds > 16) {
        DestroyWindow(mainWindow);
        app.Run();
        std::wcerr << L"Widget collapse handler exceeded the 16 ms interaction budget\n";
        return 54;
    }
    if (expandMilliseconds > 16) {
        DestroyWindow(mainWindow);
        app.Run();
        std::wcerr << L"Widget expand handler exceeded the 16 ms interaction budget\n";
        return 55;
    }
    if (dragPrepareMilliseconds > 16) {
        DestroyWindow(mainWindow);
        app.Run();
        std::wcerr << L"Widget drag preparation exceeded the 16 ms interaction budget\n";
        return 59;
    }
    if (dragStartMilliseconds > 16) {
        DestroyWindow(mainWindow);
        app.Run();
        std::wcerr << L"Widget drag start exceeded the 16 ms interaction budget\n";
        return 57;
    }
    if (std::max(reorderMilliseconds, multiReorderMilliseconds) > 16) {
        DestroyWindow(mainWindow);
        app.Run();
        std::wcerr << L"Widget reorder handler exceeded the 16 ms interaction budget\n";
        return 56;
    }
    if (configLoadMilliseconds > 16) {
        DestroyWindow(mainWindow);
        app.Run();
        std::wcerr << L"Widget config synchronization exceeded the 16 ms interaction budget\n";
        return 60;
    }
    if (!ConfigStore::DrainPendingWrites(5000)) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget interaction async persistence did not drain", 58);
    }
    const std::optional<std::string> configBeforeUpdateExit =
        ReadFileBytes(configStore.ConfigPath());
    const UINT updateExitMessage =
        RegisterWindowMessageW(L"Lattice.RequestExitForUpdate.V1");
    if (!configBeforeUpdateExit.has_value() || updateExitMessage == 0 ||
        !PostMessageW(mainWindow, updateExitMessage, 0, 0)) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget update-exit preservation setup failed", 49);
    }
    MessageDialog::Show(
        instance,
        mainWindow,
        L"Update-exit modal loop propagation smoke",
        L"Lattice update exit smoke",
        MB_OK | MB_ICONINFORMATION);
    const int runResult = app.Run();
    if (runResult != 0) {
        return fail(L"Widget interaction app loop returned failure", 39);
    }

    const AppConfig persisted = configStore.LoadAppConfig();
    const auto persistedCategory = std::find_if(
        persisted.categories.begin(),
        persisted.categories.end(),
        [](const CategoryConfig& value) { return value.id == L"widget-category"; });
    if (persisted.window.x != uncategorizedX || persisted.window.y != uncategorizedY ||
        persistedCategory == persisted.categories.end() ||
        persistedCategory->layout.x != categoryX || persistedCategory->layout.y != categoryY) {
        return fail(L"Widget positions were overwritten during app shutdown", 43);
    }
    const std::optional<std::string> configAfterUpdateExit =
        ReadFileBytes(configStore.ConfigPath());
    if (!configAfterUpdateExit.has_value() ||
        *configAfterUpdateExit != *configBeforeUpdateExit ||
        std::filesystem::exists(dataRoot / L"ManagedShortcuts")) {
        return fail(
            L"Update exit changed original-path state or created ManagedShortcuts",
            50);
    }

    cleanup();
    std::wcout << L"Widget header hit testing, lower-edge resize, geometry lock, internal order, drag ghost, modal update-exit propagation, and byte-identical preservation passed\n";
    return 0;
}

int RunSmokeWidgetNormalExit(HINSTANCE instance) {
    AttachParentConsole();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD required = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
    if (required == 0) {
        std::wcerr << L"Explicit widget normal-exit smoke directory is required\n";
        return 1;
    }
    std::wstring baseValue(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        baseValue.data(),
        required);
    if (copied == 0 || copied >= required) {
        std::wcerr << L"Widget normal-exit smoke directory is invalid\n";
        return 1;
    }
    baseValue.resize(copied);

    const std::filesystem::path testRoot =
        std::filesystem::absolute(baseValue).lexically_normal() /
        (L"widget-normal-exit-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const std::filesystem::path configRoot = testRoot / L"Config";
    const std::filesystem::path dataRoot = testRoot / L"Data";
    const std::filesystem::path desktopRoot = testRoot / L"Desktop";
    const std::wstring categoryName =
        L"正常退出恢复测试-" + std::to_wstring(GetCurrentProcessId());
    const std::filesystem::path desktopPath = desktopRoot / L"退出原路径保持.txt";
    const std::filesystem::path staleLayoutPath =
        desktopRoot / L"已经不存在的历史桌面项.lnk";
    std::error_code fileError;
    std::filesystem::create_directories(configRoot, fileError);
    std::filesystem::create_directories(desktopRoot, fileError);

    const auto cleanup = [&]() {
        std::error_code cleanupError;
        std::filesystem::remove_all(testRoot, cleanupError);
    };
    const auto fail = [&](const std::wstring& message, int code) {
        std::wcerr << message << L"\n";
        cleanup();
        return code;
    };

    if (fileError ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_CONFIG_DIR", configRoot.c_str()) ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DATA_DIR", dataRoot.c_str()) ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DESKTOP_DIR", desktopRoot.c_str())) {
        return fail(L"Widget normal-exit smoke setup failed", 81);
    }
    {
        std::ofstream fixture(desktopPath, std::ios::binary);
        fixture << "isolated widget normal-exit original-path fixture";
        if (!fixture) {
            return fail(L"Widget normal-exit fixture creation failed", 82);
        }
    }

    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        return fail(L"Widget normal-exit monitor lookup failed", 83);
    }

    AppConfig config;
    config.settings.lastVisible = true;
    config.window.x = monitorInfo.rcWork.left + 160;
    config.window.y = monitorInfo.rcWork.top + 160;
    config.window.width = 390;
    config.window.height = 360;
    config.window.normalHeight = 360;
    config.window.viewMode = 1;
    config.window.monitorId = monitorInfo.szDevice;
    config.uncategorizedName = categoryName;
    config.uncategorizedStorageFolder = categoryName;
    config.uncategorizedItemIds = {L"widget-normal-exit-item"};
    ItemConfig item;
    item.id = L"widget-normal-exit-item";
    item.path = desktopPath.wstring();
    item.displayName = L"退出恢复";
    item.originalDesktopPath = desktopPath.wstring();
    config.items.push_back(item);
    config.desktopLayout.push_back(DesktopPlacementConfig{
        staleLayoutPath.wstring(),
        123,
        456});
    ConfigStore configStore;
    if (!configStore.SaveAppConfig(config)) {
        return fail(L"Widget normal-exit config save failed", 84);
    }
    const auto identityBefore = ReadStableFileIdentity(desktopPath);
    const auto bytesBefore = ReadFileBytes(desktopPath.wstring());
    const auto countTopLevel = [](const std::filesystem::path& directory) {
        size_t count = 0;
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end;
             !error && it != end; it.increment(error)) {
            ++count;
        }
        return error ? static_cast<size_t>(-1) : count;
    };
    const size_t desktopCountBefore = countTopLevel(desktopRoot);

    App app(instance);
    if (!app.InitializeForIsolatedSmoke(SW_SHOWNOACTIVATE)) {
        return fail(L"Widget normal-exit app initialization failed", 85);
    }
    const HWND mainWindow = FindCurrentProcessMainWindow();
    const HWND widgetWindow = FindLatticeWidgetWindow(categoryName.c_str());
    auto* widget = reinterpret_cast<WidgetWindow*>(
        widgetWindow == nullptr
            ? 0
            : GetWindowLongPtrW(widgetWindow, GWLP_USERDATA));
    if (mainWindow == nullptr || widgetWindow == nullptr || widget == nullptr) {
        if (mainWindow != nullptr) {
            DestroyWindow(mainWindow);
            app.Run();
        }
        return fail(L"Widget normal-exit windows not found", 86);
    }

    if (!WidgetWindowSmokeAccess::RequestApplicationExit(*widget) ||
        IsWindow(mainWindow) == FALSE || IsWindow(widgetWindow) == FALSE) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(
            L"Widget exit request destroyed its sender synchronously",
            87);
    }

    const ULONGLONG exitStartedAt = GetTickCount64();
    const int runResult = app.Run();
    const ULONGLONG exitElapsedMilliseconds = GetTickCount64() - exitStartedAt;
    if (!app.LastNormalExitError().empty()) {
        const std::filesystem::path diagnosticPath = testRoot / L"exit-error-utf16.txt";
        HANDLE diagnostic = CreateFileW(
            diagnosticPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (diagnostic != INVALID_HANDLE_VALUE) {
            const std::wstring& error = app.LastNormalExitError();
            DWORD written = 0;
            WriteFile(
                diagnostic,
                error.data(),
                static_cast<DWORD>(error.size() * sizeof(wchar_t)),
                &written,
                nullptr);
            CloseHandle(diagnostic);
        }
        std::wcerr << L"Normal exit reported: " << app.LastNormalExitError() << L"\n";
        return 94;
    }
    if (runResult != 0) {
        return fail(L"Widget normal-exit app loop returned failure", 88);
    }
    if (exitElapsedMilliseconds >= 2000) {
        return fail(L"Normal exit waited for a missing historical desktop item", 93);
    }
    if (!SameStableFileIdentity(identityBefore, ReadStableFileIdentity(desktopPath)) ||
        !std::filesystem::exists(desktopPath) ||
        std::filesystem::exists(dataRoot / L"ManagedShortcuts") ||
        countTopLevel(desktopRoot) != desktopCountBefore) {
        return fail(L"Normal exit changed the original desktop item or created managed storage", 89);
    }
    const std::optional<std::string> restoredBytes =
        ReadFileBytes(desktopPath.wstring());
    if (!restoredBytes.has_value() || restoredBytes != bytesBefore) {
        return fail(L"Normal exit changed the original item contents", 90);
    }
    const AppConfig restoredConfig = configStore.LoadAppConfig();
    if (restoredConfig.items.size() != 1 ||
        restoredConfig.items.front().path != desktopPath.wstring() ||
        restoredConfig.items.front().originalDesktopPath != desktopPath.wstring() ||
        restoredConfig.uncategorizedItemIds != config.uncategorizedItemIds ||
        !std::any_of(
            restoredConfig.desktopLayout.begin(),
            restoredConfig.desktopLayout.end(),
            [&](const DesktopPlacementConfig& placement) {
                return CompareStringOrdinal(
                           placement.path.c_str(), -1,
                           staleLayoutPath.c_str(), -1,
                           TRUE) == CSTR_EQUAL;
            })) {
        return fail(L"Normal exit changed item ownership, order, path, or existing layout metadata", 91);
    }
    if (std::filesystem::exists(
            dataRoot / L"ManagedShortcuts" / L"move-journal.bin")) {
        return fail(L"Normal exit left a move journal behind", 92);
    }

    cleanup();
    std::wcout << L"Widget exit request stayed asynchronous and preserved the original desktop item, File ID, contents, count, ownership, order, and layout metadata in "
               << exitElapsedMilliseconds << L" ms\n";
    return 0;
}

int RunSmokeWidgetDropLatency(HINSTANCE instance) {
    AttachParentConsole();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    constexpr double kMaximumReleaseLatencyMilliseconds = 50.0;
    constexpr POINT kReleaseDelta{37, 23};

    const DWORD required = GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
    if (required == 0) {
        std::wcerr << L"Explicit widget drop latency smoke directory is required\n";
        return 1;
    }
    std::wstring baseValue(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        baseValue.data(),
        required);
    if (copied == 0 || copied >= required) {
        std::wcerr << L"Widget drop latency smoke directory is invalid\n";
        return 1;
    }
    baseValue.resize(copied);

    const std::filesystem::path testRoot =
        std::filesystem::absolute(baseValue).lexically_normal() /
        (L"widget-drop-latency-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const std::filesystem::path configRoot = testRoot / L"Config";
    const std::filesystem::path dataRoot = testRoot / L"Data";
    const std::filesystem::path desktopRoot = testRoot / L"Desktop";
    const std::wstring categoryId = L"widget-drop-latency-category";
    const std::wstring categoryName =
        L"拖放延迟隔离测试-" + std::to_wstring(GetCurrentProcessId());
    const std::filesystem::path desktopPath = desktopRoot / L"延迟测试.txt";
    std::error_code fileError;
    std::filesystem::create_directories(configRoot, fileError);
    std::filesystem::create_directories(dataRoot, fileError);
    std::filesystem::create_directories(desktopRoot, fileError);

    WidgetWindow* widgetForCleanup = nullptr;
    HWND ownerWindow = nullptr;
    const auto cleanup = [&]() {
        DragGhostWindow::Instance().End();
        if (widgetForCleanup != nullptr) {
            widgetForCleanup->Close();
        }
        if (ownerWindow != nullptr && IsWindow(ownerWindow) != FALSE) {
            DestroyWindow(ownerWindow);
        }
        std::error_code cleanupError;
        std::filesystem::remove_all(testRoot, cleanupError);
    };
    const auto fail = [&](const std::wstring& message, int code) {
        std::wcerr << message << L"\n";
        cleanup();
        return code;
    };

    if (fileError ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_CONFIG_DIR", configRoot.c_str()) ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DATA_DIR", dataRoot.c_str()) ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DESKTOP_DIR", desktopRoot.c_str())) {
        return fail(L"Widget drop latency smoke setup failed", 51);
    }
    {
        std::ofstream file(desktopPath, std::ios::binary);
        file << "isolated widget drop latency fixture";
        if (!file) {
            return fail(L"Widget drop latency fixture creation failed", 52);
        }
    }
    const auto originalIdentity = ReadStableFileIdentity(desktopPath);
    const auto originalBytes = ReadFileBytes(desktopPath.wstring());
    const size_t originalDesktopCount = static_cast<size_t>(std::distance(
        std::filesystem::directory_iterator(desktopRoot),
        std::filesystem::directory_iterator{}));

    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        return fail(L"Widget drop latency monitor lookup failed", 53);
    }

    AppConfig config;
    config.settings.showPublicDesktopItems = false;
    config.settings.singleClickOpen = false;
    ItemConfig item;
    item.id = L"widget-drop-latency-item";
    item.path = desktopPath.wstring();
    item.displayName = L"延迟测试";
    item.originalDesktopPath = desktopPath.wstring();
    config.items.push_back(item);
    for (int index = 0; index < 163; ++index) {
        ItemConfig filler;
        filler.id = L"widget-drop-latency-filler-" + std::to_wstring(index);
        filler.path =
            (dataRoot / (L"大配置填充-" + std::to_wstring(index) +
                L"-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ-0123456789.txt")).wstring();
        filler.displayName = L"大配置填充项目-" + std::to_wstring(index);
        config.items.push_back(std::move(filler));
    }
    for (int index = 0; index < 219; ++index) {
        config.desktopLayout.push_back(DesktopPlacementConfig{
            (desktopRoot / (L"释放延迟布局填充-" + std::to_wstring(index) +
                L"-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ-0123456789.txt")).wstring(),
            index * 13,
            index * 9});
    }
    CategoryConfig category;
    category.id = categoryId;
    category.name = categoryName;
    category.storageFolder = categoryName;
    category.itemIds.push_back(item.id);
    category.layout.x = monitorInfo.rcWork.left + 96;
    category.layout.y = monitorInfo.rcWork.top + 96;
    category.layout.width = 420;
    category.layout.height = 260;
    category.layout.normalHeight = 260;
    category.layout.iconSize = 48;
    category.layout.density = 0;
    category.layout.contentViewMode = 0;
    category.layout.collapsed = false;
    category.layout.locked = false;
    category.layout.monitorId = monitorInfo.szDevice;
    config.categories.push_back(category);
    ConfigStore configStore;
    if (!configStore.SaveAppConfig(config)) {
        return fail(L"Widget drop latency config save failed", 54);
    }

    constexpr wchar_t kOwnerClassName[] = L"Lattice.SmokeDesktopPlacementOwner";
    WNDCLASSEXW ownerClass{};
    ownerClass.cbSize = sizeof(ownerClass);
    ownerClass.lpfnWndProc = SmokeDesktopPlacementOwnerProc;
    ownerClass.hInstance = instance;
    ownerClass.lpszClassName = kOwnerClassName;
    if (RegisterClassExW(&ownerClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return fail(L"Widget drop latency owner class registration failed", 55);
    }
    SmokeDesktopPlacementOwnerState ownerState;
    ownerWindow = CreateWindowExW(
        0,
        kOwnerClassName,
        L"",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        instance,
        &ownerState);
    if (ownerWindow == nullptr) {
        return fail(L"Widget drop latency owner window creation failed", 56);
    }

    WidgetWindow widget(instance, ownerWindow, categoryId, 0);
    widgetForCleanup = &widget;
    if (!widget.Create()) {
        return fail(L"Widget drop latency widget creation failed", 57);
    }
    widget.Show(SW_SHOWNOACTIVATE);
    HWND widgetWindow = FindLatticeWidgetWindow(categoryName.c_str());
    DWORD widgetProcessId = 0;
    if (widgetWindow == nullptr ||
        GetWindowThreadProcessId(widgetWindow, &widgetProcessId) == 0 ||
        widgetProcessId != GetCurrentProcessId()) {
        return fail(L"Widget drop latency widget window was not found", 58);
    }

    const int widgetDpi = static_cast<int>(std::max<UINT>(96, GetDpiForWindow(widgetWindow)));
    const auto dip = [&](int value) { return MulDiv(value, widgetDpi, 96); };
    const POINT sourcePoint{dip(43), dip(50)};
    SendMessageW(
        widgetWindow,
        WM_LBUTTONDOWN,
        MK_LBUTTON,
        MAKELPARAM(sourcePoint.x, sourcePoint.y));
    if (GetCapture() != widgetWindow) {
        return fail(L"Widget drop latency source icon was not captured", 59);
    }

    RECT clientRect{};
    GetClientRect(widgetWindow, &clientRect);
    const POINT movePoint{clientRect.right + dip(80), sourcePoint.y + dip(24)};
    const POINT releasePoint{movePoint.x + kReleaseDelta.x, movePoint.y + kReleaseDelta.y};
    SendMessageW(
        widgetWindow,
        WM_MOUSEMOVE,
        MK_LBUTTON,
        MAKELPARAM(movePoint.x, movePoint.y));
    if (!DragGhostWindow::Instance().IsVisible() || GetCapture() != widgetWindow) {
        return fail(L"Widget drop latency drag ghost or capture setup failed", 60);
    }
    const POINT moveGhostTopLeft = DragGhostWindow::Instance().TopLeftScreenPoint();
    const std::uint64_t dragGhostGeneration = DragGhostWindow::Instance().CurrentGeneration();
    if (dragGhostGeneration == 0) {
        return fail(L"Widget drop latency drag ghost generation was not assigned", 61);
    }

    ownerState.releaseStarted = std::chrono::steady_clock::now();
    const auto releaseStarted = ownerState.releaseStarted;
    SendMessageW(
        widgetWindow,
        WM_LBUTTONUP,
        0,
        MAKELPARAM(releasePoint.x, releasePoint.y));
    const double releaseDurationMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - releaseStarted).count();

    MSG syncMessage{};
    while (PeekMessageW(
            &syncMessage,
            ownerWindow,
            kOrganizerConfigChangedMessage,
            kOrganizerConfigSyncMessage,
            PM_REMOVE)) {
        DispatchMessageW(&syncMessage);
    }

    if (!ownerState.requestReceived || !ownerState.requestCopied || ownerState.requestCount != 1) {
        return fail(L"Widget drop latency placement handoff was not copied exactly once", 62);
    }
    if (ownerState.configSyncCount != 0) {
        return fail(L"Widget drop latency handoff performed an eager config synchronization", 71);
    }
    if (ownerState.fullRefreshCount != 0) {
        return fail(L"Widget drop latency queued handoff triggered an eager full refresh", 72);
    }
    if (ownerState.captureAtRequest == widgetWindow || GetCapture() == widgetWindow) {
        return fail(L"Widget drop latency placement handoff occurred before mouse capture release", 63);
    }
    const POINT expectedDropPoint{
        moveGhostTopLeft.x + kReleaseDelta.x,
        moveGhostTopLeft.y + kReleaseDelta.y};
    if (ownerState.request.screenPoint.x != expectedDropPoint.x ||
        ownerState.request.screenPoint.y != expectedDropPoint.y) {
        return fail(L"Widget drop latency request used a stale mouse-move coordinate", 64);
    }
    if (ownerState.request.dragGhostGeneration != dragGhostGeneration ||
        ownerState.request.sourceWindow != widgetWindow ||
        !ownerState.request.commitMoveOut ||
        ownerState.request.itemId != item.id ||
        CompareStringOrdinal(
            ownerState.request.sourcePath.c_str(),
            -1,
            desktopPath.c_str(),
            -1,
            TRUE) != CSTR_EQUAL ||
        CompareStringOrdinal(
            ownerState.request.path.c_str(),
            -1,
            desktopPath.c_str(),
            -1,
            TRUE) != CSTR_EQUAL) {
        return fail(L"Widget drop latency placement request metadata mismatch", 65);
    }
    {
        std::ofstream timing(
            std::filesystem::path(baseValue) / L"widget-drop-latency-timing.txt",
            std::ios::binary | std::ios::trunc);
        timing << "request=" << ownerState.requestLatencyMilliseconds << "\n"
               << "release=" << releaseDurationMilliseconds << "\n";
    }
    if (ownerState.requestLatencyMilliseconds < 0.0 ||
        ownerState.requestLatencyMilliseconds > kMaximumReleaseLatencyMilliseconds ||
        releaseDurationMilliseconds > kMaximumReleaseLatencyMilliseconds) {
        return fail(L"Widget drop latency exceeded the 50 ms synchronous release budget", 66);
    }
    std::wstring committedDesktopPath;
    std::wstring commitError;
    if (!CommitDesktopMoveOutTransaction(
            ownerState.request,
            committedDesktopPath,
            commitError) ||
        CompareStringOrdinal(
            committedDesktopPath.c_str(),
            -1,
            desktopPath.c_str(),
            -1,
            TRUE) != CSTR_EQUAL) {
        return fail(
            L"Widget drop latency background transaction failed: " + commitError,
            73);
    }
    const size_t finalDesktopCount = static_cast<size_t>(std::distance(
        std::filesystem::directory_iterator(desktopRoot),
        std::filesystem::directory_iterator{}));
    if (!SameStableFileIdentity(
            originalIdentity, ReadStableFileIdentity(desktopPath)) ||
        originalBytes != ReadFileBytes(desktopPath.wstring()) ||
        finalDesktopCount != originalDesktopCount ||
        std::filesystem::exists(dataRoot / L"ManagedShortcuts")) {
        return fail(L"Widget drop latency changed the original-path fixture", 67);
    }
    const AppConfig persisted = configStore.LoadAppConfig();
    const bool itemStillRegistered = std::any_of(
        persisted.items.begin(),
        persisted.items.end(),
        [&](const ItemConfig& value) { return value.id == item.id; });
    const auto persistedCategory = std::find_if(
        persisted.categories.begin(),
        persisted.categories.end(),
        [&](const CategoryConfig& value) { return value.id == categoryId; });
    const bool categoryStillReferencesItem =
        persistedCategory != persisted.categories.end() &&
        std::find(
            persistedCategory->itemIds.begin(),
            persistedCategory->itemIds.end(),
            item.id) != persistedCategory->itemIds.end();
    if (itemStillRegistered || categoryStillReferencesItem) {
        return fail(L"Widget drop latency item was not removed from isolated config", 68);
    }
    if (!DragGhostWindow::Instance().IsVisible() ||
        !DragGhostWindow::Instance().IsCommitted() ||
        DragGhostWindow::Instance().CurrentGeneration() != dragGhostGeneration) {
        return fail(L"Widget drop latency ghost was not preserved for asynchronous handoff", 69);
    }
    DragGhostWindow::Instance().EndIfGeneration(ownerState.request.dragGhostGeneration);
    if (DragGhostWindow::Instance().IsVisible()) {
        return fail(L"Widget drop latency ghost did not close by matching generation", 70);
    }

    cleanup();
    std::wcout << L"Widget drop handoff request latency: "
               << ownerState.requestLatencyMilliseconds << L" ms; full release: "
               << releaseDurationMilliseconds << L" ms (budget <= 50 ms)\n";
    return 0;
}

int RunSmokeWidgetDropPlacement(HINSTANCE instance) {
    AttachParentConsole();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const auto cellCenter = [](const RECT& cell) {
        return POINT{
            (cell.left + cell.right) / 2,
            (cell.top + cell.bottom) / 2,
        };
    };
    std::vector<DesktopItem> gridItems;
    for (int index = 0; index < 7; ++index) {
        DesktopItem item;
        item.id = L"grid-" + std::to_wstring(index);
        item.displayName = item.id;
        gridItems.push_back(std::move(item));
    }

    IconGrid grid;
    grid.SetWidgetStyle(true);
    grid.SetListMode(false);
    grid.SetBounds(RECT{0, 0, 240, 192});
    grid.SetItems(std::vector<DesktopItem>(gridItems.begin(), gridItems.begin() + 4));
    for (size_t index = 0; index <= 4; ++index) {
        const POINT point = cellCenter(grid.InsertionCellAt(index));
        const int actual = grid.InsertionIndexForPoint(point);
        if (actual != static_cast<int>(index)) {
            std::wcerr << L"IconGrid insertion mismatch at " << index
                       << L": actual=" << actual << L"\n";
            return 81;
        }
    }
    const RECT emptySecondRowCell = grid.InsertionCellAt(4);
    if (emptySecondRowCell.left != 80 || emptySecondRowCell.top != 96 ||
        emptySecondRowCell.right != 160 || emptySecondRowCell.bottom != 192) {
        std::wcerr << L"IconGrid second-row empty slot mismatch: "
                   << emptySecondRowCell.left << L"," << emptySecondRowCell.top
                   << L"," << emptySecondRowCell.right << L","
                   << emptySecondRowCell.bottom << L"\n";
        return 82;
    }
    const RECT firstReorderCell = grid.CellAt(0);
    if (grid.ReorderInsertionIndexForPoint(POINT{
            firstReorderCell.left + 1,
            (firstReorderCell.top + firstReorderCell.bottom) / 2}) != 0 ||
        grid.ReorderInsertionIndexForPoint(POINT{
            firstReorderCell.right - 1,
            (firstReorderCell.top + firstReorderCell.bottom) / 2}) != 1 ||
        grid.ReorderInsertionIndexForPoint(
            cellCenter(emptySecondRowCell)) != 4 ||
        grid.ReorderInsertionIndexForPoint(POINT{0, -1}) != 0 ||
        grid.ReorderInsertionIndexForPoint(POINT{0, 192}) != 4) {
        std::wcerr << L"IconGrid native reorder boundary mismatch\n";
        return 85;
    }
    grid.SetBounds(RECT{0, 0, 240, 288});
    if (grid.ReorderInsertionIndexForPoint(POINT{40, 240}) != 4) {
        std::wcerr << L"IconGrid lower blank area did not append\n";
        return 86;
    }

    grid.SetItems(gridItems);
    grid.SetBounds(RECT{0, 0, 240, 192});
    grid.SetScrollOffset(96);
    for (const size_t index : std::array<size_t, 2>{3, 6}) {
        const int actual =
            grid.InsertionIndexForPoint(cellCenter(grid.InsertionCellAt(index)));
        if (actual != static_cast<int>(index)) {
            std::wcerr << L"IconGrid scrolled insertion mismatch at " << index
                       << L": actual=" << actual << L"\n";
            return 83;
        }
    }
    const RECT scrolledFirstCell = grid.CellAt(3);
    const RECT scrolledLastCell = grid.CellAt(6);
    if (grid.ReorderInsertionIndexForPoint(POINT{
            scrolledFirstCell.left + 1,
            (scrolledFirstCell.top + scrolledFirstCell.bottom) / 2}) != 3 ||
        grid.ReorderInsertionIndexForPoint(POINT{
            scrolledFirstCell.right - 1,
            (scrolledFirstCell.top + scrolledFirstCell.bottom) / 2}) != 4 ||
        grid.ReorderInsertionIndexForPoint(POINT{
            scrolledLastCell.right - 1,
            (scrolledLastCell.top + scrolledLastCell.bottom) / 2}) != 7) {
        std::wcerr << L"IconGrid scrolled reorder boundary mismatch\n";
        return 87;
    }

    grid.SetListMode(true);
    grid.SetBounds(RECT{0, 0, 240, 96});
    grid.SetItems(std::vector<DesktopItem>(gridItems.begin(), gridItems.begin() + 5));
    grid.SetScrollOffset(32);
    const RECT listCellOne = grid.InsertionCellAt(1);
    const RECT listCellThree = grid.InsertionCellAt(3);
    const int listOneY = (listCellOne.top + listCellOne.bottom) / 2;
    if (grid.InsertionIndexForPoint(POINT{1, listOneY}) != 1 ||
        grid.InsertionIndexForPoint(POINT{239, listOneY}) != 1 ||
        grid.InsertionIndexForPoint(cellCenter(listCellThree)) != 3) {
        std::wcerr << L"IconGrid list insertion mismatch\n";
        return 84;
    }
    if (grid.ReorderInsertionIndexForPoint(POINT{
            1, listCellOne.top + 1}) != 1 ||
        grid.ReorderInsertionIndexForPoint(POINT{
            239, listCellOne.bottom - 1}) != 2) {
        std::wcerr << L"IconGrid list reorder half-cell mismatch\n";
        return 88;
    }
    grid.SetScrollOffset(0);
    grid.SetBounds(RECT{0, 0, 240, 192});
    if (grid.ReorderInsertionIndexForPoint(POINT{120, 180}) != 5) {
        std::wcerr << L"IconGrid list lower blank area did not append\n";
        return 89;
    }
    grid.SetItems({});
    if (grid.ReorderInsertionIndexForPoint(POINT{20, 40}) != 0) {
        std::wcerr << L"IconGrid empty reorder boundary mismatch\n";
        return 90;
    }

    DesktopScanner mergeScanner;
    const std::wstring mergeCollisionId = L"merge-collision";
    const std::wstring mergeLivePath = L"merge-live.txt";
    const std::wstring mergeRegisteredPath = L"merge-registered.txt";
    DesktopItem mergeLiveItem;
    mergeLiveItem.id = mergeCollisionId;
    mergeLiveItem.path = mergeLivePath;
    mergeLiveItem.displayName = L"live";
    std::vector<DesktopItem> mergeItems{mergeLiveItem};
    mergeScanner.MergeRegisteredItem(
        mergeItems,
        mergeCollisionId,
        mergeRegisteredPath,
        L"registered");
    const auto mergedLive = std::find_if(
        mergeItems.begin(),
        mergeItems.end(),
        [&](const DesktopItem& item) {
            return item.id == mergeCollisionId + L"|live";
        });
    const auto mergedRegistered = std::find_if(
        mergeItems.begin(),
        mergeItems.end(),
        [&](const DesktopItem& item) {
            return item.id == mergeCollisionId;
        });
    if (mergeItems.size() != 2 ||
        mergedLive == mergeItems.end() ||
        mergedRegistered == mergeItems.end() ||
        mergedLive->path != mergeLivePath ||
        mergedRegistered->path != mergeRegisteredPath ||
        mergedRegistered->displayName != L"registered" ||
        mergedLive->path == mergedRegistered->path) {
        std::wcerr << L"DesktopScanner registered/live ID collision merge mismatch\n";
        return 98;
    }

    DesktopItem scannedAlias;
    scannedAlias.id = L"shortcut|c:\\smoke\\same.lnk";
    scannedAlias.path = L"C:\\Smoke\\Same.lnk";
    scannedAlias.displayName = L"shell name";
    scannedAlias.targetPath = L"C:\\Apps\\Same.exe";
    scannedAlias.arguments = L"--from-shell";
    scannedAlias.workingDirectory = L"C:\\Apps";
    scannedAlias.kind = DesktopItemKind::Shortcut;
    std::vector<DesktopItem> samePathItems{scannedAlias};
    mergeScanner.MergeRegisteredItem(
        samePathItems,
        L"managed-stable-id",
        L"c:\\smoke\\same.lnk",
        L"registered name");
    if (samePathItems.size() != 1 ||
        samePathItems.front().id != L"managed-stable-id" ||
        samePathItems.front().displayName != L"registered name" ||
        samePathItems.front().targetPath != L"C:\\Apps\\Same.exe" ||
        samePathItems.front().arguments != L"--from-shell" ||
        samePathItems.front().workingDirectory != L"C:\\Apps" ||
        samePathItems.front().kind != DesktopItemKind::Shortcut) {
        std::wcerr << L"DesktopScanner same-path ownership merge mismatch\n";
        return 99;
    }

    const DWORD required =
        GetEnvironmentVariableW(L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
    if (required == 0) {
        std::wcerr << L"Explicit widget drop placement smoke directory is required\n";
        return 80;
    }
    std::wstring baseValue(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        baseValue.data(),
        required);
    if (copied == 0 || copied >= required) {
        std::wcerr << L"Widget drop placement smoke directory is invalid\n";
        return 80;
    }
    baseValue.resize(copied);

    const std::filesystem::path testRoot =
        std::filesystem::absolute(baseValue).lexically_normal() /
        (L"widget-drop-placement-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const std::filesystem::path configRoot = testRoot / L"Config";
    const std::filesystem::path dataRoot = testRoot / L"Data";
    const std::filesystem::path desktopRoot = testRoot / L"Desktop";
    const std::wstring categoryId = L"widget-drop-placement-category";
    const std::wstring secondaryCategoryId =
        L"widget-drop-placement-secondary";
    const std::wstring categoryName =
        L"拖放落点隔离测试-" + std::to_wstring(GetCurrentProcessId());
    const std::filesystem::path managedRoot =
        dataRoot / L"ManagedShortcuts" / categoryName;
    const std::filesystem::path referenceRoot =
        dataRoot / L"ReferenceFixtures";

    WidgetWindow* widgetForCleanup = nullptr;
    HWND ownerWindow = nullptr;
    const auto cleanup = [&]() {
        if (widgetForCleanup != nullptr) {
            widgetForCleanup->Close();
        }
        if (ownerWindow != nullptr && IsWindow(ownerWindow) != FALSE) {
            DestroyWindow(ownerWindow);
        }
        std::error_code cleanupError;
        std::filesystem::remove_all(testRoot, cleanupError);
        return !cleanupError && !std::filesystem::exists(testRoot);
    };
    const auto fail = [&](const std::wstring& message, int code) {
        std::wcerr << message << L"\n";
        if (!cleanup()) {
            std::wcerr << L"Widget drop placement cleanup failed: "
                       << testRoot.wstring() << L"\n";
            return 97;
        }
        return code;
    };
    const auto samePath = [](const std::wstring& left, const std::wstring& right) {
        return CompareStringOrdinal(
                   left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
    };
    const auto writeFixture = [](const std::filesystem::path& path, const char* body) {
        std::ofstream file(path, std::ios::binary);
        file << body;
        return static_cast<bool>(file);
    };

    std::error_code fileError;
    for (const std::filesystem::path& directory :
         std::array<std::filesystem::path, 5>{
             configRoot, dataRoot, desktopRoot, managedRoot, referenceRoot}) {
        std::filesystem::create_directories(directory, fileError);
        if (fileError) {
            return fail(L"Widget drop placement directory setup failed", 85);
        }
    }
    if (!SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_CONFIG_DIR", configRoot.c_str()) ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DATA_DIR", dataRoot.c_str()) ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DESKTOP_DIR", desktopRoot.c_str())) {
        return fail(L"Widget drop placement environment setup failed", 85);
    }

    const std::array<std::filesystem::path, 4> basePaths{
        referenceRoot / L"A.txt",
        referenceRoot / L"B.txt",
        referenceRoot / L"C.txt",
        referenceRoot / L"D.txt",
    };
    const std::array<std::filesystem::path, 4> incomingPaths{
        desktopRoot / L"N1.txt",
        desktopRoot / L"N2.txt",
        desktopRoot / L"N3.txt",
        desktopRoot / L"资料文件夹",
    };
    const std::filesystem::path collisionOriginalPath =
        desktopRoot / L"再次出现.txt";
    const std::filesystem::path collisionManagedPath =
        managedRoot / collisionOriginalPath.filename();
    for (size_t index = 0; index < basePaths.size(); ++index) {
        if (!writeFixture(basePaths[index], "reference base fixture")) {
            return fail(L"Widget drop placement base fixture creation failed", 85);
        }
    }
    for (size_t index = 0; index + 1 < incomingPaths.size(); ++index) {
        if (!writeFixture(incomingPaths[index], "incoming batch fixture")) {
            return fail(L"Widget drop placement incoming fixture creation failed", 85);
        }
    }
    std::filesystem::create_directories(incomingPaths.back(), fileError);
    if (fileError ||
        !writeFixture(incomingPaths.back() / L"内容.md", "folder child fixture")) {
        return fail(L"Widget drop placement folder fixture creation failed", 85);
    }
    if (!writeFixture(collisionManagedPath, "existing managed collision fixture")) {
        return fail(L"Widget drop placement ID collision fixture creation failed", 85);
    }

    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        return fail(L"Widget drop placement monitor lookup failed", 85);
    }

    AppConfig config;
    config.settings.showPublicDesktopItems = false;
    config.settings.singleClickOpen = false;
    const std::array<std::wstring, 4> baseIds{
        L"base-a", L"base-b", L"base-c", L"base-d"};
    for (size_t index = 0; index < basePaths.size(); ++index) {
        ItemConfig item;
        item.id = baseIds[index];
        item.path = basePaths[index].wstring();
        item.displayName = std::wstring(1, static_cast<wchar_t>(L'A' + index));
        item.originalDesktopPath =
            (desktopRoot / basePaths[index].filename()).wstring();
        config.items.push_back(std::move(item));
    }
    for (int index = 0; index < 160; ++index) {
        ItemConfig filler;
        filler.id = L"widget-drop-placement-filler-" + std::to_wstring(index);
        filler.path =
            (dataRoot / (L"入格性能规模填充-" + std::to_wstring(index) +
                L"-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ-0123456789.txt")).wstring();
        filler.displayName = L"入格性能规模填充项目-" + std::to_wstring(index);
        config.items.push_back(std::move(filler));
    }
    for (int index = 0; index < 219; ++index) {
        config.desktopLayout.push_back(DesktopPlacementConfig{
            (desktopRoot / (L"入格布局规模填充-" + std::to_wstring(index) +
                L"-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ-0123456789.txt")).wstring(),
            index * 17,
            index * 12});
    }
    CategoryConfig category;
    category.id = categoryId;
    category.name = categoryName;
    category.storageFolder = categoryName;
    category.itemIds.assign(baseIds.begin(), baseIds.end());
    category.layout.x = monitorInfo.rcWork.left + 96;
    category.layout.y = monitorInfo.rcWork.top + 96;
    category.layout.width = 246;
    category.layout.height = 250;
    category.layout.normalHeight = 250;
    category.layout.iconSize = 48;
    category.layout.density = 0;
    category.layout.contentViewMode = 0;
    category.layout.sortMode = 1;
    category.layout.autoArrange = true;
    category.layout.collapsed = false;
    category.layout.locked = false;
    category.layout.monitorId = monitorInfo.szDevice;
    config.categories.push_back(category);
    CategoryConfig secondaryCategory;
    secondaryCategory.id = secondaryCategoryId;
    secondaryCategory.name = L"引用分类移动测试";
    secondaryCategory.storageFolder = secondaryCategory.name;
    secondaryCategory.layout = category.layout;
    config.categories.push_back(secondaryCategory);
    ConfigStore configStore;
    if (!configStore.SaveAppConfig(config)) {
        return fail(L"Widget drop placement config save failed", 85);
    }

    constexpr wchar_t kOwnerClassName[] = L"Lattice.SmokeDesktopPlacementOwner";
    WNDCLASSEXW ownerClass{};
    ownerClass.cbSize = sizeof(ownerClass);
    ownerClass.lpfnWndProc = SmokeDesktopPlacementOwnerProc;
    ownerClass.hInstance = instance;
    ownerClass.lpszClassName = kOwnerClassName;
    if (RegisterClassExW(&ownerClass) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return fail(L"Widget drop placement owner class registration failed", 85);
    }
    SmokeDesktopPlacementOwnerState ownerState;
    ownerWindow = CreateWindowExW(
        0,
        kOwnerClassName,
        L"",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        instance,
        &ownerState);
    if (ownerWindow == nullptr) {
        return fail(L"Widget drop placement owner window creation failed", 85);
    }

    WidgetWindow widget(instance, ownerWindow, categoryId, 0);
    widgetForCleanup = &widget;
    if (!widget.Create()) {
        return fail(L"Widget drop placement widget creation failed", 86);
    }
    widget.Show(SW_SHOWNOACTIVATE);
    const HWND widgetWindow = WidgetWindowSmokeAccess::Window(widget);
    if (widgetWindow == nullptr || IsWindow(widgetWindow) == FALSE) {
        return fail(L"Widget drop placement widget HWND unavailable", 86);
    }
    const int widgetDpi =
        static_cast<int>(std::max<UINT>(96, GetDpiForWindow(widgetWindow)));
    SetWindowPos(
        widgetWindow,
        nullptr,
        0,
        0,
        MulDiv(246, widgetDpi, 96),
        MulDiv(250, widgetDpi, 96),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    MSG message{};
    while (PeekMessageW(
               &message,
               widgetWindow,
               kWidgetRefreshMessage,
               kWidgetRefreshMessage,
               PM_REMOVE)) {
        DispatchMessageW(&message);
    }
    const auto pumpOwnerSync = [&]() {
        MSG syncMessage{};
        while (PeekMessageW(
                   &syncMessage,
                   ownerWindow,
                   kOrganizerConfigChangedMessage,
                   kOrganizerConfigSyncMessage,
                   PM_REMOVE)) {
            DispatchMessageW(&syncMessage);
        }
    };
    const auto hasQueuedWidgetRefresh = [&]() {
        MSG refreshMessage{};
        return PeekMessageW(
                   &refreshMessage,
                   widgetWindow,
                   kWidgetRefreshMessage,
                   kWidgetRefreshMessage,
                   PM_NOREMOVE) != FALSE;
    };
    const auto waitForIcons = [&](
        const std::vector<std::wstring>& paths,
        DWORD timeoutMs) {
        for (const std::wstring& path : paths) {
            WidgetWindowSmokeAccess::PreloadIcon(widget, path);
        }
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            bool allReady = true;
            for (const std::wstring& path : paths) {
                if (!WidgetWindowSmokeAccess::IsIconReady(widget, path)) {
                    allReady = false;
                }
            }
            if (allReady) {
                return true;
            }
            if (GetTickCount64() >= deadline) {
                return false;
            }
            Sleep(5);
        }
    };

    std::vector<std::wstring> baseIconPaths;
    baseIconPaths.reserve(basePaths.size());
    for (const std::filesystem::path& path : basePaths) {
        baseIconPaths.push_back(path.wstring());
    }
    if (!waitForIcons(baseIconPaths, 3000) ||
        !RedrawWindow(
            widgetWindow,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE) ||
        WidgetWindowSmokeAccess::LastFallbackDrawCount(widget) != 0 ||
        WidgetWindowSmokeAccess::LastPlaceholderDrawCount(widget) != 0) {
        return fail(
            L"Widget drop placement base icon warmup did not stabilize",
            114);
    }

    DesktopScanner scanner;
    std::array<std::wstring, 4> incomingIds{};
    const std::vector<std::wstring> dropPaths{
        incomingPaths[0].wstring(),
        incomingPaths[1].wstring(),
        incomingPaths[1].wstring(),
        incomingPaths[2].wstring(),
        incomingPaths[3].wstring(),
    };
    const POINT insertionPoint =
        WidgetWindowSmokeAccess::InsertionScreenPoint(widget, 2);
    WidgetWindowSmokeAccess::PreviewPaths(
        widget,
        dropPaths,
        insertionPoint,
        false);
    if (!RedrawWindow(
            widgetWindow,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW)) {
        return fail(L"Widget drop placement preview frame was not painted", 115);
    }
    const std::uint64_t loadGenerationBefore =
        WidgetWindowSmokeAccess::LoadItemsGeneration(widget);
    const std::uint64_t gridGenerationBefore =
        WidgetWindowSmokeAccess::GridItemsGeneration(widget);
    const ULONGLONG dropStartedAt = GetTickCount64();
    const std::vector<std::pair<std::wstring, POINT>> incomingDesktopPositions{
        {incomingPaths[0].wstring(), POINT{40, 40}},
        {incomingPaths[1].wstring(), POINT{120, 40}},
        {incomingPaths[2].wstring(), POINT{200, 40}},
        {incomingPaths[3].wstring(), POINT{280, 40}},
    };
    if (!WidgetWindowSmokeAccess::QueuePathsWithDesktopPositions(
            widget,
            dropPaths,
            insertionPoint,
            incomingDesktopPositions)) {
        return fail(L"Widget drop placement batch queue returned false", 87);
    }
    const ULONGLONG queueDurationMs = GetTickCount64() - dropStartedAt;
    if (!WidgetWindowSmokeAccess::DropQueued(widget) ||
        !WidgetWindowSmokeAccess::DropBusy(widget) ||
        !WidgetWindowSmokeAccess::DropProjectionActive(widget)) {
        return fail(L"Widget drop placement queued state mismatch", 105);
    }
    if (WidgetWindowSmokeAccess::PendingPaths(widget) != dropPaths ||
        WidgetWindowSmokeAccess::PendingIndex(widget) != 2 ||
        WidgetWindowSmokeAccess::PendingShowError(widget)) {
        return fail(L"Widget drop placement pending payload mismatch", 106);
    }
    const std::vector<std::wstring> rejectedDropPaths{
        incomingPaths[0].wstring(),
    };
    const POINT rejectedInsertionPoint =
        WidgetWindowSmokeAccess::InsertionScreenPoint(widget, 0);
    if (WidgetWindowSmokeAccess::QueuePaths(
            widget,
            rejectedDropPaths,
            rejectedInsertionPoint,
            true) ||
        WidgetWindowSmokeAccess::PendingPaths(widget) != dropPaths ||
        WidgetWindowSmokeAccess::PendingIndex(widget) != 2 ||
        WidgetWindowSmokeAccess::PendingShowError(widget)) {
        return fail(L"Widget drop placement second queue changed pending payload", 107);
    }
    const ULONGLONG dispatchStartedAt = GetTickCount64();
    if (!WidgetWindowSmokeAccess::DispatchQueuedDrop(widget)) {
        return fail(L"Widget drop placement commit message was not dispatched", 108);
    }
    const ULONGLONG dispatchDurationMs = GetTickCount64() - dispatchStartedAt;
    if (WidgetWindowSmokeAccess::DropQueued(widget) ||
        !WidgetWindowSmokeAccess::DropBusy(widget) ||
        !WidgetWindowSmokeAccess::DropProjectionActive(widget) ||
        WidgetWindowSmokeAccess::PendingPaths(widget) != dropPaths ||
        WidgetWindowSmokeAccess::PendingIndex(widget) != 2 ||
        WidgetWindowSmokeAccess::PendingShowError(widget) ||
        !ownerState.collectionRequestCopied ||
        ownerState.collectionRequestCount != 1) {
        return fail(L"Widget drop placement async handoff state mismatch", 109);
    }
    const ULONGLONG dropDurationMs = GetTickCount64() - dropStartedAt;
    {
        std::ofstream timing(
            std::filesystem::path(baseValue) / L"widget-drop-placement-timing.txt",
            std::ios::binary | std::ios::trunc);
        timing << "queue=" << queueDurationMs << "\n";
        timing << "dispatch=" << dispatchDurationMs << "\n";
        timing << "queue_and_commit=" << dropDurationMs << "\n";
    }
    if (dropDurationMs > 50) {
        return fail(
            L"Widget drop placement exceeded the 50 ms UI-thread commit budget",
            110);
    }
    constexpr size_t kUniqueIncomingCount = 4;
    for (size_t index = 0; index < kUniqueIncomingCount; ++index) {
        if (!ownerState.collectionRequestCopied) {
            return fail(L"Widget drop placement did not queue the next atomic item", 111);
        }
        const DesktopCollectionItemRequest request = ownerState.collectionRequest;
        ownerState.collectionRequestCopied = false;
        DesktopCollectionItemResult result =
            CommitDesktopCollectionItemTransaction(request);
        SendMessageW(
            widgetWindow,
            kDesktopCollectionResultMessage,
            0,
            reinterpret_cast<LPARAM>(&result));
        if (!result.succeeded) {
            std::ofstream diagnostic(
                std::filesystem::path(baseValue) /
                    L"widget-drop-placement-error.txt",
                std::ios::trunc);
            diagnostic << "index=" << index << "\nerror="
                       << Utf8Text(result.errorMessage) << "\n";
            return fail(
                L"Widget drop placement atomic background transaction failed: " +
                    result.errorMessage,
                112);
        }
    }
    if (ownerState.collectionRequestCount !=
            static_cast<int>(kUniqueIncomingCount) ||
        WidgetWindowSmokeAccess::DropQueued(widget) ||
        WidgetWindowSmokeAccess::DropBusy(widget) ||
        WidgetWindowSmokeAccess::DropProjectionActive(widget) ||
        !WidgetWindowSmokeAccess::PendingPaths(widget).empty() ||
        WidgetWindowSmokeAccess::PendingIndex(widget) != -1 ||
        !WidgetWindowSmokeAccess::PendingShowError(widget)) {
        return fail(L"Widget drop placement async completion state mismatch", 113);
    }
    if (WidgetWindowSmokeAccess::LoadItemsGeneration(widget) !=
        loadGenerationBefore) {
        return fail(L"Widget drop placement batch called LoadItems", 88);
    }
    if (WidgetWindowSmokeAccess::GridItemsGeneration(widget) !=
            gridGenerationBefore) {
        return fail(
            L"Widget drop placement rebuilt the already-painted projection",
            116);
    }
    const std::uint64_t committedGridGeneration =
        WidgetWindowSmokeAccess::GridItemsGeneration(widget);
    WidgetWindowSmokeAccess::ClearIconCache(widget);
    if (!RedrawWindow(
            widgetWindow,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE) ||
        WidgetWindowSmokeAccess::LastFallbackDrawCount(widget) != 0 ||
        WidgetWindowSmokeAccess::LastPlaceholderDrawCount(widget) == 0) {
        return fail(
            L"Widget drop placement cold committed frame did not use typed placeholders",
            99);
    }
    pumpOwnerSync();
    if (ownerState.configSyncCount != 1 || ownerState.fullRefreshCount != 0) {
        std::wcerr << L"Widget drop placement owner messages: sync="
                   << ownerState.configSyncCount << L", changed="
                   << ownerState.fullRefreshCount << L"\n";
        return fail(L"Widget drop placement owner message count mismatch", 89);
    }
    if (hasQueuedWidgetRefresh()) {
        return fail(L"Widget drop placement queued a widget full refresh", 94);
    }

    AppConfig persisted = configStore.LoadAppConfig();
    auto persistedCategory = std::find_if(
        persisted.categories.begin(),
        persisted.categories.end(),
        [&](const CategoryConfig& value) { return value.id == categoryId; });
    for (size_t index = 0; index < incomingPaths.size(); ++index) {
        std::vector<const ItemConfig*> matches;
        for (const ItemConfig& item : persisted.items) {
            if (samePath(item.path, incomingPaths[index].wstring()) &&
                samePath(
                    item.originalDesktopPath,
                    incomingPaths[index].wstring())) {
                matches.push_back(&item);
            }
        }
        if (matches.size() != 1 || matches.front()->id.empty() ||
            std::find(
                incomingIds.begin(),
                incomingIds.begin() + static_cast<std::ptrdiff_t>(index),
                matches.front()->id) !=
                incomingIds.begin() + static_cast<std::ptrdiff_t>(index)) {
            return fail(
                L"Widget drop placement did not assign unique reference IDs",
                92);
        }
        incomingIds[index] = matches.front()->id;
    }
    const std::vector<std::wstring> expectedIds{
        baseIds[0], baseIds[1], incomingIds[0], incomingIds[1], incomingIds[2],
        incomingIds[3],
        baseIds[2], baseIds[3]};
    if (persistedCategory == persisted.categories.end() ||
        persistedCategory->itemIds != expectedIds) {
        std::wcerr << L"Widget drop placement persisted order mismatch\n";
        if (persistedCategory != persisted.categories.end()) {
            for (const std::wstring& id : persistedCategory->itemIds) {
                std::wcerr << L"  " << id << L"\n";
            }
        }
        return fail(L"Widget drop placement did not insert four references at index 2", 90);
    }
    if (persistedCategory->layout.autoArrange ||
        persistedCategory->layout.sortMode != 0) {
        return fail(L"Widget drop placement did not switch to manual order", 91);
    }
    if (WidgetWindowSmokeAccess::CurrentItemIds(widget) != expectedIds) {
        return fail(L"Widget drop placement in-memory order mismatch", 93);
    }

    std::vector<std::wstring> destinationPaths;
    for (size_t index = 0; index < incomingIds.size(); ++index) {
        const std::wstring& id = incomingIds[index];
        const size_t configCount = static_cast<size_t>(std::count_if(
            persisted.items.begin(),
            persisted.items.end(),
            [&](const ItemConfig& value) { return value.id == id; }));
        const size_t categoryCount = static_cast<size_t>(std::count(
            persistedCategory->itemIds.begin(),
            persistedCategory->itemIds.end(),
            id));
        const auto registered = std::find_if(
            persisted.items.begin(),
            persisted.items.end(),
            [&](const ItemConfig& value) { return value.id == id; });
        if (configCount != 1 || categoryCount != 1 ||
            registered == persisted.items.end() ||
            !samePath(registered->path, incomingPaths[index].wstring()) ||
            !samePath(
                registered->originalDesktopPath,
                incomingPaths[index].wstring()) ||
            !std::filesystem::exists(incomingPaths[index]) ||
            std::filesystem::exists(
                managedRoot / incomingPaths[index].filename()) ||
            std::any_of(
                destinationPaths.begin(),
                destinationPaths.end(),
                [&](const std::wstring& path) {
                    return samePath(path, registered->path);
                })) {
            return fail(L"Widget drop placement reference/config uniqueness mismatch", 92);
        }
        destinationPaths.push_back(registered->path);
    }
    std::vector<std::wstring> exactIconPaths = baseIconPaths;
    exactIconPaths.insert(
        exactIconPaths.end(),
        destinationPaths.begin(),
        destinationPaths.end());
    if (!waitForIcons(exactIconPaths, 5000) ||
        !RedrawWindow(
            widgetWindow,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE) ||
        WidgetWindowSmokeAccess::LastFallbackDrawCount(widget) != 0 ||
        WidgetWindowSmokeAccess::LastPlaceholderDrawCount(widget) != 0 ||
        WidgetWindowSmokeAccess::GridItemsGeneration(widget) !=
            committedGridGeneration) {
        return fail(
            L"Widget drop placement exact icons did not replace placeholders in place",
            100);
    }

    const std::uint64_t noOpLoadGeneration =
        WidgetWindowSmokeAccess::LoadItemsGeneration(widget);
    const std::uint64_t noOpGridGeneration =
        WidgetWindowSmokeAccess::GridItemsGeneration(widget);
    const std::vector<std::wstring> noOpItemIds =
        WidgetWindowSmokeAccess::CurrentItemIds(widget);
    widget.RefreshFromConfig();
    bool noOpRefreshDispatched = false;
    while (PeekMessageW(
               &message,
               widgetWindow,
               kWidgetRefreshMessage,
               kWidgetRefreshMessage,
               PM_REMOVE)) {
        noOpRefreshDispatched = true;
        DispatchMessageW(&message);
    }
    if (!noOpRefreshDispatched ||
        WidgetWindowSmokeAccess::LoadItemsGeneration(widget) !=
            noOpLoadGeneration + 1 ||
        WidgetWindowSmokeAccess::GridItemsGeneration(widget) !=
            noOpGridGeneration ||
        WidgetWindowSmokeAccess::CurrentItemIds(widget) != noOpItemIds ||
        hasQueuedWidgetRefresh() ||
        !RedrawWindow(
            widgetWindow,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE) ||
        WidgetWindowSmokeAccess::LastFallbackDrawCount(widget) != 0 ||
        WidgetWindowSmokeAccess::LastPlaceholderDrawCount(widget) != 0) {
        return fail(
            L"Widget drop placement no-op refresh replaced or disturbed the grid",
            101);
    }

    if (!std::filesystem::exists(incomingPaths.back() / L"内容.md")) {
        return fail(L"Widget drop placement changed the referenced folder tree", 92);
    }
    for (const std::filesystem::path& incomingPath : incomingPaths) {
        if (!std::filesystem::exists(incomingPath) ||
            std::filesystem::exists(managedRoot / incomingPath.filename())) {
            return fail(L"Widget drop placement activation moved a reference", 92);
        }
    }

    if (!writeFixture(collisionOriginalPath, "reappeared original-path fixture")) {
        return fail(L"Widget drop placement live collision fixture creation failed", 117);
    }
    const std::wstring collisionId =
        scanner.CreateItemFromPath(collisionOriginalPath.wstring()).id;
    const std::wstring collisionLiveId = collisionId + L"|live";
    ItemConfig collisionItem;
    collisionItem.id = collisionId;
    collisionItem.path = collisionManagedPath.wstring();
    collisionItem.displayName = L"旧托管项";
    collisionItem.originalDesktopPath = collisionOriginalPath.wstring();
    persisted.items.push_back(collisionItem);
    persistedCategory = std::find_if(
        persisted.categories.begin(),
        persisted.categories.end(),
        [&](const CategoryConfig& value) { return value.id == categoryId; });
    if (persistedCategory == persisted.categories.end()) {
        return fail(L"Widget drop placement collision category missing", 118);
    }
    persistedCategory->itemIds.push_back(collisionId);
    persisted.uncategorizedItemIds.push_back(collisionLiveId);
    if (!configStore.SaveAppConfig(persisted)) {
        return fail(L"Widget drop placement collision setup save failed", 119);
    }
    widget.RefreshFromConfig();
    while (PeekMessageW(
               &message,
               widgetWindow,
               kWidgetRefreshMessage,
               kWidgetRefreshMessage,
               PM_REMOVE)) {
        DispatchMessageW(&message);
    }
    DesktopItem collisionLiveItem =
        scanner.CreateItemFromPath(collisionOriginalPath.wstring(), false);
    collisionLiveItem.id = collisionLiveId;
    WidgetWindowSmokeAccess::AddLoadedItem(widget, collisionLiveItem);
    if (WidgetWindowSmokeAccess::LoadedItemIdForPath(
            widget,
            collisionOriginalPath.wstring()) != collisionLiveId) {
        return fail(
            L"Widget drop placement did not expose the live collision ID",
            102);
    }
    ownerState.configSyncCount = 0;
    ownerState.fullRefreshCount = 0;
    const std::uint64_t collisionLoadGeneration =
        WidgetWindowSmokeAccess::LoadItemsGeneration(widget);
    const POINT collisionInsertionPoint =
        WidgetWindowSmokeAccess::InsertionScreenPoint(
            widget,
            WidgetWindowSmokeAccess::CurrentItemIds(widget).size());
    const std::vector<std::wstring> collisionDropPaths{
        collisionOriginalPath.wstring(),
    };
    const int collectionRequestCountBeforeCollision =
        ownerState.collectionRequestCount;
    if (!WidgetWindowSmokeAccess::QueuePathsWithDesktopPositions(
            widget,
            collisionDropPaths,
            collisionInsertionPoint,
            {{collisionOriginalPath.wstring(), POINT{360, 40}}})) {
        return fail(L"Widget drop placement reappeared-path queue returned false", 120);
    }
    if (!WidgetWindowSmokeAccess::DispatchQueuedDrop(widget)) {
        return fail(L"Widget drop placement collision commit message missing", 121);
    }
    if (!ownerState.collectionRequestCopied ||
        ownerState.collectionRequestCount !=
            collectionRequestCountBeforeCollision + 1) {
        return fail(L"Widget drop placement collision async handoff missing", 122);
    }
    const DesktopCollectionItemRequest collisionRequest =
        ownerState.collectionRequest;
    ownerState.collectionRequestCopied = false;
    DesktopCollectionItemResult collisionResult =
        CommitDesktopCollectionItemTransaction(collisionRequest);
    SendMessageW(
        widgetWindow,
        kDesktopCollectionResultMessage,
        0,
        reinterpret_cast<LPARAM>(&collisionResult));
    if (!collisionResult.succeeded) {
        return fail(
            L"Widget drop placement collision background transaction failed: " +
                collisionResult.errorMessage,
            123);
    }
    if (WidgetWindowSmokeAccess::LoadItemsGeneration(widget) !=
        collisionLoadGeneration) {
        return fail(L"Widget drop placement collision path called LoadItems", 88);
    }
    pumpOwnerSync();
    if (ownerState.configSyncCount != 1 || ownerState.fullRefreshCount != 0 ||
        hasQueuedWidgetRefresh()) {
        return fail(L"Widget drop placement collision owner message mismatch", 89);
    }

    const AppConfig collisionPersisted = configStore.LoadAppConfig();
    const auto collisionCategory = std::find_if(
        collisionPersisted.categories.begin(),
        collisionPersisted.categories.end(),
        [&](const CategoryConfig& value) { return value.id == categoryId; });
    const auto oldCollisionItem = std::find_if(
        collisionPersisted.items.begin(),
        collisionPersisted.items.end(),
        [&](const ItemConfig& item) {
            return item.id == collisionId &&
                   samePath(item.path, collisionManagedPath.wstring());
        });
    std::vector<std::wstring> newCollisionIds;
    if (collisionCategory != collisionPersisted.categories.end()) {
        for (const std::wstring& itemId : collisionCategory->itemIds) {
            if (itemId != collisionId &&
                itemId != collisionLiveId &&
                std::find(expectedIds.begin(), expectedIds.end(), itemId) ==
                    expectedIds.end()) {
                newCollisionIds.push_back(itemId);
            }
        }
    }
    const auto newCollisionItem = newCollisionIds.size() == 1
        ? std::find_if(
              collisionPersisted.items.begin(),
              collisionPersisted.items.end(),
              [&](const ItemConfig& item) {
                  return item.id == newCollisionIds.front();
              })
        : collisionPersisted.items.end();
    const bool liveCollisionIdStillReferenced =
        std::find(
            collisionPersisted.uncategorizedItemIds.begin(),
            collisionPersisted.uncategorizedItemIds.end(),
            collisionLiveId) != collisionPersisted.uncategorizedItemIds.end() ||
        std::any_of(
            collisionPersisted.categories.begin(),
            collisionPersisted.categories.end(),
            [&](const CategoryConfig& value) {
                return std::find(
                           value.itemIds.begin(),
                           value.itemIds.end(),
                           collisionLiveId) != value.itemIds.end();
            });
    if (liveCollisionIdStillReferenced) {
        return fail(
            L"Widget drop placement left the source visible live ID referenced",
            102);
    }
    if (collisionCategory == collisionPersisted.categories.end() ||
        newCollisionIds.size() != 1 ||
        oldCollisionItem == collisionPersisted.items.end() ||
        newCollisionItem == collisionPersisted.items.end() ||
        !samePath(
            oldCollisionItem->originalDesktopPath,
            collisionOriginalPath.wstring()) ||
        !samePath(
            newCollisionItem->originalDesktopPath,
            collisionOriginalPath.wstring()) ||
        std::count(
            collisionCategory->itemIds.begin(),
            collisionCategory->itemIds.end(),
            collisionId) != 1 ||
        std::count(
            collisionCategory->itemIds.begin(),
            collisionCategory->itemIds.end(),
            newCollisionItem->id) != 1 ||
        !std::filesystem::exists(collisionManagedPath) ||
        !samePath(newCollisionItem->path, collisionOriginalPath.wstring()) ||
        samePath(newCollisionItem->path, collisionManagedPath.wstring()) ||
        !std::filesystem::exists(collisionOriginalPath)) {
        std::wofstream diagnostic(
            std::filesystem::path(baseValue) /
                L"widget-drop-placement-collision.txt",
            std::ios::trunc);
        diagnostic << L"category_found="
                   << (collisionCategory != collisionPersisted.categories.end())
                   << L"\nnew_id_count=" << newCollisionIds.size()
                   << L"\nold_found="
                   << (oldCollisionItem != collisionPersisted.items.end())
                   << L"\nnew_found="
                   << (newCollisionItem != collisionPersisted.items.end())
                   << L"\nmanaged_exists="
                   << std::filesystem::exists(collisionManagedPath)
                   << L"\noriginal_exists="
                   << std::filesystem::exists(collisionOriginalPath)
                   << L"\nlive_referenced=" << liveCollisionIdStillReferenced
                   << L"\n";
        if (oldCollisionItem != collisionPersisted.items.end()) {
            diagnostic << L"old_id=" << oldCollisionItem->id
                       << L"\nold_path=" << oldCollisionItem->path
                       << L"\nold_original=" << oldCollisionItem->originalDesktopPath
                       << L"\n";
        }
        if (newCollisionItem != collisionPersisted.items.end()) {
            diagnostic << L"new_id=" << newCollisionItem->id
                       << L"\nnew_path=" << newCollisionItem->path
                       << L"\nnew_original=" << newCollisionItem->originalDesktopPath
                       << L"\n";
        }
        if (collisionCategory != collisionPersisted.categories.end()) {
            for (const std::wstring& id : collisionCategory->itemIds) {
                diagnostic << L"category_id=" << id << L"\n";
            }
        }
        return fail(
            L"Widget drop placement reused an ID for a live reference path collision",
            124);
    }

    WidgetWindowSmokeAccess::MoveItemToCategory(
        widget,
        incomingIds.front(),
        secondaryCategoryId);
    AppConfig movedReferenceConfig = configStore.LoadAppConfig();
    const auto movedReferenceCategory = std::find_if(
        movedReferenceConfig.categories.begin(),
        movedReferenceConfig.categories.end(),
        [&](const CategoryConfig& value) {
            return value.id == secondaryCategoryId;
        });
    if (movedReferenceCategory == movedReferenceConfig.categories.end() ||
        std::count(
            movedReferenceCategory->itemIds.begin(),
            movedReferenceCategory->itemIds.end(),
            incomingIds.front()) != 1 ||
        !std::filesystem::exists(incomingPaths.front()) ||
        std::filesystem::exists(managedRoot / incomingPaths.front().filename())) {
        return fail(L"Widget reference category move touched the original item", 125);
    }
    const std::wstring removedReferenceId = incomingIds[1];
    if (!WidgetWindowSmokeAccess::MoveItemOut(widget, removedReferenceId)) {
        return fail(L"Widget reference removal returned false", 126);
    }
    const AppConfig removedReferenceConfig = configStore.LoadAppConfig();
    const bool removedReferenceStillRegistered = std::any_of(
        removedReferenceConfig.items.begin(),
        removedReferenceConfig.items.end(),
        [&](const ItemConfig& value) {
            return value.id == removedReferenceId;
        });
    if (removedReferenceStillRegistered ||
        !std::filesystem::exists(incomingPaths[1]) ||
        std::filesystem::exists(managedRoot / incomingPaths[1].filename())) {
        return fail(L"Widget reference removal touched the original item", 127);
    }

    if (!cleanup()) {
        std::wcerr << L"Widget drop placement cleanup failed: "
                   << testRoot.wstring() << L"\n";
        return 97;
    }
    std::wcout << L"Widget drop placement isolated regression passed; drop="
               << dropDurationMs << L"ms\n";
    return 0;
}

int RunDialogPreview(HINSTANCE instance) {
    MessageDialog::Show(
        instance,
        nullptr,
        L"解散格子后，其中项目会取消格子显示归属并重新显示在桌面；原件路径不会改变。\n\n是否继续？",
        L"确认解散",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    return 0;
}

int RunInputDialogPreview(HINSTANCE instance) {
    InputDialog::Prompt(instance, nullptr, L"新建格子", L"格子名称", L"工作资料");
    return 0;
}

int RunSettingsDialogPreview(HINSTANCE instance) {
    AppSettings settings;
    SettingsDialog::Show(instance, nullptr, settings);
    return 0;
}

int RunSmokeUpdateAndDialog() {
    AttachParentConsole();
    std::wstring version;
    std::wstring url;
    if (UpdateService::CompareVersions(L"0.4.29", L"0.4.28") <= 0 ||
        UpdateService::CompareVersions(L"0.4.28", L"0.4.28") != 0 ||
        UpdateService::CompareVersions(L"0.4.27", L"0.4.28") >= 0) {
        std::wcerr << L"Update version comparison failed\n";
        return 1;
    }

    const std::string allAssetsForward = R"({"tag_name":"v0.4.50","assets":[{"name":"Lattice-Setup-Latest-Offline.exe","browser_download_url":"https://example.invalid/Lattice-Setup-Latest-Offline.exe"},{"name":"Lattice-Setup-0.4.50-Offline.exe","browser_download_url":"https://example.invalid/Lattice-Setup-0.4.50-Offline.exe"},{"name":"Lattice-Setup-Latest.exe","browser_download_url":"https://example.invalid/Lattice-Setup-Latest.exe"},{"name":"Lattice-Setup-0.4.50.exe","browser_download_url":"https://example.invalid/Lattice-Setup-0.4.50.exe"}]})";
    const std::string allAssetsReverse = R"({"tag_name":"v0.4.50","assets":[{"name":"Lattice-Setup-0.4.50.exe","browser_download_url":"https://example.invalid/Lattice-Setup-0.4.50.exe"},{"name":"Lattice-Setup-Latest.exe","browser_download_url":"https://example.invalid/Lattice-Setup-Latest.exe"},{"name":"Lattice-Setup-0.4.50-Offline.exe","browser_download_url":"https://example.invalid/Lattice-Setup-0.4.50-Offline.exe"},{"name":"Lattice-Setup-Latest-Offline.exe","browser_download_url":"https://example.invalid/Lattice-Setup-Latest-Offline.exe"}]})";
    const auto selects = [&version, &url](
        const std::string& metadata,
        UpdatePackageVariant variant,
        const wchar_t* expectedAsset) {
        return UpdateService::SelectReleaseAssetForVariant(
                   metadata, variant, version, url) &&
            version == L"0.4.50" &&
            url == std::wstring(L"https://example.invalid/") + expectedAsset;
    };
    if (!selects(
            allAssetsForward,
            UpdatePackageVariant::Standard,
            L"Lattice-Setup-0.4.50.exe") ||
        !selects(
            allAssetsReverse,
            UpdatePackageVariant::Standard,
            L"Lattice-Setup-0.4.50.exe") ||
        !selects(
            allAssetsForward,
            UpdatePackageVariant::Offline,
            L"Lattice-Setup-0.4.50-Offline.exe") ||
        !selects(
            allAssetsReverse,
            UpdatePackageVariant::Offline,
            L"Lattice-Setup-0.4.50-Offline.exe")) {
        std::wcerr << L"Update asset selection depends on release order\n";
        return 2;
    }

    const std::string standardLatestJson = R"({"tag_name":"0.4.50","assets":[{"name":"Lattice-Setup-Latest-Offline.exe","browser_download_url":"https://example.invalid/Lattice-Setup-Latest-Offline.exe"},{"name":"Lattice-Setup-0.4.50-Offline.exe","browser_download_url":"https://example.invalid/Lattice-Setup-0.4.50-Offline.exe"},{"name":"Lattice-Setup-Latest.exe","browser_download_url":"https://example.invalid/Lattice-Setup-Latest.exe"}]})";
    const std::string offlineOnlyJson = R"({"tag_name":"0.4.50","assets":[{"name":"Lattice-Setup-Latest-Offline.exe","browser_download_url":"https://example.invalid/Lattice-Setup-Latest-Offline.exe"},{"name":"Lattice-Setup-0.4.50-Offline.exe","browser_download_url":"https://example.invalid/Lattice-Setup-0.4.50-Offline.exe"}]})";
    const std::string latestOfflineJson = R"({"tag_name":"0.4.50","assets":[{"name":"Lattice-Setup-0.4.50.exe","browser_download_url":"https://example.invalid/Lattice-Setup-0.4.50.exe"},{"name":"Lattice-Setup-Latest-Offline.exe","browser_download_url":"https://example.invalid/Lattice-Setup-Latest-Offline.exe"}]})";
    const std::string latestOfflineOnlyJson = R"({"tag_name":"0.4.50","assets":[{"name":"Lattice-Setup-Latest-Offline.exe","browser_download_url":"https://example.invalid/Lattice-Setup-Latest-Offline.exe"}]})";
    if (!selects(
            standardLatestJson,
            UpdatePackageVariant::Standard,
            L"Lattice-Setup-Latest.exe") ||
        !selects(
            offlineOnlyJson,
            UpdatePackageVariant::Standard,
            L"Lattice-Setup-0.4.50-Offline.exe") ||
        !selects(
            latestOfflineJson,
            UpdatePackageVariant::Offline,
            L"Lattice-Setup-Latest-Offline.exe") ||
        !selects(
            latestOfflineOnlyJson,
            UpdatePackageVariant::Standard,
            L"Lattice-Setup-Latest-Offline.exe") ||
        !UpdateService::SelectReleaseAsset(
            allAssetsReverse, version, url) ||
        url != L"https://example.invalid/Lattice-Setup-0.4.50.exe") {
        std::wcerr << L"Update package priority selection failed\n";
        return 20;
    }

    const std::string standardOnlyJson = R"({"tag_name":"0.4.50","assets":[{"name":"Lattice-Setup-0.4.50.exe","browser_download_url":"https://example.invalid/Lattice-Setup-0.4.50.exe"},{"name":"Lattice-Setup-Latest.exe","browser_download_url":"https://example.invalid/Lattice-Setup-Latest.exe"}]})";
    version = L"stale";
    url = L"stale";
    if (UpdateService::SelectReleaseAssetForVariant(
            standardOnlyJson,
            UpdatePackageVariant::Offline,
            version,
            url) ||
        !version.empty() || !url.empty() ||
        UpdateService::SelectReleaseAsset(
            R"({"tag_name":"next","assets":[]})", version, url)) {
        std::wcerr << L"Offline update asset selection did not fail closed\n";
        return 21;
    }

    const RECT primaryWork{0, 0, 1920, 1040};
    const RECT edgeOwner{1800, 980, 1980, 1100};
    const RECT edge = MessageDialog::CalculatePlacement(edgeOwner, primaryWork, 590, 310);
    const RECT negativeWork{-1600, -200, 0, 700};
    const RECT negativeOwner{-1590, -190, -1500, -100};
    const RECT negative = MessageDialog::CalculatePlacement(negativeOwner, negativeWork, 590, 310);
    const RECT tinyWork{50, 60, 350, 240};
    const RECT oversized = MessageDialog::CalculatePlacement(tinyWork, tinyWork, 590, 310);
    const auto contained = [](const RECT& value, const RECT& work) {
        return value.left >= work.left && value.top >= work.top &&
            value.right <= work.right && value.bottom <= work.bottom;
    };
    if (!contained(edge, primaryWork) || !contained(negative, negativeWork) ||
        !contained(oversized, tinyWork) || oversized.right - oversized.left != 300 ||
        oversized.bottom - oversized.top != 180) {
        std::wcerr << L"Message dialog work-area placement failed\n";
        return 3;
    }

    DesktopScanner scanner;
    ManagedShortcutStore store;
    const std::array<std::wstring, 2> controlPanelNames{
        L"::{26EE0668-A00A-44D7-9371-BEB064C98683}",
        L"shell:::{26EE0668-A00A-44D7-9371-BEB064C98683}"};
    bool shellItemAccepted = false;
    for (const std::wstring& parsingName : controlPanelNames) {
        if (!store.IsShellNamespaceItem(parsingName)) continue;
        const DesktopItem item = scanner.CreateItemFromPath(parsingName, false);
        shellItemAccepted = store.IsSupportedDesktopItem(parsingName) &&
            !item.missing && !item.displayName.empty() && item.id.rfind(L"shell|", 0) == 0;
        if (shellItemAccepted) break;
    }
    if (!shellItemAccepted) {
        std::wcerr << L"Control Panel Shell parsing identity failed\n";
        return 4;
    }
    std::wcout << L"Update parsing, Shell identity, and dialog work-area regression passed\n";
    return 0;
}

int RunSmokeRealShellVisibility() {
    AttachParentConsole();
    constexpr wchar_t newStartKey[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\NewStartPanel";
    constexpr wchar_t classicKey[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\ClassicStartMenu";
    constexpr wchar_t controlPanelClsid[] = L"{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}";
    const auto readValue = [](const wchar_t* keyPath) {
        DWORD value = 0;
        DWORD bytes = sizeof(value);
        const LSTATUS status = RegGetValueW(
            HKEY_CURRENT_USER,
            keyPath,
            controlPanelClsid,
            RRF_RT_REG_DWORD,
            nullptr,
            &value,
            &bytes);
        return status == ERROR_SUCCESS ? static_cast<int>(value) : -1;
    };

    if (PreferredShellDropPreviewEffect(DROPEFFECT_MOVE | DROPEFFECT_LINK) != DROPEFFECT_MOVE ||
        PreferredShellDropPreviewEffect(DROPEFFECT_LINK) != DROPEFFECT_LINK ||
        PreferredShellDropPreviewEffect(DROPEFFECT_COPY) != DROPEFFECT_COPY ||
        PreferredShellDropPreviewEffect(DROPEFFECT_NONE) != DROPEFFECT_NONE) {
        std::wcerr << L"Shell drop effect negotiation failed\n";
        return 1;
    }

    PIDLIST_ABSOLUTE absolutePidl = nullptr;
    SFGAOF parsedAttributes = 0;
    Microsoft::WRL::ComPtr<IDataObject> controlPanelData;
    const HRESULT parseResult = SHParseDisplayName(
        L"shell:::{26EE0668-A00A-44D7-9371-BEB064C98683}",
        nullptr,
        &absolutePidl,
        0,
        &parsedAttributes);
    if (SUCCEEDED(parseResult) && absolutePidl != nullptr) {
        PIDLIST_ABSOLUTE parentPidl = ILCloneFull(absolutePidl);
        PCUITEMID_CHILD childPidl = ILFindLastID(absolutePidl);
        if (parentPidl != nullptr && ILRemoveLastID(parentPidl) != FALSE) {
            SHCreateDataObject(
                parentPidl,
                1,
                &childPidl,
                nullptr,
                IID_PPV_ARGS(controlPanelData.GetAddressOf()));
        }
        if (parentPidl != nullptr) {
            ILFree(parentPidl);
        }
        ILFree(absolutePidl);
    }

    ManagedShortcutStore store;
    const std::vector<std::wstring> dropPaths = ExtractShellDropPaths(controlPanelData.Get());
    const auto controlPanelPath = std::find_if(
        dropPaths.begin(),
        dropPaths.end(),
        [&](const std::wstring& path) { return store.IsShellNamespaceItem(path); });
    if (controlPanelData == nullptr || controlPanelPath == dropPaths.end()) {
        std::wcerr << L"Real Control Panel IDataObject extraction failed\n";
        return 2;
    }

    const int originalNewStart = readValue(newStartKey);
    const int originalClassic = readValue(classicKey);
    ItemConfig visibility;
    visibility.path = *controlPanelPath;
    std::wstring errorMessage;
    if (!store.CaptureAndSuppressDesktopVisibility(visibility.path, visibility, errorMessage)) {
        std::wcerr << L"Real Control Panel visibility capture failed: " << errorMessage << L"\n";
        return 3;
    }
    const bool hidden = visibility.desktopVisibilityMode == 2 &&
        readValue(newStartKey) == 1 && readValue(classicKey) == 1;
    const bool restored = store.RestoreDesktopVisibility(visibility, errorMessage);
    const bool exactOriginal = readValue(newStartKey) == originalNewStart &&
        readValue(classicKey) == originalClassic;
    if (!hidden || !restored || !exactOriginal) {
        std::wcerr << L"Real Control Panel visibility transaction failed: " << errorMessage << L"\n";
        return 4;
    }
    std::wcout << L"Real Control Panel OLE identity and visibility hide/restore passed\n";
    return 0;
}

int RunSmokeRealDesktopGridSnapshot() {
    AttachParentConsole();
    ConfigStore configStore;
    DesktopLayout layout;
    std::vector<DesktopPosition> positions;
    DWORD flags = 0;
    std::wstring errorMessage;
    const std::filesystem::path resultPath =
        std::filesystem::path(configStore.ConfigPath()).parent_path() /
        L"real-desktop-grid-snapshot-result.txt";
    std::ofstream result(resultPath, std::ios::trunc);
    const bool positionsCaptured =
        layout.CaptureAllPositions(positions, errorMessage);
    const bool flagsCaptured = positionsCaptured &&
        layout.CaptureViewFlags(flags, errorMessage);
    if (!positionsCaptured || !flagsCaptured) {
        result << "POSITIONS_CAPTURED=" << positionsCaptured << "\n"
               << "FLAGS_CAPTURED=" << flagsCaptured << "\n"
               << "ERROR=" << Utf8Text(errorMessage) << "\n"
               << "STATUS=FAIL\n";
        return 1;
    }
    AppConfig config;
    config.desktopLayout.reserve(positions.size());
    for (const DesktopPosition& position : positions) {
        config.desktopLayout.push_back(
            DesktopPlacementConfig{position.path, position.point.x, position.point.y});
    }
    if (!configStore.SaveAppConfig(config)) {
        result << "STATUS=FAIL\n";
        return 2;
    }
    result << "FLAGS=" << flags << "\n";
    result << "POSITION_COUNT=" << positions.size() << "\n";
    result << "STATUS=PASS\n";
    return 0;
}

int RunSmokeRealDesktopSessionRestore() {
    AttachParentConsole();
    const auto environmentValue = [](const wchar_t* name) {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) return std::wstring{};
        std::wstring value(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
        if (copied == 0 || copied >= required) return std::wstring{};
        value.resize(copied);
        return value;
    };
    ConfigStore configStore;
    ManagedShortcutStore managedStore;
    DesktopLayout layout;
    const std::filesystem::path configDirectory =
        std::filesystem::path(configStore.ConfigPath()).parent_path();
    std::ofstream result(
        configDirectory / L"real-desktop-session-restore-result.txt",
        std::ios::trunc);
    const std::wstring markerPath = environmentValue(L"LATTICE_REAL_DESKTOP_MARKER");
    const std::wstring markerName = std::filesystem::path(markerPath).filename().wstring();
    if (markerPath.empty() || !managedStore.IsDesktopPath(markerPath) ||
        markerName.rfind(L".lattice-grid-restart-", 0) != 0 ||
        GetFileAttributesW(markerPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        result << "MARKER_REJECTED=1\nSTATUS=FAIL\n";
        return 20;
    }

    {
        std::ofstream marker(markerPath, std::ios::binary | std::ios::trunc);
        marker << "Lattice real desktop session restore marker";
        if (!marker) {
            result << "MARKER_CREATE_FAILED=1\nSTATUS=FAIL\n";
            return 21;
        }
    }
    SHChangeNotify(
        SHCNE_CREATE,
        SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
        markerPath.c_str(),
        nullptr);

    POINT markerPoint{};
    POINT previousPoint{};
    int stableSamples = 0;
    std::wstring errorMessage;
    bool markerVisible = false;
    for (int attempt = 0; attempt < 80; ++attempt) {
        POINT current{};
        if (layout.CapturePosition(markerPath, current, errorMessage)) {
            if (stableSamples > 0 &&
                current.x == previousPoint.x && current.y == previousPoint.y) {
                ++stableSamples;
            } else {
                previousPoint = current;
                stableSamples = 1;
            }
            if (stableSamples >= 2) {
                markerPoint = current;
                markerVisible = true;
                break;
            }
        } else {
            stableSamples = 0;
        }
        Sleep(20);
    }
    DWORD flagsBefore = 0;
    std::vector<DesktopPosition> before;
    if (!markerVisible ||
        !layout.CaptureViewFlags(flagsBefore, errorMessage) ||
        !layout.CaptureAllPositions(before, errorMessage)) {
        result << "MARKER_NOT_STABLE=1\nSTATUS=FAIL\n";
        return 22;
    }
    const auto markerInSnapshot = std::find_if(
        before.begin(),
        before.end(),
        [&](const DesktopPosition& position) {
            return CompareStringOrdinal(
                       position.path.c_str(), -1,
                       markerPath.c_str(), -1,
                       TRUE) == CSTR_EQUAL;
        });
    if (markerInSnapshot == before.end()) {
        result << "MARKER_MISSING_FROM_SNAPSHOT=1\nSTATUS=FAIL\n";
        return 22;
    }
    markerPoint = markerInSnapshot->point;

    AppConfig config;
    config.uncategorizedName = L"真实退出恢复";
    config.uncategorizedStorageFolder = L"real-exit-restore";
    config.uncategorizedItemIds = {L"real-exit-restore-marker"};
    ItemConfig markerItem;
    markerItem.id = L"real-exit-restore-marker";
    markerItem.path = markerPath;
    markerItem.displayName = markerName;
    markerItem.originalDesktopPath = markerPath;
    config.items.push_back(markerItem);
    for (const DesktopPosition& position : before) {
        config.desktopLayout.push_back(
            DesktopPlacementConfig{position.path, position.point.x, position.point.y});
    }
    const std::wstring stalePath =
        managedStore.DesktopPath() + L"\\.lattice-missing-history-" +
        std::to_wstring(GetCurrentProcessId()) + L".lnk";
    config.desktopLayout.push_back(DesktopPlacementConfig{stalePath, 123, 456});
    if (!configStore.SaveAppConfig(config)) {
        result << "CONFIG_SETUP_FAILED=1\nSTATUS=FAIL\n";
        return 23;
    }

    DesktopSession session;
    if (!session.Activate(configStore, managedStore, errorMessage)) {
        result << "ACTIVATE_FAILED=1\nSTATUS=FAIL\n";
        return 24;
    }
    const AppConfig activeConfig = configStore.LoadAppConfig();
    if (activeConfig.items.size() != 1 ||
        !managedStore.IsManagedPath(activeConfig.items.front().path) ||
        GetFileAttributesW(markerPath.c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(activeConfig.items.front().path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result << "ACTIVATE_STATE_INVALID=1\nSTATUS=FAIL\n";
        return 25;
    }

    const ULONGLONG restoreStartedAt = GetTickCount64();
    const bool deactivated = session.Deactivate(configStore, managedStore, errorMessage);
    const ULONGLONG restoreElapsed = GetTickCount64() - restoreStartedAt;
    POINT restoredPoint1{};
    POINT restoredPoint2{};
    DWORD flagsAfter = 0;
    const bool capturedPoint1 = layout.CapturePosition(markerPath, restoredPoint1, errorMessage);
    Sleep(100);
    const bool capturedPoint2 = layout.CapturePosition(markerPath, restoredPoint2, errorMessage);
    std::vector<DesktopPosition> after;
    const bool capturedAfter = layout.CaptureAllPositions(after, errorMessage);
    const bool flagsCapturedAfter =
        layout.CaptureViewFlags(flagsAfter, errorMessage);
    const AppConfig restoredConfig = configStore.LoadAppConfig();
    const bool stalePruned = std::none_of(
        restoredConfig.desktopLayout.begin(),
        restoredConfig.desktopLayout.end(),
        [&](const DesktopPlacementConfig& placement) {
            return CompareStringOrdinal(
                       placement.path.c_str(), -1,
                       stalePath.c_str(), -1,
                       TRUE) == CSTR_EQUAL;
        });
    const auto comparable = [](const std::vector<DesktopPosition>& positions) {
        std::vector<std::tuple<std::wstring, LONG, LONG>> values;
        values.reserve(positions.size());
        for (const DesktopPosition& position : positions) {
            std::wstring normalized = position.path;
            std::transform(
                normalized.begin(), normalized.end(), normalized.begin(),
                [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
            values.emplace_back(normalized, position.point.x, position.point.y);
        }
        std::sort(values.begin(), values.end());
        return values;
    };
    const bool positionMatch = capturedPoint1 && capturedPoint2 &&
        restoredPoint1.x == markerPoint.x && restoredPoint1.y == markerPoint.y &&
        restoredPoint2.x == markerPoint.x && restoredPoint2.y == markerPoint.y;
    const bool layoutMatch = capturedAfter && comparable(before) == comparable(after);
    const bool flagsMatch = flagsCapturedAfter && flagsAfter == flagsBefore;
    const bool passed = deactivated && restoreElapsed < 2000 &&
        positionMatch && layoutMatch && flagsMatch &&
        restoredConfig.items.size() == 1 &&
        CompareStringOrdinal(
            restoredConfig.items.front().path.c_str(), -1,
            markerPath.c_str(), -1,
            TRUE) == CSTR_EQUAL &&
        stalePruned;
    result << "RESTORE_ELAPSED_MS=" << restoreElapsed << "\n";
    result << "DEACTIVATED=" << (deactivated ? 1 : 0) << "\n";
    result << "MARKER_X=" << markerPoint.x << "\n";
    result << "MARKER_Y=" << markerPoint.y << "\n";
    result << "RESTORED_X=" << restoredPoint2.x << "\n";
    result << "RESTORED_Y=" << restoredPoint2.y << "\n";
    result << "POSITION_MATCH=" << (positionMatch ? 1 : 0) << "\n";
    result << "LAYOUT_MATCH=" << (layoutMatch ? 1 : 0) << "\n";
    result << "FLAGS_MATCH=" << (flagsMatch ? 1 : 0) << "\n";
    result << "STALE_PRUNED=" << (stalePruned ? 1 : 0) << "\n";
    result << "STATUS=" << (passed ? "PASS" : "FAIL") << "\n";
    return passed ? 0 : 26;
}

int RunSmokeRealDesktopTakeoverInvariants(HINSTANCE instance) {
    AttachParentConsole();
    const auto environmentValue = [](const wchar_t* name) {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) return std::wstring{};
        std::wstring value(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
        if (copied == 0 || copied >= required) return std::wstring{};
        value.resize(copied);
        return value;
    };
    ConfigStore configStore;
    ManagedShortcutStore managedStore;
    DesktopLayout layout;
    const std::filesystem::path configDirectory =
        std::filesystem::path(configStore.ConfigPath()).parent_path();
    std::ofstream result(
        configDirectory / L"real-desktop-takeover-result.txt",
        std::ios::trunc);
    const std::wstring markerPath = environmentValue(L"LATTICE_REAL_DESKTOP_MARKER");
    const std::wstring publicPath = environmentValue(L"LATTICE_REAL_PUBLIC_DESKTOP_ITEM");
    const std::wstring dropSourcePath = environmentValue(L"LATTICE_REAL_DROP_SOURCE");
    const std::wstring markerName = std::filesystem::path(markerPath).filename().wstring();
    const DWORD publicAttributes = GetFileAttributesW(publicPath.c_str());
    const DWORD sourceAttributes = GetFileAttributesW(dropSourcePath.c_str());
    if (markerPath.empty() || publicPath.empty() ||
        dropSourcePath.empty() ||
        !managedStore.IsDesktopPath(markerPath) ||
        !managedStore.IsPublicDesktopPath(publicPath) ||
        markerName.rfind(L".lattice-grid-restart-", 0) != 0 ||
        GetFileAttributesW(markerPath.c_str()) != INVALID_FILE_ATTRIBUTES ||
        sourceAttributes == INVALID_FILE_ATTRIBUTES ||
        (sourceAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        std::filesystem::path(dropSourcePath).filename() != markerName ||
        publicAttributes == INVALID_FILE_ATTRIBUTES ||
        (publicAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        result << "INPUT_REJECTED=1\nSTATUS=FAIL\n";
        return 30;
    }

    DesktopViewSnapshot dropSnapshot;
    std::wstring errorMessage;
    if (!layout.CaptureViewSnapshot(dropSnapshot, errorMessage)) {
        result << "DROP_SNAPSHOT_FAILED=1\nSTATUS=FAIL\n";
        return 31;
    }
    DesktopSurfaceWindow dropSurface(instance);
    if (!dropSurface.Create({}, errorMessage)) {
        result << "DROP_SURFACE_FAILED=1\nSTATUS=FAIL\n";
        return 31;
    }
    Microsoft::WRL::ComPtr<IDataObject> dropData;
    PIDLIST_ABSOLUTE absolute = nullptr;
    HRESULT dropResult = SHParseDisplayName(
        dropSourcePath.c_str(), nullptr, &absolute, 0, nullptr);
    if (SUCCEEDED(dropResult) && absolute != nullptr) {
        Microsoft::WRL::ComPtr<IShellFolder> parent;
        PCUITEMID_CHILD child = nullptr;
        dropResult = SHBindToParent(
            absolute, IID_PPV_ARGS(parent.GetAddressOf()), &child);
        if (SUCCEEDED(dropResult) && parent != nullptr && child != nullptr) {
            dropResult = parent->GetUIObjectOf(
                dropSurface.Window(), 1, &child, IID_IDataObject, nullptr,
                reinterpret_cast<void**>(dropData.GetAddressOf()));
        }
        CoTaskMemFree(absolute);
    }
    IDropTarget* dropTarget = DesktopSurfaceWindowSmokeAccess::DropTarget(dropSurface);
    POINTL dropPoint{
        dropSnapshot.screenRect.right - 128,
        dropSnapshot.screenRect.bottom - 128};
    DWORD dropEffect = DROPEFFECT_COPY;
    if (FAILED(dropResult) || dropData == nullptr || dropTarget == nullptr ||
        FAILED(dropTarget->DragEnter(
            dropData.Get(), MK_LBUTTON, dropPoint, &dropEffect)) ||
        dropEffect == DROPEFFECT_NONE ||
        FAILED(dropTarget->Drop(
            dropData.Get(), 0, dropPoint, &dropEffect)) ||
        dropEffect == DROPEFFECT_NONE) {
        if (dropTarget != nullptr) {
            dropTarget->DragLeave();
        }
        dropSurface.Close();
        result << "NATIVE_BACKGROUND_DROP_FAILED=1\nSTATUS=FAIL\n";
        return 31;
    }
    dropSurface.Close();

    DesktopViewSnapshot before;
    std::vector<DesktopPosition> positionsBefore;
    const std::array<std::wstring, 2> expectedPaths{markerPath, publicPath};
    bool captured = false;
    for (int attempt = 0; attempt < 80; ++attempt) {
        if (layout.CaptureViewSnapshot(before, errorMessage) &&
            std::all_of(
                expectedPaths.begin(), expectedPaths.end(),
                [&](const std::wstring& expected) {
                    return std::any_of(before.items.begin(), before.items.end(),
                        [&](const DesktopViewItem& item) {
                            return CompareStringOrdinal(item.path.c_str(), -1,
                                expected.c_str(), -1, TRUE) == CSTR_EQUAL;
                        });
                })) {
            captured = true;
            break;
        }
        Sleep(25);
    }
    if (!captured || !layout.CaptureAllPositions(positionsBefore, errorMessage)) {
        result << "SNAPSHOT_FAILED=1\nSTATUS=FAIL\n";
        return 32;
    }
    POINT droppedPoint1{};
    POINT droppedPoint2{};
    const bool droppedPositionStable =
        layout.CaptureScreenPosition(markerPath, droppedPoint1, errorMessage) &&
        (Sleep(100), layout.CaptureScreenPosition(
            markerPath, droppedPoint2, errorMessage)) &&
        droppedPoint1.x == droppedPoint2.x && droppedPoint1.y == droppedPoint2.y;
    const auto markerIdentity = ReadStableFileIdentity(markerPath);
    const auto publicIdentity = ReadStableFileIdentity(publicPath);
    const auto markerBytes = ReadFileBytes(markerPath);
    const auto publicBytes = ReadFileBytes(publicPath);
    const auto countTopLevel = [](const std::filesystem::path& directory) {
        size_t count = 0;
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end;
             !error && it != end; it.increment(error)) ++count;
        return error ? (std::numeric_limits<size_t>::max)() : count;
    };
    const std::filesystem::path userRoot = std::filesystem::path(markerPath).parent_path();
    const std::filesystem::path publicRoot = std::filesystem::path(publicPath).parent_path();
    const size_t userCount = countTopLevel(userRoot);
    const size_t publicCount = countTopLevel(publicRoot);

    AppConfig config;
    config.settings.showPublicDesktopItems = true;
    CategoryConfig category;
    category.id = L"real-takeover";
    category.name = L"真实原路径接管";
    config.categories.push_back(category);
    for (const DesktopPosition& position : positionsBefore) {
        config.desktopLayout.push_back(
            DesktopPlacementConfig{position.path, position.point.x, position.point.y});
    }
    if (!configStore.SaveAppConfig(config)) {
        result << "CONFIG_SETUP_FAILED=1\nSTATUS=FAIL\n";
        return 33;
    }
    DesktopCollectionItemRequest userRequest;
    userRequest.categoryId = category.id;
    userRequest.path = markerPath;
    userRequest.insertionIndex = 0;
    DesktopCollectionItemRequest publicRequest = userRequest;
    publicRequest.path = publicPath;
    publicRequest.insertionIndex = 1;
    const DesktopCollectionItemResult userCollected =
        CommitDesktopCollectionItemTransaction(userRequest);
    const DesktopCollectionItemResult publicCollected =
        CommitDesktopCollectionItemTransaction(publicRequest);
    if (!userCollected.succeeded || !publicCollected.succeeded ||
        CompareStringOrdinal(userCollected.destinationPath.c_str(), -1,
            markerPath.c_str(), -1, TRUE) != CSTR_EQUAL ||
        CompareStringOrdinal(publicCollected.destinationPath.c_str(), -1,
            publicPath.c_str(), -1, TRUE) != CSTR_EQUAL) {
        result << "COLLECTION_FAILED=1\nSTATUS=FAIL\n";
        return 34;
    }

    const std::vector<std::wstring> assigned{markerPath, publicPath};
    bool partitionStable = true;
    for (int restart = 0; restart < 2; ++restart) {
        DesktopSurfaceWindow surface(instance);
        if (!surface.Create(assigned, errorMessage)) {
            partitionStable = false;
            break;
        }
        surface.Show();
        const DesktopViewSnapshot& snapshot = surface.Snapshot();
        const bool assignedPresent = std::all_of(
            assigned.begin(), assigned.end(), [&](const std::wstring& expected) {
                return std::any_of(snapshot.items.begin(), snapshot.items.end(),
                    [&](const DesktopViewItem& item) {
                        return CompareStringOrdinal(item.path.c_str(), -1,
                            expected.c_str(), -1, TRUE) == CSTR_EQUAL;
                    });
            });
        partitionStable = partitionStable && assignedPresent &&
            surface.VisibleItemCount() + assigned.size() == snapshot.items.size();
        surface.Close();
        if (!partitionStable) break;
    }

    DesktopViewSnapshot after;
    std::vector<DesktopPosition> positionsAfter;
    const bool afterCaptured = layout.CaptureViewSnapshot(after, errorMessage) &&
        layout.CaptureAllPositions(positionsAfter, errorMessage);
    const auto comparable = [](const std::vector<DesktopPosition>& positions) {
        std::vector<std::tuple<std::wstring, LONG, LONG>> values;
        for (const DesktopPosition& position : positions) {
            std::wstring path = position.path;
            std::transform(path.begin(), path.end(), path.begin(),
                [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
            values.emplace_back(path, position.point.x, position.point.y);
        }
        std::sort(values.begin(), values.end());
        return values;
    };
    const AppConfig persisted = configStore.LoadAppConfig();
    const bool metadataStable = persisted.categories.size() == 1 &&
        persisted.categories.front().itemIds ==
            std::vector<std::wstring>{userCollected.itemId, publicCollected.itemId};
    const bool pathInvariant =
        SameStableFileIdentity(markerIdentity, ReadStableFileIdentity(markerPath)) &&
        SameStableFileIdentity(publicIdentity, ReadStableFileIdentity(publicPath)) &&
        markerBytes == ReadFileBytes(markerPath) &&
        markerBytes == ReadFileBytes(dropSourcePath) &&
        publicBytes == ReadFileBytes(publicPath) &&
        countTopLevel(userRoot) == userCount &&
        countTopLevel(publicRoot) == publicCount &&
        !std::filesystem::exists(std::filesystem::path(managedStore.RootPath()));
    const bool explorerInvariant = afterCaptured &&
        after.viewFlags == before.viewFlags &&
        comparable(positionsAfter) == comparable(positionsBefore) &&
        IsWindowVisible(after.listViewWindow) != FALSE &&
        IsWindowEnabled(after.listViewWindow) != FALSE;
    const bool passed = droppedPositionStable && partitionStable && metadataStable &&
        pathInvariant && explorerInvariant;
    result << "NATIVE_BACKGROUND_DROP_GRID_STABLE=" << droppedPositionStable << "\n";
    result << "DROP_EFFECT=" << dropEffect << "\n";
    result << "PARTITION_RESTART_STABLE=" << partitionStable << "\n";
    result << "METADATA_STABLE=" << metadataStable << "\n";
    result << "PATH_FILEID_CONTENT_COUNT_STABLE=" << pathInvariant << "\n";
    result << "EXPLORER_FLAGS_COORDS_RESTORED=" << explorerInvariant << "\n";
    result << "STATUS=" << (passed ? "PASS" : "FAIL") << "\n";
    return passed ? 0 : 35;
}

int RunSmokeRealDesktopGridCleanup() {
    AttachParentConsole();
    const auto environmentValue = [](const wchar_t* name) {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) return std::wstring{};
        std::wstring value(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
        if (copied == 0 || copied >= required) return std::wstring{};
        value.resize(copied);
        return value;
    };
    ConfigStore configStore;
    ManagedShortcutStore managedStore;
    DesktopLayout layout;
    const std::filesystem::path configDirectory =
        std::filesystem::path(configStore.ConfigPath()).parent_path();
    std::ofstream result(
        configDirectory / L"real-desktop-grid-cleanup-result.txt",
        std::ios::trunc);
    const std::wstring markerPath = environmentValue(
        L"LATTICE_REAL_DESKTOP_MARKER");
    const std::wstring secondMarkerPath = environmentValue(
        L"LATTICE_INTERNAL_DRAG_MARKER_B");
    const auto validMarker = [&](const std::wstring& path) {
        if (path.empty() || !managedStore.IsDesktopPath(path)) {
            return false;
        }
        const std::wstring name =
            std::filesystem::path(path).filename().wstring();
        return name.rfind(L".lattice-grid-restart-", 0) == 0 ||
            name.rfind(L".lattice-internal-drag-", 0) == 0;
    };
    if (!validMarker(markerPath) ||
        (!secondMarkerPath.empty() && !validMarker(secondMarkerPath))) {
        result << "MARKER_REJECTED=1\nSTATUS=FAIL\n";
        return 10;
    }

    DWORD originalFlags = 0;
    std::ifstream snapshotResult(
        configDirectory / L"real-desktop-grid-snapshot-result.txt");
    std::string line;
    bool flagsFound = false;
    while (std::getline(snapshotResult, line)) {
        if (line.rfind("FLAGS=", 0) == 0) {
            originalFlags = static_cast<DWORD>(std::stoul(line.substr(6)));
            flagsFound = true;
        }
    }
    if (!flagsFound) {
        result << "FLAGS_SNAPSHOT_MISSING=1\nSTATUS=FAIL\n";
        return 11;
    }

    const AppConfig snapshot = configStore.LoadAppConfig();
    std::vector<DesktopPosition> expected;
    expected.reserve(snapshot.desktopLayout.size());
    for (const DesktopPlacementConfig& placement : snapshot.desktopLayout) {
        expected.push_back(
            DesktopPosition{placement.path, POINT{placement.x, placement.y}});
    }
    expected.erase(
        std::remove_if(
            expected.begin(),
            expected.end(),
            [&](const DesktopPosition& position) {
                return CompareStringOrdinal(
                           position.path.c_str(), -1,
                           markerPath.c_str(), -1,
                           TRUE) == CSTR_EQUAL;
            }),
        expected.end());
    bool markerRemoved = true;
    const std::array<std::wstring, 2> markerPaths{
        markerPath, secondMarkerPath};
    for (const std::wstring& path : markerPaths) {
        if (path.empty() ||
            GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        const bool removed = DeleteFileW(path.c_str()) != FALSE;
        markerRemoved = markerRemoved && removed;
        if (removed) {
            SHChangeNotify(
                SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                path.c_str(), nullptr);
        }
    }

    std::wstring errorMessage;
    DWORD currentFlags = 0;
    const bool flagsRestored =
        layout.CaptureViewFlags(currentFlags, errorMessage) &&
        currentFlags == originalFlags;

    const bool positionsRestored = layout.RestorePositions(expected, errorMessage);
    Sleep(150);
    std::vector<DesktopPosition> after1;
    std::vector<DesktopPosition> after2;
    const bool captured1 = layout.CaptureAllPositions(after1, errorMessage);
    Sleep(100);
    const bool captured2 = layout.CaptureAllPositions(after2, errorMessage);
    const auto comparable = [](const std::vector<DesktopPosition>& positions) {
        std::vector<std::tuple<std::wstring, LONG, LONG>> values;
        values.reserve(positions.size());
        for (const DesktopPosition& position : positions) {
            std::wstring normalized = position.path;
            std::transform(
                normalized.begin(), normalized.end(), normalized.begin(),
                [](wchar_t value) {
                    return static_cast<wchar_t>(std::towlower(value));
                });
            values.emplace_back(normalized, position.point.x, position.point.y);
        }
        std::sort(values.begin(), values.end());
        return values;
    };
    const bool layoutVerified = positionsRestored && captured1 && captured2 &&
        comparable(expected) == comparable(after1) &&
        comparable(after1) == comparable(after2);
    const bool passed = markerRemoved && flagsRestored && layoutVerified;
    result << "MARKER_REMOVED=" << (markerRemoved ? 1 : 0) << "\n";
    result << "FLAGS_RESTORED=" << (flagsRestored ? 1 : 0) << "\n";
    result << "LAYOUT_VERIFIED=" << (layoutVerified ? 1 : 0) << "\n";
    result << "STATUS=" << (passed ? "PASS" : "FAIL") << "\n";
    return passed ? 0 : 12;
}

int RunSmokeLegacyStorageMigration() {
    AttachParentConsole();
    const DWORD required = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
    if (required == 0) {
        std::wcerr << L"Explicit legacy migration smoke directory is required\n";
        return 1;
    }
    std::wstring baseValue(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        baseValue.data(), required);
    if (copied == 0 || copied >= required) {
        std::wcerr << L"Legacy migration smoke directory is invalid\n";
        return 1;
    }
    baseValue.resize(copied);
    const std::filesystem::path root =
        std::filesystem::absolute(baseValue).lexically_normal() /
        (L"legacy-migration-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path configRoot = root / L"Config";
    const std::filesystem::path dataRoot = root / L"Data";
    const std::filesystem::path desktopRoot = root / L"Desktop";
    const std::filesystem::path publicRoot = root / L"PublicDesktop";
    const std::filesystem::path managedRoot =
        dataRoot / L"ManagedShortcuts" / L"legacy";
    const std::filesystem::path managedUser = managedRoot / L"用户.txt";
    const std::filesystem::path managedPublic = managedRoot / L"公共文件夹";
    const std::filesystem::path userTarget = desktopRoot / managedUser.filename();
    const std::filesystem::path publicTarget = publicRoot / managedPublic.filename();
    const std::filesystem::path hiddenTarget = desktopRoot / L"旧隐藏.txt";
    const std::filesystem::path conflictManaged = managedRoot / L"冲突.txt";
    const std::filesystem::path conflictTarget = desktopRoot / conflictManaged.filename();
    const std::filesystem::path rollbackManaged =
        dataRoot / L"ManagedShortcuts" / L"rollback" / L"回滚.txt";
    const std::filesystem::path rollbackTarget = desktopRoot / rollbackManaged.filename();
    std::error_code fileError;
    std::filesystem::create_directories(configRoot, fileError);
    std::filesystem::create_directories(managedPublic, fileError);
    std::filesystem::create_directories(desktopRoot, fileError);
    std::filesystem::create_directories(publicRoot, fileError);
    const auto cleanup = [&]() {
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
    };
    const auto fail = [&](const std::wstring& message, int code) {
        std::wcerr << message << L"\n";
        cleanup();
        return code;
    };
    if (fileError ||
        !SetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_CONFIG_DIR", configRoot.c_str()) ||
        !SetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_DATA_DIR", dataRoot.c_str()) ||
        !SetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_DESKTOP_DIR", desktopRoot.c_str()) ||
        !SetEnvironmentVariableW(
            L"DESKTOP_ORGANIZER_PUBLIC_DESKTOP_DIR", publicRoot.c_str())) {
        return fail(L"Legacy migration smoke setup failed", 121);
    }
    {
        std::ofstream user(managedUser, std::ios::binary);
        std::ofstream child(
            managedPublic / L"内容.md", std::ios::binary);
        std::ofstream hidden(hiddenTarget, std::ios::binary);
        std::ofstream conflictSource(conflictManaged, std::ios::binary);
        std::ofstream conflictDestination(conflictTarget, std::ios::binary);
        user << "legacy user bytes";
        child << "legacy public folder bytes";
        hidden << "legacy hidden bytes";
        conflictSource << std::string(840, 'S');
        conflictDestination << std::string(189, 'T');
    }
    const DWORD hiddenOriginal = GetFileAttributesW(hiddenTarget.c_str());
    if (hiddenOriginal == INVALID_FILE_ATTRIBUTES ||
        SetFileAttributesW(
            hiddenTarget.c_str(),
            hiddenOriginal | FILE_ATTRIBUTE_HIDDEN |
                FILE_ATTRIBUTE_SYSTEM) == FALSE) {
        return fail(L"Legacy hidden fixture setup failed", 122);
    }

    ConfigStore configStore;
    ManagedShortcutStore managedStore(
        dataRoot.wstring(), desktopRoot.wstring(), publicRoot.wstring());
    AppConfig config;
    config.window.x = 271;
    config.window.y = 183;
    config.window.width = 397;
    config.window.height = 421;
    config.uncategorizedItemIds = {L"legacy-user", L"legacy-hidden"};
    CategoryConfig category;
    category.id = L"legacy-category";
    category.name = L"迁移分类";
    category.itemIds = {L"legacy-public", L"legacy-conflict"};
    config.categories.push_back(category);
    ItemConfig userItem;
    userItem.id = L"legacy-user";
    userItem.path = managedUser.wstring();
    userItem.originalDesktopPath = userTarget.wstring();
    userItem.displayName = L"用户";
    ItemConfig publicItem;
    publicItem.id = L"legacy-public";
    publicItem.path = managedPublic.wstring();
    publicItem.originalDesktopPath = publicTarget.wstring();
    publicItem.displayName = L"公共文件夹";
    ItemConfig hiddenItem;
    hiddenItem.id = L"legacy-hidden";
    hiddenItem.path = hiddenTarget.wstring();
    hiddenItem.originalDesktopPath = hiddenTarget.wstring();
    hiddenItem.displayName = L"旧隐藏";
    hiddenItem.desktopVisibilityMode = 1;
    hiddenItem.desktopVisibilityOriginalFlags = static_cast<int>(
        hiddenOriginal & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
    ItemConfig conflictItem;
    conflictItem.id = L"legacy-conflict";
    conflictItem.path = conflictManaged.wstring();
    conflictItem.originalDesktopPath = conflictTarget.wstring();
    conflictItem.displayName = L"冲突";
    config.items = {userItem, publicItem, hiddenItem, conflictItem};
    if (!configStore.SaveAppConfig(config)) {
        return fail(L"Legacy migration config setup failed", 123);
    }

    HWND owner = CreateWindowExW(
        0, L"STATIC", L"Lattice legacy migration smoke owner",
        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (owner == nullptr) {
        return fail(L"Legacy migration owner creation failed", 124);
    }
    LegacyStorageMigrator migrator;
    std::wstring errorMessage;
    const std::optional<std::string> configBeforeConflict =
        ReadFileBytes(configStore.ConfigPath());
    const std::optional<std::string> conflictSourceBytes =
        ReadFileBytes(conflictManaged.wstring());
    const std::optional<std::string> conflictTargetBytes =
        ReadFileBytes(conflictTarget.wstring());
    const LegacyStorageMigrator::StartupAttempt conflictAttempt =
        migrator.AttemptForStartup(
            configStore, managedStore, owner);
    errorMessage = conflictAttempt.warning;
    const bool conflictRejected =
        conflictAttempt.required && !conflictAttempt.completed;
    const std::optional<std::string> configAfterConflict =
        ReadFileBytes(configStore.ConfigPath());
    if (!conflictRejected ||
        !configBeforeConflict.has_value() ||
        configAfterConflict != configBeforeConflict ||
        ReadFileBytes(conflictManaged.wstring()) != conflictSourceBytes ||
        ReadFileBytes(conflictTarget.wstring()) != conflictTargetBytes ||
        !std::filesystem::exists(managedUser) ||
        !std::filesystem::exists(managedPublic) ||
        std::filesystem::exists(userTarget) ||
        std::filesystem::exists(publicTarget) ||
        std::filesystem::exists(
            dataRoot / L"ManagedShortcuts" / L"move-journal.bin")) {
        DestroyWindow(owner);
        return fail(
            L"Legacy migration conflict preflight changed an endpoint or config",
            126);
    }
    std::filesystem::remove(conflictTarget, fileError);
    if (fileError) {
        DestroyWindow(owner);
        return fail(L"Legacy migration conflict cleanup failed", 127);
    }
    const bool migrated = migrator.Migrate(
        configStore, managedStore, owner, errorMessage);
    const AppConfig after = configStore.LoadAppConfig();
    const std::optional<std::string> firstConfigBytes =
        ReadFileBytes(configStore.ConfigPath());
    const bool idempotent = migrated && migrator.Migrate(
        configStore, managedStore, owner, errorMessage);
    const std::optional<std::string> secondConfigBytes =
        ReadFileBytes(configStore.ConfigPath());

    const auto findItem = [&](const std::wstring& id) {
        return std::find_if(
            after.items.begin(), after.items.end(),
            [&](const ItemConfig& item) { return item.id == id; });
    };
    const auto migratedUser = findItem(L"legacy-user");
    const auto migratedPublic = findItem(L"legacy-public");
    const auto migratedHidden = findItem(L"legacy-hidden");
    const auto migratedConflict = findItem(L"legacy-conflict");
    const DWORD hiddenAfter = GetFileAttributesW(hiddenTarget.c_str());
    const bool configStable =
        after.window.x == config.window.x &&
        after.window.y == config.window.y &&
        after.window.width == config.window.width &&
        after.window.height == config.window.height &&
        after.uncategorizedItemIds == config.uncategorizedItemIds &&
        after.categories.size() == 1 &&
        after.categories.front().id == category.id &&
        after.categories.front().itemIds == category.itemIds &&
        after.items.size() == 4 &&
        migratedUser != after.items.end() &&
        migratedUser->path == userTarget.wstring() &&
        migratedPublic != after.items.end() &&
        migratedPublic->path == publicTarget.wstring() &&
        migratedHidden != after.items.end() &&
        migratedHidden->path == hiddenTarget.wstring() &&
        migratedHidden->desktopVisibilityMode == 0 &&
        migratedConflict != after.items.end() &&
        migratedConflict->path == conflictTarget.wstring();
    const bool endpointsStable =
        !std::filesystem::exists(managedUser) &&
        !std::filesystem::exists(managedPublic) &&
        !std::filesystem::exists(conflictManaged) &&
        std::filesystem::exists(userTarget) &&
        std::filesystem::exists(publicTarget / L"内容.md") &&
        ReadFileBytes(userTarget.wstring()) ==
            std::optional<std::string>("legacy user bytes") &&
        ReadFileBytes((publicTarget / L"内容.md").wstring()) ==
            std::optional<std::string>("legacy public folder bytes") &&
        ReadFileBytes(conflictTarget.wstring()) == conflictSourceBytes &&
        hiddenAfter != INVALID_FILE_ATTRIBUTES &&
        (hiddenAfter & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ==
            (hiddenOriginal & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) &&
        !std::filesystem::exists(
            dataRoot / L"ManagedShortcuts" / L"move-journal.bin");
    const bool stableSecondRun = idempotent &&
        firstConfigBytes.has_value() && secondConfigBytes.has_value() &&
        *firstConfigBytes == *secondConfigBytes;
    if (!migrated || !configStable || !endpointsStable ||
        !stableSecondRun) {
        DestroyWindow(owner);
        return fail(
            L"Legacy migration invariant failed: " + errorMessage,
            125);
    }
    std::filesystem::create_directories(
        rollbackManaged.parent_path(), fileError);
    {
        std::ofstream rollback(rollbackManaged, std::ios::binary);
        rollback << "legacy rollback bytes";
    }
    const std::optional<std::string> rollbackBytes =
        ReadFileBytes(rollbackManaged.wstring());
    std::vector<std::pair<std::wstring, std::wstring>> rollbackDestinations;
    const bool rollbackUnexpectedlyCommitted =
        managedStore.MoveToOriginalDesktopBatch(
            {{L"legacy-rollback", rollbackManaged.wstring(),
              rollbackTarget.wstring(), true}},
            [](const auto&) { return false; },
            rollbackDestinations,
            errorMessage,
            owner);
    DestroyWindow(owner);
    if (fileError || rollbackUnexpectedlyCommitted ||
        !rollbackDestinations.empty() ||
        ReadFileBytes(rollbackManaged.wstring()) != rollbackBytes ||
        std::filesystem::exists(rollbackTarget) ||
        std::filesystem::exists(
            dataRoot / L"ManagedShortcuts" / L"move-journal.bin")) {
        return fail(
            L"Legacy migration persistence failure did not roll back cleanly",
            128);
    }

    std::filesystem::create_directories(
        conflictManaged.parent_path(), fileError);
    {
        std::ofstream conflictSource(conflictManaged, std::ios::binary);
        std::ofstream conflictDestination(conflictTarget, std::ios::binary);
        conflictSource << std::string(840, 'S');
        conflictDestination << std::string(189, 'T');
    }
    AppConfig startupConfig;
    startupConfig.window = config.window;
    CategoryConfig startupCategory = category;
    startupCategory.itemIds = {conflictItem.id};
    startupConfig.categories = {startupCategory};
    startupConfig.items = {conflictItem};
    const bool startupConfigSaved =
        !fileError && configStore.SaveAppConfig(startupConfig);
    const std::optional<std::string> startupConfigBefore =
        ReadFileBytes(configStore.ConfigPath());
    const std::optional<std::string> startupSourceBefore =
        ReadFileBytes(conflictManaged.wstring());
    const std::optional<std::string> startupTargetBefore =
        ReadFileBytes(conflictTarget.wstring());
    bool appInitialized = false;
    bool mainWindowCreated = false;
    {
        App app(GetModuleHandleW(nullptr));
        appInitialized = app.Initialize(SW_HIDE);
        const HWND testMainWindow = FindCurrentProcessMainWindow();
        mainWindowCreated = testMainWindow != nullptr;
        if (testMainWindow != nullptr) {
            DestroyWindow(testMainWindow);
        }
    }
    const bool applicationConflictSafe =
        startupConfigSaved && appInitialized && mainWindowCreated &&
        FindCurrentProcessMainWindow() == nullptr &&
        ReadFileBytes(configStore.ConfigPath()) == startupConfigBefore &&
        ReadFileBytes(conflictManaged.wstring()) == startupSourceBefore &&
        ReadFileBytes(conflictTarget.wstring()) == startupTargetBefore &&
        !std::filesystem::exists(
            dataRoot / L"ManagedShortcuts" / L"move-journal.bin");
    if (!applicationConflictSafe) {
        std::ofstream diagnostic(
            std::filesystem::path(baseValue) /
                L"app-conflict-startup-diagnostics.txt",
            std::ios::binary | std::ios::trunc);
        diagnostic
            << "STARTUP_CONFIG_SAVED=" << (startupConfigSaved ? 1 : 0) << "\n"
            << "APP_INITIALIZED=" << (appInitialized ? 1 : 0) << "\n"
            << "MAIN_WINDOW_CREATED=" << (mainWindowCreated ? 1 : 0) << "\n"
            << "MAIN_WINDOW_DESTROYED="
            << (FindCurrentProcessMainWindow() == nullptr ? 1 : 0) << "\n"
            << "CONFIG_UNCHANGED="
            << (ReadFileBytes(configStore.ConfigPath()) == startupConfigBefore ? 1 : 0) << "\n"
            << "SOURCE_UNCHANGED="
            << (ReadFileBytes(conflictManaged.wstring()) == startupSourceBefore ? 1 : 0) << "\n"
            << "TARGET_UNCHANGED="
            << (ReadFileBytes(conflictTarget.wstring()) == startupTargetBefore ? 1 : 0) << "\n"
            << "JOURNAL_ABSENT="
            << (!std::filesystem::exists(
                    dataRoot / L"ManagedShortcuts" / L"move-journal.bin")
                    ? 1 : 0)
            << "\n";
        return fail(
            L"Legacy migration conflict still blocked full application startup or changed an endpoint",
            129);
    }
    cleanup();
    std::wcout
        << L"Legacy ManagedShortcuts conflict preflight, full application startup, endpoint contents, migration, idempotent restart, and persistence rollback passed\n";
    return 0;
}

HWND FindShellPocListViewInRoot(HWND root) {
    if (root == nullptr || IsWindow(root) == FALSE) {
        return nullptr;
    }
    for (HWND defView = FindWindowExW(
             root, nullptr, L"SHELLDLL_DefView", nullptr);
         defView != nullptr;
         defView = FindWindowExW(
             root, defView, L"SHELLDLL_DefView", nullptr)) {
        const HWND listView = FindWindowExW(
            defView, nullptr, L"SysListView32", nullptr);
        if (listView != nullptr && IsWindow(listView) != FALSE) {
            return listView;
        }
    }
    return nullptr;
}

HWND FindShellPocListView() {
    const HWND progman = FindWindowW(L"Progman", nullptr);
    if (const HWND listView = FindShellPocListViewInRoot(progman)) {
        return listView;
    }
    if (progman != nullptr) {
        for (HWND worker = FindWindowExW(
                 progman, nullptr, L"WorkerW", nullptr);
             worker != nullptr;
             worker = FindWindowExW(
                 progman, worker, L"WorkerW", nullptr)) {
            if (const HWND listView = FindShellPocListViewInRoot(worker)) {
                return listView;
            }
        }
    }
    for (HWND worker = FindWindowExW(
             nullptr, nullptr, L"WorkerW", nullptr);
         worker != nullptr;
         worker = FindWindowExW(
             nullptr, worker, L"WorkerW", nullptr)) {
        if (const HWND listView = FindShellPocListViewInRoot(worker)) {
            return listView;
        }
    }
    return nullptr;
}

int RunSmokeShellDesktopBridge() {
    AttachParentConsole();
    const DWORD required = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SHELL_POC_RESULT", nullptr, 0);
    if (required == 0) {
        std::wcerr << L"Shell bridge PoC result path is missing\n";
        return 150;
    }
    std::wstring resultValue(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SHELL_POC_RESULT",
        resultValue.data(), required);
    if (copied == 0 || copied >= required) {
        std::wcerr << L"Shell bridge PoC result path is invalid\n";
        return 150;
    }
    resultValue.resize(copied);
    std::ofstream result(
        std::filesystem::path(resultValue), std::ios::trunc);
    if (!result) {
        std::wcerr << L"Shell bridge PoC result file could not be opened\n";
        return 150;
    }
    const auto setStage = [&](const char* stage) {
        result << "STAGE=" << stage << "\n";
        result.flush();
    };
    const auto fail = [&](const char* stage, HRESULT error, int exitCode) {
        result << "FAILED_STAGE=" << stage << "\n"
               << "HRESULT=" << static_cast<long long>(error) << "\n"
               << "STATUS=FAIL\n";
        result.flush();
        return exitCode;
    };

    setStage("SysListView32");
    const HWND directListView = FindShellPocListView();
    if (directListView == nullptr) {
        return fail("SysListView32", HRESULT_FROM_WIN32(ERROR_NOT_FOUND), 151);
    }
    result << "LISTVIEW_HWND="
           << static_cast<unsigned long long>(
                  reinterpret_cast<std::uintptr_t>(directListView))
           << "\n";

    setStage("CoCreateInstance(CLSID_ShellWindows)");
    Microsoft::WRL::ComPtr<IShellWindows> shellWindows;
    HRESULT error = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&shellWindows));
    if (FAILED(error) || shellWindows == nullptr) {
        return fail("CoCreateInstance(CLSID_ShellWindows)", error, 152);
    }

    setStage("IShellWindows::FindWindowSW");
    VARIANT location{};
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    VARIANT root{};
    root.vt = VT_EMPTY;
    long desktopHwnd = 0;
    Microsoft::WRL::ComPtr<IDispatch> dispatch;
    error = shellWindows->FindWindowSW(
        &location, &root, SWC_DESKTOP, &desktopHwnd,
        SWFO_NEEDDISPATCH, &dispatch);
    if (FAILED(error) || dispatch == nullptr) {
        return fail("IShellWindows::FindWindowSW", error, 153);
    }

    setStage("IServiceProvider::QueryService");
    Microsoft::WRL::ComPtr<IServiceProvider> serviceProvider;
    error = dispatch.As(&serviceProvider);
    if (FAILED(error) || serviceProvider == nullptr) {
        return fail("IDispatch::QueryInterface(IServiceProvider)", error, 154);
    }
    Microsoft::WRL::ComPtr<IShellBrowser> shellBrowser;
    error = serviceProvider->QueryService(
        SID_STopLevelBrowser, IID_PPV_ARGS(&shellBrowser));
    if (FAILED(error) || shellBrowser == nullptr) {
        return fail("IServiceProvider::QueryService", error, 155);
    }

    setStage("IShellBrowser::QueryActiveShellView");
    Microsoft::WRL::ComPtr<IShellView> shellView;
    error = shellBrowser->QueryActiveShellView(&shellView);
    if (FAILED(error) || shellView == nullptr) {
        return fail("IShellBrowser::QueryActiveShellView", error, 156);
    }
    HWND shellViewWindow = nullptr;
    error = shellView->GetWindow(&shellViewWindow);
    if (FAILED(error) || shellViewWindow == nullptr) {
        return fail("IShellView::GetWindow", error, 157);
    }

    setStage("IFolderView");
    Microsoft::WRL::ComPtr<IFolderView> folderView;
    error = shellView.As(&folderView);
    if (FAILED(error) || folderView == nullptr) {
        return fail("IShellView::QueryInterface(IFolderView)", error, 158);
    }
    int itemCount = 0;
    error = folderView->ItemCount(SVGIO_ALLVIEW, &itemCount);
    if (FAILED(error)) {
        return fail("IFolderView::ItemCount", error, 159);
    }
    const HWND folderListView = FindWindowExW(
        shellViewWindow, nullptr, L"SysListView32", nullptr);
    if (folderListView == nullptr || folderListView != directListView) {
        return fail("FolderView/ListView identity", E_UNEXPECTED, 160);
    }

    setStage("IFolderView2");
    Microsoft::WRL::ComPtr<IFolderView2> folderView2;
    error = folderView.As(&folderView2);
    if (FAILED(error) || folderView2 == nullptr) {
        return fail("IFolderView::QueryInterface(IFolderView2)", error, 161);
    }
    DWORD viewFlags = 0;
    error = folderView2->GetCurrentFolderFlags(&viewFlags);
    if (FAILED(error)) {
        return fail("IFolderView2::GetCurrentFolderFlags", error, 162);
    }
    FOLDERVIEWMODE viewMode = FVM_AUTO;
    int iconSize = 0;
    error = folderView2->GetViewModeAndIconSize(&viewMode, &iconSize);
    if (FAILED(error)) {
        return fail("IFolderView2::GetViewModeAndIconSize", error, 163);
    }
    result << "SHELL_VIEW_HWND="
           << static_cast<unsigned long long>(
                  reinterpret_cast<std::uintptr_t>(shellViewWindow))
           << "\n"
           << "ITEM_COUNT=" << itemCount << "\n"
           << "VIEW_FLAGS=" << viewFlags << "\n"
           << "VIEW_MODE=" << static_cast<int>(viewMode) << "\n"
           << "ICON_SIZE=" << iconSize << "\n"
           << "STATUS=PASS\n";
    result.flush();
    std::wcout << L"Shell desktop bridge PoC passed\n";
    return 0;
}

int RunSmokeResourceIdle(HINSTANCE instance) {
    AttachParentConsole();
    SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const DWORD required = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR", nullptr, 0);
    if (required == 0) {
        std::wcerr << L"Resource smoke directory is missing\n";
        return 139;
    }
    std::wstring baseValue(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        baseValue.data(),
        required);
    if (copied == 0 || copied >= required) {
        std::wcerr << L"Resource smoke directory is invalid\n";
        return 139;
    }
    baseValue.resize(copied);
    wchar_t visibleValue[2]{};
    const bool visibleMode = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_RESOURCE_VISIBLE",
        visibleValue,
        static_cast<DWORD>(_countof(visibleValue))) > 0 &&
        visibleValue[0] == L'1';
    const std::filesystem::path fixtureRoot =
        std::filesystem::absolute(baseValue).lexically_normal() /
        (L"resource-idle-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path desktopRoot = fixtureRoot / L"Desktop";
    std::error_code fileError;
    std::filesystem::create_directories(desktopRoot, fileError);
    if (fileError) {
        std::wcerr << L"Resource smoke fixture directory failed\n";
        return 140;
    }
    SetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_DESKTOP_DIR", desktopRoot.c_str());

    HMONITOR monitor = MonitorFromPoint(
        POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr ||
        GetMonitorInfoW(monitor, &monitorInfo) == FALSE) {
        std::wcerr << L"Resource smoke monitor lookup failed\n";
        return 141;
    }
    AppConfig config;
    config.settings.startHidden = !visibleMode;
    config.settings.restoreHiddenState = false;
    config.settings.lastVisible = visibleMode;
    config.settings.iconCacheSize = 256;
    config.window.monitorId = monitorInfo.szDevice;
    config.window.x = monitorInfo.rcWork.left + 40;
    config.window.y = monitorInfo.rcWork.top + 40;
    config.window.width = 390;
    config.window.height = 489;
    config.window.normalHeight = 489;
    for (int categoryIndex = 0; categoryIndex < 10; ++categoryIndex) {
        CategoryConfig category;
        category.id = L"resource-category-" +
            std::to_wstring(categoryIndex);
        category.name = L"资源夹具 " +
            std::to_wstring(categoryIndex + 1);
        category.storageFolder = category.name;
        category.layout = config.window;
        category.layout.width = 260;
        category.layout.height = 250;
        category.layout.normalHeight = 250;
        category.layout.x = monitorInfo.rcWork.left +
            40 + (categoryIndex % 5) * 285;
        category.layout.y = monitorInfo.rcWork.top +
            80 + (categoryIndex / 5) * 285;
        for (int itemIndex = 0; itemIndex < 8; ++itemIndex) {
            const std::wstring itemId =
                L"resource-item-" + std::to_wstring(categoryIndex) +
                L"-" + std::to_wstring(itemIndex);
            const std::filesystem::path itemPath =
                desktopRoot / (itemId + L".txt");
            {
                std::ofstream itemFile(itemPath, std::ios::binary);
                itemFile << "resource fixture";
            }
            if (!std::filesystem::exists(itemPath)) {
                std::wcerr << L"Resource smoke item creation failed\n";
                return 142;
            }
            ItemConfig item{
                itemId,
                itemPath.wstring(),
                L"资源项目 " + std::to_wstring(itemIndex + 1)};
            item.originalDesktopPath = itemPath.wstring();
            config.items.push_back(std::move(item));
            category.itemIds.push_back(itemId);
        }
        config.categories.push_back(std::move(category));
    }
    ConfigStore configStore;
    if (!configStore.SaveAppConfig(config)) {
        std::wcerr << L"Resource smoke config save failed\n";
        return 143;
    }
    App app(instance);
    if (!app.Initialize(
            visibleMode ? SW_SHOWNOACTIVATE : SW_HIDE)) {
        std::wcerr << L"Resource smoke application initialization failed\n";
        return 144;
    }
    const HWND mainWindow = FindCurrentProcessMainWindow();
    if (mainWindow == nullptr) {
        std::wcerr << L"Resource smoke main window missing\n";
        return 145;
    }
    {
        std::ofstream ready(
            std::filesystem::path(baseValue) / L"resource-ready.txt",
            std::ios::trunc);
        ready << "PID=" << GetCurrentProcessId() << "\n"
              << "VISIBLE=" << (visibleMode ? 1 : 0) << "\n"
              << "CATEGORIES=10\n"
              << "ITEMS=80\n";
    }
    const ULONGLONG deadline = GetTickCount64() + 22000;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(
                &message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                return static_cast<int>(message.wParam);
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const ULONGLONG remaining = deadline - GetTickCount64();
        MsgWaitForMultipleObjects(
            0,
            nullptr,
            FALSE,
            static_cast<DWORD>((std::min<ULONGLONG>)(remaining, 100)),
            QS_ALLINPUT);
    }
    DestroyWindow(mainWindow);
    return app.Run();
}

int RunSmokeCollapseSelectionLogic(HINSTANCE instance) {
    AttachParentConsole();
    if (WidgetWindowSmokeAccess::WallpaperDrawMode() !=
            WallpaperBackdropDrawMode::CropTopLeft ||
        DesktopSurfaceWindowSmokeAccess::WallpaperDrawMode() !=
            WallpaperBackdropDrawMode::StretchToDestination) {
        std::wcerr << L"Wallpaper draw policies are bound to the wrong window type\n";
        return 171;
    }

    WallpaperBackdropDrawGeometry collapsedGeometry{};
    const D2D1_RECT_F collapsedTarget =
        D2D1::RectF(0.0f, 0.0f, 390.0f, 32.0f);
    if (!CalculateWallpaperBackdropDrawGeometry(
            D2D1::SizeF(390.0f, 489.0f),
            collapsedTarget,
            WallpaperBackdropDrawMode::CropTopLeft,
            collapsedGeometry) ||
        collapsedGeometry.source.left != 0.0f ||
        collapsedGeometry.source.top != 0.0f ||
        collapsedGeometry.source.right != 390.0f ||
        collapsedGeometry.source.bottom != 32.0f ||
        collapsedGeometry.destination.left != 0.0f ||
        collapsedGeometry.destination.top != 0.0f ||
        collapsedGeometry.destination.right != 390.0f ||
        collapsedGeometry.destination.bottom != 32.0f) {
        std::wcerr << L"Collapsed wallpaper crop geometry regressed\n";
        return 172;
    }
    WallpaperBackdropDrawGeometry equalCropGeometry{};
    const D2D1_RECT_F equalCropTarget =
        D2D1::RectF(5.0f, 7.0f, 395.0f, 496.0f);
    if (!CalculateWallpaperBackdropDrawGeometry(
            D2D1::SizeF(390.0f, 489.0f),
            equalCropTarget,
            WallpaperBackdropDrawMode::CropTopLeft,
            equalCropGeometry) ||
        equalCropGeometry.source.right != 390.0f ||
        equalCropGeometry.source.bottom != 489.0f ||
        equalCropGeometry.destination.left != 5.0f ||
        equalCropGeometry.destination.top != 7.0f ||
        equalCropGeometry.destination.right != 395.0f ||
        equalCropGeometry.destination.bottom != 496.0f) {
        std::wcerr << L"Equal-size wallpaper crop geometry regressed\n";
        return 173;
    }
    WallpaperBackdropDrawGeometry smallCropGeometry{};
    const D2D1_RECT_F largeCropTarget =
        D2D1::RectF(5.0f, 7.0f, 395.0f, 39.0f);
    if (!CalculateWallpaperBackdropDrawGeometry(
            D2D1::SizeF(200.0f, 16.0f),
            largeCropTarget,
            WallpaperBackdropDrawMode::CropTopLeft,
            smallCropGeometry) ||
        smallCropGeometry.source.right != 200.0f ||
        smallCropGeometry.source.bottom != 16.0f ||
        smallCropGeometry.destination.left != 5.0f ||
        smallCropGeometry.destination.top != 7.0f ||
        smallCropGeometry.destination.right != 205.0f ||
        smallCropGeometry.destination.bottom != 23.0f) {
        std::wcerr << L"Wallpaper crop unexpectedly upscaled a small bitmap\n";
        return 174;
    }
    WallpaperBackdropDrawGeometry stretchGeometry{};
    const D2D1_RECT_F stretchTarget =
        D2D1::RectF(11.0f, 13.0f, 401.0f, 45.0f);
    if (!CalculateWallpaperBackdropDrawGeometry(
            D2D1::SizeF(390.0f, 489.0f),
            stretchTarget,
            WallpaperBackdropDrawMode::StretchToDestination,
            stretchGeometry) ||
        stretchGeometry.source.left != 0.0f ||
        stretchGeometry.source.top != 0.0f ||
        stretchGeometry.source.right != 390.0f ||
        stretchGeometry.source.bottom != 489.0f ||
        stretchGeometry.destination.left != 11.0f ||
        stretchGeometry.destination.top != 13.0f ||
        stretchGeometry.destination.right != 401.0f ||
        stretchGeometry.destination.bottom != 45.0f) {
        std::wcerr << L"Desktop wallpaper stretch geometry regressed\n";
        return 175;
    }

    const RECT bounds{0, 0, 200, 160};
    const RECT expected{20, 10, 81, 71};
    const std::array<std::pair<POINT, POINT>, 4> directions{{
        {{20, 10}, {80, 70}},
        {{80, 10}, {20, 70}},
        {{20, 70}, {80, 10}},
        {{80, 70}, {20, 10}},
    }};
    for (const auto& direction : directions) {
        const RECT actual =
            DesktopSurfaceWindowSmokeAccess::NormalizeMarqueeRect(
                direction.first, direction.second, bounds);
        if (!EqualRect(&actual, &expected)) {
            std::wcerr << L"Marquee direction normalization regressed\n";
            return 176;
        }
    }
    const RECT clamped =
        DesktopSurfaceWindowSmokeAccess::NormalizeMarqueeRect(
            POINT{-50, -25}, POINT{280, 210}, bounds);
    if (!EqualRect(&clamped, &bounds)) {
        std::wcerr << L"Marquee client-bound clamping regressed\n";
        return 177;
    }

    const std::vector<DesktopViewItem> items{
        DesktopViewItem{L"identity-a", L"A", {}, {10, 10}},
        DesktopViewItem{L"identity-b", L"B", {}, {100, 10}},
        DesktopViewItem{L"identity-c", L"C", {}, {190, 10}},
    };
    DesktopSurfaceWindow surface(instance);

    const int dragThreshold =
        (std::max)(1, GetSystemMetrics(SM_CXDRAG));
    DesktopSurfaceWindowSmokeAccess::ConfigureSelectionFixture(
        surface, items, {});
    const RECT firstCell =
        DesktopSurfaceWindowSmokeAccess::CellRect(surface, 0);
    const POINT firstCellGap{0, 25};
    if (!PtInRect(&firstCell, firstCellGap) ||
        !DesktopSurfaceWindowSmokeAccess::IsBlankPoint(
            surface, firstCellGap)) {
        std::wcerr
            << L"Desktop cell gap was still treated as an item\n";
        return 195;
    }
    DesktopSurfaceWindowSmokeAccess::BeginPointerGesture(
        surface, POINT{0, 100}, false);
    const POINT belowThreshold{
        dragThreshold > 1 ? dragThreshold - 1 : 0,
        100};
    if (!DesktopSurfaceWindowSmokeAccess::ContinuePointerGesture(
            surface, belowThreshold).empty() ||
        !DesktopSurfaceWindowSmokeAccess::IsMarqueePending(surface)) {
        std::wcerr << L"Blank pointer exceeded the marquee threshold too early\n";
        return 178;
    }
    DesktopSurfaceWindowSmokeAccess::ContinuePointerGesture(
        surface, POINT{dragThreshold, 100});
    if (!DesktopSurfaceWindowSmokeAccess::IsMarqueeActive(surface)) {
        std::wcerr << L"Blank pointer did not activate at the drag threshold\n";
        return 179;
    }

    DesktopSurfaceWindowSmokeAccess::ConfigureSelectionFixture(
        surface, items, {});
    DesktopSurfaceWindowSmokeAccess::BeginPointerGesture(
        surface, POINT{0, 100}, false);
    DesktopSurfaceWindowSmokeAccess::ContinuePointerGesture(
        surface, POINT{150, 0});
    DesktopSurfaceWindowSmokeAccess::ContinuePointerGesture(
        surface, POINT{50, 0});
    if (!DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-a") ||
        DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-b")) {
        std::wcerr << L"Shrunk marquee did not recompute from its baseline\n";
        return 180;
    }
    DesktopSurfaceWindowSmokeAccess::ContinuePointerGesture(
        surface, POINT{150, 0});
    if (!DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-a") ||
        !DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-b")) {
        std::wcerr << L"Expanded marquee accumulated an incorrect toggle state\n";
        return 181;
    }
    DesktopSurfaceWindowSmokeAccess::ContinuePointerGesture(
        surface, POINT{1, 99});
    if (!DesktopSurfaceWindowSmokeAccess::IsMarqueeActive(surface) ||
        !DesktopSurfaceWindowSmokeAccess::SelectedPaths(surface).empty()) {
        std::wcerr << L"Active marquee stopped updating after moving back inside the threshold\n";
        return 182;
    }

    DesktopSurfaceWindowSmokeAccess::ConfigureSelectionFixture(
        surface, items, {});
    DesktopSurfaceWindowSmokeAccess::BeginPointerGesture(
        surface, POINT{0, 25}, false);
    DesktopSurfaceWindowSmokeAccess::ContinuePointerGesture(
        surface, POINT{10, 25});
    if (!DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-a") ||
        DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-b")) {
        std::wcerr << L"Thin marquee did not select a touched desktop cell\n";
        return 183;
    }

    DesktopSurfaceWindowSmokeAccess::ConfigureSelectionFixture(
        surface, items, {L"identity-c"});
    DesktopSurfaceWindowSmokeAccess::BeginPointerGesture(
        surface, POINT{0, 100}, false);
    DesktopSurfaceWindowSmokeAccess::ContinuePointerGesture(
        surface, POINT{150, 0});
    if (!DesktopSurfaceWindowSmokeAccess::IsMarqueeActive(surface) ||
        !DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-a") ||
        !DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-b") ||
        DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-c")) {
        std::wcerr << L"Plain marquee replacement selection regressed\n";
        return 184;
    }
    DesktopSurfaceWindowSmokeAccess::CompletePointerGesture(surface);

    DesktopSurfaceWindowSmokeAccess::ConfigureSelectionFixture(
        surface, items, {L"identity-a"});
    DesktopSurfaceWindowSmokeAccess::BeginPointerGesture(
        surface, POINT{0, 100}, true);
    DesktopSurfaceWindowSmokeAccess::ContinuePointerGesture(
        surface, POINT{150, 0});
    if (DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-a") ||
        !DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-b")) {
        std::wcerr << L"Control marquee baseline XOR regressed\n";
        return 185;
    }
    DesktopSurfaceWindowSmokeAccess::CancelPointerGesture(surface);
    if (!DesktopSurfaceWindowSmokeAccess::HasNoPointerGesture(surface) ||
        !DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-b")) {
        std::wcerr << L"Capture cancellation corrupted stable selection\n";
        return 186;
    }

    DesktopSurfaceWindowSmokeAccess::ConfigureSelectionFixture(
        surface, items, {L"identity-a"});
    DesktopSurfaceWindowSmokeAccess::BeginPointerGesture(
        surface, POINT{270, 180}, false);
    DesktopSurfaceWindowSmokeAccess::CompletePointerGesture(surface);
    if (!DesktopSurfaceWindowSmokeAccess::SelectedPaths(surface).empty()) {
        std::wcerr << L"Plain blank click did not clear selection\n";
        return 187;
    }
    DesktopSurfaceWindowSmokeAccess::ConfigureSelectionFixture(
        surface, items, {L"identity-a"});
    DesktopSurfaceWindowSmokeAccess::BeginPointerGesture(
        surface, POINT{270, 180}, true);
    DesktopSurfaceWindowSmokeAccess::CompletePointerGesture(surface);
    if (!DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-a")) {
        std::wcerr << L"Control blank click did not preserve selection\n";
        return 188;
    }

    DesktopSurfaceWindowSmokeAccess::ConfigureSelectionFixture(
        surface, items, {L"identity-a", L"identity-b"});
    DesktopSurfaceWindowSmokeAccess::BeginPointerGesture(
        surface, POINT{20, 20}, true);
    DesktopSurfaceWindowSmokeAccess::CompletePointerGesture(surface);
    if (DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-a") ||
        !DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-b")) {
        std::wcerr << L"Control item toggle regressed\n";
        return 189;
    }

    DesktopSurfaceWindowSmokeAccess::ConfigureSelectionFixture(
        surface, items, {L"identity-b", L"identity-a"});
    DesktopSurfaceWindowSmokeAccess::BeginPointerGesture(
        surface, POINT{20, 20}, false);
    const std::vector<std::wstring> belowThresholdDrag =
        DesktopSurfaceWindowSmokeAccess::ContinuePointerGesture(
            surface,
            POINT{
                20 + (dragThreshold > 1
                    ? dragThreshold - 1
                    : 0),
                20});
    if (!belowThresholdDrag.empty() ||
        !DesktopSurfaceWindowSmokeAccess::IsItemPressed(surface)) {
        std::wcerr << L"Selected item drag exceeded its threshold too early\n";
        return 190;
    }
    const std::vector<std::wstring> dragPaths =
        DesktopSurfaceWindowSmokeAccess::ContinuePointerGesture(
            surface, POINT{20 + dragThreshold, 20});
    if (dragPaths !=
            std::vector<std::wstring>{
                L"identity-a", L"identity-b"} ||
        !DesktopSurfaceWindowSmokeAccess::HasNoPointerGesture(surface)) {
        std::wcerr << L"Selected group drag ordering regressed\n";
        return 191;
    }
    const std::vector<DesktopPosition> offsetPositions =
        DesktopSurfaceWindowSmokeAccess::OffsetDragPositions(
            std::vector<DesktopPosition>{
                {L"identity-a", POINT{10, 20}},
                {L"identity-b", POINT{100, 70}}},
            POINT{25, 35},
            POINT{205, 155});
    if (offsetPositions.size() != 2 ||
        offsetPositions[0].point.x != 190 ||
        offsetPositions[0].point.y != 140 ||
        offsetPositions[1].point.x != 280 ||
        offsetPositions[1].point.y != 190 ||
        offsetPositions[1].point.x - offsetPositions[0].point.x != 90 ||
        offsetPositions[1].point.y - offsetPositions[0].point.y != 50) {
        std::wcerr << L"Internal desktop drag did not preserve group geometry\n";
        return 196;
    }

    const RECT plannerBounds{0, 0, 320, 250};
    const std::vector<DesktopPosition> plannerVisible{
        {L"planner-a", POINT{20, 0}},
        {L"planner-b", POINT{20, 50}},
        {L"planner-c", POINT{100, 0}},
        {L"planner-d", POINT{100, 50}},
    };
    const auto plannedPoint = [](
        const std::vector<DesktopPosition>& positions,
        const std::wstring& identity) -> std::optional<POINT> {
        const auto item = std::find_if(
            positions.begin(), positions.end(),
            [&](const DesktopPosition& value) {
                return CompareStringOrdinal(
                           value.path.c_str(), -1,
                           identity.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
        return item == positions.end()
            ? std::nullopt
            : std::optional<POINT>(item->point);
    };
    std::vector<DesktopPosition> planned;
    if (!DesktopSurfaceWindowSmokeAccess::PlanVisibleGridDrop(
            plannerVisible,
            {plannerVisible[0]},
            POINT{30, 10},
            POINT{110, 60},
            plannerBounds,
            80,
            50,
            40,
            planned) ||
        planned.size() != 2 ||
        !plannedPoint(planned, L"planner-a").has_value() ||
        plannedPoint(planned, L"planner-a")->x != 100 ||
        plannedPoint(planned, L"planner-a")->y != 50 ||
        !plannedPoint(planned, L"planner-d").has_value() ||
        plannedPoint(planned, L"planner-d")->x != 100 ||
        plannedPoint(planned, L"planner-d")->y != 100) {
        std::wcerr << L"Visible-only occupied-cell cascade regressed";
        return 197;
    }

    planned.clear();
    if (!DesktopSurfaceWindowSmokeAccess::PlanVisibleGridDrop(
            plannerVisible,
            {plannerVisible[0]},
            POINT{30, 10},
            POINT{80, 40},
            plannerBounds,
            80,
            50,
            40,
            planned) ||
        !plannedPoint(planned, L"planner-a").has_value() ||
        plannedPoint(planned, L"planner-a")->x != 100 ||
        plannedPoint(planned, L"planner-a")->y != 50) {
        std::wcerr << L"Non-grid desktop release did not snap";
        return 198;
    }

    planned.clear();
    if (!DesktopSurfaceWindowSmokeAccess::PlanVisibleGridDrop(
            plannerVisible,
            {plannerVisible[0], plannerVisible[1]},
            POINT{30, 10},
            POINT{1000, 1000},
            plannerBounds,
            80,
            50,
            40,
            planned) ||
        !plannedPoint(planned, L"planner-a").has_value() ||
        plannedPoint(planned, L"planner-a")->x != 260 ||
        plannedPoint(planned, L"planner-a")->y != 150 ||
        !plannedPoint(planned, L"planner-b").has_value() ||
        plannedPoint(planned, L"planner-b")->x != 260 ||
        plannedPoint(planned, L"planner-b")->y != 200) {
        std::wcerr << L"Multi-item edge clamping changed group geometry";
        return 199;
    }

    const std::vector<DesktopPosition> visibleWithoutAssigned{
        {L"planner-a", POINT{20, 0}},
        {L"planner-b", POINT{20, 50}},
    };
    planned.clear();
    if (!DesktopSurfaceWindowSmokeAccess::PlanVisibleGridDrop(
            visibleWithoutAssigned,
            {visibleWithoutAssigned[0]},
            POINT{30, 10},
            POINT{110, 10},
            plannerBounds,
            80,
            50,
            40,
            planned) ||
        planned.size() != 1 ||
        !plannedPoint(planned, L"planner-a").has_value() ||
        plannedPoint(planned, L"planner-a")->x != 100 ||
        plannedPoint(planned, L"planner-a")->y != 0) {
        std::wcerr << L"Assigned items affected the visible-only layout";
        return 200;
    }

    planned.clear();
    if (!DesktopSurfaceWindowSmokeAccess::PlanVisibleGridDrop(
            plannerVisible,
            {plannerVisible[3]},
            POINT{110, 60},
            POINT{-1000, -1000},
            plannerBounds,
            80,
            50,
            40,
            planned) ||
        !plannedPoint(planned, L"planner-d").has_value() ||
        plannedPoint(planned, L"planner-d")->x != 20 ||
        plannedPoint(planned, L"planner-d")->y != 0) {
        std::wcerr << L"Reverse edge clamping regressed";
        return 201;
    }

    const std::vector<DesktopPosition> visibleWithUnrelatedDuplicateCell{
        {L"duplicate-a", POINT{20, 0}},
        {L"duplicate-b", POINT{30, 10}},
        {L"dragged-item", POINT{100, 0}},
    };
    planned.clear();
    if (!DesktopSurfaceWindowSmokeAccess::PlanVisibleGridDrop(
            visibleWithUnrelatedDuplicateCell,
            {visibleWithUnrelatedDuplicateCell[2]},
            POINT{110, 10},
            POINT{110, 60},
            plannerBounds,
            80,
            50,
            40,
            planned) ||
        planned.size() != 1 ||
        !plannedPoint(planned, L"dragged-item").has_value() ||
        plannedPoint(planned, L"dragged-item")->x != 100 ||
        plannedPoint(planned, L"dragged-item")->y != 50 ||
        plannedPoint(planned, L"duplicate-a").has_value() ||
        plannedPoint(planned, L"duplicate-b").has_value()) {
        std::wcerr
            << L"An unrelated pre-existing duplicate cell blocked dragging";
        return 202;
    }

    const std::vector<DesktopPosition> visibleWithSelectedDuplicateCell{
        {L"selected-duplicate-a", POINT{20, 0}},
        {L"selected-duplicate-b", POINT{30, 10}},
        {L"stationary-item", POINT{180, 0}},
    };
    planned.clear();
    if (!DesktopSurfaceWindowSmokeAccess::PlanVisibleGridDrop(
            visibleWithSelectedDuplicateCell,
            {visibleWithSelectedDuplicateCell[0],
             visibleWithSelectedDuplicateCell[1]},
            POINT{25, 5},
            POINT{105, 55},
            plannerBounds,
            80,
            50,
            40,
            planned) ||
        planned.size() != 2 ||
        !plannedPoint(planned, L"selected-duplicate-a").has_value() ||
        plannedPoint(planned, L"selected-duplicate-a")->x != 100 ||
        plannedPoint(planned, L"selected-duplicate-a")->y != 50 ||
        !plannedPoint(planned, L"selected-duplicate-b").has_value() ||
        plannedPoint(planned, L"selected-duplicate-b")->x != 110 ||
        plannedPoint(planned, L"selected-duplicate-b")->y != 60 ||
        plannedPoint(planned, L"stationary-item").has_value()) {
        std::wcerr
            << L"A selected pre-existing duplicate cell blocked group dragging";
        return 203;
    }

    DesktopSurfaceWindowSmokeAccess::ConfigureSelectionFixture(
        surface, items, {L"identity-a", L"identity-b"});
    DesktopSurfaceWindowSmokeAccess::ReconcileSelection(
        surface,
        std::vector<DesktopViewItem>{items[0], items[2]},
        {});
    if (!DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-a") ||
        DesktopSurfaceWindowSmokeAccess::IsSelected(
            surface, L"identity-b")) {
        std::wcerr << L"Selection identity reconciliation regressed\n";
        return 192;
    }

    wchar_t smokeRoot[32768]{};
    const DWORD smokeRootLength = GetEnvironmentVariableW(
        L"DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR",
        smokeRoot,
        ARRAYSIZE(smokeRoot));
    if (smokeRootLength == 0 ||
        smokeRootLength >= ARRAYSIZE(smokeRoot)) {
        std::wcerr << L"Selection smoke root is unavailable\n";
        return 193;
    }
    const std::filesystem::path firstPath =
        std::filesystem::path(smokeRoot) / L"multi-drag-a.txt";
    const std::filesystem::path secondPath =
        std::filesystem::path(smokeRoot) / L"multi-drag-b.txt";
    {
        std::ofstream first(firstPath, std::ios::binary);
        std::ofstream second(secondPath, std::ios::binary);
        first << "A";
        second << "B";
    }
    Microsoft::WRL::ComPtr<IDataObject> dataObject;
    const std::vector<std::wstring> filePaths{
        firstPath.wstring(), secondPath.wstring()};
    const HRESULT createResult = CreateShellDragDataObject(
        nullptr, filePaths, dataObject.GetAddressOf());
    const auto extractedPaths = ExtractShellDropPaths(dataObject.Get());
    if (FAILED(createResult) || dataObject == nullptr ||
        extractedPaths != filePaths) {
        std::ofstream diagnostic(
            std::filesystem::path(smokeRoot) / L"multi-drag-result.txt");
        diagnostic << "create=" << createResult << " count=" << extractedPaths.size() << "\n";
        Microsoft::WRL::ComPtr<IShellItemArray> array;
        const HRESULT arrayResult = dataObject == nullptr ? E_POINTER :
            SHCreateShellItemArrayFromDataObject(dataObject.Get(), IID_PPV_ARGS(array.GetAddressOf()));
        diagnostic << "array=" << arrayResult << "\n";
        for (const auto& path : extractedPaths) {
            diagnostic << Utf8Text(path) << "\n";
        }
        std::wcerr << L"Multi-file CF_HDROP round trip regressed\n";
        return 194;
    }
    FORMATETC dropFormat{
        CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM dropMedium{};
    if (FAILED(dataObject->GetData(
            &dropFormat, &dropMedium)) ||
        dropMedium.tymed != TYMED_HGLOBAL ||
        dropMedium.hGlobal == nullptr) {
        std::wcerr << L"Multi-file CF_HDROP storage is unavailable\n";
        return 195;
    }
    bool hasDoubleTerminator = false;
    const auto* dropFiles = static_cast<const DROPFILES*>(
        GlobalLock(dropMedium.hGlobal));
    const SIZE_T dropBytes = GlobalSize(dropMedium.hGlobal);
    if (dropFiles != nullptr &&
        dropFiles->pFiles <= dropBytes) {
        const SIZE_T characterCount =
            (dropBytes - dropFiles->pFiles) /
                sizeof(wchar_t);
        const auto* values =
            reinterpret_cast<const wchar_t*>(
                reinterpret_cast<const BYTE*>(dropFiles) +
                dropFiles->pFiles);
        hasDoubleTerminator =
            characterCount >= 2 &&
            values[characterCount - 1] == L'\0' &&
            values[characterCount - 2] == L'\0';
        GlobalUnlock(dropMedium.hGlobal);
    }
    ReleaseStgMedium(&dropMedium);
    if (!hasDoubleTerminator) {
        std::wcerr << L"Multi-file CF_HDROP lost its double terminator\n";
        return 196;
    }
    return 0;
}


int RunSmokeAutoOrganizeLogic() {
    using lattice::organize::Confidence;
    using lattice::organize::Decision;
    using lattice::organize::ExistingCategorySnapshot;
    using lattice::organize::GroupPlan;
    using lattice::organize::ItemSnapshot;
    using lattice::organize::Plan;
    using lattice::organize::Snapshot;

    Snapshot snapshot;
    snapshot.configRevision = 42;
    snapshot.categories = {
        ExistingCategorySnapshot{
            L"existing-dev", L"开发与运维", L"monitor-a", false},
    };

    const auto makeItem = [](
                              const wchar_t* id,
                              const wchar_t* name,
                              const wchar_t* identity,
                              const wchar_t* monitor) {
        ItemSnapshot item;
        item.id = id;
        item.displayName = name;
        item.parsingIdentity = identity;
        item.monitorId = monitor;
        return item;
    };

    ItemSnapshot existing = makeItem(
        L"existing", L"已有工具", L"identity-existing", L"monitor-a");
    existing.kind = DesktopItemKind::Shortcut;
    existing.sourceCategoryId = L"existing-dev";
    existing.sourceCategoryName = L"开发与运维";
    existing.productName = L"Existing Tool";
    snapshot.items.push_back(existing);

    ItemSnapshot uncategorized = makeItem(
        L"uncategorized-grid", L"Discord",
        L"identity-uncategorized-grid", L"monitor-a");
    uncategorized.kind = DesktopItemKind::Shortcut;
    uncategorized.sourceCategoryId = L"uncategorized";
    uncategorized.sourceCategoryName = L"未分类";
    uncategorized.productName = L"Discord";
    snapshot.items.push_back(uncategorized);

    ItemSnapshot aiFirst = makeItem(
        L"ai-first", L"ComfyUI", L"identity-ai-first", L"monitor-a");
    aiFirst.kind = DesktopItemKind::Shortcut;
    aiFirst.targetPath = L"C:\\Apps\\ComfyUI\\python.exe";
    aiFirst.productName = L"ComfyUI";
    snapshot.items.push_back(aiFirst);

    ItemSnapshot aiSecond = makeItem(
        L"ai-second", L"WebUI 启动器", L"identity-ai-second", L"monitor-a");
    aiSecond.kind = DesktopItemKind::Shortcut;
    aiSecond.targetPath = L"C:\\Apps\\WebUI\\launch.exe";
    aiSecond.description = L"Stable Diffusion WebUI";
    snapshot.items.push_back(aiSecond);

    ItemSnapshot developer = makeItem(
        L"developer", L"Visual Studio Code", L"identity-developer", L"monitor-a");
    developer.kind = DesktopItemKind::Shortcut;
    developer.targetPath = L"C:\\Program Files\\Microsoft VS Code\\Code.exe";
    developer.productName = L"Visual Studio Code";
    snapshot.items.push_back(developer);

    ItemSnapshot ambiguous = makeItem(
        L"ambiguous", L"Studio", L"identity-ambiguous", L"monitor-a");
    ambiguous.kind = DesktopItemKind::Shortcut;
    ambiguous.productName = L"Figma Premiere Studio";
    snapshot.items.push_back(ambiguous);

    ItemSnapshot unknown = makeItem(
        L"unknown", L"临时入口", L"identity-unknown", L"monitor-a");
    unknown.kind = DesktopItemKind::Shortcut;
    snapshot.items.push_back(unknown);

    ItemSnapshot projectDocument = makeItem(
        L"project-document", L"交付说明.docx",
        L"identity-project-document", L"monitor-a");
    projectDocument.kind = DesktopItemKind::File;
    projectDocument.path = L"C:\\Work\\ProjectPhoenix\\交付说明.docx";
    projectDocument.workingDirectory = L"C:\\Work\\ProjectPhoenix";
    snapshot.items.push_back(projectDocument);

    ItemSnapshot projectSheet = makeItem(
        L"project-sheet", L"预算.xlsx",
        L"identity-project-sheet", L"monitor-a");
    projectSheet.kind = DesktopItemKind::File;
    projectSheet.path = L"C:\\Work\\ProjectPhoenix\\预算.xlsx";
    projectSheet.workingDirectory = L"C:\\Work\\ProjectPhoenix";
    snapshot.items.push_back(projectSheet);

    ItemSnapshot otherScreenAi = makeItem(
        L"other-screen-ai", L"Ollama", L"identity-other-screen-ai", L"monitor-b");
    otherScreenAi.kind = DesktopItemKind::Shortcut;
    otherScreenAi.targetPath = L"D:\\Apps\\Ollama\\ollama.exe";
    otherScreenAi.productName = L"Ollama";
    snapshot.items.push_back(otherScreenAi);

    ItemSnapshot typeOnly = makeItem(
        L"type-only", L"单独报告.pdf", L"identity-type-only", L"monitor-b");
    typeOnly.kind = DesktopItemKind::File;
    typeOnly.path = L"C:\\Users\\Smoke\\Desktop\\单独报告.pdf";
    snapshot.items.push_back(typeOnly);

    ItemSnapshot duplicate = aiFirst;
    duplicate.id = L"duplicate-ai";
    snapshot.items.push_back(duplicate);

    ItemSnapshot missing = makeItem(
        L"missing", L"已删除文件", L"identity-missing", L"monitor-a");
    missing.missing = true;
    snapshot.items.push_back(missing);

    const Plan plan = lattice::organize::BuildPlan(snapshot);
    const Plan repeated = lattice::organize::BuildPlan(snapshot);
    if (plan.baseConfigRevision != 42 || plan.id.empty() ||
        plan.id != repeated.id || plan.decisions.size() != 12) {
        std::wcerr << L"Auto-organize plan identity or duplicate filtering failed\n";
        return 210;
    }

    const auto decisionFor = [&](const wchar_t* itemId) -> const Decision* {
        const auto found = std::find_if(
            plan.decisions.begin(), plan.decisions.end(),
            [&](const Decision& decision) { return decision.itemId == itemId; });
        return found == plan.decisions.end() ? nullptr : &*found;
    };
    const auto groupForName = [&](const wchar_t* name,
                                  const wchar_t* monitor) -> const GroupPlan* {
        const auto found = std::find_if(
            plan.groups.begin(), plan.groups.end(),
            [&](const GroupPlan& group) {
                return group.name == name && group.monitorId == monitor;
            });
        return found == plan.groups.end() ? nullptr : &*found;
    };

    const Decision* existingDecision = decisionFor(L"existing");
    const Decision* uncategorizedDecision =
        decisionFor(L"uncategorized-grid");
    const Decision* aiFirstDecision = decisionFor(L"ai-first");
    const Decision* aiSecondDecision = decisionFor(L"ai-second");
    const Decision* developerDecision = decisionFor(L"developer");
    const Decision* ambiguousDecision = decisionFor(L"ambiguous");
    const Decision* unknownDecision = decisionFor(L"unknown");
    const Decision* projectDocumentDecision = decisionFor(L"project-document");
    const Decision* projectSheetDecision = decisionFor(L"project-sheet");
    const Decision* otherScreenDecision = decisionFor(L"other-screen-ai");
    const Decision* typeOnlyDecision = decisionFor(L"type-only");
    const Decision* missingDecision = decisionFor(L"missing");
    if (existingDecision == nullptr || uncategorizedDecision == nullptr ||
        aiFirstDecision == nullptr ||
        aiSecondDecision == nullptr || developerDecision == nullptr ||
        ambiguousDecision == nullptr || unknownDecision == nullptr ||
        projectDocumentDecision == nullptr || projectSheetDecision == nullptr ||
        otherScreenDecision == nullptr || typeOnlyDecision == nullptr ||
        missingDecision == nullptr) {
        std::wcerr << L"Auto-organize decision coverage is incomplete\n";
        return 211;
    }

    if (existingDecision->targetCategoryId != L"existing-dev" ||
        existingDecision->selected ||
        existingDecision->confidence != Confidence::High ||
        !existingDecision->targetIsExistingCategory) {
        std::wcerr << L"Existing ownership was not preserved\n";
        return 212;
    }
    if (uncategorizedDecision->sourceCategoryId != L"uncategorized" ||
        uncategorizedDecision->sourceCategoryName != L"未分类" ||
        uncategorizedDecision->targetCategoryId != L"uncategorized" ||
        uncategorizedDecision->targetCategoryName != L"未分类" ||
        uncategorizedDecision->selected ||
        !uncategorizedDecision->targetIsExistingCategory ||
        lattice::organize::IsOwnershipAdjustment(*uncategorizedDecision)) {
        std::wcerr << L"Uncategorized grid ownership was treated as desktop\n";
        return 221;
    }
    const GroupPlan* existingGroup = groupForName(
        L"开发与运维", L"monitor-a");
    const auto keepGroup = std::find_if(
        plan.groups.begin(), plan.groups.end(), [](const GroupPlan& group) {
            return group.name == L"保持桌面";
        });
    if (existingGroup == nullptr ||
        std::find(existingGroup->itemIds.begin(), existingGroup->itemIds.end(),
                  L"existing") != existingGroup->itemIds.end() ||
        keepGroup == plan.groups.end() ||
        std::find(keepGroup->itemIds.begin(), keepGroup->itemIds.end(),
                  L"existing") != keepGroup->itemIds.end() ||
        std::find(keepGroup->itemIds.begin(), keepGroup->itemIds.end(),
                  L"uncategorized-grid") != keepGroup->itemIds.end() ||
        !lattice::organize::IsOwnershipAdjustment(*developerDecision) ||
        lattice::organize::IsOwnershipAdjustment(*existingDecision)) {
        std::wcerr << L"No-op existing ownership leaked into suggestions\n";
        return 220;
    }
    const GroupPlan* aiGroup = groupForName(L"AI 创作", L"monitor-a");
    if (aiGroup == nullptr || !aiGroup->createNewCategory ||
        aiGroup->existingCategory || aiGroup->itemIds.size() != 2 ||
        !aiFirstDecision->selected || !aiSecondDecision->selected ||
        aiFirstDecision->confidence != Confidence::High ||
        aiSecondDecision->confidence != Confidence::High) {
        std::wcerr << L"High-confidence application purpose group failed\n";
        return 213;
    }
    if (developerDecision->targetCategoryId != L"existing-dev" ||
        !developerDecision->selected ||
        !developerDecision->targetIsExistingCategory) {
        std::wcerr << L"Matching existing category was not reused\n";
        return 214;
    }
    if (ambiguousDecision->confidence != Confidence::Medium ||
        ambiguousDecision->selected ||
        unknownDecision->confidence != Confidence::Low ||
        unknownDecision->selected ||
        !unknownDecision->targetCategoryId.empty()) {
        std::wcerr << L"Ambiguous or unknown application safety failed\n";
        return 215;
    }

    const GroupPlan* projectGroup = groupForName(L"ProjectPhoenix", L"monitor-a");
    if (projectGroup == nullptr || !projectGroup->createNewCategory ||
        projectGroup->itemIds.size() != 2 ||
        !projectDocumentDecision->selected || !projectSheetDecision->selected) {
        std::wcerr << L"Project relationship grouping failed\n";
        return 216;
    }
    const GroupPlan* otherScreenGroup = groupForName(L"AI 创作", L"monitor-b");
    if (otherScreenGroup == nullptr || otherScreenGroup->createNewCategory ||
        otherScreenDecision->selected ||
        otherScreenDecision->confidence != Confidence::Low ||
        otherScreenDecision->targetCategoryId == aiFirstDecision->targetCategoryId) {
        std::wcerr << L"Cross-monitor isolation or singleton threshold failed\n";
        return 217;
    }
    if (typeOnlyDecision->confidence != Confidence::Medium ||
        typeOnlyDecision->selected ||
        typeOnlyDecision->targetCategoryName != L"办公与文档" ||
        missingDecision->selected ||
        missingDecision->confidence != Confidence::Low ||
        !missingDecision->targetCategoryId.empty()) {
        std::wcerr << L"Type fallback or missing-item safety failed\n";
        return 218;
    }
    if (aiFirstDecision->reason.empty() || developerDecision->reason.empty() ||
        projectDocumentDecision->reason.empty() || unknownDecision->reason.empty()) {
        std::wcerr << L"Explainable reasons were not generated\n";
        return 219;
    }
    return 0;
}

int RunSmokeAutoOrganizeLayout() {
    using lattice::organize::CandidateWidgetLayout;
    using lattice::organize::ExistingWidgetLayout;
    using lattice::organize::LayoutContext;
    using lattice::organize::LayoutPlan;
    using lattice::organize::MonitorLayout;
    using lattice::organize::RectI;
    using lattice::organize::WidgetPlacement;

    LayoutContext context;
    context.gap = 12;
    context.defaultWidth = 390;
    context.monitors = {
        MonitorLayout{L"monitor-a", RectI{0, 0, 1000, 800}, 96},
        MonitorLayout{L"monitor-b", RectI{-1200, 0, 0, 900}, 144},
        MonitorLayout{L"monitor-c", RectI{0, 900, 200, 1100}, 96},
    };
    context.existingWidgets = {
        ExistingWidgetLayout{
            L"existing-right", L"monitor-a", RectI{808, 12, 988, 172}},
        ExistingWidgetLayout{
            L"existing-left", L"monitor-a", RectI{12, 600, 192, 760}},
    };

    CandidateWidgetLayout first;
    first.groupId = L"first";
    first.name = L"AI 创作";
    first.monitorId = L"monitor-a";
    first.desiredHeight = 240;
    first.itemCount = 5;

    CandidateWidgetLayout second = first;
    second.groupId = L"second";
    second.name = L"开发与运维";
    second.itemCount = 4;

    CandidateWidgetLayout tall = first;
    tall.groupId = L"tall";
    tall.name = L"大型项目";
    tall.desiredHeight = 700;
    tall.itemCount = 3;

    CandidateWidgetLayout manual;
    manual.groupId = L"manual";
    manual.name = L"手动格子";
    manual.monitorId = L"monitor-b";
    manual.desiredHeight = 200;
    manual.itemCount = 2;
    manual.manuallyPositioned = true;
    manual.manualBounds = RectI{-1188, 12, -798, 212};

    CandidateWidgetLayout otherScreen;
    otherScreen.groupId = L"other-screen";
    otherScreen.name = L"另一屏";
    otherScreen.monitorId = L"monitor-b";
    otherScreen.desiredHeight = 200;
    otherScreen.itemCount = 1;

    CandidateWidgetLayout noSpace;
    noSpace.groupId = L"no-space";
    noSpace.name = L"无空间";
    noSpace.monitorId = L"monitor-c";
    noSpace.desiredHeight = 100;
    noSpace.itemCount = 1;

    CandidateWidgetLayout missingMonitor = noSpace;
    missingMonitor.groupId = L"missing-monitor";
    missingMonitor.monitorId = L"removed-monitor";

    const std::vector<CandidateWidgetLayout> candidates = {
        second, missingMonitor, otherScreen, tall, manual, first, noSpace};
    const LayoutPlan plan =
        lattice::organize::PlanWidgetLayout(context, candidates);
    const auto placementFor = [&](const wchar_t* id) -> const WidgetPlacement* {
        const auto found = std::find_if(
            plan.placements.begin(), plan.placements.end(),
            [&](const WidgetPlacement& placement) {
                return placement.groupId == id;
            });
        return found == plan.placements.end() ? nullptr : &*found;
    };

    const WidgetPlacement* firstPlacement = placementFor(L"first");
    const WidgetPlacement* secondPlacement = placementFor(L"second");
    const WidgetPlacement* tallPlacement = placementFor(L"tall");
    const WidgetPlacement* manualPlacement = placementFor(L"manual");
    const WidgetPlacement* otherScreenPlacement =
        placementFor(L"other-screen");
    const WidgetPlacement* noSpacePlacement = placementFor(L"no-space");
    const WidgetPlacement* missingPlacement =
        placementFor(L"missing-monitor");
    if (plan.placements.size() != candidates.size() ||
        firstPlacement == nullptr || secondPlacement == nullptr ||
        tallPlacement == nullptr || manualPlacement == nullptr ||
        otherScreenPlacement == nullptr || noSpacePlacement == nullptr ||
        missingPlacement == nullptr) {
        std::wcerr << L"Auto-organize layout coverage is incomplete\n";
        return 220;
    }

    const auto equals = [](const RectI& left, const RectI& right) {
        return left.left == right.left && left.top == right.top &&
            left.right == right.right && left.bottom == right.bottom;
    };
    if (lattice::organize::RecommendedWidgetWidth(context, L"monitor-a") != 180 ||
        !firstPlacement->placed ||
        !equals(firstPlacement->bounds, RectI{808, 184, 988, 424}) ||
        !secondPlacement->placed ||
        !equals(secondPlacement->bounds, RectI{808, 436, 988, 676}) ||
        !tallPlacement->placed ||
        !equals(tallPlacement->bounds, RectI{616, 12, 796, 492}) ||
        !tallPlacement->internalScrollRequired) {
        std::wcerr << L"Right-down-left placement or height cap failed\n";
        return 221;
    }
    if (!manualPlacement->placed || !manualPlacement->manuallyPositioned ||
        !equals(manualPlacement->bounds, manual.manualBounds) ||
        !otherScreenPlacement->placed ||
        otherScreenPlacement->monitorId != L"monitor-b" ||
        !lattice::organize::ContainsRectangle(
            context.monitors[1].workArea, otherScreenPlacement->bounds)) {
        std::wcerr << L"Manual placement or monitor isolation failed\n";
        return 222;
    }
    if (noSpacePlacement->placed || noSpacePlacement->reason.empty() ||
        missingPlacement->placed || missingPlacement->reason.empty()) {
        std::wcerr << L"No-space or missing-monitor fail-closed behavior failed\n";
        return 223;
    }

    std::vector<RectI> occupied;
    for (const ExistingWidgetLayout& widget : context.existingWidgets) {
        occupied.push_back(widget.bounds);
    }
    for (const WidgetPlacement& placement : plan.placements) {
        if (!placement.placed) {
            continue;
        }
        const auto monitor = std::find_if(
            context.monitors.begin(), context.monitors.end(),
            [&](const MonitorLayout& candidate) {
                return candidate.id == placement.monitorId;
            });
        if (monitor == context.monitors.end() ||
            !lattice::organize::ContainsRectangle(
                monitor->workArea, placement.bounds)) {
            std::wcerr << L"Placed widget escaped its monitor work area\n";
            return 224;
        }
        for (const RectI& previous : occupied) {
            if (lattice::organize::RectanglesOverlap(previous, placement.bounds)) {
                const bool previousOnSameMonitor = std::any_of(
                    context.existingWidgets.begin(), context.existingWidgets.end(),
                    [&](const ExistingWidgetLayout& widget) {
                        return widget.monitorId == placement.monitorId &&
                            equals(widget.bounds, previous);
                    });
                const bool placedOnSameMonitor = std::any_of(
                    plan.placements.begin(), plan.placements.end(),
                    [&](const WidgetPlacement& earlier) {
                        return earlier.placed &&
                            earlier.groupId != placement.groupId &&
                            earlier.monitorId == placement.monitorId &&
                            equals(earlier.bounds, previous);
                    });
                if (previousOnSameMonitor || placedOnSameMonitor) {
                    std::wcerr << L"Placed widgets overlap on the same monitor\n";
                    return 225;
                }
            }
        }
        occupied.push_back(placement.bounds);
    }

    std::vector<MonitorLayout> reordered = context.monitors;
    std::reverse(reordered.begin(), reordered.end());
    if (!lattice::organize::IsMonitorContextCurrent(
            plan.monitorContextSignature, reordered)) {
        std::wcerr << L"Monitor context signature depends on enumeration order\n";
        return 226;
    }
    reordered.front().dpi += 24;
    if (lattice::organize::IsMonitorContextCurrent(
            plan.monitorContextSignature, reordered)) {
        std::wcerr << L"Monitor DPI change did not invalidate preview context\n";
        return 227;
    }
    return 0;
}

int RunSmokeAutoOrganizeTransaction() {
    AppConfig current;
    CategoryConfig existing;
    existing.id = L"existing";
    existing.name = L"已有格子";
    existing.itemIds = {L"a", L"c"};
    current.categories.push_back(existing);
    current.items.push_back(ItemConfig{L"a", L"C:\\Smoke\\a.lnk", L"A"});
    current.items.push_back(ItemConfig{L"c", L"C:\\Smoke\\c.lnk", L"C"});

    CategoryConfig created;
    created.id = L"auto-new";
    created.name = L"AI 创作";
    created.storageFolder = L"auto-new";
    created.color = L"#8B5CF6";
    created.layout = WindowConfig{};
    created.layout.x = 1500;
    created.layout.y = 12;
    created.layout.width = 390;
    created.layout.height = 420;
    created.layout.normalHeight = 420;
    created.layout.monitorId = L"\\\\.\\DISPLAY1";

    AutoOrganizeApplyRequest request;
    request.transactionId = L"smoke-auto-organize-1";
    request.newCategories.push_back(created);
    AutoOrganizeMoveRequest moveA;
    moveA.item = current.items.front();
    moveA.identity = moveA.item.path;
    moveA.expectedSourceCategoryId = L"existing";
    moveA.expectedSourceIndex = 0;
    moveA.targetCategoryId = created.id;
    request.moves.push_back(moveA);
    AutoOrganizeMoveRequest moveB;
    moveB.item = ItemConfig{L"b", L"C:\\Smoke\\b.lnk", L"B"};
    moveB.identity = moveB.item.path;
    moveB.expectedSourceIndex = -1;
    moveB.targetCategoryId = created.id;
    request.moves.push_back(moveB);

    AppConfig applied;
    AutoOrganizeTransactionResult result;
    if (!lattice::organize::ApplyAutoOrganizeTransaction(
            current, request, applied, result) ||
        !result.succeeded || result.appliedChanges != 2 ||
        applied.categories.size() != 2 ||
        applied.categories[0].itemIds != std::vector<std::wstring>{L"c"} ||
        applied.categories[1].itemIds != std::vector<std::wstring>({L"a", L"b"}) ||
        applied.items.size() != 3 ||
        applied.autoOrganizeUndoHistory.size() != 1) {
        std::wcerr << L"Auto-organize atomic apply failed\n";
        return 232;
    }

    ConfigStore store;
    if (!store.SaveAppConfig(applied)) {
        std::wcerr << L"Auto-organize persistence setup failed\n";
        return 233;
    }
    AppConfig persisted = store.LoadAppConfig();
    if (persisted.autoOrganizeUndoHistory.size() != 1 ||
        persisted.autoOrganizeUndoHistory.front().changes.size() != 2 ||
        persisted.autoOrganizeUndoHistory.front().createdCategories.size() != 1 ||
        persisted.autoOrganizeUndoHistory.front().createdCategories.front().layout.x != 1500) {
        std::wcerr << L"Auto-organize undo history did not survive reload\n";
        return 234;
    }

    auto newCategory = std::find_if(
        persisted.categories.begin(), persisted.categories.end(),
        [](const CategoryConfig& value) { return value.id == L"auto-new"; });
    newCategory->itemIds.erase(newCategory->itemIds.begin());
    persisted.uncategorizedItemIds.push_back(L"a");
    persisted.items.push_back(ItemConfig{L"manual", L"C:\\Smoke\\manual.txt", L"Manual"});
    newCategory->itemIds.push_back(L"manual");
    AppConfig undone;
    if (!lattice::organize::UndoLastAutoOrganizeTransaction(
            persisted, undone, result) || !result.succeeded ||
        result.appliedChanges != 1 || result.preservedChanges < 1 ||
        undone.autoOrganizeUndoHistory.size() != 0 ||
        std::find(undone.uncategorizedItemIds.begin(),
            undone.uncategorizedItemIds.end(), L"a") ==
            undone.uncategorizedItemIds.end() ||
        std::any_of(undone.items.begin(), undone.items.end(),
            [](const ItemConfig& item) { return item.id == L"b"; }) ||
        std::none_of(undone.categories.begin(), undone.categories.end(),
            [](const CategoryConfig& value) {
                return value.id == L"auto-new" &&
                    value.itemIds == std::vector<std::wstring>{L"manual"};
            })) {
        std::wcerr << L"Auto-organize incremental undo overwrote manual changes\n";
        return 235;
    }

    AppConfig capped;
    CategoryConfig destination;
    destination.id = L"destination";
    destination.name = L"目标";
    capped.categories.push_back(destination);
    for (int index = 0; index < 11; ++index) {
        AutoOrganizeApplyRequest repeated;
        repeated.transactionId = L"history-" + std::to_wstring(index);
        AutoOrganizeMoveRequest move;
        move.item.id = L"history-item-" + std::to_wstring(index);
        move.item.path = L"C:\\Smoke\\history-" + std::to_wstring(index) + L".lnk";
        move.item.displayName = move.item.id;
        move.identity = move.item.path;
        move.expectedSourceIndex = -1;
        move.targetCategoryId = L"destination";
        repeated.moves.push_back(move);
        AppConfig next;
        if (!lattice::organize::ApplyAutoOrganizeTransaction(
                capped, repeated, next, result)) {
            std::wcerr << L"Auto-organize bounded history setup failed\n";
            return 236;
        }
        capped = std::move(next);
    }
    if (capped.autoOrganizeUndoHistory.size() != 10 ||
        capped.autoOrganizeUndoHistory.front().transactionId != L"history-1" ||
        capped.autoOrganizeUndoHistory.back().transactionId != L"history-10") {
        std::wcerr << L"Auto-organize undo history is not bounded to ten\n";
        return 237;
    }

    AutoOrganizeApplyRequest conflict = request;
    conflict.transactionId = L"conflict";
    conflict.moves.push_back(conflict.moves.front());
    AppConfig rejected;
    if (lattice::organize::ApplyAutoOrganizeTransaction(
            current, conflict, rejected, result) || !result.conflict) {
        std::wcerr << L"Auto-organize conflict did not fail closed\n";
        return 238;
    }

    if (!store.SaveAppConfig(current) ||
        !store.ApplyAutoOrganizeAsync(request, nullptr, WM_APP + 72, 9001) ||
        !ConfigStore::DrainPendingWrites(5000)) {
        std::wcerr << L"Auto-organize async apply did not drain\n";
        return 245;
    }
    const AppConfig asyncApplied = store.LoadAppConfig();
    if (asyncApplied.autoOrganizeUndoHistory.size() != 1 ||
        asyncApplied.categories.size() != 2 ||
        asyncApplied.categories.back().itemIds !=
            std::vector<std::wstring>({L"a", L"b"})) {
        std::wcerr << L"Auto-organize async apply was not one persisted transaction\n";
        return 246;
    }
    if (!store.UndoAutoOrganizeAsync(nullptr, WM_APP + 72, 9002) ||
        !ConfigStore::DrainPendingWrites(5000)) {
        std::wcerr << L"Auto-organize async undo did not drain\n";
        return 247;
    }
    const AppConfig asyncUndone = store.LoadAppConfig();
    if (!asyncUndone.autoOrganizeUndoHistory.empty() ||
        asyncUndone.categories.size() != 1 ||
        asyncUndone.categories.front().itemIds !=
            std::vector<std::wstring>({L"a", L"c"}) ||
        std::any_of(
            asyncUndone.items.begin(), asyncUndone.items.end(),
            [](const ItemConfig& item) { return item.id == L"b"; })) {
        std::wcerr << L"Auto-organize async undo did not restore the safe prior state\n";
        return 248;
    }

    const std::optional<std::string> beforeWriteFailure =
        ReadFileBytes(store.ConfigPath());
    SetEnvironmentVariableW(L"LATTICE_SMOKE_FAIL_CONFIG_WRITE", L"1");
    const bool failureQueued = store.ApplyAutoOrganizeAsync(
        request, nullptr, WM_APP + 72, 9003);
    const bool failureDrained = ConfigStore::DrainPendingWrites(5000);
    SetEnvironmentVariableW(L"LATTICE_SMOKE_FAIL_CONFIG_WRITE", nullptr);
    const std::optional<std::string> afterWriteFailure =
        ReadFileBytes(store.ConfigPath());
    if (!failureQueued || failureDrained || !beforeWriteFailure.has_value() ||
        beforeWriteFailure != afterWriteFailure ||
        !store.LoadAppConfig().autoOrganizeUndoHistory.empty()) {
        std::wcerr << L"Auto-organize write failure did not preserve the old file\n";
        return 254;
    }

    for (int index = 0; index < 3; ++index) {
        AutoOrganizeApplyRequest next;
        next.transactionId = L"restart-sequence-" + std::to_wstring(index);
        AutoOrganizeMoveRequest move;
        move.item.id = L"restart-item-" + std::to_wstring(index);
        move.item.path = L"C:\\Smoke\\restart-" +
            std::to_wstring(index) + L".txt";
        move.item.displayName = move.item.id;
        move.identity = move.item.path;
        move.expectedSourceIndex = -1;
        move.targetCategoryId = L"existing";
        next.moves.push_back(std::move(move));
        if (!store.ApplyAutoOrganizeAsync(
                next, nullptr, WM_APP + 72, 9100 + index) ||
            !ConfigStore::DrainPendingWrites(5000)) {
            std::wcerr << L"Auto-organize restart sequence apply failed\n";
            return 255;
        }
    }
    ConfigStore reopenedStore;
    if (reopenedStore.LoadAppConfig().autoOrganizeUndoHistory.size() != 3) {
        std::wcerr << L"Auto-organize history did not survive store restart\n";
        return 256;
    }
    for (int index = 2; index >= 0; --index) {
        if (!reopenedStore.UndoAutoOrganizeAsync(
                nullptr, WM_APP + 72, 9200 + index) ||
            !ConfigStore::DrainPendingWrites(5000) ||
            reopenedStore.LoadAppConfig().autoOrganizeUndoHistory.size() !=
                static_cast<std::size_t>(index)) {
            std::wcerr << L"Auto-organize reverse restart undo failed\n";
            return 257;
        }
    }
    const AppConfig restartUndone = reopenedStore.LoadAppConfig();
    if (restartUndone.categories.size() != asyncUndone.categories.size() ||
        restartUndone.categories.front().itemIds !=
            asyncUndone.categories.front().itemIds ||
        restartUndone.items.size() != asyncUndone.items.size()) {
        std::wcerr << L"Auto-organize reverse restart undo changed the baseline\n";
        return 258;
    }
    return 0;
}

int RunSmokeAutoOrganizePreview(HINSTANCE instance) {
    AutoOrganizePreviewInput input;
    input.snapshot.configRevision = 73;
    input.layoutContext.gap = 12;
    input.layoutContext.defaultWidth = 390;
    input.layoutContext.monitors = {
        lattice::organize::MonitorLayout{
            L"\\\\.\\DISPLAY1", lattice::organize::RectI{0, 0, 2560, 1400}, 144},
    };
    input.layoutContext.existingWidgets = {
        lattice::organize::ExistingWidgetLayout{
            L"existing-dev", L"\\\\.\\DISPLAY1",
            lattice::organize::RectI{2158, 12, 2548, 480}},
    };
    input.snapshot.categories = {
        lattice::organize::ExistingCategorySnapshot{
            L"existing-dev", L"开发与运维", L"\\\\.\\DISPLAY1", false},
        lattice::organize::ExistingCategorySnapshot{
            L"existing-browser", L"浏览器与网络", L"\\\\.\\DISPLAY1", false},
    };
    const auto makeItem = [](const wchar_t* id, const wchar_t* name,
                             const wchar_t* path, const wchar_t* target) {
        lattice::organize::ItemSnapshot item;
        item.id = id;
        item.parsingIdentity = path;
        item.displayName = name;
        item.path = path;
        item.targetPath = target;
        item.monitorId = L"\\\\.\\DISPLAY1";
        item.kind = DesktopItemKind::Shortcut;
        return item;
    };
    input.snapshot.items.push_back(makeItem(
        L"comfy", L"ComfyUI", L"C:\\Smoke\\ComfyUI.lnk",
        L"C:\\Apps\\ComfyUI\\python.exe"));
    input.snapshot.items.push_back(makeItem(
        L"webui", L"WebUI 启动器", L"C:\\Smoke\\WebUI.lnk",
        L"C:\\Apps\\Stable Diffusion WebUI\\launch.exe"));
    input.snapshot.items.push_back(makeItem(
        L"code", L"Visual Studio Code", L"C:\\Smoke\\Code.lnk",
        L"C:\\Program Files\\Microsoft VS Code\\Code.exe"));
    input.snapshot.items.push_back(makeItem(
        L"chrome", L"Google Chrome", L"C:\\Smoke\\Chrome.lnk",
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe"));
    auto existing = makeItem(
        L"existing", L"PowerShell", L"C:\\Smoke\\PowerShell.lnk",
        L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    existing.sourceCategoryId = L"existing-dev";
    existing.sourceCategoryName = L"开发与运维";
    input.snapshot.items.push_back(std::move(existing));
    auto uncategorized = makeItem(
        L"uncategorized", L"Discord", L"C:\\Smoke\\Discord.lnk",
        L"C:\\Apps\\Discord\\Discord.exe");
    uncategorized.sourceCategoryId = L"uncategorized";
    uncategorized.sourceCategoryName = L"未分类";
    input.snapshot.items.push_back(std::move(uncategorized));
    auto ambiguous = makeItem(
        L"ambiguous", L"Studio", L"C:\\Smoke\\Studio.lnk", L"");
    ambiguous.description = L"Figma Premiere Studio";
    input.snapshot.items.push_back(std::move(ambiguous));

    AutoOrganizePreviewInput currentInput = input;
    bool applyInvoked = false;
    bool undoInvoked = false;
    AutoOrganizePreviewWindow window(
        instance,
        nullptr,
        [&currentInput]() { return currentInput; },
        [&](const lattice::organize::Plan&,
            const lattice::organize::LayoutPlan&,
            HWND) { applyInvoked = true; },
        [&](HWND) { undoInvoked = true; });
    if (!window.Create()) {
        std::wcerr << L"Auto-organize preview window could not be created\n";
        return 228;
    }
    ShowWindow(window.Window(), SW_SHOWNORMAL);
    SetWindowPos(
        window.Window(), HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window.Window());
    const ULONGLONG deadline = GetTickCount64() + 3000;
    while (AutoOrganizePreviewWindowSmokeAccess::Scanning(window) &&
           GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(5);
    }
    UpdateWindow(window.Window());
    AutoOrganizePreviewWindowSmokeAccess::RenderNow(window);
    const LONG_PTR extendedStyle = GetWindowLongPtrW(window.Window(), GWL_EXSTYLE);
    if (!AutoOrganizePreviewWindowSmokeAccess::Ready(window) ||
        AutoOrganizePreviewWindowSmokeAccess::DecisionCount(window) != 7 ||
        AutoOrganizePreviewWindowSmokeAccess::PlacementCount(window) != 1 ||
        AutoOrganizePreviewWindowSmokeAccess::HitCount(window) < 15 ||
        !AutoOrganizePreviewWindowSmokeAccess::HasFullCardDropTarget(window) ||
        (extendedStyle & WS_EX_TOPMOST) != 0 || applyInvoked) {
        std::wcerr << L"Auto-organize preview state, controls, or window layer failed\n";
        window.Close();
        return 229;
    }
    SetWindowPos(
        window.Window(), HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    wchar_t modulePath[32768]{};
    const DWORD moduleLength = GetModuleFileNameW(
        nullptr, modulePath, ARRAYSIZE(modulePath));
    const std::filesystem::path projectRoot = moduleLength == 0
        ? std::filesystem::current_path()
        : std::filesystem::path(modulePath).parent_path().parent_path().parent_path();
    const std::filesystem::path artifactDirectory =
        projectRoot / L".workspace" / L"artifacts";
    std::error_code directoryError;
    std::filesystem::create_directories(artifactDirectory, directoryError);
    const std::filesystem::path capturePath =
        artifactDirectory / L"auto-organize-preview-150dpi.bmp";
    const std::filesystem::path hoverCapturePath =
        artifactDirectory / L"auto-organize-preview-hover-150dpi.bmp";
    RECT captureBounds{};
    GetClientRect(window.Window(), &captureBounds);
    POINT captureOrigin{captureBounds.left, captureBounds.top};
    ClientToScreen(window.Window(), &captureOrigin);
    OffsetRect(&captureBounds, captureOrigin.x, captureOrigin.y);
    const POINT captureProbe{
        (captureBounds.left + captureBounds.right) / 2,
        (captureBounds.top + captureBounds.bottom) / 2};
    const HWND probeWindow = WindowFromPoint(captureProbe);
    if (probeWindow == nullptr ||
        GetAncestor(probeWindow, GA_ROOT) != window.Window()) {
        std::wcerr << L"Auto-organize preview is covered by another window\n";
        window.Close();
        return 261;
    }
    std::vector<std::uint32_t> capturePixels;
    DwmFlush();
    const bool pixelsCaptured =
        CaptureScreenPixels(captureBounds, capturePixels);
    const bool pixelsSaved = pixelsCaptured &&
        SaveCapturedPixelsBmp(captureBounds, capturePixels, capturePath);
    if (directoryError || !pixelsCaptured || !pixelsSaved) {
        std::wcerr << L"Auto-organize preview capture diagnostics: bounds="
                   << captureBounds.left << L"," << captureBounds.top << L","
                   << captureBounds.right << L"," << captureBounds.bottom
                   << L" directoryError=" << directoryError.value()
                   << L" captured=" << pixelsCaptured
                   << L" pixels=" << capturePixels.size()
                   << L" saved=" << pixelsSaved
                   << L" lastError=" << GetLastError() << L"\n";
        std::wcerr << L"Auto-organize preview capture failed\n";
        window.Close();
        return 230;
    }
    RECT hoverBounds{};
    std::vector<std::uint32_t> hoverBefore;
    std::vector<std::uint32_t> hoverAfter;
    if (AutoOrganizePreviewWindowSmokeAccess::SuggestionCount(window) != 5 ||
        AutoOrganizePreviewWindowSmokeAccess::DecisionAppearsInAnyGroup(
            window, L"existing") ||
        AutoOrganizePreviewWindowSmokeAccess::DecisionAppearsInAnyGroup(
            window, L"uncategorized") ||
        AutoOrganizePreviewWindowSmokeAccess::SourceNameForItem(
            window, L"uncategorized") != L"未分类" ||
        !AutoOrganizePreviewWindowSmokeAccess::RegenerateScreenBounds(
            window, hoverBounds) ||
        !CaptureScreenPixels(hoverBounds, hoverBefore) ||
        !AutoOrganizePreviewWindowSmokeAccess::HoverRegenerate(window)) {
        std::wcerr << L"Auto-organize suggestion filtering or hover setup failed\n";
        window.Close();
        return 251;
    }
    DwmFlush();
    if (!CaptureScreenPixels(hoverBounds, hoverAfter) ||
        hoverBefore.size() != hoverAfter.size() ||
        std::equal(hoverBefore.begin(), hoverBefore.end(), hoverAfter.begin())) {
        std::wcerr << L"Auto-organize hover produced no visible pixel change\n";
        window.Close();
        return 252;
    }
    std::vector<std::uint32_t> hoverCapturePixels;
    if (!CaptureScreenPixels(captureBounds, hoverCapturePixels) ||
        !SaveCapturedPixelsBmp(
            captureBounds, hoverCapturePixels, hoverCapturePath)) {
        std::wcerr << L"Auto-organize hover capture failed\n";
        window.Close();
        return 253;
    }
    SetWindowPos(
        window.Window(), HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    if (!AutoOrganizePreviewWindowSmokeAccess::MoveDecisionToNamedGroup(
            window, 0, L"开发与运维") ||
        AutoOrganizePreviewWindowSmokeAccess::TargetCategory(window, 0) !=
            L"开发与运维") {
        std::wcerr << L"Auto-organize item drag target did not update\n";
        window.Close();
        return 231;
    }
    AutoOrganizePreviewWindowSmokeAccess::KeepOnDesktop(window, 0);
    if (!AutoOrganizePreviewWindowSmokeAccess::TargetCategory(window, 0).empty()) {
        std::wcerr << L"Auto-organize keep-desktop destination did not clear adjustment\n";
        window.Close();
        return 239;
    }
    if (!AutoOrganizePreviewWindowSmokeAccess::MoveDecisionToNamedGroup(
            window, 0, L"AI 创作")) {
        std::wcerr << L"Auto-organize item could not return to candidate group\n";
        window.Close();
        return 240;
    }
    AutoOrganizePreviewWindowSmokeAccess::ScrollGroups(window);
    if (AutoOrganizePreviewWindowSmokeAccess::GroupScrollOffset(window) <= 0) {
        std::wcerr << L"Auto-organize group virtualization did not scroll\n";
        window.Close();
        return 241;
    }
    currentInput.snapshot.items.push_back(makeItem(
        L"unrelated", L"Unrelated", L"C:\\Smoke\\Unrelated.bin", L""));
    AutoOrganizePreviewWindowSmokeAccess::MarkDesktopChanged(window);
    AutoOrganizePreviewWindowSmokeAccess::RenderNow(window);
    if (!AutoOrganizePreviewWindowSmokeAccess::Changed(window) ||
        AutoOrganizePreviewWindowSmokeAccess::ChangeBlocksApply(window) ||
        !AutoOrganizePreviewWindowSmokeAccess::ActivateApply(window) ||
        !applyInvoked) {
        std::wcerr << L"Unrelated desktop addition incorrectly blocked apply\n";
        window.Close();
        return 242;
    }
    window.CompleteApply(true, L"smoke applied");
    AutoOrganizePreviewWindowSmokeAccess::ActivateOverlayAction(window);
    if (!undoInvoked || !AutoOrganizePreviewWindowSmokeAccess::Undoing(window)) {
        std::wcerr << L"Auto-organize success state did not invoke undo\n";
        window.Close();
        return 243;
    }
    window.CompleteUndo(true, false, L"smoke undone");
    if (!AutoOrganizePreviewWindowSmokeAccess::UndoSucceeded(window)) {
        std::wcerr << L"Auto-organize undo completion state failed\n";
        window.Close();
        return 244;
    }
    AutoOrganizePreviewWindowSmokeAccess::StartScan(window);
    const ULONGLONG rescanDeadline = GetTickCount64() + 3000;
    while (AutoOrganizePreviewWindowSmokeAccess::Scanning(window) &&
           GetTickCount64() < rescanDeadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(5);
    }
    if (!AutoOrganizePreviewWindowSmokeAccess::Ready(window)) {
        std::wcerr << L"Auto-organize preview rescan did not become ready\n";
        window.Close();
        return 249;
    }
    currentInput.snapshot.items.front().parsingIdentity =
        L"C:\\Smoke\\ComfyUI-renamed.lnk";
    AutoOrganizePreviewWindowSmokeAccess::MarkDesktopChanged(window);
    if (!AutoOrganizePreviewWindowSmokeAccess::Changed(window) ||
        !AutoOrganizePreviewWindowSmokeAccess::ChangeBlocksApply(window)) {
        std::wcerr << L"Related identity change did not block the whole plan\n";
        window.Close();
        return 250;
    }
    std::wcout << L"AUTO_ORGANIZE_PREVIEW_CAPTURE=" << capturePath.wstring() << L"\n";
    std::wcout << L"AUTO_ORGANIZE_PREVIEW_HOVER_CAPTURE="
               << hoverCapturePath.wstring() << L"\n";
    window.Close();
    return 0;
}

bool HasArgument(PWSTR commandLine, const wchar_t* target) {
    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(commandLine, &argc);
    if (argv == nullptr) {
        return false;
    }

    bool found = false;
    for (int i = 0; i < argc; ++i) {
        if (std::wstring(argv[i]) == target) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

}  // namespace

std::optional<int> RunSmokeOrPreviewCommand(
    HINSTANCE instance,
    PWSTR commandLine) {
    if (HasArgument(commandLine, L"--smoke-scan")) {
        return RunSmokeScan();
    }
    if (HasArgument(commandLine, L"--smoke-config")) {
        return RunSmokeConfig();
    }
    if (HasArgument(commandLine, L"--smoke-layout")) {
        return RunSmokeDesktopLayout();
    }
    if (HasArgument(commandLine, L"--smoke-auto-organize-logic")) {
        return RunSmokeAutoOrganizeLogic();
    }
    if (HasArgument(commandLine, L"--smoke-auto-organize-layout")) {
        return RunSmokeAutoOrganizeLayout();
    }
    if (HasArgument(commandLine, L"--smoke-auto-organize-transaction")) {
        return RunSmokeAutoOrganizeTransaction();
    }
    if (HasArgument(commandLine, L"--smoke-auto-organize-preview")) {
        return RunSmokeAutoOrganizePreview(instance);
    }
    if (HasArgument(commandLine, L"--smoke-shell-new")) {
        return RunSmokeShellNewMenu();
    }
    if (HasArgument(commandLine, L"--smoke-shell-rename")) {
        return RunSmokeShellRename();
    }
    if (HasArgument(commandLine, L"--smoke-managed-items")) {
        return RunSmokeManagedItems();
    }
    if (HasArgument(commandLine, L"--smoke-legacy-storage-migration")) {
        return RunSmokeLegacyStorageMigration();
    }
    if (HasArgument(commandLine, L"--smoke-category-storage")) {
        return RunSmokeCategoryStorage();
    }
    if (HasArgument(commandLine, L"--smoke-shortcut-overlay")) {
        return RunSmokeShortcutOverlay(instance);
    }
    if (HasArgument(commandLine, L"--smoke-widget-alignment")) {
        return RunSmokeWidgetAlignment(instance);
    }
    if (HasArgument(commandLine, L"--smoke-widget-desktop-layer")) {
        return RunSmokeWidgetDesktopLayer(instance);
    }
    if (HasArgument(commandLine, L"--smoke-desktop-targeted")) {
        const int fidelityResult = RunSmokeDesktopIconFidelity(instance);
        return fidelityResult == 0
            ? RunSmokeDesktopDisplayTakeover(instance)
            : fidelityResult;
    }
    if (HasArgument(
            commandLine,
            L"--smoke-desktop-icon-fidelity")) {
        return RunSmokeDesktopIconFidelity(instance);
    }
    if (HasArgument(commandLine, L"--smoke-shell-desktop-bridge")) {
        return RunSmokeShellDesktopBridge();
    }
    if (HasArgument(commandLine, L"--smoke-resource-idle")) {
        return RunSmokeResourceIdle(instance);
    }
    if (HasArgument(
            commandLine,
            L"--smoke-collapse-selection-logic")) {
        return RunSmokeCollapseSelectionLogic(instance);
    }
    if (HasArgument(
            commandLine,
            L"--smoke-desktop-display-takeover")) {
        return RunSmokeDesktopDisplayTakeover(instance);
    }
    if (HasArgument(
            commandLine,
            L"--smoke-desktop-internal-drag")) {
        return RunSmokeDesktopInternalDrag(instance);
    }
    if (HasArgument(
            commandLine,
            L"--smoke-desktop-current-config-drag")) {
        return RunSmokeDesktopCurrentConfigDrag(instance);
    }
    if (HasArgument(
            commandLine,
            L"--smoke-desktop-native-drag-oracle")) {
        return RunSmokeDesktopNativeDragOracle();
    }
    if (HasArgument(commandLine, L"--smoke-widget-interaction")) {
        return RunSmokeWidgetInteraction(instance);
    }
    if (HasArgument(commandLine, L"--smoke-widget-normal-exit")) {
        return RunSmokeWidgetNormalExit(instance);
    }
    if (HasArgument(commandLine, L"--smoke-widget-drop-latency")) {
        return RunSmokeWidgetDropLatency(instance);
    }
    if (HasArgument(commandLine, L"--smoke-widget-drop-placement")) {
        return RunSmokeWidgetDropPlacement(instance);
    }
    if (HasArgument(commandLine, L"--smoke-update-dialog")) {
        return RunSmokeUpdateAndDialog();
    }
    if (HasArgument(commandLine, L"--smoke-real-desktop-grid-snapshot")) {
        return RunSmokeRealDesktopGridSnapshot();
    }
    if (HasArgument(commandLine, L"--smoke-real-desktop-takeover-invariants")) {
        return RunSmokeRealDesktopTakeoverInvariants(instance);
    }
    if (HasArgument(commandLine, L"--smoke-real-desktop-grid-cleanup")) {
        return RunSmokeRealDesktopGridCleanup();
    }
    if (HasArgument(commandLine, L"--dialog-preview")) {
        return RunDialogPreview(instance);
    }
    if (HasArgument(commandLine, L"--input-dialog-preview")) {
        return RunInputDialogPreview(instance);
    }
    if (HasArgument(commandLine, L"--settings-dialog-preview")) {
        return RunSettingsDialogPreview(instance);
    }
    return std::nullopt;
}
