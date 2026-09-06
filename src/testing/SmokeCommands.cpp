#include <Windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <process.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "app/App.h"
#include "app/UpdateService.h"
#include "config/ConfigStore.h"
#include "desktop/DesktopScanner.h"
#include "desktop/DesktopLayout.h"
#include "desktop/DesktopPlacementCoordinator.h"
#include "desktop/CategoryStorageManager.h"
#include "desktop/ManagedShortcutStore.h"
#include "desktop/DesktopSession.h"
#include "shell/ShellDropTarget.h"
#include "ui/InputDialog.h"
#include "ui/DragGhostWindow.h"
#include "ui/MessageDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/WidgetWindow.h"
#include "testing/SmokeCommands.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")

struct WidgetWindowSmokeAccess {
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
        loadedAppConfig.desktopLayout.front().y != desktopPlacement.y) {
        std::wcerr << L"Desktop layout config persistence failed\n";
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
    return 0;
}

int RunSmokeDesktopLayout() {
    AttachParentConsole();
    DesktopScanner scanner;
    DesktopLayout layout;
    std::wstring lastError;
    const std::filesystem::path diagnosticPath =
        std::filesystem::path(ConfigStore{}.ConfigPath()).parent_path() /
        L"smoke-layout-diagnostics.txt";
    std::ofstream diagnostics(diagnosticPath, std::ios::trunc);
    const auto utf8 = [](const std::wstring& value) {
        const int required = WideCharToMultiByte(
            CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>((std::max)(required, 0)), '\0');
        if (required > 0) {
            WideCharToMultiByte(
                CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
        }
        return result;
    };
    const std::vector<DesktopItem> items = scanner.Scan(false);
    diagnostics << "ITEM_COUNT=" << items.size() << "\n";
    for (const DesktopItem& item : items) {
        POINT point{};
        if (layout.CapturePosition(item.path, point, lastError)) {
            diagnostics << "CAPTURED=" << utf8(item.path) << " @ " << point.x << "," << point.y << "\n";
            std::wcout << L"Desktop layout item: " << item.displayName << L" @ " << point.x << L"," << point.y << L"\n";
            return 0;
        }
        diagnostics << "FAILED=" << utf8(item.path) << " | " << utf8(lastError) << "\n";
    }
    diagnostics.flush();
    std::wcerr << L"Desktop layout capture failed: " << lastError << L"\n";
    return 1;
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
    if (!store.IsSupportedDesktopItem(sourceFile.wstring()) ||
        !store.IsSupportedDesktopItem(sourceFolder.wstring()) ||
        store.IsSupportedShortcut(sourceFile.wstring()) ||
        !store.RequiresManagedStorage(sourceFile.wstring()) ||
        !store.RequiresManagedStorage(sourceFolder.wstring()) ||
        !store.RequiresManagedStorage(sourceShortcut.wstring()) ||
        !store.RequiresManagedStorage(sourceUrl.wstring()) ||
        !store.RequiresManagedStorage(publicShortcut.wstring()) ||
        !store.RequiresManagedStorage(publicFolder.wstring()) ||
        store.RequiresManagedStorage(externalFile.wstring())) {
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
    std::filesystem::remove(sourceFile, fileError);
    if (fileError) {
        return fail(L"Desktop conflict cleanup failed");
    }
    if (!store.MoveToOriginalDesktop(
            L"smoke-conflict",
            managedConflict,
            sourceFile.wstring(),
            [](const std::wstring&) { return true; },
            restoredFile,
            moveError)) {
        return fail(L"Desktop conflict final restore failed: " + moveError);
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
    std::wcout << L"Ordinary and Public Desktop unique-original transactions passed\n";
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
    ManagedShortcutStore managedStore(dataDirectory.wstring(), desktopDirectory.wstring());
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

    CategoryStorageManager storageManager(configStore, managedStore);
    std::wstring errorMessage;
    if (!storageManager.SynchronizeAll(errorMessage)) {
        return fail(L"Legacy category storage migration failed: " + errorMessage);
    }
    AppConfig migrated = configStore.LoadAppConfig();
    const auto migratedCategory = std::find_if(
        migrated.categories.begin(),
        migrated.categories.end(),
        [&](const CategoryConfig& value) { return value.id == categoryId; });
    const auto migratedUncategorizedItem = std::find_if(
        migrated.items.begin(),
        migrated.items.end(),
        [&](const ItemConfig& value) { return value.id == uncategorizedItem.id; });
    const auto migratedCategoryItem = std::find_if(
        migrated.items.begin(),
        migrated.items.end(),
        [&](const ItemConfig& value) { return value.id == categoryItem.id; });
    if (migrated.uncategorizedStorageFolder != L"AI" ||
        migratedCategory == migrated.categories.end() || migratedCategory->storageFolder != L"AI编程" ||
        migratedUncategorizedItem == migrated.items.end() ||
        migratedUncategorizedItem->path != (managedDirectory / L"AI" / L"豆包.lnk").wstring() ||
        migratedCategoryItem == migrated.items.end() ||
        migratedCategoryItem->path != (managedDirectory / L"AI编程" / L"代码.txt").wstring() ||
        !std::filesystem::exists(managedDirectory / L"AI" / L"豆包.lnk") ||
        !std::filesystem::exists(managedDirectory / L"AI编程" / L"代码.txt") ||
        std::filesystem::exists(managedDirectory / L"uncategorized") ||
        std::filesystem::exists(managedDirectory / categoryId)) {
        return fail(L"Legacy category storage migration result mismatch");
    }

    if (!storageManager.Rename(categoryId, L"编程工具", errorMessage)) {
        return fail(L"Category storage rename failed: " + errorMessage);
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
    if (renamedCategory == renamed.categories.end() || renamedCategory->name != L"编程工具" ||
        renamedCategory->storageFolder != L"编程工具" || renamedItem == renamed.items.end() ||
        renamedItem->path != (managedDirectory / L"编程工具" / L"代码.txt").wstring() ||
        !std::filesystem::exists(managedDirectory / L"编程工具" / L"代码.txt") ||
        std::filesystem::exists(managedDirectory / L"AI编程")) {
        return fail(L"Category storage rename result mismatch");
    }
    if (storageManager.CanUseName(categoryId, L"AI", errorMessage) ||
        storageManager.CanUseName(categoryId, L"CON", errorMessage)) {
        return fail(L"Category storage name validation failed");
    }
    std::filesystem::create_directories(managedDirectory / L"已存在目录", fileError);
    if (fileError || storageManager.CanUseName(L"new-category", L"已存在目录", errorMessage)) {
        return fail(L"Existing unmanaged category directory validation failed");
    }
    std::filesystem::remove(managedDirectory / L"编程工具" / L"代码.txt", fileError);
    if (fileError || !storageManager.RemoveEmpty(categoryId, errorMessage) ||
        std::filesystem::exists(managedDirectory / L"编程工具")) {
        return fail(L"Empty category storage cleanup failed: " + errorMessage);
    }

    const std::array<std::filesystem::path, 4> preservedConfigPaths{
        configDirectory / L"config.ini",
        configDirectory / L"config.backup.ini",
        configDirectory / L"config.backup.ini.1",
        configDirectory / L"config.backup.ini.2"};
    std::array<std::optional<std::string>, 4> preservedConfigBytes{};
    for (size_t index = 0; index < preservedConfigPaths.size(); ++index) {
        preservedConfigBytes[index] = ReadFileBytes(preservedConfigPaths[index]);
        if (!preservedConfigBytes[index].has_value()) {
            return fail(L"Category storage no-op snapshot failed");
        }
    }
    if (!storageManager.SynchronizeAll(errorMessage) ||
        !std::filesystem::exists(managedDirectory / L"编程工具")) {
        return fail(L"Category storage no-op synchronization failed: " + errorMessage);
    }
    for (size_t index = 0; index < preservedConfigPaths.size(); ++index) {
        const std::optional<std::string> after =
            ReadFileBytes(preservedConfigPaths[index]);
        if (!after.has_value() ||
            *after != *preservedConfigBytes[index]) {
            return fail(L"Category storage no-op synchronization rewrote config history");
        }
    }

    std::filesystem::remove_all(testRoot, fileError);
    if (fileError) {
        std::wcerr << L"Category storage smoke cleanup failed\n";
        return 1;
    }
    std::wcout << L"Category storage migration and rename passed\n";
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
    std::filesystem::create_directories(dataDirectory / L"ManagedShortcuts" / L"对齐测试一", fileError);
    std::filesystem::create_directories(dataDirectory / L"ManagedShortcuts" / L"对齐测试二", fileError);
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
    siblingCategory.layout.y = targetY + 80;
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
    POINT overlapPoint{
        (widgetRect.left + widgetRect.right) / 2,
        (widgetRect.top + widgetRect.bottom) / 2};
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

    ShowWindow(coverWindow, SW_HIDE);
    widget->SetVisible(false);
    DwmFlush();
    std::vector<std::uint32_t> desktopPixels;
    if (!CaptureScreenPixels(captureRect, desktopPixels)) {
        return fail(L"Desktop pixels could not be captured with the widget hidden", 70);
    }
    widget->SetVisible(true);
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
        SendMessageW(widgetWindow, WM_MOUSEACTIVATE, 0, 0) != MA_NOACTIVATE) {
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
    const std::filesystem::path managedRoot = dataRoot / L"ManagedShortcuts";
    const std::filesystem::path uncategorizedRoot = managedRoot / L"未分类";
    const std::filesystem::path categoryRoot = managedRoot / L"交互测试";
    std::error_code fileError;
    std::filesystem::create_directories(uncategorizedRoot, fileError);
    std::filesystem::create_directories(categoryRoot, fileError);
    std::filesystem::create_directories(desktopRoot, fileError);
    if (fileError) {
        std::wcerr << L"Widget interaction directories failed\n";
        return 1;
    }
    const std::filesystem::path uncategorizedPath = uncategorizedRoot / L"未分类.txt";
    const std::filesystem::path firstPath = categoryRoot / L"第一项.txt";
    const std::filesystem::path secondPath = categoryRoot / L"第二项.txt";
    {
        std::ofstream file(uncategorizedPath, std::ios::binary);
        file << "uncategorized";
    }
    {
        std::ofstream file(firstPath, std::ios::binary);
        file << "first";
    }
    {
        std::ofstream file(secondPath, std::ios::binary);
        file << "second";
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
    config.items.push_back(ItemConfig{L"widget-uncategorized", uncategorizedPath.wstring(), L"未分类项"});
    config.items.push_back(ItemConfig{L"widget-first", firstPath.wstring(), L"第一项"});
    config.items.push_back(ItemConfig{L"widget-second", secondPath.wstring(), L"第二项"});
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
    category.itemIds = {L"widget-first", L"widget-second"};
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
    HWND mainWindow = FindCurrentProcessMainWindow();
    HWND uncategorizedWindow = FindLatticeWidgetWindow(L"未分类");
    HWND categoryWindow = FindLatticeWidgetWindow(L"交互测试");
    if (mainWindow == nullptr || uncategorizedWindow == nullptr || categoryWindow == nullptr) {
        if (mainWindow != nullptr) {
            DestroyWindow(mainWindow);
            app.Run();
        }
        return fail(L"Widget interaction windows not found", 34);
    }
    const auto* categoryWidget = reinterpret_cast<const WidgetWindow*>(
        GetWindowLongPtrW(categoryWindow, GWLP_USERDATA));
    if (categoryWidget == nullptr || !categoryWidget->HasShellDropTarget()) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget OLE shell drop target was not registered", 44);
    }

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
            *const_cast<WidgetWindow*>(categoryWidget))) {
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
    const LPARAM sourcePoint = MAKELPARAM(dip(43), dip(89));
    const LPARAM targetPoint = MAKELPARAM(dip(123), dip(89));
    const auto dragPrepareStart = std::chrono::steady_clock::now();
    SendMessageW(categoryWindow, WM_LBUTTONDOWN, MK_LBUTTON, sourcePoint);
    const auto dragPrepareMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - dragPrepareStart).count();
    const auto dragStart = std::chrono::steady_clock::now();
    SendMessageW(categoryWindow, WM_MOUSEMOVE, MK_LBUTTON, targetPoint);
    const auto dragStartMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - dragStart).count();
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
    const auto persistStart = std::chrono::steady_clock::now();
    if (!WidgetWindowSmokeAccess::FlushInteractionSave(
            *const_cast<WidgetWindow*>(categoryWidget))) {
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
        reorderedCategory->itemIds.size() != 2 ||
        reorderedCategory->itemIds.front() != L"widget-second") {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget internal drag did not persist reordered items", 38);
    }
    SendMessageW(categoryWindow, WM_LBUTTONDOWN, MK_LBUTTON, lockPoint);
    SendMessageW(categoryWindow, WM_LBUTTONUP, 0, lockPoint);
    if (!WidgetWindowSmokeAccess::FlushInteractionSave(
            *const_cast<WidgetWindow*>(categoryWidget))) {
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
    auto* uncategorizedWidget = reinterpret_cast<WidgetWindow*>(
        GetWindowLongPtrW(uncategorizedWindow, GWLP_USERDATA));
    if (uncategorizedWidget == nullptr ||
        !WidgetWindowSmokeAccess::FlushInteractionSave(*uncategorizedWidget) ||
        !WidgetWindowSmokeAccess::FlushInteractionSave(
            *const_cast<WidgetWindow*>(categoryWidget))) {
        DestroyWindow(mainWindow);
        app.Run();
        return fail(L"Widget deferred position persistence failed", 43);
    }
    std::wcout << L"WIDGET_INTERACTION_TIMING_MS collapse=" << collapseMilliseconds
               << L" expand=" << expandMilliseconds
               << L" dragPrepare=" << dragPrepareMilliseconds
               << L" dragStart=" << dragStartMilliseconds
               << L" reorder=" << reorderMilliseconds
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
                   << "reorder=" << reorderMilliseconds << "\n"
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
    if (reorderMilliseconds > 16) {
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
        *configAfterUpdateExit != *configBeforeUpdateExit) {
        return fail(
            L"Update exit changed layout, settings, category membership, or item order",
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
    const std::filesystem::path managedRoot =
        dataRoot / L"ManagedShortcuts" / categoryName;
    const std::filesystem::path managedPath = managedRoot / L"退出恢复.txt";
    const std::filesystem::path desktopPath = desktopRoot / managedPath.filename();
    const std::filesystem::path staleLayoutPath =
        desktopRoot / L"已经不存在的历史桌面项.lnk";
    std::error_code fileError;
    std::filesystem::create_directories(configRoot, fileError);
    std::filesystem::create_directories(managedRoot, fileError);
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
        std::ofstream fixture(managedPath, std::ios::binary);
        fixture << "isolated widget normal-exit restore fixture";
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
    item.path = managedPath.wstring();
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
    if (runResult != 0) {
        return fail(L"Widget normal-exit app loop returned failure", 88);
    }
    if (exitElapsedMilliseconds >= 2000) {
        return fail(L"Normal exit waited for a missing historical desktop item", 93);
    }
    if (std::filesystem::exists(managedPath) ||
        !std::filesystem::exists(desktopPath)) {
        return fail(L"Normal exit did not restore the managed item", 89);
    }
    const std::optional<std::string> restoredBytes =
        ReadFileBytes(desktopPath.wstring());
    if (!restoredBytes.has_value() ||
        *restoredBytes != "isolated widget normal-exit restore fixture") {
        return fail(L"Normal exit changed the restored item", 90);
    }
    const AppConfig restoredConfig = configStore.LoadAppConfig();
    if (restoredConfig.items.size() != 1 ||
        restoredConfig.items.front().path != desktopPath.wstring() ||
        restoredConfig.items.front().originalDesktopPath != desktopPath.wstring() ||
        std::any_of(
            restoredConfig.desktopLayout.begin(),
            restoredConfig.desktopLayout.end(),
            [&](const DesktopPlacementConfig& placement) {
                return CompareStringOrdinal(
                           placement.path.c_str(), -1,
                           staleLayoutPath.c_str(), -1,
                           TRUE) == CSTR_EQUAL;
            })) {
        return fail(L"Normal exit did not commit the restored item path", 91);
    }
    if (std::filesystem::exists(
            dataRoot / L"ManagedShortcuts" / L"move-journal.bin")) {
        return fail(L"Normal exit left a move journal behind", 92);
    }

    cleanup();
    std::wcout << L"Widget exit request stayed asynchronous, ignored stale layout, and restored the managed item in "
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
    const std::filesystem::path managedRoot =
        dataRoot / L"ManagedShortcuts" / categoryName;
    const std::filesystem::path managedPath = managedRoot / L"延迟测试.txt";
    const std::filesystem::path desktopPath = desktopRoot / managedPath.filename();
    std::error_code fileError;
    std::filesystem::create_directories(configRoot, fileError);
    std::filesystem::create_directories(managedRoot, fileError);
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
        std::ofstream file(managedPath, std::ios::binary);
        file << "isolated widget drop latency fixture";
        if (!file) {
            return fail(L"Widget drop latency fixture creation failed", 52);
        }
    }

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
    item.path = managedPath.wstring();
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
            managedPath.c_str(),
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
    if (std::filesystem::exists(managedPath) || !std::filesystem::exists(desktopPath)) {
        return fail(L"Widget drop latency fixture was not moved to the isolated desktop", 67);
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

    grid.SetItems(gridItems);
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
         std::array<std::filesystem::path, 4>{
             configRoot, dataRoot, desktopRoot, managedRoot}) {
        std::filesystem::create_directories(directory, fileError);
        if (fileError) {
            return fail(L"Widget drop placement directory setup failed", 85);
        }
    }
    if (!SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_CONFIG_DIR", configRoot.c_str()) ||
        !SetEnvironmentVariableW(L"DESKTOP_ORGANIZER_DATA_DIR", dataRoot.c_str())) {
        return fail(L"Widget drop placement environment setup failed", 85);
    }

    const std::array<std::filesystem::path, 4> basePaths{
        managedRoot / L"A.txt",
        managedRoot / L"B.txt",
        managedRoot / L"C.txt",
        managedRoot / L"D.txt",
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
        if (!writeFixture(basePaths[index], "managed base fixture")) {
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
    if (!WidgetWindowSmokeAccess::QueuePaths(
            widget,
            dropPaths,
            insertionPoint)) {
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
                item.originalDesktopPath.empty()) {
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
            !registered->originalDesktopPath.empty() ||
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
    if (!WidgetWindowSmokeAccess::QueuePaths(
            widget,
            collisionDropPaths,
            collisionInsertionPoint)) {
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
        !oldCollisionItem->originalDesktopPath.empty() ||
        !newCollisionItem->originalDesktopPath.empty() ||
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
        L"解散格子后，其中的桌面项目会移回原桌面位置。\n\n这个操作不会删除任何文件，是否继续？",
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
    const std::string versionedJson = R"({"tag_name":"v0.4.29","name":"Lattice 0.4.29","assets":[{"name":"Lattice-Setup-0.4.29.exe","browser_download_url":"https://github.com/canyexuanfan/Lattice/releases/download/v0.4.29/Lattice-Setup-0.4.29.exe"}]})";
    std::wstring version;
    std::wstring url;
    if (UpdateService::CompareVersions(L"0.4.29", L"0.4.28") <= 0 ||
        UpdateService::CompareVersions(L"0.4.28", L"0.4.28") != 0 ||
        UpdateService::CompareVersions(L"0.4.27", L"0.4.28") >= 0 ||
        !UpdateService::SelectReleaseAsset(versionedJson, version, url) ||
        version != L"0.4.29" || url.find(L"Lattice-Setup-0.4.29.exe") == std::wstring::npos) {
        std::wcerr << L"Update versioned release selection failed\n";
        return 1;
    }
    const std::string fallbackJson = R"({"tag_name":"0.4.30","assets":[{"name":"Lattice-Setup-Latest.exe","browser_download_url":"https://github.com/canyexuanfan/Lattice/releases/download/v0.4.30/Lattice-Setup-Latest.exe"}]})";
    if (!UpdateService::SelectReleaseAsset(fallbackJson, version, url) ||
        version != L"0.4.30" || url.find(L"Latest.exe") == std::wstring::npos ||
        UpdateService::SelectReleaseAsset(R"({"tag_name":"next","assets":[]})", version, url)) {
        std::wcerr << L"Update fallback or invalid release rejection failed\n";
        return 2;
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
    if (!layout.CaptureAllPositions(positions, errorMessage) ||
        !layout.CaptureViewFlags(flags, errorMessage)) {
        result << "STATUS=FAIL\n";
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
    const std::wstring markerPath = environmentValue(L"LATTICE_REAL_DESKTOP_MARKER");
    const std::wstring markerName = std::filesystem::path(markerPath).filename().wstring();
    if (markerPath.empty() || !managedStore.IsDesktopPath(markerPath) ||
        markerName.rfind(L".lattice-grid-restart-", 0) != 0) {
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
    if (GetFileAttributesW(markerPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        markerRemoved = DeleteFileW(markerPath.c_str()) != FALSE;
        if (markerRemoved) {
            SHChangeNotify(
                SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                markerPath.c_str(), nullptr);
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
    if (HasArgument(commandLine, L"--smoke-shell-new")) {
        return RunSmokeShellNewMenu();
    }
    if (HasArgument(commandLine, L"--smoke-managed-items")) {
        return RunSmokeManagedItems();
    }
    if (HasArgument(commandLine, L"--smoke-category-storage")) {
        return RunSmokeCategoryStorage();
    }
    if (HasArgument(commandLine, L"--smoke-widget-alignment")) {
        return RunSmokeWidgetAlignment(instance);
    }
    if (HasArgument(commandLine, L"--smoke-widget-desktop-layer")) {
        return RunSmokeWidgetDesktopLayer(instance);
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
    if (HasArgument(commandLine, L"--smoke-real-shell-visibility")) {
        return RunSmokeRealShellVisibility();
    }
    if (HasArgument(commandLine, L"--smoke-real-desktop-grid-snapshot")) {
        return RunSmokeRealDesktopGridSnapshot();
    }
    if (HasArgument(commandLine, L"--smoke-real-desktop-session-restore")) {
        return RunSmokeRealDesktopSessionRestore();
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
