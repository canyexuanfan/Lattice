#pragma once

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config/ConfigStore.h"

constexpr UINT kDesktopPlacementEventMessage = WM_APP + 17;
constexpr UINT kDesktopPlacementRequestMessage = WM_APP + 18;
constexpr UINT kDesktopCollectionRequestMessage = WM_APP + 22;
constexpr UINT kDesktopCollectionResultMessage = WM_APP + 23;

struct DesktopPlacementRequest {
    std::wstring path;
    POINT screenPoint{};
    std::uint64_t dragGhostGeneration = 0;
    bool showError = true;
    HWND sourceWindow = nullptr;
    bool commitMoveOut = false;
    std::wstring itemId;
    std::wstring sourcePath;
    ItemConfig itemState;
};

bool CommitDesktopMoveOutTransaction(
    const DesktopPlacementRequest& request,
    std::wstring& desktopPath,
    std::wstring& errorMessage);

struct DesktopCollectionItemRequest {
    std::wstring categoryId;
    std::wstring path;
    std::wstring sourceVisibleId;
    size_t insertionIndex = 0;
    POINT desktopPoint{};
    bool hasDesktopPoint = false;
    bool showError = true;
    HWND sourceWindow = nullptr;
};

struct DesktopCollectionItemResult {
    bool succeeded = false;
    std::wstring itemId;
    std::wstring sourcePath;
    std::wstring destinationPath;
    ItemConfig itemState;
    std::wstring errorMessage;
};

DesktopCollectionItemResult CommitDesktopCollectionItemTransaction(
    const DesktopCollectionItemRequest& request);

class DesktopPlacementCoordinator {
public:
    using OperationId = std::uint64_t;

    enum class EventStage {
        Visible,
        Completed,
    };

    struct Event {
        OperationId id = 0;
        EventStage stage = EventStage::Completed;
        std::wstring path;
        POINT finalPoint{};
        std::uint64_t dragGhostGeneration = 0;
        bool showError = true;
        HWND sourceWindow = nullptr;
        bool succeeded = false;
        std::wstring errorMessage;
        bool collection = false;
        DesktopCollectionItemResult collectionResult;
    };

    DesktopPlacementCoordinator();
    ~DesktopPlacementCoordinator();

    DesktopPlacementCoordinator(const DesktopPlacementCoordinator&) = delete;
    DesktopPlacementCoordinator& operator=(const DesktopPlacementCoordinator&) = delete;

    void AttachNotificationWindow(HWND hwnd);
    void DetachNotificationWindow(HWND hwnd);
    OperationId PlaceAtScreenAsync(const DesktopPlacementRequest& request);
    OperationId CollectItemAsync(const DesktopCollectionItemRequest& request);
    std::vector<Event> TakeEvents();
    bool DrainFor(DWORD timeoutMilliseconds);
    bool CancelAndStopFor(DWORD timeoutMilliseconds);

private:
    struct QueuedOperation {
        OperationId id = 0;
        bool collection = false;
        DesktopPlacementRequest placementRequest;
        DesktopCollectionItemRequest collectionRequest;
    };
    struct State;

    static void WorkerLoop(const std::shared_ptr<State>& state);
    static void Publish(const std::shared_ptr<State>& state, Event event);

    std::shared_ptr<State> state_;
    std::thread worker_;
};
