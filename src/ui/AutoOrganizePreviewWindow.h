#pragma once

#include <Windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "organize/AutoOrganizeLayout.h"
#include "organize/AutoOrganizer.h"
#include "rendering/D2DContext.h"
#include "rendering/IconCache.h"

struct AutoOrganizePreviewInput {
    lattice::organize::Snapshot snapshot;
    lattice::organize::LayoutContext layoutContext;
    bool undoAvailable = false;
};

class AutoOrganizePreviewWindow {
public:
    using InputProvider = std::function<AutoOrganizePreviewInput()>;
    using ApplyHandler = std::function<void(
        const lattice::organize::Plan&,
        const lattice::organize::LayoutPlan&,
        HWND)>;
    using UndoHandler = std::function<void(HWND)>;

    AutoOrganizePreviewWindow(
        HINSTANCE instance,
        HWND owner,
        InputProvider inputProvider,
        ApplyHandler applyHandler,
        UndoHandler undoHandler = {});
    ~AutoOrganizePreviewWindow();

    bool Create();
    void ShowOrActivate();
    void Close();
    bool IsOpen() const noexcept;
    HWND Window() const noexcept { return hwnd_; }
    void MarkDesktopChanged();
    void CompleteApply(bool succeeded, const std::wstring& message);
    void CompleteUndo(
        bool succeeded,
        bool conflict,
        const std::wstring& message);

private:
    friend struct AutoOrganizePreviewWindowSmokeAccess;

    enum class ViewState {
        Scanning,
        Ready,
        Empty,
        Cancelled,
        Changed,
        Unplaced,
        Applying,
        ApplyError,
        Success,
        ScanError,
        Undoing,
        UndoSuccess,
        UndoConflict,
        UndoError,
    };

    enum class HitKind {
        None,
        Close,
        Minimize,
        Navigation,
        Monitor,
        Item,
        ItemCheck,
        Group,
        Position,
        Regenerate,
        Cancel,
        Apply,
        OverlayAction,
    };

    struct HitTarget {
        HitKind kind = HitKind::None;
        int index = -1;
        D2D1_RECT_F bounds{};
        std::wstring tooltip;
    };

    struct ScanResult {
        unsigned int generation = 0;
        lattice::organize::Plan plan;
        lattice::organize::LayoutPlan layout;
        std::wstring error;
    };

    static LRESULT CALLBACK WindowProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void StartScan();
    void CancelScan(bool showCancelledState);
    void AdoptScanResult(ScanResult* rawResult);
    void Render();
    void RenderBase();
    void RenderOverlay();
    void RebuildHitTargets();
    void UpdateHover(POINT clientPixels);
    void ActivateHit(const HitTarget& hit);
    const HitTarget* HitTest(POINT clientPixels) const;
    bool IsHovered(HitKind kind, int index = 0) const;
    D2D1_POINT_2F PointInDips(POINT clientPixels) const;
    const lattice::organize::Decision* SelectedDecision() const;
    std::vector<const lattice::organize::GroupPlan*> VisibleGroups() const;
    std::vector<const lattice::organize::Decision*> DecisionsForGroup(
        const lattice::organize::GroupPlan& group) const;
    void SelectNextFocusable(int direction);
    void MoveDecisionToGroup(int decisionIndex, int visibleGroupIndex);
    void KeepDecisionOnDesktop(int decisionIndex);
    void ReplanCandidateLayouts();
    void ShowGroupMenu(int visibleGroupIndex, POINT screenPoint);
    void ScrollPreview(POINT clientPixels, int wheelDelta);
    void EnsureTextFormats();
    void DrawText(
        const std::wstring& text,
        const D2D1_RECT_F& bounds,
        IDWriteTextFormat* format,
        D2D1_COLOR_F color);
    void FillRect(const D2D1_RECT_F& bounds, D2D1_COLOR_F color);
    void StrokeRect(
        const D2D1_RECT_F& bounds,
        D2D1_COLOR_F color,
        float width = 1.0f,
        ID2D1StrokeStyle* style = nullptr);
    void FillRounded(
        const D2D1_RECT_F& bounds,
        float radius,
        D2D1_COLOR_F color);
    void StrokeRounded(
        const D2D1_RECT_F& bounds,
        float radius,
        D2D1_COLOR_F color,
        float width = 1.0f,
        ID2D1StrokeStyle* style = nullptr);

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    InputProvider inputProvider_;
    ApplyHandler applyHandler_;
    UndoHandler undoHandler_;
    D2DContext d2d_;
    IconCache iconCache_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> smallFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> tinyFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> headingFormat_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashedStroke_;
    AutoOrganizePreviewInput input_;
    lattice::organize::Plan plan_;
    lattice::organize::LayoutPlan layoutPlan_;
    std::thread scanThread_;
    std::atomic<bool> cancelRequested_{false};
    unsigned int scanGeneration_ = 0;
    ViewState state_ = ViewState::Scanning;
    std::wstring stateMessage_;
    std::vector<HitTarget> hitTargets_;
    int hoverHit_ = -1;
    int pressedHit_ = -1;
    int focusedHit_ = -1;
    int selectedDecision_ = -1;
    int navigationFilter_ = 0;
    int monitorFilter_ = 0;
    POINT dragOriginPixels_{};
    int draggingPosition_ = -1;
    int draggingDecision_ = -1;
    bool draggingDecisionMoved_ = false;
    int groupScrollOffset_ = 0;
    std::map<std::wstring, int> groupRowOffsets_;
    bool desktopChangeBlocksApply_ = false;
    bool trackingMouse_ = false;
    UINT dpi_ = 96;
};
