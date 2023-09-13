// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DIAGNOSTICS_CROS_HEALTHD_SYSTEM_FAKE_FLOSS_EVENT_HUB_H_
#define DIAGNOSTICS_CROS_HEALTHD_SYSTEM_FAKE_FLOSS_EVENT_HUB_H_

#include <string>

#include <dbus/object_path.h>

#include "diagnostics/cros_healthd/system/floss_event_hub.h"
#include "diagnostics/dbus_bindings/floss/dbus-proxies.h"

namespace diagnostics {

class FakeFlossEventHub final : public FlossEventHub {
 public:
  FakeFlossEventHub() = default;
  FakeFlossEventHub(const FakeFlossEventHub&) = delete;
  FakeFlossEventHub& operator=(const FakeFlossEventHub&) = delete;

  // Send fake events.
  void SendAdapterAdded(
      org::chromium::bluetooth::BluetoothProxyInterface* adapter = nullptr);
  void SendAdapterRemoved(
      const dbus::ObjectPath& adapter_path = dbus::ObjectPath(""));
};

}  // namespace diagnostics

#endif  // DIAGNOSTICS_CROS_HEALTHD_SYSTEM_FAKE_FLOSS_EVENT_HUB_H_
