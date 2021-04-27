// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RMAD_STATE_HANDLER_COMPONENTS_REPAIR_STATE_HANDLER_H_
#define RMAD_STATE_HANDLER_COMPONENTS_REPAIR_STATE_HANDLER_H_

#include "rmad/state_handler/base_state_handler.h"

namespace rmad {

class ComponentsRepairStateHandler : public BaseStateHandler {
 public:
  explicit ComponentsRepairStateHandler(scoped_refptr<JsonStore> json_store);
  ~ComponentsRepairStateHandler() override = default;

  ASSIGN_STATE(RmadState::StateCase::kSelectComponents);
  SET_REPEATABLE;

  RmadState::StateCase GetNextStateCase() const override;
  RmadErrorCode UpdateState(const RmadState& state) override;
  RmadErrorCode ResetState() override;

 private:
  bool VerifyComponents() const;
};

}  // namespace rmad

#endif  // RMAD_STATE_HANDLER_COMPONENTS_REPAIR_STATE_HANDLER_H_
