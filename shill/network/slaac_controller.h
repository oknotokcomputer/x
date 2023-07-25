// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_NETWORK_SLAAC_CONTROLLER_H_
#define SHILL_NETWORK_SLAAC_CONTROLLER_H_

#include <memory>
#include <vector>

#include <base/cancelable_callback.h>
#include <base/memory/weak_ptr.h>
#include <base/time/time.h>
#include <net-base/ipv6_address.h>

#include "shill/event_dispatcher.h"
#include "shill/mockable.h"
#include "shill/net/rtnl_handler.h"
#include "shill/net/rtnl_listener.h"
#include "shill/network/proc_fs_stub.h"

namespace shill {

class SLAACController {
 public:
  // Event type for Network callback.
  enum class UpdateType {
    kAddress = 1,
    kRDNSS = 2,
  };
  using UpdateCallback = base::RepeatingCallback<void(UpdateType)>;

  SLAACController(int interface_index,
                  ProcFsStub* proc_fs,
                  RTNLHandler* rtnl_handler,
                  EventDispatcher* dispatcher);
  virtual ~SLAACController();

  mockable void RegisterCallback(UpdateCallback update_callback);

  // Start monitoring SLAAC RTNL from kernel. Note that we force flap
  // disable-IPv6 state on this call so that netdevice IPv6 state are refreshed.
  // If |link_local_address| is present, it is configured before SLAAC starts.
  mockable void Start(
      std::optional<net_base::IPv6Address> link_local_address = std::nullopt);
  // Stop monitoring SLAAC address on the netdevice and stop the DNS timer. The
  // SLAAC process itself in the kernel is not stopped.
  mockable void Stop();

  // Return the list of all SLAAC-configured addresses. The order is guaranteed
  // to match kernel preference so that the first element is always the
  // preferred address.
  mockable std::vector<net_base::IPv6CIDR> GetAddresses() const;

  // Get the IPv6 DNS server addresses received from RDNSS.
  mockable std::vector<net_base::IPv6Address> GetRDNSSAddresses() const;

 private:
  // TODO(b/227563210): Refactor to remove friend declaration after moving all
  // SLAAC functionality from DeviceInfo and Network to SLAACController.
  friend class SLAACControllerTest;

  // The data struct to store IP address received from RTNL together with its
  // flags and scope information.
  struct AddressData {
    AddressData(const net_base::IPv6CIDR& cidr_in,
                unsigned char flags_in,
                unsigned char scope_in)
        : cidr(cidr_in), flags(flags_in), scope(scope_in) {}
    net_base::IPv6CIDR cidr;
    unsigned char flags;
    unsigned char scope;
  };

  void AddressMsgHandler(const RTNLMessage& msg);
  void RDNSSMsgHandler(const RTNLMessage& msg);

  // Timer function for monitoring RDNSS's lifetime.
  void StartRDNSSTimer(base::TimeDelta lifetime);
  void StopRDNSSTimer();
  // Called when the lifetime for RDNSS expires.
  void RDNSSExpired();

  void ConfigureLinkLocalAddress();
  void SendRouterSolicitation();

  const int interface_index_;
  std::optional<net_base::IPv6Address> link_local_address_;

  // Cache of kernel SLAAC data collected through RTNL.
  std::vector<AddressData> slaac_addresses_;
  std::vector<net_base::IPv6Address> rdnss_addresses_;

  // Internal timer for RDNSS expiration.
  base::CancelableOnceClosure rdnss_expired_callback_;

  // Callbacks registered by RegisterCallbacks().
  UpdateCallback update_callback_;

  // Owned by Network
  ProcFsStub* proc_fs_;

  RTNLHandler* rtnl_handler_;
  std::unique_ptr<RTNLListener> address_listener_;
  std::unique_ptr<RTNLListener> rdnss_listener_;

  EventDispatcher* dispatcher_;

  base::WeakPtrFactory<SLAACController> weak_factory_{this};
};

}  // namespace shill

#endif  // SHILL_NETWORK_SLAAC_CONTROLLER_H_
