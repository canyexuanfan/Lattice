#include "desktop/DesktopPlacementCoordinator.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

#include "desktop/DesktopLayout.h"
#include "util/ComInit.h"

struct DesktopPlacementCoordinator::State {
    std::mutex mutex;
    std::condition_variable workAvailable;
    std::condition_variable drained;
    std::condition_variable workerExited;
    std::deque<QueuedPlacement> pending;
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
        state->pending.push_back(QueuedPlacement{id, request});
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
        QueuedPlacement placement;
        {
            std::unique_lock lock(state->mutex);
            state->workAvailable.wait(lock, [&]() {
                return state->stopping || !state->pending.empty();
            });
            if (state->stopping && state->pending.empty()) {
                break;
            }
            placement = std::move(state->pending.front());
            state->pending.pop_front();
            ++state->activeOperations;
        }

        POINT finalPoint{};
        std::wstring errorMessage;
        bool visiblePublished = false;
        bool succeeded = false;
        try {
            DesktopLayout desktopLayout;
            succeeded = desktopLayout.RestoreScreenPosition(
                placement.request.path,
                placement.request.screenPoint,
                finalPoint,
                errorMessage,
                [&]() {
                    if (visiblePublished) {
                        return;
                    }
                    visiblePublished = true;
                    Event visible;
                    visible.id = placement.id;
                    visible.stage = EventStage::Visible;
                    visible.path = placement.request.path;
                    visible.dragGhostGeneration =
                        placement.request.dragGhostGeneration;
                    visible.showError = placement.request.showError;
                    visible.sourceWindow = placement.request.sourceWindow;
                    visible.succeeded = true;
                    Publish(state, std::move(visible));
                },
                [state]() {
                    std::lock_guard lock(state->mutex);
                    return state->stopping;
                });
        } catch (...) {
            errorMessage =
                L"文件已安全归还桌面，但后台桌面定位发生异常。";
        }

        Event completed;
        completed.id = placement.id;
        completed.stage = EventStage::Completed;
        completed.path = placement.request.path;
        completed.finalPoint = finalPoint;
        completed.dragGhostGeneration =
            placement.request.dragGhostGeneration;
        completed.showError = placement.request.showError;
        completed.sourceWindow = placement.request.sourceWindow;
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
