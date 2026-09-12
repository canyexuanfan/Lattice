#pragma once

#include <cstdint>
#include <atomic>
#include <string>
#include <vector>

#include "desktop/DesktopItem.h"

namespace lattice::organize {

enum class Confidence {
    Low,
    Medium,
    High,
};

struct ItemSnapshot {
    std::wstring id;
    std::wstring parsingIdentity;
    std::wstring displayName;
    std::wstring path;
    std::wstring targetPath;
    std::wstring arguments;
    std::wstring workingDirectory;
    std::wstring productName;
    std::wstring companyName;
    std::wstring description;
    std::wstring publisher;
    std::wstring appUserModelId;
    std::wstring urlHost;
    std::wstring sourceCategoryId;
    std::wstring sourceCategoryName;
    std::wstring monitorId;
    DesktopItemKind kind = DesktopItemKind::File;
    std::size_t sourceIndex = 0;
    int displayX = 0;
    int displayY = 0;
    bool hasDisplayPosition = false;
    bool missing = false;
};

struct ExistingCategorySnapshot {
    std::wstring id;
    std::wstring name;
    std::wstring monitorId;
    bool locked = false;
};

struct Snapshot {
    std::uint64_t configRevision = 0;
    std::vector<ItemSnapshot> items;
    std::vector<ExistingCategorySnapshot> categories;
};

struct Decision {
    std::wstring itemId;
    std::wstring parsingIdentity;
    std::wstring sourceCategoryId;
    std::wstring sourceCategoryName;
    std::wstring targetCategoryId;
    std::wstring targetCategoryName;
    std::wstring monitorId;
    std::size_t sourceIndex = 0;
    Confidence confidence = Confidence::Low;
    bool selected = false;
    bool changesExistingOwnership = false;
    bool targetIsExistingCategory = false;
    std::wstring reason;
};

struct GroupPlan {
    std::wstring id;
    std::wstring name;
    std::wstring monitorId;
    bool existingCategory = false;
    bool createNewCategory = false;
    std::vector<std::wstring> itemIds;
};

struct Plan {
    std::wstring id;
    std::uint64_t baseConfigRevision = 0;
    std::vector<Decision> decisions;
    std::vector<GroupPlan> groups;
};

bool IsOwnershipAdjustment(const Decision& decision) noexcept;
Plan BuildPlan(const Snapshot& snapshot);
void EnrichSnapshotLocalMetadata(
    Snapshot& snapshot,
    const std::atomic<bool>* cancelRequested = nullptr);

}  // namespace lattice::organize
