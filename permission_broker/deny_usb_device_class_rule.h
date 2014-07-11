// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERMISSION_BROKER_DENY_USB_DEVICE_CLASS_RULE_H_
#define PERMISSION_BROKER_DENY_USB_DEVICE_CLASS_RULE_H_

#include "permission_broker/usb_subsystem_udev_rule.h"

namespace permission_broker {

class DenyUsbDeviceClassRule : public UsbSubsystemUdevRule {
 public:
  DenyUsbDeviceClassRule(const uint8_t device_class);
  virtual ~DenyUsbDeviceClassRule();

  virtual Result ProcessUsbDevice(struct udev_device *device);

 private:
  const std::string device_class_;

  DISALLOW_COPY_AND_ASSIGN(DenyUsbDeviceClassRule);
};

}  // namespace permission_broker

#endif  // PERMISSION_BROKER_DENY_USB_DEVICE_CLASS_RULE_H_
