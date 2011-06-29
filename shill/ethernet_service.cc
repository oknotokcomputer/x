// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/ethernet_service.h"

#include <time.h>
#include <stdio.h>
#include <netinet/ether.h>
#include <linux/if.h>

#include <string>

#include <base/logging.h>
#include <chromeos/dbus/service_constants.h>

#include "shill/control_interface.h"
#include "shill/device.h"
#include "shill/device_info.h"
#include "shill/ethernet.h"
#include "shill/manager.h"
#include "shill/shill_event.h"

using std::string;

namespace shill {

EthernetService::EthernetService(ControlInterface *control_interface,
                                 EventDispatcher *dispatcher,
                                 const EthernetRefPtr &device,
                                 const string &name)
    : Service(control_interface, dispatcher, name),
      ethernet_(device),
      type_(flimflam::kTypeEthernet) {
  set_auto_connect(true);

  RegisterConstString(flimflam::kTypeProperty, &type_);
}

EthernetService::~EthernetService() { }

void EthernetService::Connect() { }

void EthernetService::Disconnect() { }

std::string EthernetService::GetDeviceRpcId() {
  return ethernet_->GetRpcIdentifier();
}

}  // namespace shill
