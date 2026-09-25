#pragma once

#include <Windows.h>
#include <ShObjIdl.h>
#include <wrl/client.h>

#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "desktop/DesktopLayout.h"
#include "rendering/D2DContext.h"
#include "rendering/IconCache.h"
#include "rendering/WallpaperBackdrop.h"
#include "shell/ShellItemReference.h"
#include "shell/ShellLauncher.h"
#include "ui/WidgetView.h"

class DesktopSurfaceDropTarget;

struct HostedWidgetDescriptor {
    std::wstring categoryId;
    std::wstring title;
    WindowConfig config{};
    int theme = 0;
    bool visible = true;
    bool singleClickOpen = false;
    std::vector<DesktopItem> items;
};

enum class HostedWidgetCommandType {
    None,
    OpenSelection,
    RenameSelection,
    DeleteSelection,
    CopySelection,
    CutSelection,
    ShowSelectionMenu,
    ShowBackgroundMenu,
    BringToFront,
    ToggleCollapsed,
    ToggleLocked,
    OpenCategoryLocation,
    ToggleContentView,
    ShowSortMenu,
    ReorderSelection,
    MoveSelectionToCategory,
    MoveSelectionOut,
    CommitLayout,
    RefreshIcons,
    ShellDropTarget,
    CollectPaths,
};

struct HostedWidgetCommand {
    HostedWidgetCommandType type = HostedWidgetCommandType::None;
    std::wstring categoryId;
    std::vector<std::wstring> itemIds;
    std::wstring targetCategoryId;
    std::wstring targetItemId;
    int insertionIndex = -1;
    POINT screenPoint{};
    std::vector<POINT> itemScreenPoints;
    std::vector<std::wstring> paths;
    WindowConfig layout{};
};

class DesktopSurfaceWindow {
public:
    using DisplayPositionCommitHandler =
        std::function<bool(
            const std::vector<DesktopPosition>&)>;
    using RenameCommitHandler =
        std::function<bool(
            const std::wstring&,
            const std::wstring&,
            const std::wstring&)>;
    using HostedWidgetCommandHandler =
        std::function<bool(const HostedWidgetCommand&)>;
    using ShellMenuForwardHandler =
        std::function<bool(UINT, WPARAM, LPARAM, LRESULT&)>;

    explicit DesktopSurfaceWindow(HINSTANCE instance);
    ~DesktopSurfaceWindow();

    DesktopSurfaceWindow(const DesktopSurfaceWindow&) = delete;
    DesktopSurfaceWindow& operator=(const DesktopSurfaceWindow&) = delete;

    bool Create(
        const std::vector<std::wstring>& assignedIdentities,
        std::wstring& errorMessage);
    void Show();
    void Hide();
    void Close();
    bool Refresh(
        std::wstring& errorMessage,
        bool refreshWallpaper = true);
    void InvalidateIconCache(
        const std::vector<std::wstring>& paths);
    void RefreshIconCache();
    void SetIconCacheCapacity(size_t capacity);
    void RemoveDeletedIdentities(
        const std::vector<std::wstring>& identities);
    void UpdateAssignedIdentities(
        const std::vector<std::wstring>& assignedIdentities);
    void UpdateDisplayPositions(
        const std::vector<DesktopPosition>& viewPositions);
    void SetDisplayPositionCommitHandler(
        DisplayPositionCommitHandler handler);
    void SetRenameCommitHandler(RenameCommitHandler handler);
    void SetHostedWidgetCommandHandler(
        HostedWidgetCommandHandler handler);
    void SetShellMenuForwardHandler(
        ShellMenuForwardHandler handler);
    bool ApplyHostedWidgets(
        const std::vector<HostedWidgetDescriptor>& widgets,
        std::wstring& errorMessage);
    void ClearHostedWidgets();
    size_t HostedWidgetCount() const noexcept;
#ifndef NDEBUG
    bool AttachDropTargetForSmoke(IDropTarget* explorerTarget);
#endif
    void PresentUnassignedItemAt(
        const std::wstring& identity,
        POINT screenPoint);
    void ConfirmUnassignedItemAt(
        const std::wstring& identity,
        POINT screenPoint);
    std::optional<bool> DropShellItemsOnTargetAtScreenPoint(
        const std::vector<std::wstring>& sourcePaths,
        POINT screenPoint) const;

