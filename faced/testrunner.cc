// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <base/at_exit.h>
#include <base/threading/thread_task_runner_handle.h>
#include <brillo/message_loops/base_message_loop.h>
#include <brillo/test_helpers.h>
#include <mojo/core/embedder/embedder.h>
#include <mojo/core/embedder/scoped_ipc_support.h>

int main(int argc, char** argv) {
  SetUpTests(&argc, argv, true);
  base::AtExitManager at_exit;

  brillo::BaseMessageLoop message_loop;
  message_loop.SetAsCurrent();

  mojo::core::Init();
  mojo::core::ScopedIPCSupport ipc_support(
      base::ThreadTaskRunnerHandle::Get(),
      mojo::core::ScopedIPCSupport::ShutdownPolicy::FAST);

  return RUN_ALL_TESTS();
}
