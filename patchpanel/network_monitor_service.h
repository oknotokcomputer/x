// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PATCHPANEL_NETWORK_MONITOR_SERVICE_H_
#define PATCHPANEL_NETWORK_MONITOR_SERVICE_H_

#include <map>
#include <memory>
#include <linux/neighbour.h>
#include <set>
#include <string>
#include <vector>

#include <base/memory/weak_ptr.h>
#include <base/timer/timer.h>
#include <gtest/gtest_prod.h>  // for FRIEND_TEST

#include "patchpanel/shill_client.h"
#include "shill/net/ip_address.h"
#include "shill/net/rtnl_listener.h"
#include "shill/net/rtnl_message.h"

namespace patchpanel {

// Monitors the reachability to the gateway and DNS servers on a given interface
// based on the information from the neighbor table in Linux kernel.
//
// This class interacts with the neighbor table via rtnetlink messages. The NUD
// (Neighbour Unreachability Detection) state in the neighbor table shows the
// bidirectional reachability between this interface and the given address. When
// OnIPConfigChanged() is called, a watching list is created with all valid
// addresses ({gateway, local dns servers} x {ipv4, ipv6}) in this ipconfig. For
// each address in the watching list, this class will:
// - Listen to the NUD state changed event from kernel;
// - When applicable, periodically set NUD state into NUD_PROBE to make the
//   kernel send probe packets.
//
// Normally, the following events will happen after an address is added:
// 1) We (this class) send a RTM_GETNEIGH request with NLM_F_DUMP flag to the
//    kernel to get the current state of this address (maybe with other
//    addresses together, since this is a dump request) (note that we cannot
//    send a real get request to retrieve a single entry, it's not supported by
//    Linux kernel v4.x and earlier versions);
// 2) On receiving the response from the kernel, we send a RTM_NEWNEIGH request
//    at once to set the NUD state of this address into NUD_PROBE, when
//    applicable;
// 3) The kernel sends out an ARP request (IPv4) or NS (IPv6) packet to this
//    address, and we are notified that the NUD state in the kernel table is
//    changed to NUD_PROBE.
// 4) The kernel receives the response packet and changes the state into
//    NUD_REACHABLE and notifies us.
// 5) Do nothing until the timer is triggered, and then jump to Step 2.
//
// In the case of "failure":
// - If we fail to get the information in Step 1, when the timer is triggered,
//   we will try to send the RTM_GETNEIGH request again (jump to Step 1).
// - If the kernel fails to detect the reachability in Step 3 (i.e., several
//   timeouts happen), we will be notified that the state is changed to
//   NUD_FAILED. Then we will do nothing for this address, until we heard about
//   it again from kernel.
//
// We use the following logic to determine L2 connectivity state of a neighbor,
// and broadcast a signal when the state changed, based on the NUD state:
// - If the NUD state is not in NUD_VALID, the neighbor is considered as
//   "disconnected".
// - If the NUD state is kept in NUD_VALID for a while, the neighbor is
//   considered as "connected". That means we will not send out the signal
//   immediately after the NUD state back to NUD_VALID, but wait for some time
//   to make sure it will not become invalid again soon.
// - A new neighbor will always be considered as "connected", before we know its
//   NUD state.
class NeighborLinkMonitor {
 public:
  static constexpr base::TimeDelta kActiveProbeInterval =
      base::TimeDelta::FromSeconds(60);

  // If a neighbor does not become invalid again in kBackToConnectedTimeout
  // after it comes back to NUD_VALID, we consider it as connected. Since
  // currently the "connected" signal is only used by shill for comparing link
  // monitors, we use a relatively longer value here.
  static constexpr base::TimeDelta kBackToConnectedTimeout =
      base::TimeDelta::FromMinutes(3);

  // Possible neighbor roles in the ipconfig. Represents each individual role by
  // a single bit to make the internal implementation easier.
  enum class NeighborRole {
    kGateway = 0x1,
    kDNSServer = 0x2,
    kGatewayAndDNSServer = 0x3,
  };

  using ConnectedStateChangedHandler =
      base::RepeatingCallback<void(int ifindex,
                                   const shill::IPAddress& ip_addr,
                                   NeighborRole role,
                                   bool connected)>;

  NeighborLinkMonitor(int ifindex,
                      const std::string& ifname,
                      shill::RTNLHandler* rtnl_handler,
                      ConnectedStateChangedHandler* neighbor_event_handler);
  ~NeighborLinkMonitor() = default;

  NeighborLinkMonitor(const NeighborLinkMonitor&) = delete;
  NeighborLinkMonitor& operator=(const NeighborLinkMonitor&) = delete;

  // This function will:
  // - Update |watching_entries_| with addresses in |ipconfig|;
  // - Call Start()/Stop() depends on whether the new |watching_entries_| is
  //   empty or not.
  // - For each new added address, send a neighbor get request to the kernel
  //   immediately.
  void OnIPConfigChanged(const ShillClient::IPConfig& ipconfig);

