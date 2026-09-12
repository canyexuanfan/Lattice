#include "ui/AutoOrganizePreviewWindow.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "app/resource.h"
#include "ui/InputDialog.h"

namespace {

constexpr wchar_t kClassName[] = L"Lattice.AutoOrganizePreviewWindow";
constexpr UINT kScanCompleteMessage = WM_APP + 61;
constexpr UINT kTooltipTimer = 8701;
constexpr float kWindowWidth = 1468.0f;
constexpr float kWindowHeight = 830.0f;
constexpr float kTitleHeight = 48.0f;
constexpr float kFooterHeight = 72.0f;
constexpr float kSidebarWidth = 210.0f;
constexpr float kDetailWidth = 354.0f;
constexpr wchar_t kKeepDesktopGroupId[] = L"__lattice_keep_desktop__";

D2D1_COLOR_F Color(unsigned int rgb, float alpha = 1.0f) {
    return D2D1::ColorF(rgb, alpha);
}

bool Contains(const D2D1_RECT_F& bounds, D2D1_POINT_2F point) {
    return point.x >= bounds.left && point.x < bounds.right &&
        point.y >= bounds.top && point.y < bounds.bottom;
}

bool SourceIsDesktop(const lattice::organize::Decision& decision) {
    return decision.sourceCategoryId.empty();
}

std::wstring ConfidenceText(const lattice::organize::Decision& decision) {
    if (!lattice::organize::IsOwnershipAdjustment(decision)) {
        return L"保持现状";
    }
    if (!decision.selected &&
        decision.confidence == lattice::organize::Confidence::High) {
        return L"高置信 · 当前保持";
    }
    if (decision.selected &&
        decision.confidence == lattice::organize::Confidence::Medium) {
        return L"需要确认 · 已选择调整";
    }
    switch (decision.confidence) {
        case lattice::organize::Confidence::High:
            return L"高置信 · 默认调整";
        case lattice::organize::Confidence::Medium:
            return L"需要确认 · 默认保持";
        case lattice::organize::Confidence::Low:
        default:
            return L"低置信 · 保持原位";
    }
}

std::wstring SourceName(const lattice::organize::Decision& decision) {
    if (decision.sourceCategoryId.empty()) return L"桌面";
    return decision.sourceCategoryName.empty()
        ? L"已有格子" : decision.sourceCategoryName;
}

std::wstring MonitorLabel(int index) {
    return L"显示器 " + std::to_wstring(index + 1);
}

}  // namespace

AutoOrganizePreviewWindow::AutoOrganizePreviewWindow(
    HINSTANCE instance,
    HWND owner,
    InputProvider inputProvider,
    ApplyHandler applyHandler,
    UndoHandler undoHandler)
    : instance_(instance),
      owner_(owner),
      inputProvider_(std::move(inputProvider)),
      applyHandler_(std::move(applyHandler)),
      undoHandler_(std::move(undoHandler)) {}

AutoOrganizePreviewWindow::~AutoOrganizePreviewWindow() {
    Close();
}

bool AutoOrganizePreviewWindow::IsOpen() const noexcept {
    return hwnd_ != nullptr && IsWindow(hwnd_) != FALSE;
}

bool AutoOrganizePreviewWindow::Create() {
    if (IsOpen()) return true;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = AutoOrganizePreviewWindow::WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        16, 16, LR_SHARED));
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    HMONITOR monitor = MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        monitorInfo.rcWork = RECT{0, 0, 1920, 1080};
    }
    const UINT dpi = owner_ != nullptr && IsWindow(owner_) != FALSE
        ? GetDpiForWindow(owner_) : GetDpiForSystem();
    const int availableWidth = (std::max)(1, static_cast<int>(
        monitorInfo.rcWork.right - monitorInfo.rcWork.left - 16));
    const int availableHeight = (std::max)(1, static_cast<int>(
        monitorInfo.rcWork.bottom - monitorInfo.rcWork.top - 16));
    const int width = (std::min)(availableWidth,
        MulDiv(static_cast<int>(kWindowWidth), static_cast<int>(dpi), 96));
    const int height = (std::min)(availableHeight,
        MulDiv(static_cast<int>(kWindowHeight), static_cast<int>(dpi), 96));
    const int x = monitorInfo.rcWork.left +
        ((monitorInfo.rcWork.right - monitorInfo.rcWork.left) - width) / 2;
    const int y = monitorInfo.rcWork.top +
        ((monitorInfo.rcWork.bottom - monitorInfo.rcWork.top) - height) / 2;

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        kClassName,
        L"Lattice 自动整理预览",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX,
        x, y, width, height,
        owner_, nullptr, instance_, this);
    return hwnd_ != nullptr;
}

