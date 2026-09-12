#pragma once

#include "config/ConfigStore.h"

namespace lattice::organize {

bool ApplyAutoOrganizeTransaction(
    const AppConfig& current,
    const AutoOrganizeApplyRequest& request,
    AppConfig& candidate,
    AutoOrganizeTransactionResult& result);

bool UndoLastAutoOrganizeTransaction(
    const AppConfig& current,
    AppConfig& candidate,
    AutoOrganizeTransactionResult& result);

}  // namespace lattice::organize
