// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_ETHERNET_SERVICE_
#define SHILL_ETHERNET_SERVICE_

#include <base/basictypes.h>

#include "shill/ethernet.h"
#include "shill/refptr_types.h"
#include "shill/shill_event.h"
#include "shill/service.h"

namespace shill {

class EthernetService : public Service {
 public:
  EthernetService(ControlInterface *control_interface,
                  EventDispatcher *dispatcher,
                  const EthernetRefPtr &device,
                  const std::string& name);
  ~EthernetService();
  void Connect();
  void Disconnect();

 protected:
  virtual std::string CalculateState() { return "idle"; }

 private:
  EthernetRefPtr ethernet_;
  const std::string type_;
  DISALLOW_COPY_AND_ASSIGN(EthernetService);
};

}  // namespace shill

#endif  // SHILL_ETHERNET_SERVICE_