    HWND Window() const noexcept { return hwnd_; }
    const DesktopViewSnapshot& Snapshot() const noexcept { return snapshot_; }
    size_t VisibleItemCount() const noexcept { return visibleItems_.size(); }
    bool IsDesktopHosted() const noexcept;

private:
    friend struct DesktopSurfaceWindowSmokeAccess;
    friend class DesktopSurfaceDropTarget;

    static constexpr WallpaperBackdropDrawMode
        kWallpaperDrawMode =
            WallpaperBackdropDrawMode::StretchToDestination;

    struct IdentityLess {
        bool operator()(
            const std::wstring& left,
            const std::wstring& right) const noexcept;
    };
    using IdentitySet =
        std::set<std::wstring, IdentityLess>;

    enum class PointerGesture {
        None,
        ItemPressed,
        MarqueePending,
        MarqueeActive,
    };

    enum class HostedPointerGesture {
        None,
        ItemPressed,
        ItemDragging,
        MarqueePending,
        MarqueeActive,
        HeaderButtonPressed,
        Moving,
        Resizing,
    };

    struct HostedWidgetEntry {
        HostedWidgetDescriptor descriptor;
        WidgetView view;
    };

#ifndef NDEBUG
    enum class InternalDropStage {
        None,
        SessionBegan,
        DragEntered,
        DropReceived,
        PlanRejected,
        PlannedNoChange,
        CoordinateRejected,
        CommitRejected,
        Applied,
        ShellTargetDropped,
    };
#endif

