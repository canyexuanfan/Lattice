#pragma once

#include <Windows.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/StartupManager.h"
#include "app/TrayIcon.h"
#include "app/UpdateService.h"
#include "config/ConfigStore.h"
#include "desktop/DesktopPlacementCoordinator.h"
#include "desktop/DesktopWatcher.h"
#include "desktop/DesktopItem.h"
#include "desktop/ManagedShortcutStore.h"
#include "model/OrganizerModel.h"
#include "rendering/D2DContext.h"
#include "rendering/IconCache.h"
#include "shell/ShellLauncher.h"
#include "ui/IconGrid.h"
#include "ui/DesktopSurfaceWindow.h"
#include "ui/SettingsDialog.h"
#include "ui/WidgetWindow.h"

class MainWindow {
public:
    using NormalExitHandler = std::function<bool(HWND, std::wstring&)>;

    MainWindow(
        HINSTANCE instance,
        NormalExitHandler normalExitHandler,
        bool keepRunningOnNormalExitFailure = true);
    ~MainWindow();

    bool Create();
    HWND Window() const noexcept { return hwnd_; }
    bool EnableDesktopDisplayTakeover(std::wstring& errorMessage);
    void ReloadPersistedState();
    void ShowNonBlockingNotice(
        const std::wstring& title,
        const std::wstring& message);
    void Show(int showCommand);
    bool ShouldStartHidden() const noexcept {
        return organizerConfig_.settings.startHidden ||
               (organizerConfig_.settings.restoreHiddenState && !organizerConfig_.settings.lastVisible);
    }
    bool WasUpdateExitRequested() const noexcept { return updateExitRequested_; }
    bool WasNormalExitCompleted() const noexcept { return normalExitCompleted_; }
    const std::wstring& LastNormalExitError() const noexcept { return lastNormalExitError_; }
    void FinishPendingDesktopPlacements();

private:
    friend struct MainWindowSmokeAccess;

    struct TileView {
        std::wstring categoryId;
        std::wstring name;
        std::wstring color;
        RECT bounds{};
        RECT headerBounds{};
        bool collapsed = false;
        size_t itemCount = 0;
        IconGrid grid;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void LoadDesktopItems();
    std::vector<std::wstring> AssignedDesktopIdentities() const;
    void RefreshDesktopSurfaceAssignments();
    void ScheduleDesktopRefresh();
    void ToggleAllVisible();
    void ToggleAllLocked();
    void ToggleStartup();
    void RefreshSearchQuery();
    void LayoutSearchEdit();
    void ShowTrayMenu();
    void OpenAllCategoryWidgets();
    void ShowSettings();
    void ExportConfig();
    void ImportConfig();
    void OpenCurrentCategoryWidget();
    void MoveCurrentCategory(int delta);
    void SetCategoryColor(const std::wstring& color);
    void SetCategoryIcon(const std::wstring& icon);
    void ExportCurrentCategory();
    void ImportCurrentCategory();
    void RenameItemDisplayName(const std::wstring& itemId);
    void RefreshIconCache();
    void CheckForUpdates(HWND sourceWindow);
    void HandleUpdateServiceResult(UpdateServiceResult* result);
    void EnsureWindowVisible();
    void LoadOrganizerConfig();
    void Render();
    void SaveWindowConfig();
    bool SaveOrganizerConfig();
    RECT GridBounds() const;
    RECT TabBounds(size_t index) const;
    RECT CollapseButtonBounds() const;
    RECT LockButtonBounds() const;
    RECT TileHeaderBounds(size_t index) const;
    int HitTestTab(POINT point) const;
    bool HitTestCollapseButton(POINT point) const;
    bool HitTestLockButton(POINT point) const;
    void RefreshCurrentItems();
    void RefreshTileViews();
    bool HitTestTileIcon(POINT point, size_t& tileIndex, int& iconIndex) const;
    bool HitTestTileHeader(POINT point, size_t& tileIndex) const;
    void DockTileWindowToWorkArea(bool rememberRestore);
    void SetViewMode(int viewMode);
    void SetTabSide(int side);
    void SaveLayoutProfile();
    void RestoreLayoutProfile();
    void ResetLayout();
    void ShowBackgroundMenu(POINT screenPoint);
    void ShowIconMenu(POINT screenPoint, int iconIndex);
    void ShowItemMenu(POINT screenPoint, const std::wstring& itemId);
    void ShowTileMenu(POINT screenPoint, size_t tileIndex);
    void CreateCategory();
    void RenameCurrentCategory();
    void DeleteCurrentCategory();
    void ToggleCollapsed();
    void ToggleLocked();
    void SetIconSize(int iconSize);
    void SetDensity(int density);
    void ImportUnassignedDesktopItems();
    void ReorderCurrentCategoryItem(size_t fromIndex, size_t toIndex);
    void ReorderItemInCategory(
        const std::wstring& categoryId,
        const std::wstring& fromItemId,
        const std::wstring& toItemId);
    void MoveItemToCategory(const std::wstring& itemId, const std::wstring& categoryId);
    void RemoveItemFromCurrentCategory(const std::wstring& itemId);
    bool MoveItemOut(
        const std::wstring& itemId,
        bool showError = true,
        const POINT* dropScreenPoint = nullptr,
        std::uint64_t dragGhostGeneration = 0);
    bool QueueDesktopPlacement(const DesktopPlacementRequest& request);
    void HandleDesktopPlacementEvents(bool allowDialogs);
    bool DrainPendingDesktopPlacementsForExit(std::wstring& errorMessage);
    void RequestNormalExit();
    void FlushDeferredRefresh();
    bool SaveDesktopPlacement(
        const std::wstring& path,
        POINT point,
        bool showError,
        HWND sourceWindow,
        bool allowDialogs);
    bool ImportPathToCategory(const std::wstring& path, const std::wstring& categoryId, bool showError = true);
    std::wstring StorageFolderForCategory(const std::wstring& categoryId) const;
    DesktopItem* FindItem(const std::wstring& itemId);
    bool IsItemAssigned(const std::wstring& itemId) const;
    Category* FindCategory(const std::wstring& categoryId);
    const Category* FindCategory(const std::wstring& categoryId) const;
    std::wstring CurrentCategoryName() const;
    std::wstring GenerateCategoryId() const;
    bool MatchesSearch(const DesktopItem& item) const;
    int HoverButton() const;

