// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "permission_broker/allow_hidraw_device_rule.h"

#include <libudev.h>

using std::string;

namespace permission_broker {

AllowHidrawDeviceRule::AllowHidrawDeviceRule()
    : HidrawSubsystemUdevRule("AllowHidrawDeviceRule") {}

AllowHidrawDeviceRule::~AllowHidrawDeviceRule() {}

Rule::Result AllowHidrawDeviceRule::ProcessHidrawDevice(
    struct udev_device *device) {
  return ALLOW;
}

}  // namespace permission_broker
