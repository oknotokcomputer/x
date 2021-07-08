// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RMAD_STATE_HANDLER_UPDATE_CHROME_STATE_HANDLER_H_
#define RMAD_STATE_HANDLER_UPDATE_CHROME_STATE_HANDLER_H_

#include "rmad/state_handler/base_state_handler.h"

namespace rmad {

class UpdateChromeStateHandler : public BaseStateHandler {
 public:
  explicit UpdateChromeStateHandler(scoped_refptr<JsonStore> json_store);

  ASSIGN_STATE(RmadState::StateCase::kUpdateChrome);
  SET_REPEATABLE;

  RmadErrorCode InitializeState() override;
  GetNextStateCaseReply GetNextStateCase(const RmadState& state) override;

 protected:
  ~UpdateChromeStateHandler() override = default;
};

}  // namespace rmad

#endif  // RMAD_STATE_HANDLER_UPDATE_CHROME_STATE_HANDLER_H_
