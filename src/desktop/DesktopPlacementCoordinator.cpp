#include "desktop/DesktopPlacementCoordinator.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

#include "desktop/DesktopLayout.h"
#include "desktop/DesktopScanner.h"
#include "desktop/ManagedShortcutStore.h"
#include "util/ComInit.h"

namespace {

bool RemoveItemFromPersistedConfig(
    ConfigStore& configStore,
    const std::wstring& itemId) {
    AppConfig config = configStore.LoadAppConfig();
    config.uncategorizedItemIds.erase(
        std::remove(
            config.uncategorizedItemIds.begin(),
            config.uncategorizedItemIds.end(),
            itemId),
        config.uncategorizedItemIds.end());
    for (CategoryConfig& category : config.categories) {
        category.itemIds.erase(
            std::remove(category.itemIds.begin(), category.itemIds.end(), itemId),
            category.itemIds.end());
    }
    config.items.erase(
        std::remove_if(
            config.items.begin(),
            config.items.end(),
            [&](const ItemConfig& value) { return value.id == itemId; }),
        config.items.end());
    return configStore.SaveAppConfig(config);
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(
               left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void PersistDesktopPlacementSnapshot(
    const std::wstring& path,
    POINT point,
    std::wstring& warningMessage) {
    ConfigStore configStore;
    AppConfig config = configStore.LoadAppConfig();
    auto placement = std::find_if(
        config.desktopLayout.begin(),
        config.desktopLayout.end(),
        [&](const DesktopPlacementConfig& value) {
            return SamePath(value.path, path);
        });
    if (placement == config.desktopLayout.end()) {
        config.desktopLayout.push_back(
            DesktopPlacementConfig{path, point.x, point.y});
    } else {
        placement->x = point.x;
        placement->y = point.y;
    }
    if (!configStore.SaveAppConfig(config)) {
        warningMessage =
            L"图标已放到鼠标释放位置，但无法把新位置写入桌面布局快照。";
    }
}

}  // namespace

bool CommitDesktopMoveOutTransaction(
    const DesktopPlacementRequest& request,
    std::wstring& desktopPath,
    std::wstring& errorMessage) {
    desktopPath = request.path;
    errorMessage.clear();
    if (!request.commitMoveOut) {
        return !desktopPath.empty();
    }
    if (request.itemId.empty() || request.sourcePath.empty() || request.path.empty()) {
        errorMessage = L"移出格子的后台事务参数不完整。";
        return false;
    }

    ConfigStore configStore;
    ManagedShortcutStore shortcutStore;
    const auto persistRemoval = [&](const std::wstring&) {
        return RemoveItemFromPersistedConfig(configStore, request.itemId);
    };
    if (shortcutStore.IsManagedPath(request.sourcePath)) {
        return shortcutStore.MoveToOriginalDesktop(
            request.itemId,
            request.sourcePath,
            request.itemState.originalDesktopPath,
            persistRemoval,
            desktopPath,
            errorMessage,
            request.sourceWindow,
            true,
            true);
    }
    if (!persistRemoval({})) {
        errorMessage =
            L"项目原件未发生改变，但无法保存移出格子的显示归属。";
        return false;
    }
    return true;
}

DesktopCollectionItemResult CommitDesktopCollectionItemTransaction(
    const DesktopCollectionItemRequest& request) {
    DesktopCollectionItemResult result;
    result.sourcePath = request.path;
    if (request.categoryId.empty() || request.path.empty()) {
        result.errorMessage = L"桌面项目收纳参数不完整。";
        return result;
    }

    ConfigStore configStore;
    ManagedShortcutStore shortcutStore;
    DesktopScanner scanner;
    AppConfig config = configStore.LoadAppConfig();
    const auto targetItemIds = [&](AppConfig& value) -> std::vector<std::wstring>* {
        if (request.categoryId == L"uncategorized") {
            return &value.uncategorizedItemIds;
        }
        const auto category = std::find_if(
            value.categories.begin(),
            value.categories.end(),
            [&](const CategoryConfig& candidate) {
                return candidate.id == request.categoryId;
            });
        return category == value.categories.end() ? nullptr : &category->itemIds;
    };
    const auto targetLayout = [&](AppConfig& value) -> WindowConfig* {
        if (request.categoryId == L"uncategorized") {
            return &value.window;
        }
        const auto category = std::find_if(
            value.categories.begin(),
            value.categories.end(),
            [&](const CategoryConfig& candidate) {
                return candidate.id == request.categoryId;
            });
        return category == value.categories.end() ? nullptr : &category->layout;
    };
    std::vector<std::wstring>* destinationIds = targetItemIds(config);
    WindowConfig* destinationLayout = targetLayout(config);
    if (destinationIds == nullptr || destinationLayout == nullptr) {
        result.errorMessage = L"目标格子已经不存在，未改变项目的显示归属。";
        return result;
    }

    DesktopItem item = scanner.CreateItemFromPath(request.path, false);
    const std::wstring sourceDerivedId = item.id;
    if (!request.sourceVisibleId.empty()) {
        item.id = request.sourceVisibleId;
    }
    if (!shortcutStore.IsSupportedDesktopItem(item.path)) {
        result.errorMessage =
            L"只能收纳 Explorer 桌面当前显示的文件、文件夹、快捷方式或系统图标；原件未发生改变。";
        return result;
    }

    if (GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        DesktopLayout desktopLayout;
        POINT ignoredPoint{};
        std::wstring identityError;
        if (!desktopLayout.CapturePosition(
                item.path, ignoredPoint, identityError)) {
            result.errorMessage =
                L"该 Shell 项目不在当前桌面显示层中，未改变其显示归属。";
            return result;
        }
    }

    const std::wstring sourcePath = item.path;
    const bool legacyManaged = shortcutStore.IsManagedPath(sourcePath);
    if (legacyManaged) {
        result.errorMessage =
            L"该项目仍处于旧版受管目录，请先完成一次性历史迁移。";
        return result;
    }
    std::wstring originalDesktopPath;
    POINT originalDesktopPoint = request.desktopPoint;
    bool hasOriginalDesktopPoint = request.hasDesktopPoint;
    if (shortcutStore.IsDesktopPath(sourcePath)) {
        originalDesktopPath = sourcePath;
        if (!hasOriginalDesktopPoint) {
            DesktopLayout desktopLayout;
            std::wstring captureError;
            hasOriginalDesktopPoint = desktopLayout.CapturePosition(
                sourcePath,
                originalDesktopPoint,
                captureError);
        }
    }

    const auto existingByPath = std::find_if(
        config.items.begin(),
        config.items.end(),
        [&](const ItemConfig& existing) {
            return SamePath(existing.path, item.path);
        });
    if (existingByPath != config.items.end()) {
        item.id = existingByPath->id;
    } else if (!scanner.TryCreateManagedItemId(
                   [&](const std::wstring& candidate) {
                       return std::any_of(
                           config.items.begin(),
                           config.items.end(),
                           [&](const ItemConfig& value) {
                               return value.id == candidate;
                           });
                   },
                   item.id)) {
        result.errorMessage = L"无法为该项目创建安全的唯一标识，未改变其显示归属。";
        return result;
    }

    const auto persistCollectedItem = [&](const std::wstring& storedPath) {
        auto registered = std::find_if(
            config.items.begin(),
            config.items.end(),
            [&](const ItemConfig& value) { return value.id == item.id; });
        if (registered == config.items.end()) {
            config.items.push_back(ItemConfig{item.id, storedPath, item.displayName});
            registered = std::prev(config.items.end());
        } else {
            registered->path = storedPath;
        }
        registered->originalDesktopPath = originalDesktopPath;
        registered->desktopX = originalDesktopPoint.x;
        registered->desktopY = originalDesktopPoint.y;
        registered->hasDesktopPosition = hasOriginalDesktopPoint;
        registered->desktopVisibilityMode = 0;
        registered->desktopVisibilityOriginalFlags = 0;
        registered->desktopVisibilityNewStartValue = -1;
        registered->desktopVisibilityClassicValue = -1;
        if (hasOriginalDesktopPoint) {
            auto position = std::find_if(
                config.desktopLayout.begin(),
                config.desktopLayout.end(),
                [&](const DesktopPlacementConfig& value) {
                    return SamePath(value.path, originalDesktopPath);
                });
            if (position == config.desktopLayout.end()) {
                config.desktopLayout.push_back(DesktopPlacementConfig{
                    originalDesktopPath,
                    originalDesktopPoint.x,
                    originalDesktopPoint.y});
            } else {
                position->x = originalDesktopPoint.x;
                position->y = originalDesktopPoint.y;
            }
        }

        destinationIds = targetItemIds(config);
        if (destinationIds == nullptr) {
            return false;
        }
        size_t insertionIndex = (std::min)(request.insertionIndex, destinationIds->size());
        std::vector<std::wstring> removalIds{item.id};
        const auto appendRemovableAlias = [&](const std::wstring& aliasId) {
            if (aliasId.empty() ||
                std::find(removalIds.begin(), removalIds.end(), aliasId) !=
                    removalIds.end()) {
                return;
            }
            const auto registeredAlias = std::find_if(
                config.items.begin(),
                config.items.end(),
                [&](const ItemConfig& value) { return value.id == aliasId; });
            if (registeredAlias != config.items.end() &&
                !SamePath(registeredAlias->path, sourcePath)) {
                return;
            }
            removalIds.push_back(aliasId);
        };
        appendRemovableAlias(sourceDerivedId);
        appendRemovableAlias(request.sourceVisibleId);
        for (const std::wstring& removalId : removalIds) {
            for (auto found = std::find(destinationIds->begin(), destinationIds->end(), removalId);
                 found != destinationIds->end();
                 found = std::find(destinationIds->begin(), destinationIds->end(), removalId)) {
                if (static_cast<size_t>(std::distance(destinationIds->begin(), found)) < insertionIndex) {
                    --insertionIndex;
                }
                destinationIds->erase(found);
            }
        }
        for (CategoryConfig& category : config.categories) {
            for (const std::wstring& removalId : removalIds) {
                category.itemIds.erase(
                    std::remove(category.itemIds.begin(), category.itemIds.end(), removalId),
                    category.itemIds.end());
            }
        }
        for (const std::wstring& removalId : removalIds) {
            config.uncategorizedItemIds.erase(
                std::remove(
                    config.uncategorizedItemIds.begin(),
                    config.uncategorizedItemIds.end(),
                    removalId),
                config.uncategorizedItemIds.end());
        }
        destinationIds = targetItemIds(config);
        if (destinationIds == nullptr) {
            return false;
        }
        destinationIds->insert(
            destinationIds->begin() + static_cast<std::ptrdiff_t>(
                (std::min)(insertionIndex, destinationIds->size())),
            item.id);
        destinationLayout = targetLayout(config);
        if (destinationLayout != nullptr) {
            destinationLayout->autoArrange = false;
            destinationLayout->sortMode = 0;
        }
        return configStore.SaveAppConfig(config);
    };

    std::wstring destinationPath = sourcePath;
    const bool collected = persistCollectedItem(destinationPath);
    if (!collected) {
        result.errorMessage =
            L"无法保存该桌面项目的显示归属，原件未发生改变。";
    }
    if (!collected) {
        return result;
    }
    result.succeeded = true;
    result.itemId = item.id;
    result.destinationPath = std::move(destinationPath);
    const auto committedItem = std::find_if(
        config.items.begin(),
        config.items.end(),
        [&](const ItemConfig& value) { return value.id == item.id; });
    if (committedItem != config.items.end()) {
        result.itemState = *committedItem;
    }
    return result;
}

struct DesktopPlacementCoordinator::State {
    std::mutex mutex;
    std::condition_variable workAvailable;
    std::condition_variable drained;
    std::condition_variable workerExited;
    std::deque<QueuedOperation> pending;
    std::deque<Event> events;
    HWND notificationWindow = nullptr;
    DWORD workerThreadId = 0;
    OperationId nextOperationId = 1;
    size_t activeOperations = 0;
    bool stopping = false;
    bool workerFinished = false;
};

DesktopPlacementCoordinator::DesktopPlacementCoordinator()
    : state_(std::make_shared<State>()),
      worker_([state = state_]() {
          WorkerLoop(state);
      }) {}

DesktopPlacementCoordinator::~DesktopPlacementCoordinator() {
    CancelAndStopFor(INFINITE);
    state_.reset();
}

void DesktopPlacementCoordinator::AttachNotificationWindow(HWND hwnd) {
    const std::shared_ptr<State> state = state_;
    bool hasPendingEvents = false;
    {
        std::lock_guard lock(state->mutex);
        state->notificationWindow = hwnd;
        hasPendingEvents = !state->events.empty();
    }
    if (hasPendingEvents && hwnd != nullptr) {
        PostMessageW(hwnd, kDesktopPlacementEventMessage, 0, 0);
    }
}

void DesktopPlacementCoordinator::DetachNotificationWindow(HWND hwnd) {
    const std::shared_ptr<State> state = state_;
    std::lock_guard lock(state->mutex);
    if (state->notificationWindow == hwnd) {
        state->notificationWindow = nullptr;
    }
}

DesktopPlacementCoordinator::OperationId
DesktopPlacementCoordinator::PlaceAtScreenAsync(const DesktopPlacementRequest& request) {
    if (request.path.empty()) {
        return 0;
    }

    const std::shared_ptr<State> state = state_;
    OperationId id = 0;
    {
        std::lock_guard lock(state->mutex);
        if (state->stopping) {
            return 0;
        }
        id = state->nextOperationId++;
        if (state->nextOperationId == 0) {
            state->nextOperationId = 1;
        }
        QueuedOperation operation;
        operation.id = id;
        operation.placementRequest = request;
        state->pending.push_back(std::move(operation));
    }
    state->workAvailable.notify_one();
    return id;
}

DesktopPlacementCoordinator::OperationId
DesktopPlacementCoordinator::CollectItemAsync(
    const DesktopCollectionItemRequest& request) {
    if (request.categoryId.empty() || request.path.empty()) {
        return 0;
    }
    const std::shared_ptr<State> state = state_;
    OperationId id = 0;
    {
        std::lock_guard lock(state->mutex);
        if (state->stopping || state->pending.size() >= 32) {
            return 0;
        }
        id = state->nextOperationId++;
        if (state->nextOperationId == 0) {
            state->nextOperationId = 1;
        }
        QueuedOperation operation;
        operation.id = id;
        operation.collection = true;
        operation.collectionRequest = request;
        state->pending.push_back(std::move(operation));
    }
    state->workAvailable.notify_one();
    return id;
}

std::vector<DesktopPlacementCoordinator::Event>
DesktopPlacementCoordinator::TakeEvents() {
    const std::shared_ptr<State> state = state_;
    std::vector<Event> result;
    std::lock_guard lock(state->mutex);
    result.reserve(state->events.size());
    while (!state->events.empty()) {
        result.push_back(std::move(state->events.front()));
        state->events.pop_front();
    }
    return result;
}

bool DesktopPlacementCoordinator::DrainFor(DWORD timeoutMilliseconds) {
    const std::shared_ptr<State> state = state_;
    std::unique_lock lock(state->mutex);
    return state->drained.wait_for(
        lock,
        std::chrono::milliseconds(timeoutMilliseconds),
        [&]() {
            return state->pending.empty() && state->activeOperations == 0;
        });
}

bool DesktopPlacementCoordinator::CancelAndStopFor(DWORD timeoutMilliseconds) {
    const std::shared_ptr<State> state = state_;
    if (state == nullptr) {
        return true;
    }

    DWORD workerThreadId = 0;
    {
        std::lock_guard lock(state->mutex);
        state->stopping = true;
        state->notificationWindow = nullptr;
        state->pending.clear();
        workerThreadId = state->workerThreadId;
        if (state->activeOperations == 0) {
            state->drained.notify_all();
        }
    }
    state->workAvailable.notify_all();
    if (workerThreadId != 0 && workerThreadId != GetCurrentThreadId()) {
        CoCancelCall(workerThreadId, 0);
    }

    bool workerFinished = false;
    {
        std::unique_lock lock(state->mutex);
        if (timeoutMilliseconds == INFINITE) {
            state->workerExited.wait(lock, [&]() {
                return state->workerFinished;
            });
            workerFinished = true;
        } else {
            workerFinished = state->workerExited.wait_for(
                lock,
                std::chrono::milliseconds(timeoutMilliseconds),
                [&]() { return state->workerFinished; });
        }
    }
    if (workerFinished && worker_.joinable()) {
        worker_.join();
    }
    return workerFinished;
}

void DesktopPlacementCoordinator::WorkerLoop(
    const std::shared_ptr<State>& state) {
    ComInit com;
    const HRESULT callCancellationResult = CoEnableCallCancellation(nullptr);
    const DPI_AWARENESS_CONTEXT previousDpiContext =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    {
        std::lock_guard lock(state->mutex);
        state->workerThreadId = GetCurrentThreadId();
    }

    for (;;) {
        QueuedOperation operation;
        {
            std::unique_lock lock(state->mutex);
            state->workAvailable.wait(lock, [&]() {
                return state->stopping || !state->pending.empty();
            });
            if (state->stopping && state->pending.empty()) {
                break;
            }
            operation = std::move(state->pending.front());
            state->pending.pop_front();
            ++state->activeOperations;
        }

        if (operation.collection) {
            Event completed;
            completed.id = operation.id;
            completed.stage = EventStage::Completed;
            completed.collection = true;
            completed.sourceWindow = operation.collectionRequest.sourceWindow;
            completed.showError = operation.collectionRequest.showError;
            try {
                completed.collectionResult =
                    CommitDesktopCollectionItemTransaction(
                        operation.collectionRequest);
            } catch (...) {
                completed.collectionResult.errorMessage =
                    L"后台收纳桌面项目时发生异常。";
            }
            completed.succeeded = completed.collectionResult.succeeded;
            Publish(state, std::move(completed));
            {
                std::lock_guard lock(state->mutex);
                --state->activeOperations;
                if (state->pending.empty() && state->activeOperations == 0) {
                    state->drained.notify_all();
                }
            }
            continue;
        }

        const DesktopPlacementRequest& request = operation.placementRequest;
        POINT finalPoint{};
        std::wstring errorMessage;
        std::wstring desktopPath = request.path;
        bool visiblePublished = false;
        bool succeeded = false;
        try {
            if (CommitDesktopMoveOutTransaction(
                    request,
                    desktopPath,
                    errorMessage)) {
                visiblePublished = true;
                Event visible;
                visible.id = operation.id;
                visible.stage = EventStage::Visible;
                visible.path = desktopPath;
                visible.finalPoint = request.screenPoint;
                visible.dragGhostGeneration =
                    request.dragGhostGeneration;
                visible.showError = request.showError;
                visible.sourceWindow = request.sourceWindow;
                visible.succeeded = true;
                Publish(state, std::move(visible));
                DesktopLayout desktopLayout;
                succeeded = desktopLayout.RestoreScreenPosition(
                    desktopPath,
                    request.screenPoint,
                    finalPoint,
                    errorMessage,
                    [&]() {
                        if (visiblePublished) {
                            return;
                        }
                        visiblePublished = true;
                        Event visible;
                        visible.id = operation.id;
                        visible.stage = EventStage::Visible;
                        visible.path = desktopPath;
                        visible.dragGhostGeneration =
                            request.dragGhostGeneration;
                        visible.showError = request.showError;
                        visible.sourceWindow = request.sourceWindow;
                        visible.succeeded = true;
                        Publish(state, std::move(visible));
                    },
                    [state]() {
                        std::lock_guard lock(state->mutex);
                        return state->stopping;
                    });
                if (succeeded) {
                    PersistDesktopPlacementSnapshot(
                        desktopPath,
                        finalPoint,
                        errorMessage);
                }
            }
        } catch (...) {
            errorMessage =
                L"后台桌面显示归属提交或图标定位发生异常；原件路径没有改变。";
        }

        Event completed;
        completed.id = operation.id;
        completed.stage = EventStage::Completed;
        completed.path = desktopPath;
        completed.finalPoint = finalPoint;
        completed.dragGhostGeneration =
            request.dragGhostGeneration;
        completed.showError = request.showError;
        completed.sourceWindow = request.sourceWindow;
        completed.succeeded = succeeded;
        completed.errorMessage = std::move(errorMessage);
        Publish(state, std::move(completed));

        {
            std::lock_guard lock(state->mutex);
            --state->activeOperations;
            if (state->pending.empty() && state->activeOperations == 0) {
                state->drained.notify_all();
            }
        }
    }

    if (previousDpiContext != nullptr) {
        SetThreadDpiAwarenessContext(previousDpiContext);
    }
    if (SUCCEEDED(callCancellationResult)) {
        CoDisableCallCancellation(nullptr);
    }
    {
        std::lock_guard lock(state->mutex);
        state->workerThreadId = 0;
        state->workerFinished = true;
    }
    state->workerExited.notify_all();
}

void DesktopPlacementCoordinator::Publish(
    const std::shared_ptr<State>& state,
    Event event) {
    HWND notificationWindow = nullptr;
    {
        std::lock_guard lock(state->mutex);
        if (state->stopping) {
            return;
        }
        state->events.push_back(std::move(event));
        notificationWindow = state->notificationWindow;
    }
    if (notificationWindow != nullptr) {
        PostMessageW(notificationWindow, kDesktopPlacementEventMessage, 0, 0);
    }
}