  static std::string NeighborRoleToString(
      NeighborLinkMonitor::NeighborRole role);

 private:
  // Represents an address and its corresponding role (a gateway or dns server
  // or both) we are watching. Also tracks the NUD state of this address in the
  // kernel.
  struct WatchingEntry {
    WatchingEntry(shill::IPAddress addr, NeighborRole role);
    WatchingEntry(const WatchingEntry&) = delete;
    WatchingEntry& operator=(const WatchingEntry&) = delete;

    std::string ToString() const;

    shill::IPAddress addr;
    NeighborRole role;

    // Reflects the NUD state of |addr| in the kernel neighbor table. Notes that
    // we use NUD_NONE (which is a dummy state in the kernel) to indicate that
    // we don't know this address from the kernel (i.e., this entry is just
    // added or the kernel tells us this entry has been deleted). If an entry is
    // in this state, we will send a dump request to the kernel when the timer
    // is triggered.
    // TODO(jiejiang): The following three fields are related. We may consider
    // changing this struct into a class if it becomes more complicated.
    uint16_t nud_state = NUD_NONE;

    // Indicates the L2 connectivity state of this neighbor. See the class
    // comment above for more details.
    bool connected = true;

    // This timer is set when the NUD state of neighbor back to NUD_VALID to
    // broadcast the connected signal, and reset if the NUD state becomes
    // invalid again before triggered.
    base::OneShotTimer back_to_connected_timer;
  };

  // ProbeAll() is invoked periodically by |probe_timer_|. It will scan the
  // entries in |watching_entries_|, and 1) send a RTM_NEWNEIGH message to set
  // the NUD state in the kernel to NUD_PROBE for each applicable entry, and 2)
  // send a dump request for this interface if there are any unknown entries.
  void ProbeAll();

  // Start() will set a repeating timer to run ProbeAll() periodically and start
  // the listener for RTNL messages (if they are already running then Start()
  // has no effect). Stop() will stop the timer and the listener.
  void Start();
  void Stop();

  void AddWatchingEntries(int prefix_length,
                          const std::string& addr,
                          const std::string& gateway,
                          const std::vector<std::string>& dns_addresses);

  // Creates a new entry if not exist or updates the role of an existing entry.
  void UpdateWatchingEntry(const shill::IPAddress& addr, NeighborRole role);

  // Sets the connected state of the watching entry with |addr| to |connected|,
  // and invokes |neighbor_event_handler_| to sent out a signal if the state
  // changes.
  void ChangeWatchingEntryState(const shill::IPAddress& addr, bool connected);

  void SendNeighborDumpRTNLMessage();
  void SendNeighborProbeRTNLMessage(const WatchingEntry& entry);
  void OnNeighborMessage(const shill::RTNLMessage& msg);

  int ifindex_;
  const std::string ifname_;
  std::map<shill::IPAddress, WatchingEntry> watching_entries_;
  std::unique_ptr<shill::RTNLListener> listener_;

  // Timer for running ProbeAll().
  base::RepeatingTimer probe_timer_;

  // RTNLHandler is a singleton object. Stores it here for test purpose.
  shill::RTNLHandler* rtnl_handler_;

  const ConnectedStateChangedHandler* neighbor_event_handler_;
};

class NetworkMonitorService {
 public:
  explicit NetworkMonitorService(
      ShillClient* shill_client,
      const NeighborLinkMonitor::ConnectedStateChangedHandler&
          neighbor_handler);
  ~NetworkMonitorService() = default;

  NetworkMonitorService(const NetworkMonitorService&) = delete;
  NetworkMonitorService& operator=(const NetworkMonitorService&) = delete;

  void Start();

 private:
  void OnDevicesChanged(const std::set<std::string>& added,
                        const std::set<std::string>& removed);
  void OnIPConfigsChanged(const std::string& device,
                          const ShillClient::IPConfig& ipconfig);

  // ifname => NeighborLinkMonitor.
  std::map<std::string, std::unique_ptr<NeighborLinkMonitor>>
      neighbor_link_monitors_;
  NeighborLinkMonitor::ConnectedStateChangedHandler neighbor_event_handler_;
  ShillClient* shill_client_;
  // RTNLHandler is a singleton object. Stores it here for test purpose.
  shill::RTNLHandler* rtnl_handler_;

  FRIEND_TEST(NetworkMonitorServiceTest, StartRTNLHanlderOnServiceStart);
  FRIEND_TEST(NetworkMonitorServiceTest, CallGetDevicePropertiesOnNewDevice);

  base::WeakPtrFactory<NetworkMonitorService> weak_factory_{this};
};

}  // namespace patchpanel

#endif  // PATCHPANEL_NETWORK_MONITOR_SERVICE_H_
