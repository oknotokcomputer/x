// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RMAD_MOCK_RMAD_INTERFACE_H_
#define RMAD_MOCK_RMAD_INTERFACE_H_

#include "rmad/rmad_interface.h"

#include <string>

#include <gmock/gmock.h>

namespace rmad {

class MockRmadInterface : public RmadInterface {
 public:
  MockRmadInterface() = default;
  virtual ~MockRmadInterface() = default;

  MOCK_METHOD(bool, SetUp, (scoped_refptr<DaemonCallback>), (override));
  MOCK_METHOD(RmadState::StateCase, GetCurrentStateCase, (), (override));
  MOCK_METHOD(void, TryTransitionNextStateFromCurrentState, (), (override));
  MOCK_METHOD(void, GetCurrentState, (GetStateCallback), (override));
  MOCK_METHOD(void,
              TransitionNextState,
              (const TransitionNextStateRequest&, GetStateCallback),
              (override));
  MOCK_METHOD(void, TransitionPreviousState, (GetStateCallback), (override));
  MOCK_METHOD(void, AbortRma, (AbortRmaCallback), (override));
  MOCK_METHOD(void, GetLog, (GetLogCallback), (override));
  MOCK_METHOD(void, SaveLog, (const std::string&, SaveLogCallback), (override));
  MOCK_METHOD(void,
              RecordBrowserActionMetric,
              (const RecordBrowserActionMetricRequest&,
               RecordBrowserActionMetricCallback),
              (override));
  MOCK_METHOD(bool, CanAbort, (), (const, override));
};

}  // namespace rmad

#endif  // RMAD_MOCK_RMAD_INTERFACE_H_
