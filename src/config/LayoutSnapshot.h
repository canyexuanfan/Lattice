#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config/ConfigStore.h"

struct LayoutRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct LayoutMonitorSnapshot {
    std::wstring id;
    LayoutRect workArea;
    int dpi = 96;
    bool primary = false;
};

struct LayoutCategorySnapshot {
    std::wstring categoryId;
    WindowConfig layout;
    std::vector<std::wstring> itemOrder;
};

struct LayoutSnapshot {
    int schemaVersion = 2;
    std::uint64_t timestamp = 0;
    bool legacyMainOnly = false;
    bool globalVisible = true;
    int viewMode = 1;
    WindowConfig main;
    std::vector<std::wstring> uncategorizedItemOrder;
    std::vector<LayoutCategorySnapshot> categories;
    std::vector<DesktopPlacementConfig> desktopDisplayLayout;
    std::vector<LayoutMonitorSnapshot> monitors;
};

struct LayoutIdentityIndex {
    std::vector<std::wstring> uniqueItemIds;
    std::vector<std::wstring> uniquePaths;
};

struct LayoutRestorePlan {
    AppConfig before;
    AppConfig candidate;
    bool legacyMainOnly = false;
    bool exactMonitorTopology = false;
    int restoredCategories = 0;
    int restoredItemOrders = 0;
    int restoredDesktopPositions = 0;
};

struct LayoutRestoreRequest {
    LayoutSnapshot snapshot;
    std::vector<LayoutMonitorSnapshot> currentMonitors;
    LayoutIdentityIndex identities;
    std::uint64_t desktopSnapshotRevision = 0;
    bool recoveredFromBackup = false;
};

struct LayoutRestoreResult {
    std::uint64_t token = 0;
    bool succeeded = false;
    bool rollback = false;
    bool legacyMainOnly = false;
    bool recoveredFromBackup = false;
    std::uint64_t desktopSnapshotRevision = 0;
    std::vector<LayoutMonitorSnapshot> expectedMonitors;
    AppConfig before;
    AppConfig candidate;
    std::wstring message;
};

LayoutSnapshot BuildLayoutSnapshot(
    const AppConfig& config,
    const std::vector<LayoutMonitorSnapshot>& monitors,
    std::uint64_t timestamp);

bool ValidateLayoutSnapshot(
    const LayoutSnapshot& snapshot,
    std::wstring& errorMessage);

bool SameLayoutMonitorTopology(
    const std::vector<LayoutMonitorSnapshot>& left,
    const std::vector<LayoutMonitorSnapshot>& right);

bool BuildLayoutRestorePlan(
    const LayoutSnapshot& snapshot,
    const AppConfig& current,
    const std::vector<LayoutMonitorSnapshot>& currentMonitors,
    const LayoutIdentityIndex& identities,
    LayoutRestorePlan& plan,
    std::wstring& errorMessage);
