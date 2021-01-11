// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <base/logging.h>

#include "minios/minios.h"
#include "minios/process_manager.h"

const char kDebugConsole[] = "/dev/pts/2";
const char kLogFile[] = "/log/recovery.log";

int MiniOs::Run() {
  LOG(INFO) << "Starting miniOS.";
  if (!screens_.Init()) {
    LOG(ERROR) << "Screens init failed. Exiting.";
    return 1;
  }
  screens_.MiniOsWelcomeOnSelect();

  // Start the shell on DEBUG console.
  return ProcessManager().RunCommand({"/bin/sh"}, kDebugConsole, kDebugConsole);
}
