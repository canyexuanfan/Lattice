#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "config/ConfigStore.h"
#include "desktop/ManagedShortcutStore.h"
#include "desktop/DesktopItem.h"
#include "rendering/D2DContext.h"
#include "rendering/IconCache.h"
#include "rendering/WallpaperBackdrop.h"
#include "shell/ShellLauncher.h"
#include "ui/IconGrid.h"

constexpr UINT kOrganizerConfigChangedMessage = WM_APP + 12;
constexpr UINT kWidgetRefreshMessage = WM_APP + 13;
constexpr UINT kWidgetHostCommandMessage = WM_APP + 15;
constexpr UINT kWidgetActivateCategoryMessage = WM_APP + 16;
constexpr UINT kOrganizerConfigSyncMessage = WM_APP + 19;
constexpr UINT kWidgetShellDropCommitMessage = WM_APP + 20;

struct WidgetWindowSmokeAccess;
struct DesktopCollectionItemResult;

enum class WidgetHostCommand : UINT {
    CreateCategory = 1,
    HideAll,
    ToggleAllLocked,
    RefreshAll,
    ToggleStartup,
    ShowSettings,
    ExportConfig,
    ImportConfig,
    ImportUnassigned,
    ExportCategory,
    ImportCategory,
    MoveCategoryUp,
    MoveCategoryDown,
    ExitApplication,
    CheckForUpdates,
    AutoOrganize,
    UndoAutoOrganize,
};

class WidgetWindow {
public:
    WidgetWindow(HINSTANCE instance, HWND owner, std::wstring categoryId, int spawnOffset);
    ~WidgetWindow();

    bool Create();
    void Show(int showCommand);
    void SetVisible(bool visible);
    void SetLocked(bool locked);
    void Close();
    void RefreshFromConfig();
    void RefreshIconCache();
    bool IsOpen() const noexcept { return hwnd_ != nullptr && IsWindow(hwnd_) != FALSE; }
    bool IsVisible() const noexcept { return IsOpen() && IsWindowVisible(hwnd_) != FALSE; }
    bool IsForCategory(const std::wstring& categoryId) const noexcept { return categoryId_ == categoryId; }
    bool HasShellDropTarget() const noexcept { return shellDropTargetRegistered_; }
    bool FlushPendingStateForExit() { return FlushPendingInteractionSave(); }

private:
    friend struct WidgetWindowSmokeAccess;
#ifndef NDEBUG
    HRESULT lastRenderResult_ = E_PENDING;
    unsigned int renderCount_ = 0;
#endif

    static constexpr WallpaperBackdropDrawMode
        kWallpaperDrawMode =
            WallpaperBackdropDrawMode::CropTopLeft;

    struct DesktopDropPosition {
        std::wstring path;
        POINT point{};
    };

