// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HEARTD_DAEMON_HEARTD_H_
#define HEARTD_DAEMON_HEARTD_H_

#include <memory>

#include <brillo/daemons/daemon.h>
#include <mojo/core/embedder/scoped_ipc_support.h>

#include "heartd/daemon/action_runner.h"
#include "heartd/daemon/dbus_connector.h"
#include "heartd/daemon/heartbeat_manager.h"
#include "heartd/daemon/mojo_service.h"
#include "heartd/mojom/heartd.mojom.h"

namespace heartd {

class HeartdDaemon final : public brillo::Daemon {
 public:
  explicit HeartdDaemon(int sysrq_fd);
  HeartdDaemon(const HeartdDaemon&) = delete;
  HeartdDaemon& operator=(const HeartdDaemon&) = delete;
  ~HeartdDaemon() override;

 private:
  // For mojo thread initialization.
  std::unique_ptr<mojo::core::ScopedIPCSupport> ipc_support_;
  // Used to connect to dbus.
  std::unique_ptr<DbusConnector> dbus_connector_ = nullptr;
  // Used to run action.
  std::unique_ptr<ActionRunner> action_runner_ = nullptr;
  // Used to manage heartbeat service.
  std::unique_ptr<HeartbeatManager> heartbeat_manager_ = nullptr;
  // Used to provide mojo interface to mojo service manager.
  std::unique_ptr<HeartdMojoService> mojo_service_ = nullptr;
};

}  // namespace heartd

#endif  // HEARTD_DAEMON_HEARTD_H_
