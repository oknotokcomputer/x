// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWERD_SYSTEM_FLOSS_BATTERY_PROVIDER_H_
#define POWER_MANAGER_POWERD_SYSTEM_FLOSS_BATTERY_PROVIDER_H_

#include <string>

#include <brillo/dbus/exported_object_manager.h>
#include <chromeos/dbus/service_constants.h>
#include <dbus/message.h>
#include <dbus/object_manager.h>

#include "power_manager/powerd/system/bluetooth_battery_provider.h"
#include "power_manager/powerd/system/bluetooth_manager_interface.h"
#include "power_manager/powerd/system/dbus_wrapper.h"

namespace power_manager::system {

// Represents Floss's battery provider for Human Interface Devices (HID). It
// manages the sending of battery data changes to the Floss daemon.
class FlossBatteryProvider : public BluetoothBatteryProvider,
                             public BluetoothManagerInterface {
 public:
  FlossBatteryProvider();

  // Initializes the provider.
  void Init(DBusWrapperInterface* dbus_wrapper);

  // Resets the state like it was just init-ed.
  void Reset() override;

  // Notify the battery provider manager about a change in device battery level.
  void UpdateDeviceBattery(const std::string& address, int level) override;

 private:
  friend class FlossBatteryProviderTest;

  // Whether or not this battery provider is registered with all services.
  bool IsRegistered();

  // BluetoothManagerInterface overrides.
  void RegisterBluetoothManagerCallback(bool available) override;
  void OnRegisteredBluetoothManagerCallback(dbus::Response* respose) override;
  void OnHciEnabledChanged(
      dbus::MethodCall* method_call,
      dbus::ExportedObject::ResponseSender response_sender) override;

  // Wrapper for interacting with DBus.
  DBusWrapperInterface* dbus_wrapper_;

  // DBus object proxy for interacting with the Bluetooth manager.
  dbus::ObjectProxy* bluetooth_manager_object_proxy_;

  // This provider is registered with the Bluetooth manager.
  bool is_registered_with_bluetooth_manager_ = false;

  // Weak pointer for callbacks to this object.
  base::WeakPtrFactory<FlossBatteryProvider> weak_ptr_factory_;
};

}  // namespace power_manager::system

#endif  // POWER_MANAGER_POWERD_SYSTEM_FLOSS_BATTERY_PROVIDER_H_
