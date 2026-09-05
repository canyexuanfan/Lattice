#pragma once

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

constexpr UINT kDesktopPlacementEventMessage = WM_APP + 17;
constexpr UINT kDesktopPlacementRequestMessage = WM_APP + 18;

struct DesktopPlacementRequest {
    std::wstring path;
    POINT screenPoint{};
    std::uint64_t dragGhostGeneration = 0;
    bool showError = true;
    HWND sourceWindow = nullptr;
};

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
    };

    DesktopPlacementCoordinator();
    ~DesktopPlacementCoordinator();

    DesktopPlacementCoordinator(const DesktopPlacementCoordinator&) = delete;
    DesktopPlacementCoordinator& operator=(const DesktopPlacementCoordinator&) = delete;

    void AttachNotificationWindow(HWND hwnd);
    void DetachNotificationWindow(HWND hwnd);
    OperationId PlaceAtScreenAsync(const DesktopPlacementRequest& request);
    std::vector<Event> TakeEvents();
    bool DrainFor(DWORD timeoutMilliseconds);
    bool CancelAndStopFor(DWORD timeoutMilliseconds);

private:
    struct QueuedPlacement {
        OperationId id = 0;
        DesktopPlacementRequest request;
    };
    struct State;

    static void WorkerLoop(const std::shared_ptr<State>& state);
    static void Publish(const std::shared_ptr<State>& state, Event event);

    std::shared_ptr<State> state_;
    std::thread worker_;
};