    static LRESULT CALLBACK WindowProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK RenameEditProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData);
    static LRESULT CALLBACK KeyboardHookProc(
        int code,
        WPARAM wParam,
        LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void Render();
    int HostedWidgetIndexAt(POINT clientPoint) const;
    int RaiseHostedWidget(int index);
    bool ActivateHostedWidgetAt(
        POINT clientPoint,
        bool controlPressed);
    bool DispatchHostedWidgetKey(
        UINT virtualKey,
        bool controlPressed,
        bool shiftPressed);
    bool DispatchHostedWidgetAction(
        size_t widgetIndex,
        const WidgetViewAction& action,
        POINT screenPoint = POINT{});
    bool BeginHostedPointerGesture(
        POINT clientPoint,
        bool controlPressed);
    void ContinueHostedPointerGesture(POINT clientPoint);
    void CompleteHostedPointerGesture(POINT clientPoint);
    void ResetHostedPointerGesture() noexcept;
    void ClearHostedDragFeedback() noexcept;
    void DispatchHostedHeaderButton(
        size_t widgetIndex,
        int button,
        POINT screenPoint);
    int HitTest(POINT clientPoint) const;
    RECT CellRect(const DesktopViewItem& item) const;
    RECT LabelRect(const DesktopViewItem& item) const;
    RECT FallbackInteractionRect(
        const DesktopViewItem& item) const;
    bool IsInFallbackHitRegion(
        const DesktopViewItem& item,
        POINT clientPoint) const;
    RECT InteractionRect(size_t visibleIndex) const;
    bool TryNativeHitTest(
        POINT clientPoint,
        int& visibleIndex) const;
    bool TryNativeInteractionRect(
        const DesktopViewItem& item,
        RECT& interactionRect) const;
    bool SendListViewQuery(
        UINT message,
        WPARAM wParam,
        void* localBuffer,
        size_t bufferSize,
        LRESULT& messageResult) const;
    bool InitializeListViewQueryAccess() noexcept;
    void ReleaseListViewQueryAccess() noexcept;
    void RebuildInteractionRects();
    RECT ClientBounds() const noexcept;
    static RECT NormalizeMarqueeRect(
        POINT anchor,
        POINT current,
        const RECT& bounds) noexcept;
    static bool HasExceededDragThreshold(
        POINT anchor,
        POINT current) noexcept;
    bool IsSelected(const std::wstring& identity) const;
    void AddSelected(const std::wstring& identity);
    void RemoveSelected(const std::wstring& identity);
    void SelectOnly(const std::wstring& identity);
    bool ActivateExplorerDesktopView() const noexcept;
    bool SynchronizeExplorerSelection();
    bool ResolveExplorerViewItem(
        const DesktopViewItem& item,
        PIDLIST_RELATIVE& currentPidl) const;
    void PruneSelectionToVisibleItems();
    std::vector<std::wstring> SelectedPathsInVisibleOrder() const;
    std::vector<ShellItemReference>
        SelectedShellItemsInVisibleOrder() const;
    void BeginPointerGesture(
        POINT clientPoint,
        bool controlPressed);
    std::vector<std::wstring> ContinuePointerGesture(
        POINT clientPoint);
    void CompletePointerGesture();
    void ResetPointerGesture() noexcept;
    void CancelPointerCapture() noexcept;
    void CancelPendingRename() noexcept;
    bool InstallKeyboardHook() noexcept;
    void RemoveKeyboardHook() noexcept;
    bool ShouldRouteDesktopF2() const noexcept;
    bool BeginRename(const std::wstring& identity);
    void FinishRename(bool commit);
    void UpdateRenameEditGeometry();
    bool FocusKeyboardWindow(HWND target) const noexcept;
    void ReplaceRenamedIdentity(
        const std::wstring& previousIdentity,
        const DesktopShellRenameResult& renamedItem);
    void ApplyMarqueeSelection(const RECT& marqueeRect);
    bool IsAssigned(const std::wstring& identity) const;
    void RebuildVisibleItems(
        const std::vector<std::wstring>& newlyObserved = {});
    void MaintainDesktopLayer();
    void UpdateViewMetrics();
    void ConfigurePixelRenderTarget();
    bool RefreshWallpaperForCurrentTarget();
    void RecoverWallpaperAfterRenderFailure();
    void StartWallpaperRecovery(bool hideSurface);
    void StopWallpaperRecovery() noexcept;
    void HandleWallpaperRecoveryTimer();
    void SetItemScreenPoint(
        const std::wstring& identity,
        POINT screenPoint);
    bool BuildShellDragImage(
        POINT sourceClientPoint,
        SHDRAGIMAGE& dragImage);
    bool BeginInternalDragSession(POINT sourceClientPoint);
    void EndInternalDragSession() noexcept;
    bool CommitInternalDesktopDrop(POINT dropScreenPoint);
    bool TryInternalShellDropTargetAtScreenPoint(
        POINT screenPoint,
        ShellItemReference& targetItem) const;
    bool TryShellDropTargetAtScreenPoint(
        POINT screenPoint,
        const std::vector<ShellItemReference>& excludedItems,
        ShellItemReference& targetItem) const;
    static std::vector<DesktopPosition> OffsetDragPositions(
        const std::vector<DesktopPosition>& originalPositions,
        POINT sourceScreenPoint,
        POINT dropScreenPoint);
    static bool PlanVisibleGridDrop(
        const std::vector<DesktopPosition>& visiblePositions,
        const std::vector<DesktopPosition>& selectedPositions,
        POINT sourceScreenPoint,
        POINT dropScreenPoint,
        const RECT& screenRect,
        int cellWidth,
        int cellHeight,
        int iconSize,
        std::vector<DesktopPosition>& plannedPositions);
    static std::vector<DesktopPosition> ResolveVisiblePositionCollisions(
        const std::vector<DesktopPosition>& nativePositions,
        const std::vector<DesktopPosition>& displayOverrides,
        const RECT& viewBounds,
        int cellWidth,
        int cellHeight,
        const std::vector<std::wstring>& newlyObserved = {});

    struct PositionOverride {
        std::wstring identity;
        POINT screenPoint{};
        POINT nativeScreenPoint{};
        POINT viewPoint{};
    };

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    DesktopViewSnapshot snapshot_;
    Microsoft::WRL::ComPtr<IFolderView> explorerFolderView_;
    Microsoft::WRL::ComPtr<IShellView> explorerShellView_;
    Microsoft::WRL::ComPtr<IShellFolder> explorerDesktopFolder_;
    HRESULT lastExplorerSelectionSyncResult_ = E_PENDING;
    int lastExplorerSelectionSyncStage_ = 0;
    std::vector<std::wstring> assignedIdentities_;
    std::vector<DesktopViewItem> visibleItems_;
    std::vector<RECT> visibleInteractionRects_;
    std::vector<PositionOverride> positionOverrides_;
    DisplayPositionCommitHandler displayPositionCommitHandler_;
    RenameCommitHandler renameCommitHandler_;
    HostedWidgetCommandHandler hostedWidgetCommandHandler_;
    ShellMenuForwardHandler shellMenuForwardHandler_;
    D2DContext d2d_;
    IconCache iconCache_;
    WallpaperBackdrop wallpaper_;
    ShellLauncher launcher_;
    int hoverIndex_ = -1;
    IdentitySet selectedIdentities_;
    IdentitySet selectionBaseline_;
    std::wstring pressedIdentity_;
    POINT pointerStart_{};
    POINT pointerCurrent_{};
    RECT marqueeRect_{};
    PointerGesture pointerGesture_ = PointerGesture::None;
    bool controlAtPointerDown_ = false;
    bool pressedWasSelected_ = false;
    std::wstring lastCompletedClickIdentity_;
    std::wstring lastCompletedClickCategoryId_;
    POINT lastCompletedClickPoint_{};
    DWORD lastCompletedClickTime_ = 0;
    bool swallowDoubleClickRelease_ = false;
    bool renameClickCandidate_ = false;
    std::wstring pendingRenameIdentity_;
    HWND renameEdit_ = nullptr;
    HFONT renameFont_ = nullptr;
    std::wstring renameIdentity_;
    std::wstring renameOriginalDisplayName_;
    bool renameFinalizing_ = false;
    HHOOK keyboardHook_ = nullptr;
    bool desktopKeyboardSelectionArmed_ = false;
    bool swallowF2Key_ = false;
    static DesktopSurfaceWindow* keyboardHookOwner_;
    size_t shellDragStartCount_ = 0;
    bool suppressShellDragForSmoke_ = false;
    bool internalDragActive_ = false;
    POINT internalDragSourceScreenPoint_{};
    std::vector<DesktopPosition> internalDragOriginalPositions_;
    std::vector<std::unique_ptr<HostedWidgetEntry>> hostedWidgets_;
    int activeHostedWidgetIndex_ = -1;
    HostedPointerGesture hostedPointerGesture_ =
        HostedPointerGesture::None;
    int hostedPointerWidgetIndex_ = -1;
    int hostedPointerTargetWidgetIndex_ = -1;
    WidgetViewHit hostedPointerHit_{};
    POINT hostedPointerStart_{};
    POINT hostedPointerCurrent_{};
    RECT hostedPointerOriginalBounds_{};
    WindowConfig hostedPointerOriginalLayout_{};
    bool hostedPointerControlPressed_ = false;
    bool hostedPointerPressedWasSelected_ = false;
    std::vector<std::wstring> hostedPointerSelectionBaseline_;
    std::vector<std::wstring> hostedDraggingItemIds_;
    std::vector<RECT> hostedAlignmentRects_;
    int hostedDragInsertionIndex_ = -1;
    std::uint64_t hostedDragGhostGeneration_ = 0;
#ifndef NDEBUG
    InternalDropStage internalDropStage_ = InternalDropStage::None;
    std::unique_ptr<WidgetView> hostedWidgetSlice_;
    int hostedWidgetPublishFailureIndex_ = -1;
    bool suppressDesktopOpenForSmoke_ = false;
    size_t doubleClickMessageCountForSmoke_ = 0;
    size_t leftDownMessageCountForSmoke_ = 0;
    size_t leftUpMessageCountForSmoke_ = 0;
    POINT hostedDragActivationPointForSmoke_{};
    POINT doubleClickPointForSmoke_{};
    size_t desktopOpenRequestCountForSmoke_ = 0;
    std::wstring desktopOpenPathForSmoke_;
#endif
    bool dropTargetRegistered_ = false;
    Microsoft::WRL::ComPtr<IDropTarget> desktopDropTarget_;
    int cellWidth_ = 76;
    int cellHeight_ = 84;
    int iconSize_ = 48;
    HANDLE listViewProcess_ = nullptr;
    void* listViewQueryBuffer_ = nullptr;
    DWORD listViewProcessId_ = 0;
    bool listViewQueryReady_ = false;
    bool wallpaperReadyForTarget_ = false;
    bool wallpaperRecoveryActive_ = false;
    bool wallpaperRecoveryHidden_ = false;
    unsigned int wallpaperRecoveryAttempts_ = 0;
};