    HINSTANCE instance_;
    NormalExitHandler normalExitHandler_;
    bool keepRunningOnNormalExitFailure_ = true;
    HWND hwnd_ = nullptr;
    HWND searchEdit_ = nullptr;
    HWND searchScopeCombo_ = nullptr;
    ConfigStore configStore_;
    StartupManager startupManager_;
    TrayIcon trayIcon_;
    DesktopWatcher desktopWatcher_;
    DesktopPlacementCoordinator desktopPlacementCoordinator_;
    ManagedShortcutStore shortcutStore_;
    WindowConfig windowConfig_;
    WindowConfig tileRestoreConfig_{};
    bool hasTileRestoreConfig_ = false;
    D2DContext d2d_;
    IconCache iconCache_;
    IconGrid iconGrid_;
    ShellLauncher launcher_;
    std::vector<DesktopItem> items_;
    OrganizerConfig organizerConfig_;
    std::vector<DesktopItem> currentItems_;
    int draggingIconIndex_ = -1;
    bool dragVisualActive_ = false;
    std::uint64_t dragGhostGeneration_ = 0;
    std::wstring draggingItemId_;
    std::wstring draggingSourceCategoryId_;
    bool refreshPending_ = false;
    std::wstring searchQuery_;
    int searchScope_ = 0;
    POINT lastMousePoint_{};
    int hoverTabIndex_ = -1;
    int hoverButtonIndex_ = -1;
    std::vector<std::unique_ptr<WidgetWindow>> widgetWindows_;
    std::unique_ptr<DesktopSurfaceWindow> desktopSurface_;
    std::vector<TileView> tileViews_;
    std::unordered_map<std::wstring, bool> tileCollapsed_;
    int tileScrollOffset_ = 0;
    int tileContentHeight_ = 0;
    int hoverTileIndex_ = -1;
    int draggingTileIndex_ = -1;
    int draggingTileIconIndex_ = -1;
    POINT dragStartPoint_{};
    bool updateExitRequested_ = false;
    bool normalExitInProgress_ = false;
    bool normalExitCompleted_ = false;
    std::wstring lastNormalExitError_;
};
