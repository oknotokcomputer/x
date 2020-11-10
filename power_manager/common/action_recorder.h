// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_COMMON_ACTION_RECORDER_H_
#define POWER_MANAGER_COMMON_ACTION_RECORDER_H_

#include <string>

#include <base/macros.h>

namespace power_manager {

// Joins a sequence of strings describing actions using commas. The list of
// actions must be terminated by a NULL pointer.
//
// The general pattern is that a test's implementation of a delegate derives
// from ActionRecorder and calls AppendAction() to build up a string listing
// actions in the order that they are invoked. The test then compares the
// delegate's list (as returned by GetActions()) against a string generated by
// passing the expected actions to JoinActions().
std::string JoinActions(const char* action, ...);

// Class that delegates used in testing can inherit from to record calls that
// are made by the code being tested.
class ActionRecorder {
 public:
  ActionRecorder();
  ActionRecorder(const ActionRecorder&) = delete;
  ActionRecorder& operator=(const ActionRecorder&) = delete;

  virtual ~ActionRecorder();

  // Returns a comma-separated string describing the actions that were
  // requested since the previous call to GetActions() (i.e. results are
  // non-repeatable).
  std::string GetActions();

 protected:
  // Appends |new_action| to |actions_|, using a comma as a separator if
  // other actions are already listed.
  void AppendAction(const std::string& new_action);

 private:
  // Comma-separated list of actions that have been performed.
  std::string actions_;
};

}  // namespace power_manager

#endif  // POWER_MANAGER_COMMON_ACTION_RECORDER_H_
