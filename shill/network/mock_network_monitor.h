// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_NETWORK_MOCK_NETWORK_MONITOR_H_
#define SHILL_NETWORK_MOCK_NETWORK_MONITOR_H_

#include <memory>

#include <gmock/gmock.h>
#include <net-base/ip_address.h>

#include "shill/metrics.h"
#include "shill/network/network_monitor.h"
#include "shill/network/validation_log.h"
#include "shill/technology.h"

namespace shill {

class MockNetworkMonitor : public NetworkMonitor {
 public:
  MockNetworkMonitor();
  ~MockNetworkMonitor() override;

  MOCK_METHOD(void, Start, (ValidationReason), (override));
  MOCK_METHOD(bool, Stop, (), (override));
  MOCK_METHOD(bool, IsRunning, (), (const, override));
};

class MockNetworkMonitorFactory : public NetworkMonitorFactory {
 public:
  MockNetworkMonitorFactory();
  ~MockNetworkMonitorFactory() override;

  MOCK_METHOD(std::unique_ptr<NetworkMonitor>,
              Create,
              (EventDispatcher*,
               Metrics*,
               NetworkMonitor::ClientNetwork*,
               Technology,
               int,
               std::string_view,
               PortalDetector::ProbingConfiguration,
               NetworkMonitor::ValidationMode validation_mode,
               std::unique_ptr<ValidationLog>,
               std::string_view),
              (override));
};

}  // namespace shill

#endif  // SHILL_NETWORK_MOCK_NETWORK_MONITOR_H_