void AutoOrganizePreviewWindow::ShowOrActivate() {
    if (!Create()) return;
    ShowWindow(hwnd_, IsIconic(hwnd_) != FALSE ? SW_RESTORE : SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    if (plan_.decisions.empty() && state_ != ViewState::Scanning) StartScan();
}

void AutoOrganizePreviewWindow::Close() {
    cancelRequested_.store(true);
    if (scanThread_.joinable()) scanThread_.join();
    if (IsOpen()) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void AutoOrganizePreviewWindow::MarkDesktopChanged() {
    if (state_ != ViewState::Ready && state_ != ViewState::Changed &&
        state_ != ViewState::Unplaced) return;
    if (!inputProvider_) return;

    AutoOrganizePreviewInput current;
    try {
        current = inputProvider_();
    } catch (...) {
        state_ = ViewState::Changed;
        desktopChangeBlocksApply_ = true;
        stateMessage_ = L"无法重新校验桌面状态，请重新生成建议后再应用。";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    bool relatedChange = !lattice::organize::IsMonitorContextCurrent(
        layoutPlan_.monitorContextSignature,
        current.layoutContext.monitors);
    bool anyChange = current.snapshot.configRevision !=
            input_.snapshot.configRevision ||
        current.snapshot.items.size() != input_.snapshot.items.size() ||
        current.snapshot.categories.size() != input_.snapshot.categories.size() ||
        relatedChange;
    if (!anyChange) {
        for (const lattice::organize::ItemSnapshot& previous :
             input_.snapshot.items) {
            const auto item = std::find_if(
                current.snapshot.items.begin(), current.snapshot.items.end(),
                [&](const lattice::organize::ItemSnapshot& value) {
                    return value.id == previous.id;
                });
            if (item == current.snapshot.items.end() || item->missing != previous.missing ||
                CompareStringOrdinal(
                    item->parsingIdentity.c_str(), -1,
                    previous.parsingIdentity.c_str(), -1, TRUE) != CSTR_EQUAL ||
                item->sourceCategoryId != previous.sourceCategoryId ||
                item->sourceIndex != previous.sourceIndex) {
                anyChange = true;
                break;
            }
        }
    }
    if (!anyChange) return;
    for (const lattice::organize::Decision& decision : plan_.decisions) {
        if (!decision.selected || decision.targetCategoryId.empty() ||
            decision.targetCategoryId == decision.sourceCategoryId) continue;
        const auto item = std::find_if(
            current.snapshot.items.begin(), current.snapshot.items.end(),
            [&](const lattice::organize::ItemSnapshot& value) {
                return value.id == decision.itemId;
            });
        if (item == current.snapshot.items.end() || item->missing ||
            CompareStringOrdinal(
                item->parsingIdentity.c_str(), -1,
                decision.parsingIdentity.c_str(), -1, TRUE) != CSTR_EQUAL ||
            item->sourceCategoryId != decision.sourceCategoryId ||
            item->sourceIndex != decision.sourceIndex) {
            relatedChange = true;
            break;
        }
        const auto category = std::find_if(
            current.snapshot.categories.begin(),
            current.snapshot.categories.end(),
            [&](const lattice::organize::ExistingCategorySnapshot& value) {
                return value.id == decision.targetCategoryId;
            });
        if (decision.targetIsExistingCategory &&
            (category == current.snapshot.categories.end() || category->locked)) {
            relatedChange = true;
            break;
        }
        if (!decision.sourceCategoryId.empty() &&
            decision.sourceCategoryId != L"uncategorized") {
            const auto source = std::find_if(
                current.snapshot.categories.begin(),
                current.snapshot.categories.end(),
                [&](const lattice::organize::ExistingCategorySnapshot& value) {
                    return value.id == decision.sourceCategoryId;
                });
            if (source == current.snapshot.categories.end() || source->locked) {
                relatedChange = true;
                break;
            }
        }
    }
    input_.undoAvailable = current.undoAvailable;
    if (state_ == ViewState::Unplaced && !relatedChange) return;
    state_ = ViewState::Changed;
    desktopChangeBlocksApply_ = relatedChange;
    stateMessage_ = relatedChange
        ? L"涉及项目、格子或显示器布局已经变化，请重新生成建议后再应用。"
        : L"桌面出现了与本计划无关的新变化；当前建议仍可应用，新项目本次不包含。";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AutoOrganizePreviewWindow::CompleteApply(
    bool succeeded,
    const std::wstring& message) {
    state_ = succeeded ? ViewState::Success : ViewState::ApplyError;
    desktopChangeBlocksApply_ = false;
    if (succeeded) input_.undoAvailable = true;
    stateMessage_ = message;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AutoOrganizePreviewWindow::CompleteUndo(
    bool succeeded,
    bool conflict,
    const std::wstring& message) {
    state_ = succeeded
        ? ViewState::UndoSuccess
        : conflict ? ViewState::UndoConflict : ViewState::UndoError;
    stateMessage_ = message;
    desktopChangeBlocksApply_ = false;
    input_.undoAvailable = !succeeded;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK AutoOrganizePreviewWindow::WindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    AutoOrganizePreviewWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<AutoOrganizePreviewWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<AutoOrganizePreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return window == nullptr
        ? DefWindowProcW(hwnd, message, wParam, lParam)
        : window->HandleMessage(message, wParam, lParam);
}

LRESULT AutoOrganizePreviewWindow::HandleMessage(
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            dpi_ = GetDpiForWindow(hwnd_);
            d2d_.Initialize(hwnd_);
            iconCache_.SetCapacity(128);
            iconCache_.SetInvalidateCallback([hwnd = hwnd_]() {
                if (IsWindow(hwnd) != FALSE) InvalidateRect(hwnd, nullptr, FALSE);
            });
            EnsureTextFormats();
            StartScan();
            return 0;
        case WM_CLOSE:
            CancelScan(false);
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kTooltipTimer);
            iconCache_.SetInvalidateCallback(nullptr);
            cancelRequested_.store(true);
            hwnd_ = nullptr;
            return 0;
        case WM_NCCALCSIZE:
            if (wParam != 0) return 0;
            break;
        case WM_NCHITTEST: {
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT window{};
            GetWindowRect(hwnd_, &window);
            const int grip = MulDiv(7, static_cast<int>(dpi_), 96);
            const bool left = screen.x < window.left + grip;
            const bool right = screen.x >= window.right - grip;
            const bool top = screen.y < window.top + grip;
            const bool bottom = screen.y >= window.bottom - grip;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            POINT client = screen;
            ScreenToClient(hwnd_, &client);
            const D2D1_POINT_2F dip = PointInDips(client);
            if (dip.y < kTitleHeight && dip.x < kWindowWidth - 96.0f) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            d2d_.RecreateTarget(hwnd_);
            textFormat_.Reset(); smallFormat_.Reset(); tinyFormat_.Reset();
            titleFormat_.Reset(); headingFormat_.Reset();
            EnsureTextFormats();
            return 0;
        }
        case WM_SIZE:
            d2d_.Resize(LOWORD(lParam), HIWORD(lParam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize.x = MulDiv(1040, static_cast<int>(dpi_), 96);
            limits->ptMinTrackSize.y = MulDiv(650, static_cast<int>(dpi_), 96);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            UpdateHover(point);
            if (draggingDecision_ >= 0 && GetCapture() == hwnd_) {
                draggingDecisionMoved_ = draggingDecisionMoved_ ||
                    std::abs(point.x - dragOriginPixels_.x) >= GetSystemMetrics(SM_CXDRAG) ||
                    std::abs(point.y - dragOriginPixels_.y) >= GetSystemMetrics(SM_CYDRAG);
            }
            if (draggingPosition_ >= 0 && GetCapture() == hwnd_ &&
                draggingPosition_ < static_cast<int>(layoutPlan_.placements.size())) {
                auto& placement = layoutPlan_.placements[static_cast<size_t>(draggingPosition_)];
                const auto monitor = std::find_if(
                    input_.layoutContext.monitors.begin(),
                    input_.layoutContext.monitors.end(),
                    [&](const auto& value) {
                        return value.id == placement.monitorId;
                    });
                if (monitor != input_.layoutContext.monitors.end() &&
                    d2d_.Target() != nullptr) {
                    const D2D1_SIZE_F size = d2d_.Target()->GetSize();
                    const float footerTop = size.height - kFooterHeight;
                    const float detailLeft = size.width - kDetailWidth;
                    const D2D1_RECT_F map = D2D1::RectF(
                        detailLeft + 17, footerTop - 136,
                        size.width - 17, footerTop - 17);
                    const float sx = (map.right - map.left - 16) /
                        static_cast<float>((std::max)(1, monitor->workArea.Width()));
                    const float sy = (map.bottom - map.top - 16) /
                        static_cast<float>((std::max)(1, monitor->workArea.Height()));
                    const D2D1_POINT_2F currentDip = PointInDips(point);
                    const D2D1_POINT_2F originDip = PointInDips(dragOriginPixels_);
                    const int dx = static_cast<int>(std::lround(
                        (currentDip.x - originDip.x) / (std::max)(sx, .001f)));
                    const int dy = static_cast<int>(std::lround(
                        (currentDip.y - originDip.y) / (std::max)(sy, .001f)));
                    lattice::organize::RectI proposed = placement.bounds;
                    proposed.left += dx; proposed.right += dx;
                    proposed.top += dy; proposed.bottom += dy;
                    bool valid = lattice::organize::ContainsRectangle(
                        monitor->workArea, proposed);
                    for (const auto& existing : input_.layoutContext.existingWidgets) {
                        valid = valid && (existing.monitorId != placement.monitorId ||
                            !lattice::organize::RectanglesOverlap(
                                existing.bounds, proposed));
                    }
                    for (std::size_t index = 0;
                         valid && index < layoutPlan_.placements.size(); ++index) {
                        if (static_cast<int>(index) == draggingPosition_) continue;
                        const auto& other = layoutPlan_.placements[index];
                        valid = other.monitorId != placement.monitorId ||
                            !other.placed || !lattice::organize::RectanglesOverlap(
                                other.bounds, proposed);
                    }
                    if (valid) {
                        placement.bounds = proposed;
                        placement.manuallyPositioned = true;
                        dragOriginPixels_ = point;
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            hoverHit_ = -1;
            KillTimer(hwnd_, kTooltipTimer);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd_);
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const HitTarget* hit = HitTest(point);
            pressedHit_ = hit == nullptr ? -1 : static_cast<int>(hit - hitTargets_.data());
            focusedHit_ = pressedHit_;
            dragOriginPixels_ = point;
            if (hit != nullptr && hit->kind == HitKind::Position) {
                draggingPosition_ = hit->index;
                SetCapture(hwnd_);
            } else if (hit != nullptr && hit->kind == HitKind::Item) {
                draggingDecision_ = hit->index;
                draggingDecisionMoved_ = false;
                SetCapture(hwnd_);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const HitTarget* hit = HitTest(point);
            const int released = hit == nullptr ? -1 : static_cast<int>(hit - hitTargets_.data());
            if (draggingPosition_ >= 0) {
                draggingPosition_ = -1;
                if (GetCapture() == hwnd_) ReleaseCapture();
            } else if (draggingDecision_ >= 0) {
                const int decisionIndex = draggingDecision_;
                draggingDecision_ = -1;
                if (GetCapture() == hwnd_) ReleaseCapture();
                if (hit != nullptr && hit->kind == HitKind::Group) {
                    MoveDecisionToGroup(decisionIndex, hit->index);
                } else if (hit != nullptr && hit->kind == HitKind::Navigation &&
                           hit->index == 2) {
                    KeepDecisionOnDesktop(decisionIndex);
                } else if (!draggingDecisionMoved_ && hit != nullptr &&
                           hit->kind == HitKind::Item) {
                    ActivateHit(*hit);
                }
                draggingDecisionMoved_ = false;
            } else if (released >= 0 && released == pressedHit_) {
                ActivateHit(hitTargets_[static_cast<size_t>(released)]);
            }
            pressedHit_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            draggingPosition_ = -1;
            draggingDecision_ = -1;
            draggingDecisionMoved_ = false;
            pressedHit_ = -1;
            return 0;
        case WM_RBUTTONUP: {
            POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const HitTarget* hit = HitTest(client);
            if (hit != nullptr && hit->kind == HitKind::Group) {
                POINT screen = client;
                ClientToScreen(hwnd_, &screen);
                ShowGroupMenu(hit->index, screen);
                return 0;
            }
            break;
        }
        case WM_MOUSEWHEEL: {
            POINT screen{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &screen);
            ScrollPreview(screen, GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                if (state_ == ViewState::Scanning) CancelScan(true);
                else ShowWindow(hwnd_, SW_HIDE);
                return 0;
            }
            if (wParam == VK_TAB) {
                SelectNextFocusable((GetKeyState(VK_SHIFT) & 0x8000) != 0 ? -1 : 1);
                return 0;
            }
            if ((wParam == VK_RETURN || wParam == VK_SPACE) && focusedHit_ >= 0 &&
                focusedHit_ < static_cast<int>(hitTargets_.size())) {
                ActivateHit(hitTargets_[static_cast<size_t>(focusedHit_)]);
                return 0;
            }
            break;
        case WM_TIMER:
            if (wParam == kTooltipTimer) {
                KillTimer(hwnd_, kTooltipTimer);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case kScanCompleteMessage:
            AdoptScanResult(reinterpret_cast<ScanResult*>(lParam));
            return 0;
        case WM_PAINT:
            Render();
            return 0;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void AutoOrganizePreviewWindow::StartScan() {
    CancelScan(false);
    if (!inputProvider_) {
        state_ = ViewState::ScanError;
        stateMessage_ = L"无法读取当前桌面与格子快照。";
        return;
    }
    try {
        input_ = inputProvider_();
    } catch (...) {
        state_ = ViewState::ScanError;
        stateMessage_ = L"读取当前快照时发生错误，配置未改变。";
        return;
    }
    state_ = ViewState::Scanning;
    desktopChangeBlocksApply_ = false;
    stateMessage_ = L"正在本机分析桌面项目、已有归属和显示器布局…";
    plan_ = {}; layoutPlan_ = {}; selectedDecision_ = -1; hoverHit_ = -1;
    cancelRequested_.store(false);
    const unsigned int generation = ++scanGeneration_;
    const HWND notificationWindow = hwnd_;
    AutoOrganizePreviewInput inputCopy = input_;
    scanThread_ = std::thread(
        [notificationWindow, generation, input = std::move(inputCopy),
         cancel = &cancelRequested_]() mutable {
            auto result = std::make_unique<ScanResult>();
            result->generation = generation;
            try {
                if (!cancel->load()) {
                    lattice::organize::EnrichSnapshotLocalMetadata(
                        input.snapshot, cancel);
                    result->plan = lattice::organize::BuildPlan(input.snapshot);
                }
                if (!cancel->load()) {
                    std::vector<lattice::organize::CandidateWidgetLayout> candidates;
                    for (const auto& group : result->plan.groups) {
                        if (!group.createNewCategory) continue;
                        lattice::organize::CandidateWidgetLayout candidate;
                        candidate.groupId = group.id;
                        candidate.name = group.name;
                        candidate.monitorId = group.monitorId;
                        candidate.itemCount = group.itemIds.size();
                        candidate.desiredHeight = static_cast<int>(86 + group.itemIds.size() * 74);
                        candidates.push_back(std::move(candidate));
                    }
                    result->layout = lattice::organize::PlanWidgetLayout(input.layoutContext, candidates);
                }
            } catch (...) {
                result->error = L"本地分类计算失败，未修改任何配置。";
            }
            if (cancel->load()) return;
            if (PostMessageW(notificationWindow, kScanCompleteMessage, generation,
                    reinterpret_cast<LPARAM>(result.get())) != FALSE) result.release();
        });
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AutoOrganizePreviewWindow::CancelScan(bool showCancelledState) {
    cancelRequested_.store(true);
    ++scanGeneration_;
    if (scanThread_.joinable()) scanThread_.join();
    if (showCancelledState) {
        state_ = ViewState::Cancelled;
        stateMessage_ = L"扫描已取消。桌面、格子和配置保持原样。";
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void AutoOrganizePreviewWindow::AdoptScanResult(ScanResult* rawResult) {
    std::unique_ptr<ScanResult> result(rawResult);
    if (result == nullptr || result->generation != scanGeneration_) return;
    if (scanThread_.joinable()) scanThread_.join();
    if (!result->error.empty()) {
        state_ = ViewState::ScanError;
        stateMessage_ = result->error;
        return;
    }
    plan_ = std::move(result->plan);
    layoutPlan_ = std::move(result->layout);
    const auto firstSuggestion = std::find_if(
        plan_.decisions.begin(), plan_.decisions.end(),
        lattice::organize::IsOwnershipAdjustment);
    selectedDecision_ = firstSuggestion == plan_.decisions.end()
        ? -1
        : static_cast<int>(firstSuggestion - plan_.decisions.begin());
    const bool hasSuggestions = firstSuggestion != plan_.decisions.end();
    const bool hasUnplaced = std::any_of(layoutPlan_.placements.begin(), layoutPlan_.placements.end(),
        [](const auto& placement) { return !placement.placed; });
    state_ = !hasSuggestions ? ViewState::Empty : hasUnplaced ? ViewState::Unplaced : ViewState::Ready;
    desktopChangeBlocksApply_ = false;
    stateMessage_ = hasUnplaced
        ? L"至少一个获准新建的格子尚未放置，应用已禁用。"
        : L"建议已在本机生成。确认前不会写入配置或移动真实文件。";
    for (const auto& item : input_.snapshot.items) {
        if (!item.path.empty()) iconCache_.Preload(item.path);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AutoOrganizePreviewWindow::EnsureTextFormats() {
    IDWriteFactory* factory = d2d_.WriteFactory();
    if (factory == nullptr || textFormat_ != nullptr) return;
    constexpr wchar_t font[] = L"Segoe UI";
    factory->CreateTextFormat(font, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14.0f, L"zh-CN", textFormat_.GetAddressOf());
    factory->CreateTextFormat(font, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        12.0f, L"zh-CN", smallFormat_.GetAddressOf());
    factory->CreateTextFormat(font, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        10.0f, L"zh-CN", tinyFormat_.GetAddressOf());
    factory->CreateTextFormat(font, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        16.0f, L"zh-CN", titleFormat_.GetAddressOf());
    factory->CreateTextFormat(font, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        21.0f, L"zh-CN", headingFormat_.GetAddressOf());
    D2D1_STROKE_STYLE_PROPERTIES properties{};
    properties.dashStyle = D2D1_DASH_STYLE_DASH;
    if (d2d_.Factory() != nullptr) {
        d2d_.Factory()->CreateStrokeStyle(
            properties, nullptr, 0, dashedStroke_.GetAddressOf());
    }
}

void AutoOrganizePreviewWindow::DrawText(
    const std::wstring& text,
    const D2D1_RECT_F& bounds,
    IDWriteTextFormat* format,
    D2D1_COLOR_F color) {
    if (text.empty() || format == nullptr || d2d_.Target() == nullptr) return;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    d2d_.Target()->CreateSolidColorBrush(color, brush.GetAddressOf());
    d2d_.Target()->DrawTextW(
        text.c_str(), static_cast<UINT32>(text.size()), format,
        bounds, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void AutoOrganizePreviewWindow::FillRect(
    const D2D1_RECT_F& bounds,
    D2D1_COLOR_F color) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    d2d_.Target()->CreateSolidColorBrush(color, brush.GetAddressOf());
    d2d_.Target()->FillRectangle(bounds, brush.Get());
}

void AutoOrganizePreviewWindow::StrokeRect(
    const D2D1_RECT_F& bounds,
    D2D1_COLOR_F color,
    float width,
    ID2D1StrokeStyle* style) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    d2d_.Target()->CreateSolidColorBrush(color, brush.GetAddressOf());
    d2d_.Target()->DrawRectangle(bounds, brush.Get(), width, style);
}

void AutoOrganizePreviewWindow::FillRounded(
    const D2D1_RECT_F& bounds,
    float radius,
    D2D1_COLOR_F color) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    d2d_.Target()->CreateSolidColorBrush(color, brush.GetAddressOf());
    d2d_.Target()->FillRoundedRectangle(
        D2D1::RoundedRect(bounds, radius, radius), brush.Get());
}

void AutoOrganizePreviewWindow::StrokeRounded(
    const D2D1_RECT_F& bounds,
    float radius,
    D2D1_COLOR_F color,
    float width,
    ID2D1StrokeStyle* style) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    d2d_.Target()->CreateSolidColorBrush(color, brush.GetAddressOf());
    d2d_.Target()->DrawRoundedRectangle(
        D2D1::RoundedRect(bounds, radius, radius), brush.Get(), width, style);
}

std::vector<const lattice::organize::GroupPlan*>
AutoOrganizePreviewWindow::VisibleGroups() const {
    std::vector<const lattice::organize::GroupPlan*> result;
    for (const auto& group : plan_.groups) {
        if (monitorFilter_ > 0 &&
            static_cast<size_t>(monitorFilter_ - 1) < input_.layoutContext.monitors.size() &&
            group.monitorId != input_.layoutContext.monitors[static_cast<size_t>(monitorFilter_ - 1)].id) {
            if (group.id != kKeepDesktopGroupId) {
                continue;
            }
            const std::wstring& monitorId = input_.layoutContext.monitors[
                static_cast<size_t>(monitorFilter_ - 1)].id;
            const bool hasItemOnMonitor = std::any_of(
                plan_.decisions.begin(), plan_.decisions.end(),
                [&](const lattice::organize::Decision& decision) {
                    return decision.monitorId == monitorId &&
                        SourceIsDesktop(decision) &&
                        (decision.targetCategoryId.empty() || !decision.selected);
                });
            if (!hasItemOnMonitor) continue;
        }
        const auto decisions = DecisionsForGroup(group);
        bool visible = false;
        if (navigationFilter_ == 0) {
            visible = group.id != kKeepDesktopGroupId && !decisions.empty();
        } else if (navigationFilter_ == 3) {
            visible = group.existingCategory;
        } else if (navigationFilter_ == 4) {
            visible = group.createNewCategory && !decisions.empty();
        } else if (navigationFilter_ == 2 &&
                   group.id == kKeepDesktopGroupId) {
            visible = true;
        } else if (navigationFilter_ == 1 &&
                   group.id != kKeepDesktopGroupId) {
            visible = std::any_of(
                decisions.begin(), decisions.end(), [](const auto* decision) {
                    return decision->confidence ==
                        lattice::organize::Confidence::Medium;
                });
        }
        if (visible) result.push_back(&group);
    }
    return result;
}

std::vector<const lattice::organize::Decision*>
AutoOrganizePreviewWindow::DecisionsForGroup(
    const lattice::organize::GroupPlan& group) const {
    std::vector<const lattice::organize::Decision*> result;
    for (const auto& decision : plan_.decisions) {
        if (monitorFilter_ > 0 &&
            static_cast<size_t>(monitorFilter_ - 1) <
                input_.layoutContext.monitors.size() &&
            decision.monitorId != input_.layoutContext.monitors[
                static_cast<size_t>(monitorFilter_ - 1)].id) {
            continue;
        }
        if ((group.id == kKeepDesktopGroupId &&
             SourceIsDesktop(decision) &&
             (decision.targetCategoryId.empty() || !decision.selected)) ||
            (group.id != kKeepDesktopGroupId &&
             decision.targetCategoryId == group.id &&
             lattice::organize::IsOwnershipAdjustment(decision))) {
            result.push_back(&decision);
        }
    }
    return result;
}

const lattice::organize::Decision*
AutoOrganizePreviewWindow::SelectedDecision() const {
    return selectedDecision_ >= 0 && selectedDecision_ < static_cast<int>(plan_.decisions.size())
        ? &plan_.decisions[static_cast<size_t>(selectedDecision_)] : nullptr;
}

void AutoOrganizePreviewWindow::MoveDecisionToGroup(
    int decisionIndex,
    int visibleGroupIndex) {
    if (decisionIndex < 0 ||
        decisionIndex >= static_cast<int>(plan_.decisions.size())) return;
    const auto groups = VisibleGroups();
    const int actual = groupScrollOffset_ + visibleGroupIndex;
    if (actual < 0 || actual >= static_cast<int>(groups.size())) return;
    lattice::organize::Decision& decision =
        plan_.decisions[static_cast<std::size_t>(decisionIndex)];
    const lattice::organize::GroupPlan& target =
        *groups[static_cast<std::size_t>(actual)];
    if (target.id == kKeepDesktopGroupId) {
        KeepDecisionOnDesktop(decisionIndex);
        return;
    }
    decision.targetCategoryId = target.id;
    decision.targetCategoryName = target.name;
    decision.targetIsExistingCategory = target.existingCategory;
    decision.selected = decision.sourceCategoryId != target.id;
    selectedDecision_ = decisionIndex;
    ReplanCandidateLayouts();
}

void AutoOrganizePreviewWindow::KeepDecisionOnDesktop(int decisionIndex) {
    if (decisionIndex < 0 ||
        decisionIndex >= static_cast<int>(plan_.decisions.size())) return;
    lattice::organize::Decision& decision =
        plan_.decisions[static_cast<std::size_t>(decisionIndex)];
    decision.targetCategoryId.clear();
    decision.targetCategoryName.clear();
    decision.targetIsExistingCategory = false;
    decision.selected = false;
    selectedDecision_ = decisionIndex;
    ReplanCandidateLayouts();
}

void AutoOrganizePreviewWindow::ReplanCandidateLayouts() {
    plan_.groups.erase(
        std::remove_if(
            plan_.groups.begin(), plan_.groups.end(),
            [&](const lattice::organize::GroupPlan& group) {
                return group.createNewCategory &&
                    std::none_of(
                        plan_.decisions.begin(), plan_.decisions.end(),
                        [&](const lattice::organize::Decision& decision) {
                            return decision.targetCategoryId == group.id;
                        });
            }),
        plan_.groups.end());
    std::vector<lattice::organize::CandidateWidgetLayout> candidates;
    for (const lattice::organize::GroupPlan& group : plan_.groups) {
        if (!group.createNewCategory ||
            DecisionsForGroup(group).empty()) continue;
        lattice::organize::CandidateWidgetLayout candidate;
        candidate.groupId = group.id;
        candidate.name = group.name;
        candidate.monitorId = group.monitorId;
        candidate.itemCount = DecisionsForGroup(group).size();
        candidate.desiredHeight = static_cast<int>(
            86 + candidate.itemCount * 74);
        const auto previous = std::find_if(
            layoutPlan_.placements.begin(), layoutPlan_.placements.end(),
            [&](const lattice::organize::WidgetPlacement& placement) {
                return placement.groupId == group.id && placement.placed &&
                    placement.manuallyPositioned;
            });
        if (previous != layoutPlan_.placements.end()) {
            candidate.manuallyPositioned = true;
            candidate.manualBounds = previous->bounds;
        }
        candidates.push_back(std::move(candidate));
    }
    layoutPlan_ = lattice::organize::PlanWidgetLayout(
        input_.layoutContext, candidates);
    const bool unplaced = std::any_of(
        layoutPlan_.placements.begin(), layoutPlan_.placements.end(),
        [](const auto& placement) { return !placement.placed; });
    state_ = unplaced ? ViewState::Unplaced : ViewState::Ready;
    desktopChangeBlocksApply_ = false;
    stateMessage_ = unplaced
        ? L"至少一个获准新建的格子尚未放置，应用已禁用。"
        : L"建议已调整。确认前不会写入配置或移动真实文件。";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AutoOrganizePreviewWindow::ShowGroupMenu(
    int visibleGroupIndex,
    POINT screenPoint) {
    const auto groups = VisibleGroups();
    const int actual = groupScrollOffset_ + visibleGroupIndex;
    if (actual < 0 || actual >= static_cast<int>(groups.size())) return;
    const lattice::organize::GroupPlan* selectedGroup =
        groups[static_cast<std::size_t>(actual)];
    HMENU menu = CreatePopupMenu();
    HMENU mergeMenu = CreatePopupMenu();
    if (menu == nullptr || mergeMenu == nullptr) {
        if (menu != nullptr) DestroyMenu(menu);
        if (mergeMenu != nullptr) DestroyMenu(mergeMenu);
        return;
    }
    constexpr UINT kRename = 1;
    constexpr UINT kCancel = 2;
    constexpr UINT kSplit = 3;
    constexpr UINT kMoveChecked = 4;
    constexpr UINT kMergeBase = 100;
    AppendMenuW(
        menu,
        selectedGroup->createNewCategory ? MF_STRING : MF_GRAYED,
        kRename, L"重命名候选格子…");
    AppendMenuW(menu, MF_STRING, kMoveChecked, L"将已勾选项目移到此格子");
    const bool canSplit = selectedDecision_ >= 0 &&
        DecisionsForGroup(*selectedGroup).size() > 1 &&
        plan_.decisions[static_cast<std::size_t>(selectedDecision_)].targetCategoryId ==
            selectedGroup->id;
    AppendMenuW(menu, canSplit ? MF_STRING : MF_GRAYED,
        kSplit, L"将当前项目拆为新候选格子");
    for (std::size_t index = 0; index < plan_.groups.size(); ++index) {
        if (plan_.groups[index].id == selectedGroup->id) continue;
        AppendMenuW(mergeMenu, MF_STRING, kMergeBase + static_cast<UINT>(index),
            plan_.groups[index].name.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(mergeMenu),
        L"合并到其他格子");
    AppendMenuW(
        menu,
        selectedGroup->createNewCategory ? MF_STRING : MF_GRAYED,
        kCancel, L"取消此候选格子");
    const int command = TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
        screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    const std::wstring selectedId = selectedGroup->id;
    if (command == static_cast<int>(kRename) &&
        selectedGroup->createNewCategory) {
        const auto name = InputDialog::Prompt(
            instance_, hwnd_, L"重命名候选格子", L"格子名称", selectedGroup->name);
        if (name.has_value() && !name->empty()) {
            for (auto& group : plan_.groups) {
                if (group.id == selectedId) group.name = *name;
            }
            for (auto& decision : plan_.decisions) {
                if (decision.targetCategoryId == selectedId) {
                    decision.targetCategoryName = *name;
                }
            }
        }
    } else if (command == static_cast<int>(kMoveChecked)) {
        for (std::size_t index = 0; index < plan_.decisions.size(); ++index) {
            auto& decision = plan_.decisions[index];
            if (!decision.selected) continue;
            if (selectedId == kKeepDesktopGroupId) {
                KeepDecisionOnDesktop(static_cast<int>(index));
                continue;
            }
            decision.targetCategoryId = selectedId;
            decision.targetCategoryName = selectedGroup->name;
            decision.targetIsExistingCategory = selectedGroup->existingCategory;
            decision.selected = decision.sourceCategoryId != selectedId;
        }
    } else if (command == static_cast<int>(kCancel) &&
               selectedGroup->createNewCategory) {
        for (auto& decision : plan_.decisions) {
            if (decision.targetCategoryId == selectedId) {
                decision.targetCategoryId.clear();
                decision.targetCategoryName.clear();
                decision.targetIsExistingCategory = false;
                decision.selected = false;
            }
        }
    } else if (command == static_cast<int>(kSplit) && canSplit) {
        auto& decision = plan_.decisions[static_cast<std::size_t>(selectedDecision_)];
        lattice::organize::GroupPlan split;
        split.id = L"manual-" + plan_.id + L"-" + decision.itemId;
        split.name = L"新格子";
        split.monitorId = decision.monitorId;
        split.createNewCategory = true;
        split.itemIds = {decision.itemId};
        plan_.groups.push_back(split);
        decision.targetCategoryId = split.id;
        decision.targetCategoryName = split.name;
        decision.targetIsExistingCategory = false;
        decision.selected = true;
    } else if (command >= static_cast<int>(kMergeBase) &&
               command < static_cast<int>(kMergeBase + plan_.groups.size())) {
        const std::size_t targetIndex = static_cast<std::size_t>(
            command - static_cast<int>(kMergeBase));
        if (targetIndex < plan_.groups.size() &&
            plan_.groups[targetIndex].id != selectedId) {
            const auto target = plan_.groups[targetIndex];
            for (auto& decision : plan_.decisions) {
                const bool belongs = selectedId == kKeepDesktopGroupId
                    ? decision.targetCategoryId.empty() || !decision.selected
                    : decision.targetCategoryId == selectedId;
                if (!belongs) continue;
                if (target.id == kKeepDesktopGroupId) {
                    decision.targetCategoryId.clear();
                    decision.targetCategoryName.clear();
                    decision.targetIsExistingCategory = false;
                    decision.selected = false;
                } else {
                    decision.targetCategoryId = target.id;
                    decision.targetCategoryName = target.name;
                    decision.targetIsExistingCategory = target.existingCategory;
                    decision.selected = decision.sourceCategoryId != target.id;
                }
            }
        }
    }
    DestroyMenu(menu);
    ReplanCandidateLayouts();
}

void AutoOrganizePreviewWindow::ScrollPreview(
    POINT clientPixels,
    int wheelDelta) {
    const D2D1_POINT_2F point = PointInDips(clientPixels);
    if (d2d_.Target() == nullptr) return;
    const float detailLeft = d2d_.Target()->GetSize().width - kDetailWidth;
    if (point.x <= kSidebarWidth || point.x >= detailLeft) return;
    const auto groups = VisibleGroups();
    if (groups.empty()) return;
    const int direction = wheelDelta < 0 ? 1 : -1;
    const float cardWidth = (detailLeft - kSidebarWidth - 64.0f) / 3.0f;
    const int visibleIndex = static_cast<int>((point.x - kSidebarWidth - 20) /
        (cardWidth + 12));
    const int actual = groupScrollOffset_ + visibleIndex;
    if (visibleIndex >= 0 && visibleIndex < 3 &&
        actual >= 0 && actual < static_cast<int>(groups.size())) {
        const auto decisions = DecisionsForGroup(*groups[static_cast<std::size_t>(actual)]);
        int& row = groupRowOffsets_[groups[static_cast<std::size_t>(actual)]->id];
        const int maximum = (std::max)(0, static_cast<int>(decisions.size()) - 4);
        if (maximum > 0) {
            row = std::clamp(row + direction, 0, maximum);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }
    groupScrollOffset_ = std::clamp(
        groupScrollOffset_ + direction, 0,
        (std::max)(0, static_cast<int>(groups.size()) - 3));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AutoOrganizePreviewWindow::RenderBase() {
    ID2D1HwndRenderTarget* target = d2d_.Target();
    if (target == nullptr) return;
    const D2D1_SIZE_F size = target->GetSize();
    const float footerTop = size.height - kFooterHeight;
    const float detailLeft = size.width - kDetailWidth;
    FillRect(D2D1::RectF(0, 0, size.width, size.height), Color(0x061D27));
    FillRect(D2D1::RectF(0, 0, size.width, kTitleHeight), Color(0x092D39));
    FillRect(D2D1::RectF(0, kTitleHeight, kSidebarWidth, footerTop), Color(0x072630));
    FillRect(D2D1::RectF(kSidebarWidth, kTitleHeight, detailLeft, footerTop), Color(0x082A35));
    FillRect(D2D1::RectF(detailLeft, kTitleHeight, size.width, footerTop), Color(0x092B36));
    FillRect(D2D1::RectF(0, footerTop, size.width, size.height), Color(0x082731));
    StrokeRect(D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f), Color(0x3F7686));
    StrokeRect(D2D1::RectF(0, kTitleHeight - .5f, size.width, kTitleHeight + .5f), Color(0x285969));
    StrokeRect(D2D1::RectF(kSidebarWidth - .5f, kTitleHeight, kSidebarWidth + .5f, footerTop), Color(0x285969));
    StrokeRect(D2D1::RectF(detailLeft - .5f, kTitleHeight, detailLeft + .5f, footerTop), Color(0x285969));
    StrokeRect(D2D1::RectF(0, footerTop - .5f, size.width, footerTop + .5f), Color(0x285969));

    FillRounded(D2D1::RectF(16, 11, 41, 36), 5, Color(0x55C6E7));
    DrawText(L"L", D2D1::RectF(23, 12, 39, 34), titleFormat_.Get(), Color(0x061D27));
    DrawText(L"Lattice 自动整理预览", D2D1::RectF(51, 13, 260, 38), titleFormat_.Get(), Color(0xF4FBFD));
    DrawText(L"本地离线分析", D2D1::RectF(238, 17, 340, 37), smallFormat_.Get(), Color(0x82AAB5));
    const bool minimizeHovered = IsHovered(HitKind::Minimize);
    const bool closeHovered = IsHovered(HitKind::Close);
    if (minimizeHovered) {
        FillRounded(D2D1::RectF(size.width - 88, 6, size.width - 48, 42), 3,
            Color(0x174654));
    }
    if (closeHovered) {
        FillRounded(D2D1::RectF(size.width - 48, 6, size.width - 4, 42), 3,
            Color(0xC42B3A));
    }
    DrawText(L"—", D2D1::RectF(size.width - 82, 8, size.width - 48, 39), titleFormat_.Get(), Color(minimizeHovered ? 0xFFFFFF : 0xB9D5DC));
    DrawText(L"×", D2D1::RectF(size.width - 40, 8, size.width - 8, 39), titleFormat_.Get(), Color(closeHovered ? 0xFFFFFF : 0xB9D5DC));

    DrawText(L"整理范围", D2D1::RectF(22, 67, 180, 89), smallFormat_.Get(), Color(0x7EA8B4));
    static const wchar_t* navNames[] = {L"全部建议", L"需要确认", L"保持桌面", L"已有格子", L"将新建格子"};
    static const wchar_t* navGlyphs[] = {L"▦", L"!", L"⌂", L"□", L"＋"};
    for (int index = 0; index < 5; ++index) {
        const float top = 96.0f + index * 44.0f;
        const bool selected = navigationFilter_ == index;
        const bool hovered = IsHovered(HitKind::Navigation, index);
        if (selected) {
            FillRounded(D2D1::RectF(12, top, 198, top + 40), 3, Color(0x144554));
            FillRect(D2D1::RectF(12, top, 15, top + 40), Color(0x55C6E7));
        } else if (hovered) {
            FillRounded(D2D1::RectF(12, top, 198, top + 40), 3, Color(0x103B49));
        }
        DrawText(navGlyphs[index], D2D1::RectF(25, top + 8, 46, top + 32), textFormat_.Get(), selected || hovered ? Color(0xFFFFFF) : Color(0x8FB5BF));
        DrawText(navNames[index], D2D1::RectF(51, top + 10, 157, top + 33), textFormat_.Get(), selected || hovered ? Color(0xFFFFFF) : Color(0xB9D5DD));
        int count = 0;
        if (index == 3 || index == 4) {
            count = static_cast<int>(std::count_if(
                plan_.groups.begin(), plan_.groups.end(),
                [index](const auto& group) {
                    return index == 3 ? group.existingCategory
                                      : group.createNewCategory;
                }));
        } else {
            for (const auto& decision : plan_.decisions) {
                if ((index == 0 &&
                     lattice::organize::IsOwnershipAdjustment(decision)) ||
                    (index == 1 &&
                     lattice::organize::IsOwnershipAdjustment(decision) &&
                     decision.confidence == lattice::organize::Confidence::Medium) ||
                    (index == 2 && SourceIsDesktop(decision) &&
                     (decision.targetCategoryId.empty() || !decision.selected))) {
                    ++count;
                }
            }
        }
        FillRounded(D2D1::RectF(163, top + 10, 190, top + 30), 10, Color(selected ? 0x1C6074 : hovered ? 0x164C5B : 0x123B48));
        DrawText(std::to_wstring(count), D2D1::RectF(169, top + 11, 188, top + 30), tinyFormat_.Get(), Color(selected || hovered ? 0xDFFAFF : 0xA9CBD3));
    }
    FillRounded(D2D1::RectF(12, footerTop - 138, 198, footerTop - 16), 5, Color(0x092F3A));
    StrokeRounded(D2D1::RectF(12, footerTop - 138, 198, footerTop - 16), 5, Color(0x285969));
    DrawText(L"✓", D2D1::RectF(24, footerTop - 125, 45, footerTop - 102), titleFormat_.Get(), Color(0x66D2A2));
    DrawText(L"完全本地", D2D1::RectF(50, footerTop - 125, 180, footerTop - 102), textFormat_.Get(), Color(0xD9EDF2));
    DrawText(L"不会上传文件名、路径或应用清单。\n应用只改变 Lattice 显示归属。", D2D1::RectF(24, footerTop - 96, 184, footerTop - 20), smallFormat_.Get(), Color(0x8FB7C1));

    const int selectedCount = static_cast<int>(std::count_if(
        plan_.decisions.begin(), plan_.decisions.end(), [](const auto& decision) {
            return decision.selected &&
                lattice::organize::IsOwnershipAdjustment(decision);
        }));
    const int keepCount = static_cast<int>(plan_.decisions.size()) - selectedCount;
    DrawText(L"桌面整理建议", D2D1::RectF(kSidebarWidth + 20, 65, detailLeft - 250, 96), headingFormat_.Get(), Color(0xF4FBFD));
    DrawText(L"先审阅分类与位置，再一次性应用。真实文件始终留在原路径。", D2D1::RectF(kSidebarWidth + 20, 95, detailLeft - 290, 116), smallFormat_.Get(), Color(0x9FC0C9));
    DrawText(std::to_wstring(plan_.decisions.size()), D2D1::RectF(detailLeft - 220, 64, detailLeft - 170, 94), headingFormat_.Get(), Color(0xFFFFFF));
    DrawText(L"扫描项目", D2D1::RectF(detailLeft - 235, 93, detailLeft - 160, 111), tinyFormat_.Get(), Color(0x8FB3BE));
    DrawText(std::to_wstring(selectedCount), D2D1::RectF(detailLeft - 135, 64, detailLeft - 85, 94), headingFormat_.Get(), Color(0xFFFFFF));
    DrawText(L"建议调整", D2D1::RectF(detailLeft - 150, 93, detailLeft - 75, 111), tinyFormat_.Get(), Color(0x8FB3BE));
    DrawText(std::to_wstring(keepCount), D2D1::RectF(detailLeft - 55, 64, detailLeft - 10, 94), headingFormat_.Get(), Color(0xFFFFFF));
    DrawText(L"保持现状", D2D1::RectF(detailLeft - 70, 93, detailLeft - 5, 111), tinyFormat_.Get(), Color(0x8FB3BE));

    float monitorLeft = kSidebarWidth + 20;
    const int monitorCount = static_cast<int>(input_.layoutContext.monitors.size());
    for (int index = 0; index <= monitorCount; ++index) {
        const float width = index == 0 ? 82.0f : 94.0f;
        const D2D1_RECT_F bounds = D2D1::RectF(monitorLeft, 126, monitorLeft + width, 157);
        const bool hovered = IsHovered(HitKind::Monitor, index);
        if (monitorFilter_ == index) {
            FillRounded(bounds, 3, Color(0x0D3E4C));
            StrokeRounded(bounds, 3, Color(0x408294));
        } else if (hovered) {
            FillRounded(bounds, 3, Color(0x123D49));
            StrokeRounded(bounds, 3, Color(0x397586));
        }
        DrawText(index == 0 ? L"全部屏幕" : MonitorLabel(index - 1), D2D1::RectF(bounds.left + 10, bounds.top + 7, bounds.right - 6, bounds.bottom - 4), smallFormat_.Get(), Color(monitorFilter_ == index || hovered ? 0xEAFBFF : 0x8FB5BF));
        monitorLeft += width + 5.0f;
    }
    DrawText(L"默认不跨屏", D2D1::RectF(detailLeft - 115, 134, detailLeft - 20, 154), tinyFormat_.Get(), Color(0x7FA6B0));
    StrokeRect(D2D1::RectF(kSidebarWidth, 169.5f, detailLeft, 170.5f), Color(0x285969));
    if (state_ == ViewState::Changed || state_ == ViewState::Unplaced) {
        const bool nonBlockingChange = state_ == ViewState::Changed &&
            !desktopChangeBlocksApply_;
        const D2D1_COLOR_F tone = nonBlockingChange
            ? Color(0x66D2A2)
            : state_ == ViewState::Changed ? Color(0xFFC66D) : Color(0xFF7B87);
        FillRounded(D2D1::RectF(kSidebarWidth + 20, 180, detailLeft - 20, 218), 4,
            Color(nonBlockingChange ? 0x123D33 :
                state_ == ViewState::Changed ? 0x493915 : 0x52242B, .55f));
        StrokeRounded(D2D1::RectF(kSidebarWidth + 20, 180, detailLeft - 20, 218), 4, tone);
        DrawText(stateMessage_, D2D1::RectF(kSidebarWidth + 32, 191, detailLeft - 30, 212), smallFormat_.Get(), tone);
    }

    const float contentTop = state_ == ViewState::Changed || state_ == ViewState::Unplaced ? 232.0f : 186.0f;
    const float cardWidth = (detailLeft - kSidebarWidth - 64.0f) / 3.0f;
    const auto groups = VisibleGroups();
    const int maximumGroupOffset = (std::max)(
        0, static_cast<int>(groups.size()) - 3);
    groupScrollOffset_ = std::clamp(
        groupScrollOffset_, 0, maximumGroupOffset);
    for (int visibleGroupIndex = 0;
         visibleGroupIndex < 3 &&
         groupScrollOffset_ + visibleGroupIndex < static_cast<int>(groups.size());
         ++visibleGroupIndex) {
        const auto& group = *groups[static_cast<std::size_t>(
            groupScrollOffset_ + visibleGroupIndex)];
        const float left = kSidebarWidth + 20.0f +
            visibleGroupIndex * (cardWidth + 12.0f);
        const float bottom = (std::min)(footerTop - 20.0f, contentTop + 360.0f);
        const D2D1_RECT_F card = D2D1::RectF(left, contentTop, left + cardWidth, bottom);
        const bool groupHovered = IsHovered(HitKind::Group, visibleGroupIndex);
        FillRounded(card, 5, Color(groupHovered ? 0x10404E : group.createNewCategory ? 0x102F40 : 0x092F3B));
        StrokeRounded(card, 5, Color(groupHovered ? 0x55B9D4 : group.createNewCategory ? 0xA78BFA : 0x356B7A), groupHovered ? 1.5f : 1.0f, group.createNewCategory ? dashedStroke_.Get() : nullptr);
        StrokeRect(D2D1::RectF(card.left, card.top + 52.5f, card.right, card.top + 53.5f), Color(group.createNewCategory ? 0x725FA4 : 0x2B5E6C));
        DrawText(group.name, D2D1::RectF(card.left + 10, card.top + 8, card.right - 86, card.top + 30), textFormat_.Get(), Color(0xF4FBFD));
        const auto decisions = DecisionsForGroup(group);
        DrawText(std::to_wstring(decisions.size()) + L" 个项目", D2D1::RectF(card.left + 10, card.top + 30, card.right - 90, card.top + 49), tinyFormat_.Get(), Color(0x8DB2BC));
        const D2D1_RECT_F tag = D2D1::RectF(card.right - 77, card.top + 15, card.right - 10, card.top + 36);
        StrokeRounded(tag, 3, Color(group.createNewCategory ? 0x8A73CF : 0x4D7F8C));
        const std::wstring tagText = group.id == kKeepDesktopGroupId
            ? L"保持桌面" : group.createNewCategory ? L"将新建" : L"已有格子";
        DrawText(tagText, D2D1::RectF(tag.left + 6, tag.top + 4, tag.right - 3, tag.bottom), tinyFormat_.Get(), Color(group.createNewCategory ? 0xC8B9FF : 0x9FC5CE));

        int& firstRow = groupRowOffsets_[group.id];
        firstRow = std::clamp(firstRow, 0,
            (std::max)(0, static_cast<int>(decisions.size()) - 4));
        for (int visibleRow = 0;
             visibleRow < 4 && firstRow + visibleRow < static_cast<int>(decisions.size());
             ++visibleRow) {
            const auto& decision = *decisions[static_cast<std::size_t>(
                firstRow + visibleRow)];
            const int decisionIndex = static_cast<int>(&decision - plan_.decisions.data());
            const float top = card.top + 61.0f + visibleRow * 62.0f;
            const D2D1_RECT_F rowBounds = D2D1::RectF(card.left + 7, top, card.right - 7, top + 58);
            const bool rowHovered = IsHovered(HitKind::Item, decisionIndex) ||
                IsHovered(HitKind::ItemCheck, decisionIndex);
            if (decisionIndex == selectedDecision_) {
                FillRounded(rowBounds, 4, Color(0x14566A));
                StrokeRounded(rowBounds, 4, Color(0x55C6E7));
            } else if (rowHovered) {
                FillRounded(rowBounds, 4, Color(0x10404E));
                StrokeRounded(rowBounds, 4, Color(0x2D6675));
            }
            const D2D1_RECT_F check = D2D1::RectF(rowBounds.left + 6, top + 19, rowBounds.left + 24, top + 37);
            FillRect(check, Color(decision.selected ? 0x55C6E7 : 0x0B3543));
            StrokeRect(check, Color(decision.selected ? 0x55C6E7 : rowHovered ? 0x78C8DA : 0x5A8A96));
            if (decision.selected) DrawText(L"✓", D2D1::RectF(check.left + 2, check.top - 1, check.right + 2, check.bottom + 2), smallFormat_.Get(), Color(0x05232E));
            const auto item = std::find_if(input_.snapshot.items.begin(), input_.snapshot.items.end(), [&](const auto& candidate) { return candidate.id == decision.itemId; });
            const std::wstring name = item == input_.snapshot.items.end() ? decision.itemId : item->displayName;
            const std::wstring path = item == input_.snapshot.items.end() ? L"" : item->path;
            if (!path.empty()) {
                const bool folder = item->kind == DesktopItemKind::Folder;
                ID2D1Bitmap* bitmap = iconCache_.GetIcon(target, path, name, folder ? IconPlaceholderKind::Folder : IconPlaceholderKind::File);
                if (bitmap != nullptr) target->DrawBitmap(bitmap, D2D1::RectF(rowBounds.left + 33, top + 13, rowBounds.left + 65, top + 45), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            DrawText(name, D2D1::RectF(rowBounds.left + 73, top + 8, rowBounds.right - 5, top + 29), smallFormat_.Get(), Color(0xD9EDF2));
            const D2D1_COLOR_F confidenceColor = decision.confidence == lattice::organize::Confidence::Medium ? Color(0xFFC66D) : decision.confidence == lattice::organize::Confidence::High ? Color(0x66D2A2) : Color(0x83A9B3);
            DrawText(ConfidenceText(decision), D2D1::RectF(rowBounds.left + 73, top + 31, rowBounds.right - 5, top + 50), tinyFormat_.Get(), confidenceColor);
        }
        const float emptyTop = card.top + 61.0f +
            (std::min)(decisions.size() - static_cast<std::size_t>(firstRow),
                       size_t{4}) * 62.0f;
        if (emptyTop + 48 < card.bottom) {
            StrokeRounded(D2D1::RectF(card.left + 7, emptyTop, card.right - 7, emptyTop + 48), 4, Color(0x315E69), 1, dashedStroke_.Get());
            DrawText(L"拖到这里加入此格子", D2D1::RectF(card.left + 28, emptyTop + 15, card.right - 10, emptyTop + 37), smallFormat_.Get(), Color(0x6F98A2));
        }
    }
    if (groups.size() > 3) {
        DrawText(
            L"滚轮查看更多格子  " +
                std::to_wstring(groupScrollOffset_ + 1) + L"–" +
                std::to_wstring((std::min)(groupScrollOffset_ + 3,
                    static_cast<int>(groups.size()))) + L" / " +
                std::to_wstring(groups.size()),
            D2D1::RectF(detailLeft - 250, 173, detailLeft - 20, 190),
            tinyFormat_.Get(), Color(0x7FA6B0));
    }
    if (groups.empty() && state_ == ViewState::Ready) {
        StrokeRounded(D2D1::RectF(kSidebarWidth + 20, contentTop, detailLeft - 20, contentTop + 160), 4, Color(0x315E69), 1, dashedStroke_.Get());
        DrawText(L"此筛选条件下没有项目", D2D1::RectF(kSidebarWidth + 55, contentTop + 67, detailLeft - 45, contentTop + 96), textFormat_.Get(), Color(0x6F98A2));
    }

    const auto* selected = SelectedDecision();
    DrawText(L"项目详情", D2D1::RectF(detailLeft + 17, 66, size.width - 20, 86), tinyFormat_.Get(), Color(0x78A6B1));
    std::wstring selectedName = L"选择一个项目";
    if (selected != nullptr) {
        const auto found = std::find_if(input_.snapshot.items.begin(), input_.snapshot.items.end(), [&](const auto& item) { return item.id == selected->itemId; });
        selectedName = found == input_.snapshot.items.end() ? selected->itemId : found->displayName;
    }
    DrawText(selectedName, D2D1::RectF(detailLeft + 17, 91, size.width - 18, 119), titleFormat_.Get(), Color(0xF4FBFD));
    DrawText(selected == nullptr ? L"查看分类依据与最终位置" : ConfidenceText(*selected), D2D1::RectF(detailLeft + 17, 120, size.width - 18, 142), smallFormat_.Get(), Color(0x90B5BF));
    StrokeRect(D2D1::RectF(detailLeft, 153.5f, size.width, 154.5f), Color(0x285969));
    if (selected != nullptr) {
        DrawText(L"调整路径", D2D1::RectF(detailLeft + 17, 172, size.width - 20, 193), smallFormat_.Get(), Color(0x8FB5BF));
        const D2D1_RECT_F source = D2D1::RectF(detailLeft + 17, 201, detailLeft + 145, 253);
        const D2D1_RECT_F destination = D2D1::RectF(detailLeft + 192, 201, size.width - 17, 253);
        FillRounded(source, 4, Color(0x0C3743)); StrokeRounded(source, 4, Color(0x386D7B));
        FillRounded(destination, 4, Color(0x0C3743)); StrokeRounded(destination, 4, Color(0x386D7B));
        DrawText(L"当前位置", D2D1::RectF(source.left + 9, source.top + 6, source.right - 5, source.top + 22), tinyFormat_.Get(), Color(0x82A9B3));
        DrawText(SourceName(*selected), D2D1::RectF(source.left + 9, source.top + 26, source.right - 5, source.bottom - 3), smallFormat_.Get(), Color(0xF4FBFD));
        DrawText(L"→", D2D1::RectF(detailLeft + 154, 216, detailLeft + 186, 242), titleFormat_.Get(), Color(0x55C6E7));
        DrawText(L"建议位置", D2D1::RectF(destination.left + 9, destination.top + 6, destination.right - 5, destination.top + 22), tinyFormat_.Get(), Color(0x82A9B3));
        DrawText(selected->targetCategoryName.empty() ? L"保持桌面" : selected->targetCategoryName, D2D1::RectF(destination.left + 9, destination.top + 26, destination.right - 5, destination.bottom - 3), smallFormat_.Get(), Color(0xF4FBFD));
        DrawText(L"分类依据", D2D1::RectF(detailLeft + 17, 277, size.width - 20, 299), smallFormat_.Get(), Color(0x8FB5BF));
        FillRounded(D2D1::RectF(detailLeft + 17, 307, size.width - 17, 385), 4, Color(0x0E3842));
        FillRect(D2D1::RectF(detailLeft + 17, 307, detailLeft + 20, 385), Color(selected->confidence == lattice::organize::Confidence::Medium ? 0xFFC66D : 0x66D2A2));
        DrawText(selected->reason, D2D1::RectF(detailLeft + 31, 318, size.width - 30, 374), smallFormat_.Get(), Color(0xBED8DE));
    }

    const float previewTop = footerTop - 178.0f;
    StrokeRect(D2D1::RectF(detailLeft, previewTop - .5f, size.width, previewTop + .5f), Color(0x285969));
    DrawText(L"桌面位置预览", D2D1::RectF(detailLeft + 17, previewTop + 14, size.width - 130, previewTop + 34), smallFormat_.Get(), Color(0xF4FBFD));
    DrawText(L"拖动虚线格子", D2D1::RectF(size.width - 115, previewTop + 14, size.width - 17, previewTop + 34), tinyFormat_.Get(), Color(0x7DA5AF));
    const D2D1_RECT_F map = D2D1::RectF(detailLeft + 17, previewTop + 42, size.width - 17, footerTop - 17);
    FillRounded(map, 4, Color(0x103F50)); StrokeRounded(map, 4, Color(0x3C7280));
    DrawText(monitorFilter_ == 0 ? L"全部屏幕" : L"当前屏幕", D2D1::RectF(map.left + 7, map.top + 5, map.left + 90, map.top + 22), tinyFormat_.Get(), Color(0x6D9AA5));
    for (size_t index = 0; index < layoutPlan_.placements.size(); ++index) {
        const auto& placement = layoutPlan_.placements[index];
        if (monitorFilter_ > 0 && static_cast<size_t>(monitorFilter_ - 1) < input_.layoutContext.monitors.size() && placement.monitorId != input_.layoutContext.monitors[static_cast<size_t>(monitorFilter_ - 1)].id) continue;
        const auto monitor = std::find_if(input_.layoutContext.monitors.begin(), input_.layoutContext.monitors.end(), [&](const auto& value) { return value.id == placement.monitorId; });
        if (monitor == input_.layoutContext.monitors.end()) continue;
        const float sx = (map.right - map.left - 16) / static_cast<float>((std::max)(1, monitor->workArea.Width()));
        const float sy = (map.bottom - map.top - 16) / static_cast<float>((std::max)(1, monitor->workArea.Height()));
        D2D1_RECT_F box = D2D1::RectF(
            map.left + 8 + (placement.bounds.left - monitor->workArea.left) * sx,
            map.top + 8 + (placement.bounds.top - monitor->workArea.top) * sy,
            map.left + 8 + (placement.bounds.right - monitor->workArea.left) * sx,
            map.top + 8 + (placement.bounds.bottom - monitor->workArea.top) * sy);
        if (!placement.placed) box = D2D1::RectF(map.left + 8, map.top + 70, map.left + 82, map.top + 105);
        const bool hovered = IsHovered(HitKind::Position, static_cast<int>(index));
        FillRounded(box, 2, Color(hovered ? 0x45396F : placement.placed ? 0x2A2445 : 0x52242B, hovered ? .9f : .75f));
        StrokeRounded(box, 2, Color(hovered ? 0x7ADCF5 : placement.placed ? 0xA78BFA : 0xFF7B87), hovered ? 1.5f : 1.0f, dashedStroke_.Get());
        DrawText(placement.placed ? L"将新建" : L"尚未放置", D2D1::RectF(box.left + 3, box.top + 3, box.right - 2, box.bottom - 2), tinyFormat_.Get(), Color(placement.placed ? 0xDED4FF : 0xFFB9C1));
    }

    DrawText(L"✓", D2D1::RectF(18, footerTop + 24, 39, footerTop + 48), textFormat_.Get(), Color(0x66D2A2));
    DrawText(L"不移动真实文件 · 原子应用 · 完成后可撤销", D2D1::RectF(43, footerTop + 25, 410, footerTop + 49), smallFormat_.Get(), Color(0x8FB4BD));
    const D2D1_RECT_F regenerate = D2D1::RectF(size.width - 392, footerTop + 17, size.width - 270, footerTop + 55);
    const D2D1_RECT_F cancel = D2D1::RectF(size.width - 260, footerTop + 17, size.width - 160, footerTop + 55);
    const D2D1_RECT_F apply = D2D1::RectF(size.width - 150, footerTop + 17, size.width - 18, footerTop + 55);
    const bool regenerateHovered = IsHovered(HitKind::Regenerate);
    const bool cancelHovered = IsHovered(HitKind::Cancel);
    const bool canApply = (state_ == ViewState::Ready ||
        (state_ == ViewState::Changed && !desktopChangeBlocksApply_)) &&
        selectedCount > 0;
    const bool applyHovered = canApply && IsHovered(HitKind::Apply);
    FillRounded(regenerate, 4, Color(regenerateHovered ? 0x104654 : 0x0A3340)); StrokeRounded(regenerate, 4, Color(regenerateHovered ? 0x55A9BF : 0x397586));
    FillRounded(cancel, 4, Color(cancelHovered ? 0x104654 : 0x0A3340)); StrokeRounded(cancel, 4, Color(cancelHovered ? 0x55A9BF : 0x397586));
    FillRounded(apply, 4, Color(canApply ? applyHovered ? 0x2BA8CD : 0x2198BC : 0x145064, canApply ? 1 : .55f));
    StrokeRounded(apply, 4, Color(canApply ? applyHovered ? 0x8EE7FA : 0x55C7E7 : 0x397586));
    DrawText(L"重新生成建议", D2D1::RectF(regenerate.left + 14, regenerate.top + 10, regenerate.right - 4, regenerate.bottom), smallFormat_.Get(), Color(0xD7EDF2));
    DrawText(L"取消", D2D1::RectF(cancel.left + 34, cancel.top + 10, cancel.right - 4, cancel.bottom), smallFormat_.Get(), Color(0xD7EDF2));
    DrawText(L"应用 " + std::to_wstring(selectedCount) + L" 项调整", D2D1::RectF(apply.left + 17, apply.top + 10, apply.right - 4, apply.bottom), smallFormat_.Get(), Color(canApply ? 0xFFFFFF : 0x8FB4BD));
}

void AutoOrganizePreviewWindow::RenderOverlay() {
    if (state_ == ViewState::Ready || state_ == ViewState::Changed || state_ == ViewState::Unplaced) return;
    auto* target = d2d_.Target();
    if (target == nullptr) return;
    const D2D1_SIZE_F size = target->GetSize();
    FillRect(D2D1::RectF(0, kTitleHeight, size.width, size.height - kFooterHeight), Color(0x031219, .88f));
    const D2D1_RECT_F card = D2D1::RectF(size.width / 2 - 260, size.height / 2 - 150, size.width / 2 + 260, size.height / 2 + 150);
    FillRounded(card, 6, Color(0x092D39)); StrokeRounded(card, 6, Color(0x448091));
    std::wstring title; std::wstring action; D2D1_COLOR_F tone = Color(0x55C6E7);
    switch (state_) {
        case ViewState::Scanning: title = L"正在生成整理建议"; action = L"取消扫描"; break;
        case ViewState::Cancelled: title = L"扫描已取消"; action = L"重新扫描"; break;
        case ViewState::Empty: title = L"没有足够可靠的建议"; action = L"重新扫描"; tone = Color(0x66D2A2); break;
        case ViewState::Applying: title = L"正在原子应用调整"; action = L"正在应用…"; break;
        case ViewState::Success: title = L"整理已完成"; action = L"关闭"; tone = Color(0x66D2A2); break;
        case ViewState::ScanError: title = L"扫描失败"; action = L"重试"; tone = Color(0xFF7B87); break;
        case ViewState::ApplyError: title = L"未应用任何调整"; action = L"返回预览"; tone = Color(0xFF7B87); break;
        case ViewState::Undoing: title = L"正在撤销自动整理"; action = L"正在撤销…"; break;
        case ViewState::UndoSuccess: title = L"撤销已完成"; action = L"关闭"; tone = Color(0x66D2A2); break;
        case ViewState::UndoConflict: title = L"部分内容已保留"; action = L"返回"; tone = Color(0xFFC66D); break;
        case ViewState::UndoError: title = L"撤销失败"; action = L"返回"; tone = Color(0xFF7B87); break;
        default: return;
    }
    if (state_ == ViewState::Success && input_.undoAvailable) {
        action = L"撤销本次整理";
    }
    FillRounded(D2D1::RectF(size.width / 2 - 27, card.top + 28, size.width / 2 + 27, card.top + 82), 27, Color(0x123F4D));
    DrawText(state_ == ViewState::Scanning || state_ == ViewState::Applying ? L"↻" : state_ == ViewState::Success || state_ == ViewState::Empty ? L"✓" : L"!", D2D1::RectF(size.width / 2 - 10, card.top + 42, size.width / 2 + 15, card.top + 72), titleFormat_.Get(), tone);
    DrawText(title, D2D1::RectF(card.left + 40, card.top + 99, card.right - 40, card.top + 132), headingFormat_.Get(), Color(0xF4FBFD));
    DrawText(stateMessage_, D2D1::RectF(card.left + 50, card.top + 139, card.right - 50, card.top + 195), smallFormat_.Get(), Color(0x9FC0C9));
    if (state_ == ViewState::Scanning || state_ == ViewState::Applying ||
        state_ == ViewState::Undoing) {
        FillRounded(D2D1::RectF(card.left + 64, card.top + 208, card.right - 64, card.top + 213), 3, Color(0x123B47));
        FillRounded(D2D1::RectF(card.left + 64, card.top + 208, card.left + 292, card.top + 213), 3, Color(0x55C6E7));
    }
    const D2D1_RECT_F button = D2D1::RectF(size.width / 2 - 65, card.bottom - 59, size.width / 2 + 65, card.bottom - 21);
    const bool actionEnabled = state_ != ViewState::Applying &&
        state_ != ViewState::Undoing;
    const bool actionHovered = actionEnabled && IsHovered(HitKind::OverlayAction);
    FillRounded(button, 4, Color(
        state_ == ViewState::ScanError || state_ == ViewState::ApplyError
            ? actionHovered ? 0x62434E : 0x49313A
            : actionHovered ? 0x2BA8CD : 0x2198BC));
    StrokeRounded(button, 4, actionHovered ? Color(0x8EE7FA) : tone);
    DrawText(action, D2D1::RectF(button.left + 25, button.top + 10, button.right - 10, button.bottom), smallFormat_.Get(), Color(0xFFFFFF));
}

void AutoOrganizePreviewWindow::RebuildHitTargets() {
    hitTargets_.clear();
    auto* target = d2d_.Target();
    if (target == nullptr) return;
    const D2D1_SIZE_F size = target->GetSize();
    const float footerTop = size.height - kFooterHeight;
    const float detailLeft = size.width - kDetailWidth;
    hitTargets_.push_back({HitKind::Minimize, 0, D2D1::RectF(size.width - 88, 6, size.width - 48, 42), L"最小化预览窗口"});
    hitTargets_.push_back({HitKind::Close, 0, D2D1::RectF(size.width - 48, 6, size.width - 4, 42), L"关闭预览，不应用任何调整"});
    if (state_ != ViewState::Ready && state_ != ViewState::Changed &&
        state_ != ViewState::Unplaced) {
        hitTargets_.push_back({
            HitKind::OverlayAction, 0,
            D2D1::RectF(size.width / 2 - 65, size.height / 2 + 91,
                        size.width / 2 + 65, size.height / 2 + 129),
            state_ == ViewState::Applying || state_ == ViewState::Undoing
                ? L"配置事务正在进行" : L"执行当前状态的主要操作"});
        return;
    }
    for (int index = 0; index < 5; ++index) {
        hitTargets_.push_back({HitKind::Navigation, index, D2D1::RectF(12, 96.0f + index * 44.0f, 198, 136.0f + index * 44.0f), L"筛选当前预览内容"});
    }
    float monitorLeft = kSidebarWidth + 20;
    for (int index = 0; index <= static_cast<int>(input_.layoutContext.monitors.size()); ++index) {
        const float width = index == 0 ? 82.0f : 94.0f;
        hitTargets_.push_back({HitKind::Monitor, index, D2D1::RectF(monitorLeft, 126, monitorLeft + width, 157), index == 0 ? L"查看全部显示器的独立建议" : L"只查看该显示器，不会自动跨屏"});
        monitorLeft += width + 5;
    }
    const float contentTop = state_ == ViewState::Changed || state_ == ViewState::Unplaced ? 232.0f : 186.0f;
    const float cardWidth = (detailLeft - kSidebarWidth - 64.0f) / 3.0f;
    const auto groups = VisibleGroups();
    for (int visibleGroupIndex = 0;
         visibleGroupIndex < 3 &&
         groupScrollOffset_ + visibleGroupIndex < static_cast<int>(groups.size());
         ++visibleGroupIndex) {
        const auto* group = groups[static_cast<std::size_t>(
            groupScrollOffset_ + visibleGroupIndex)];
        const float left = kSidebarWidth + 20 +
            visibleGroupIndex * (cardWidth + 12);
        hitTargets_.push_back({HitKind::Group, visibleGroupIndex,
            D2D1::RectF(left, contentTop, left + cardWidth, footerTop - 12),
            L"拖入此格子；右键可重命名、合并、拆分或取消候选格子"});
        const auto decisions = DecisionsForGroup(*group);
        const int firstRow = groupRowOffsets_[group->id];
        for (int visibleRow = 0;
             visibleRow < 4 && firstRow + visibleRow < static_cast<int>(decisions.size());
             ++visibleRow) {
            const int decisionIndex = static_cast<int>(
                decisions[static_cast<std::size_t>(firstRow + visibleRow)] -
                plan_.decisions.data());
            const float top = contentTop + 61 + visibleRow * 62;
            hitTargets_.push_back({HitKind::ItemCheck, decisionIndex, D2D1::RectF(left + 10, top + 12, left + 39, top + 46), L"勾选或取消本项调整"});
            hitTargets_.push_back({HitKind::Item, decisionIndex, D2D1::RectF(left + 39, top, left + cardWidth - 7, top + 58), L"查看分类依据；拖到其他格子或左侧保持桌面"});
        }
    }
    const float previewTop = footerTop - 178;
    const D2D1_RECT_F map = D2D1::RectF(detailLeft + 17, previewTop + 42, size.width - 17, footerTop - 17);
    for (size_t index = 0; index < layoutPlan_.placements.size(); ++index) {
        const auto& placement = layoutPlan_.placements[index];
        const auto monitor = std::find_if(input_.layoutContext.monitors.begin(), input_.layoutContext.monitors.end(), [&](const auto& value) { return value.id == placement.monitorId; });
        if (monitor == input_.layoutContext.monitors.end()) continue;
        const float sx = (map.right - map.left - 16) / static_cast<float>((std::max)(1, monitor->workArea.Width()));
        const float sy = (map.bottom - map.top - 16) / static_cast<float>((std::max)(1, monitor->workArea.Height()));
        D2D1_RECT_F box = D2D1::RectF(
            map.left + 8 + (placement.bounds.left - monitor->workArea.left) * sx,
            map.top + 8 + (placement.bounds.top - monitor->workArea.top) * sy,
            map.left + 8 + (placement.bounds.right - monitor->workArea.left) * sx,
            map.top + 8 + (placement.bounds.bottom - monitor->workArea.top) * sy);
        if (!placement.placed) box = D2D1::RectF(map.left + 8, map.top + 70, map.left + 82, map.top + 105);
        hitTargets_.push_back({HitKind::Position, static_cast<int>(index), box, placement.placed ? L"拖动以固定本次候选格子位置" : L"此格子尚未找到可用位置"});
    }
    hitTargets_.push_back({HitKind::Regenerate, 0, D2D1::RectF(size.width - 392, footerTop + 17, size.width - 270, footerTop + 55), L"重新读取当前状态并生成建议"});
    hitTargets_.push_back({HitKind::Cancel, 0, D2D1::RectF(size.width - 260, footerTop + 17, size.width - 160, footerTop + 55), L"关闭预览，当前配置保持不变"});
    hitTargets_.push_back({HitKind::Apply, 0, D2D1::RectF(size.width - 150, footerTop + 17, size.width - 18, footerTop + 55), state_ == ViewState::Unplaced ? L"仍有候选格子尚未放置" : state_ == ViewState::Changed && desktopChangeBlocksApply_ ? L"涉及内容已变化，请先重新生成" : L"一次性应用全部已勾选调整"});
}

void AutoOrganizePreviewWindow::Render() {
    PAINTSTRUCT paint{};
    BeginPaint(hwnd_, &paint);
    if (d2d_.Target() == nullptr) d2d_.RecreateTarget(hwnd_);
    EnsureTextFormats();
    d2d_.BeginDraw();
    RenderBase();
    RenderOverlay();
    RebuildHitTargets();
    if (hoverHit_ >= 0 && hoverHit_ < static_cast<int>(hitTargets_.size()) &&
        !hitTargets_[static_cast<size_t>(hoverHit_)].tooltip.empty()) {
        const auto& hit = hitTargets_[static_cast<size_t>(hoverHit_)];
        const float width = (std::min)(280.0f, 20.0f + static_cast<float>(hit.tooltip.size()) * 12.0f);
        const float left = (std::max)(8.0f, (std::min)(hit.bounds.left, d2d_.Target()->GetSize().width - width - 8));
        const float top = hit.bounds.top > 52 ? hit.bounds.top - 42 : hit.bounds.bottom + 8;
        const D2D1_RECT_F tip = D2D1::RectF(left, top, left + width, top + 34);
        FillRounded(tip, 4, Color(0x020C11, .96f)); StrokeRounded(tip, 4, Color(0x4D7E8B));
        DrawText(hit.tooltip, D2D1::RectF(tip.left + 9, tip.top + 8, tip.right - 7, tip.bottom - 5), tinyFormat_.Get(), Color(0xE8FAFF));
    }
    if (focusedHit_ >= 0 && focusedHit_ < static_cast<int>(hitTargets_.size())) {
        StrokeRounded(hitTargets_[static_cast<size_t>(focusedHit_)].bounds, 3, Color(0x7ADCF5), 2);
    }
    const HRESULT result = d2d_.EndDraw();
    if (result == D2DERR_RECREATE_TARGET) d2d_.RecreateTarget(hwnd_);
    EndPaint(hwnd_, &paint);
}

D2D1_POINT_2F AutoOrganizePreviewWindow::PointInDips(POINT point) const {
    return D2D1::Point2F(
        static_cast<float>(MulDiv(point.x, 96, static_cast<int>(dpi_))),
        static_cast<float>(MulDiv(point.y, 96, static_cast<int>(dpi_))));
}

const AutoOrganizePreviewWindow::HitTarget*
AutoOrganizePreviewWindow::HitTest(POINT clientPixels) const {
    const D2D1_POINT_2F point = PointInDips(clientPixels);
    for (auto it = hitTargets_.rbegin(); it != hitTargets_.rend(); ++it) {
        if (Contains(it->bounds, point)) return &*it;
    }
    return nullptr;
}

bool AutoOrganizePreviewWindow::IsHovered(HitKind kind, int index) const {
    if (hoverHit_ < 0 ||
        hoverHit_ >= static_cast<int>(hitTargets_.size())) {
        return false;
    }
    const HitTarget& hit = hitTargets_[static_cast<std::size_t>(hoverHit_)];
    return hit.kind == kind && hit.index == index;
}

void AutoOrganizePreviewWindow::UpdateHover(POINT clientPixels) {
    const HitTarget* hit = HitTest(clientPixels);
    const int next = hit == nullptr ? -1 : static_cast<int>(hit - hitTargets_.data());
    if (next != hoverHit_) {
        hoverHit_ = next;
        KillTimer(hwnd_, kTooltipTimer);
        if (hoverHit_ >= 0) SetTimer(hwnd_, kTooltipTimer, 450, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (!trackingMouse_) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tracking);
        trackingMouse_ = true;
    }
}

void AutoOrganizePreviewWindow::ActivateHit(const HitTarget& hit) {
    switch (hit.kind) {
        case HitKind::Close:
        case HitKind::Cancel:
            ShowWindow(hwnd_, SW_HIDE);
            break;
        case HitKind::Minimize:
            ShowWindow(hwnd_, SW_MINIMIZE);
            break;
        case HitKind::Navigation:
            navigationFilter_ = hit.index;
            break;
        case HitKind::Monitor:
            monitorFilter_ = hit.index;
            break;
        case HitKind::Item:
            selectedDecision_ = hit.index;
            break;
        case HitKind::ItemCheck:
            if (hit.index >= 0 && hit.index < static_cast<int>(plan_.decisions.size())) {
                auto& decision = plan_.decisions[static_cast<size_t>(hit.index)];
                if (!decision.targetCategoryId.empty()) decision.selected = !decision.selected;
                selectedDecision_ = hit.index;
            }
            break;
        case HitKind::Regenerate:
            StartScan();
            break;
        case HitKind::Apply:
            if ((state_ == ViewState::Ready ||
                 (state_ == ViewState::Changed && !desktopChangeBlocksApply_)) &&
                applyHandler_) {
                state_ = ViewState::Applying;
                stateMessage_ = L"正在写入一项完整配置事务；真实文件和 Explorer 设置不会改变。";
                applyHandler_(plan_, layoutPlan_, hwnd_);
            }
            break;
        case HitKind::OverlayAction:
            if (state_ == ViewState::Scanning) {
                CancelScan(true);
            } else if (state_ == ViewState::Cancelled ||
                       state_ == ViewState::Empty ||
                       state_ == ViewState::ScanError) {
                StartScan();
            } else if (state_ == ViewState::ApplyError) {
                state_ = ViewState::Ready;
            } else if (state_ == ViewState::Success) {
                if (input_.undoAvailable && undoHandler_) {
                    state_ = ViewState::Undoing;
                    stateMessage_ = L"正在逐字段撤销仍安全的调整；之后手动修改的内容会保留。";
                    undoHandler_(hwnd_);
                } else {
                    ShowWindow(hwnd_, SW_HIDE);
                }
            } else if (state_ == ViewState::UndoSuccess) {
                ShowWindow(hwnd_, SW_HIDE);
            } else if (state_ == ViewState::UndoConflict ||
                       state_ == ViewState::UndoError) {
                state_ = ViewState::Success;
            }
            break;
        default:
            break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AutoOrganizePreviewWindow::SelectNextFocusable(int direction) {
    if (hitTargets_.empty()) return;
    if (focusedHit_ < 0) focusedHit_ = direction > 0 ? 0 : static_cast<int>(hitTargets_.size() - 1);
    else focusedHit_ = (focusedHit_ + direction + static_cast<int>(hitTargets_.size())) % static_cast<int>(hitTargets_.size());
    InvalidateRect(hwnd_, nullptr, FALSE);
}