    enum class PointerSelectionGesture {
        None,
        ItemPressed,
        MarqueePending,
        MarqueeActive,
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool LoadConfig();
    bool IsDesktopHosted() const noexcept;
    void MaintainDesktopLayer();
    void PlaceAboveSiblingWidgets();
    bool SetWindowBoundsFromScreen(int x, int y, int width, int height, UINT flags);
    void EnsureWindowVisible();
    void LoadItems();
    void Render();
    void RefreshWallpaperBackdrop();
    void ScheduleWallpaperBackdropRefresh();
    void SaveLayout();
    bool FlushPendingInteractionSave();
    void ScheduleInteractionSave();
    bool AddDroppedPaths(
        const std::vector<std::wstring>& paths,
        const POINT* dropScreenPoint = nullptr,
        bool showError = true,
        const int* insertionIndexOverride = nullptr,
        const std::vector<DesktopDropPosition>* desktopPositions = nullptr);
    bool QueueDroppedPaths(
        const std::vector<std::wstring>& paths,
        POINT screenPoint,
        bool showError = true,
        const std::vector<DesktopDropPosition>* desktopPositions = nullptr);
    bool IsShellDropBusy() const noexcept {
        return shellDropProjectionActive_ ||
               shellDropQueued_ ||
               shellDropCommitActive_;
    }
    void UpdateShellDropPreview(
        const std::vector<std::wstring>& paths,
        POINT screenPoint,
        bool preloadIcons);
    void ApplyShellDropProjection(
        const std::vector<std::wstring>& paths,
        int insertionIndex);
    void ClearShellDropPreview(bool flushDeferredRefresh = false);
    bool QueueNextDesktopCollectionItem();
    void HandleDesktopCollectionResult(
        const DesktopCollectionItemResult& result);
    void FinishDesktopCollectionBatch(bool allSucceeded, const std::wstring& errorMessage);
    void RefreshCurrentItems();
    void ShowBackgroundMenu(POINT screenPoint);
    bool RequestApplicationExit();
    void ApplyMovingSnap(RECT& movingRect) const;
    void ApplySizingSnap(RECT& sizingRect, WPARAM sizingEdge) const;
    void ShowSortMenu(POINT screenPoint);
    void SetIconSize(int iconSize);
    void SetContentViewMode(int mode);
    void SetSortMode(int mode);
    void SetAutoArrange(bool enabled);
    void SetFixedExpanded(bool enabled);
    void OpenCategoryLocation();
    void OpenDataLocation();
    void PasteClipboardShortcuts();
    void OpenFeedbackDraft();
    void ShowPersonalCenter();
    void CheckForUpdates();
    void DissolveCategory();
    void RenameCategory();
    void ShowIconMenu(POINT screenPoint, int iconIndex);
    void RenameSelectedItem();
    bool InvokeSelectedShellVerb(const std::wstring& canonicalVerb);
    void ScheduleShellMutationCleanup(
        const std::vector<std::wstring>& itemIds,
        const std::vector<std::wstring>& paths);
    void ReconcileShellMutationCleanup();
    void ToggleCollapsed();
    void ToggleLocked();
    RECT GridBounds() const;
    RECT CollapseButtonBounds() const;
    RECT LockButtonBounds() const;
    bool HitTestCollapseButton(POINT point) const;
    bool HitTestLockButton(POINT point) const;
    int HeaderButtonAt(POINT point) const;
    void InvokeHeaderButton(int button, POINT pixelPoint);
    LRESULT ResizeHitTest(POINT pixelPoint) const;
    UINT WindowDpi() const;
    int DipToPixels(int value) const;
    int PixelsToDips(int value) const;
    POINT ClientPixelsToDips(POINT point) const;
    RECT ClientRectInDips() const;
    void UpdateHover(POINT point, bool nonClient = false);
    bool UpdateIconDrag(POINT pixelPoint);
    void FinishIconDrag(POINT pixelPoint);
    void CancelIconDrag();
    void FlushDeferredRefresh();
    bool IsItemSelected(const std::wstring& itemId) const;
    void SyncSelectionToGrid();
    void PruneSelectionToCurrentItems();
    void ClearSelection();
    void SelectOnly(int iconIndex);
    void ToggleSelection(int iconIndex);
    void SelectAllItems();
    void MoveKeyboardSelection(UINT virtualKey);
    std::vector<std::wstring> SelectedItemIdsInVisibleOrder() const;
    std::vector<std::wstring> SelectedPathsInVisibleOrder() const;
    std::vector<ShellItemReference> SelectedDesktopShellItems() const;
    void BeginItemSelection(int iconIndex, bool controlPressed);
    void BeginMarqueeSelection(POINT point, bool controlPressed);
    void UpdateMarqueeSelection(POINT point);
    void CompletePointerSelection(bool dragged);
    void ResetPointerSelection();
    size_t NormalizeReorderInsertionIndex(size_t rawInsertionIndex) const;
    void ReorderSelectedItems(size_t insertionIndex);
    WidgetWindow* DropTargetWidgetAtScreenPoint(POINT screenPoint) const;
    void MoveItemToCategory(const std::wstring& itemId, const std::wstring& targetCategoryId);
    void MoveItemsToCategory(
        const std::vector<std::wstring>& itemIds,
        const std::wstring& targetCategoryId);
    bool MoveItemOut(
        const std::wstring& itemId,
        bool showError = true,
        const POINT* dropScreenPoint = nullptr,
        std::uint64_t dragGhostGeneration = 0);
    DesktopItem* FindItem(const std::wstring& itemId);

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND desktopHost_ = nullptr;
    std::wstring categoryId_;
    std::wstring categoryName_;
    int theme_ = 0;
    bool singleClickOpen_ = false;
    int spawnOffset_ = 0;
    ConfigStore configStore_;
    ManagedShortcutStore shortcutStore_;
    WindowConfig windowConfig_;
    D2DContext d2d_;
    WallpaperBackdrop wallpaperBackdrop_;
    IconCache iconCache_;
    IconGrid iconGrid_;
    ShellLauncher launcher_;
    std::vector<DesktopItem> items_;
    std::vector<DesktopItem> currentItems_;
    std::vector<ItemConfig> registeredItems_;
    int draggingIconIndex_ = -1;
    int dragInsertionIndex_ = -1;
    POINT dragStartPoint_{};
    bool dragVisualActive_ = false;
    std::uint64_t dragGhostGeneration_ = 0;
    std::wstring draggingItemId_;
    std::vector<std::wstring> draggingSelectionIds_;
    std::vector<std::wstring> selectedItemIds_;
    std::vector<std::wstring> selectionBaselineItemIds_;
    PointerSelectionGesture pointerSelectionGesture_ =
        PointerSelectionGesture::None;
    POINT selectionStartPoint_{};
    POINT selectionCurrentPoint_{};
    RECT selectionMarqueeRect_{};
    bool selectionControlPressed_ = false;
    bool pressedItemWasSelected_ = false;
    std::wstring pressedItemId_;
    int selectionAnchorIndex_ = -1;
    std::vector<std::wstring> pendingShellCleanupItemIds_;
    std::vector<std::wstring> pendingShellCleanupPaths_;
    unsigned int pendingShellCleanupAttempts_ = 0;
    bool refreshPending_ = false;
    int hoverIconIndex_ = -1;
    int hoverHeaderButton_ = -1;
    int pressedHeaderButton_ = -1;
    bool headerHovered_ = false;
    bool shellDropTargetRegistered_ = false;
    bool shellDropPreviewActive_ = false;
    bool shellDropProjectionActive_ = false;
    bool shellDropProjectionPainted_ = false;
    bool shellDropQueued_ = false;
    bool shellDropCommitActive_ = false;
    std::vector<std::wstring> pendingShellDropPaths_;
    std::vector<DesktopDropPosition> pendingShellDropDesktopPositions_;
    int pendingShellDropInsertionIndex_ = -1;
    bool pendingShellDropShowError_ = true;
    std::vector<std::wstring> shellDropPreviewPaths_;
    std::vector<DesktopItem> shellDropProjectedItems_;
    std::vector<DesktopItem> shellDropCommittedItems_;
    int shellDropInsertionIndex_ = -1;
    size_t desktopCollectionPathIndex_ = 0;
    size_t desktopCollectionCommittedCount_ = 0;
    int desktopCollectionBaseInsertionIndex_ = 0;
    std::uint64_t loadItemsGeneration_ = 0;
    bool interactionSavePending_ = false;
    bool pendingOrderValid_ = false;
    WindowConfig pendingLayout_{};
    std::vector<std::wstring> pendingOrderIds_;
};
