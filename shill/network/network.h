// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_NETWORK_NETWORK_H_
#define SHILL_NETWORK_NETWORK_H_

#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <base/functional/callback.h>
#include <base/observer_list.h>
#include <base/time/time.h>
#include <chromeos/patchpanel/dbus/client.h>
#include <net-base/ip_address.h>
#include <net-base/ipv6_address.h>
#include <net-base/network_config.h>
#include <net-base/rtnl_handler.h>

#include "shill/connection_diagnostics.h"
#include "shill/ipconfig.h"
#include "shill/metrics.h"
#include "shill/mockable.h"
#include "shill/network/compound_network_config.h"
#include "shill/network/dhcp_controller.h"
#include "shill/network/dhcp_provider.h"
#include "shill/network/dhcpv4_config.h"
#include "shill/network/network_applier.h"
#include "shill/network/network_priority.h"
#include "shill/network/proc_fs_stub.h"
#include "shill/network/slaac_controller.h"
#include "shill/portal_detector.h"
#include "shill/technology.h"

namespace shill {

class EventDispatcher;
class Service;

// An object of Network class represents a network interface in the kernel, and
// maintains the layer 3 configuration on this interface.
// TODO(b/232177767): Currently this class is mainly a wrapper of the Connection
// class.
class Network {
 public:
  // Handler of the events of the Network class, can be added to (or removed
  // from) a Network object by `RegisterEventHandler()` (or
  // `UnregisterEventHandler()`). The object implements this interface must have
  // a longer life time that the Network object, e.g., that object can be the
  // owner of this Network object. All the callbacks provide the listener with
  // the interface index where the event happened, to allow listening for events
  // in multiple Network objects at the same time.
  class EventHandler : public base::CheckedObserver {
   public:
    // Called every time when the network config on the connection is updated.
    // When this callback is called, the Network must be in a connected state,
    // but this signal does not always indicate a change from a non-connected
    // state to a connected state.
    // TODO(b/232177767): Currently this function will not be called if there is
    // an IPv6 update when IPv4 is working.
    virtual void OnConnectionUpdated(int interface_index) = 0;

    // Called when the Network becomes idle from a non-idle state (configuring
    // or connected), no matter if this state change is caused by a failure
    // (e.g., DHCP failure) or a user-initiate disconnect. |is_failure|
    // indicates this failure is triggered by a DHCP failure. Note that
    // currently this is the only failure type generated inside the Network
    // class.
    virtual void OnNetworkStopped(int interface_index, bool is_failure) = 0;

    // The IPConfig object lists held by this Network has changed.
    virtual void OnIPConfigsPropertyUpdated(int interface_index) = 0;

    // Called when a new DHCPv4 lease is obtained for this device. This is
    // called before OnConnectionUpdated() is called as a result of the lease
    // acquisition.
    virtual void OnGetDHCPLease(int interface_index) = 0;
    // Called when DHCPv4 fails to acquire a lease.
    virtual void OnGetDHCPFailure(int interface_index) = 0;
    // Called on when an IPv6 address is obtained from SLAAC. SLAAC is initiated
    // by the kernel when the link is connected and is currently not monitored
    // by shill. Derived class should implement this function to listen to this
    // event. Base class does nothing. This is called before
    // OnConnectionUpdated() is called and before captive portal detection is
    // started if IPv4 is not configured.
    virtual void OnGetSLAACAddress(int interface_index) = 0;

    // Called after IPv4 has been configured as a result of acquiring a new DHCP
    // lease. This is called after OnGetDHCPLease, OnIPConfigsPropertyUpdated,
    // and OnConnectionUpdated.
    virtual void OnIPv4ConfiguredWithDHCPLease(int interface_index) = 0;
    // Called after IPv6 has been configured as a result of acquiring an IPv6
    // address from the kernel when SLAAC completes. This is called after
    // OnGetSLAACAddress, OnIPConfigsPropertyUpdated, and OnConnectionUpdated
    // (if IPv4 is not yet configured).
    virtual void OnIPv6ConfiguredWithSLAACAddress(int interface_index) = 0;
    // Called after shill receives a NeighborReachabilityEventSignal from
    // patchpanel's link monitor for the network interface of this Network.
    virtual void OnNeighborReachabilityEvent(
        int interface_index,
        const net_base::IPAddress& ip_address,
        patchpanel::Client::NeighborRole role,
        patchpanel::Client::NeighborStatus status) = 0;

    // Called every time PortalDetector finishes a network validation attempt
    // starts. If network validation is used for this Service, PortalDetector
    // starts the first attempt when OnConnected() is called. PortalDetector may
    // run multiple times for the same network.
    virtual void OnNetworkValidationStart(int interface_index) = 0;
    // Called every time PortalDetector is stopped before completing a trial.
    virtual void OnNetworkValidationStop(int interface_index) = 0;
    // Called when a PortalDetector trial completes.
    // Called every time a PortalDetector attempt finishes and Internet
    // connectivity has been evaluated.
    virtual void OnNetworkValidationResult(
        int interface_index, const PortalDetector::Result& result) = 0;

    // Called when the Network object is about to be destroyed and become
    // invalid. Any EventHandler still registered should stop any reference
    // they hold for that Network object.
    virtual void OnNetworkDestroyed(int interface_index) = 0;
  };

  // Options for starting a network.
  struct StartOptions {
    // Start DHCP client on this interface if |dhcp| is not empty.
    std::optional<DHCPProvider::Options> dhcp;
    // Accept router advertisements for IPv6.
    bool accept_ra = false;
    // The link local address for the device that would be an override of the
    // default EUI-64 link local address assigned by the kernel. Used in
    // cellular where the link local address is generated from the network ID
    // specified by the carrier through bearer.
    std::optional<net_base::IPv6Address> link_local_address;
    // When set to true, neighbor events from link monitoring are ignored.
    bool ignore_link_monitoring = false;
    // PortalDetector probe configuration for network validation.
    PortalDetector::ProbingConfiguration probing_configuration;
  };

  // State for tracking the L3 connectivity (e.g., portal state is not
  // included).
  enum class State {
    // The Network is not started.
    kIdle,
    // The Network has been started. Waiting for IP configuration provisioned.
    kConfiguring,
    // The layer 3 connectivity has been established. At least one of IPv4 and
    // IPv6 configuration has been provisioned, and the other one can still be
    // in the configuring state.
    kConnected,
  };

  // Reasons for starting or restarting portal detection on a Network.
  enum class ValidationReason {
    // IPv4 or IPv6 configuration of the network has completed.
    kNetworkConnectionUpdate,
    // Service order has changed.
    kServiceReorder,
    // A Service property relevant to network validation has changed.
    kServicePropertyUpdate,
    // A Manager property relevant to network validation has changed.
    kManagerPropertyUpdate,
    // A DBus request to recheck network validation has been received.
    kDBusRequest,
    // A L2 neighbor event has been received for an ethernet link indicating
    // the gateway is not reachable. This event can trigger Internet access
    // revalidation checks only on ethernet links.
    kEthernetGatewayUnreachable,
    // A L2 neighbor event has been received for an ethernet link indicating
    // the gateway is reachable. This event can trigger Internet access
    // revalidation checks only on ethernet links.
    kEthernetGatewayReachable,
  };

  // Helper struct which keeps a history of network validation results over time
  // until network validation stops for the first time or until the Network
  // disconnect.
  class ValidationLog {
   public:
    ValidationLog(Technology technology, Metrics* metrics);
    void AddResult(const PortalDetector::Result& result);
    void SetCapportDHCPSupported();
    void SetCapportRASupported();
    void RecordMetrics() const;

   private:
    Technology technology_;
    Metrics* metrics_;
    base::TimeTicks connection_start_;
    std::vector<std::pair<base::TimeTicks, PortalDetector::ValidationState>>
        results_;
    bool capport_dhcp_supported_ = false;
    bool capport_ra_supported_ = false;
  };

  // Returns true if |reason| requires that network validation be entirely
  // restarted with the latest IP configuration settings.
  static bool ShouldResetNetworkValidation(ValidationReason reason);

  // Returns true if |reason| requires that the next network validation attempt
  // be scheduled immediately.
  static bool ShouldScheduleNetworkValidationImmediately(
      ValidationReason reason);

  explicit Network(
      int interface_index,
      const std::string& interface_name,
      Technology technology,
      bool fixed_ip_params,
      ControlInterface* control_interface,
      EventDispatcher* dispatcher,
      Metrics* metrics,
      NetworkApplier* network_applier = NetworkApplier::GetInstance());
  Network(const Network&) = delete;
  Network& operator=(const Network&) = delete;
  virtual ~Network();

  // Starts the network with the given |options|.
  mockable void Start(const StartOptions& options);
  // Stops the network connection. OnNetworkStopped() will be called when
  // cleaning up the network state is finished.
  mockable void Stop();

  State state() const { return state_; }

  mockable bool IsConnected() const { return state_ == State::kConnected; }

  // Return true if network validation result is present and state is
  // PortalDetector::ValidationState::kInternetConnectivity, otherwise return
  // false.
  mockable bool HasInternetConnectivity() const;

  void RegisterEventHandler(EventHandler* handler);
  void UnregisterEventHandler(EventHandler* handler);

  // Sets network config specific to technology. Currently this is used by
  // cellular and VPN.
  mockable void set_link_protocol_network_config(
      std::unique_ptr<net_base::NetworkConfig> config) {
    config_.SetFromLinkProtocol(std::move(config));
  }

  int interface_index() const { return interface_index_; }
  std::string interface_name() const { return interface_name_; }
  Technology technology() const { return technology_; }

  // Interfaces between Service and Network.
  // Callback invoked when the static IP properties configured on the selected
  // service changed.
  mockable void OnStaticIPConfigChanged(const net_base::NetworkConfig& config);
  // Register a callback that gets called when the |current_ipconfig_| changed.
  // This should only be used by Service.
  mockable void RegisterCurrentIPConfigChangeHandler(
      base::RepeatingClosure handler);
  // Returns the IPConfig object which is used to setup the Connection of this
  // Network. Returns nullptr if there is no such IPConfig. This is only used by
  // Service to expose its IPConfig dbus API. Other user who would like to get
  // the configuration of the Network should use GetNetworkConfig() instead.
  mockable IPConfig* GetCurrentIPConfig() const;
  // The net_base::NetworkConfig before applying the static one. Only needed by
  // Service to be exposed as a Savednet_base::NetworkConfig Service property
  // via D-Bus.
  // TODO(b/227715787): This D-Bus API should be deprecated.
  const net_base::NetworkConfig* GetSavedIPConfig() const;

  // Functions for DHCP.
  // Initiates renewal of existing DHCP lease. Return false if the renewal
  // failed immediately, or we don't have active lease now.
  mockable bool RenewDHCPLease();
  // Destroy the lease, if any, with this |name|.
  // Called by the service during Unload() as part of the cleanup sequence.
  mockable void DestroyDHCPLease(const std::string& name);
  // Calculates the duration till a DHCP lease is due for renewal, and stores
  // this value in |result|. Returns std::nullopt if there is no upcoming DHCP
  // lease renewal, base::TimeDelta wrapped in std::optional otherwise.
  mockable std::optional<base::TimeDelta> TimeToNextDHCPLeaseRenewal();

  // Invalidate the IPv6 config kept in shill and wait for the new config from
  // the kernel.
  mockable void InvalidateIPv6Config();

  // Returns a WeakPtr of the Network.
  base::WeakPtr<Network> AsWeakPtr() { return weak_factory_.GetWeakPtr(); }

  // Routing policy rules have priorities, which establishes the order in which
  // policy rules will be matched against the current traffic. The higher the
  // priority value, the lower the priority of the rule. 0 is the highest rule
  // priority and is generally reserved for the kernel.
  //
  // Updates the kernel's routing policy rule database such that policy rules
  // corresponding to this Connection will use |priority| as the "base
  // priority". This call also updates the systemwide DNS configuration if
  // necessary, and triggers captive portal detection if the connection has
  // transitioned from non-default to default.
  //
  // This function should only be called when the Network is connected,
  // otherwise the call is a no-op.
  mockable void SetPriority(NetworkPriority network_priority);

  // Returns the current priority of the Network.
  NetworkPriority GetPriority();

  // Returns the current active configuration of the Network. That could be from
  // DHCPv4, static IPv4 configuration, SLAAC, data-link layer control
  // protocols, or merged from multiple of these sources.
  const net_base::NetworkConfig& GetNetworkConfig() const;

  // Returns all known (global) addresses of the Network. That includes IPv4
  // address from link protocol, or from DHCPv4, or from static IPv4
  // configuration; and IPv6 address from SLAAC and/or from link protocol.
  // TODO(b/269401899): deprecate this and use GetNetworkConfig() instead.
  mockable std::vector<net_base::IPCIDR> GetAddresses() const;

  // Return all (both IPv4 and IPv6) DNS servers configured for the Network.
  // TODO(b/269401899): deprecate this and use GetNetworkConfig() instead.
  mockable std::vector<net_base::IPAddress> GetDNSServers() const;

  // Responds to a neighbor reachability event from patchpanel.
  mockable void OnNeighborReachabilityEvent(
      const patchpanel::Client::NeighborReachabilityEvent& event);

  // Starts or restarts network validation and reschedule a network validation
  // attempt if necessary. Depending on the current stage of network validation
  // (rows) and |reason| (columns), different effects are possible as summarized
  // in the table:
  //
  //             |  IP provisioning   |  schedule attempt  |      do not
  //             |       event        |    immediately     |     reschedule
  // ----------- +--------------------+--------------------+--------------------
  //  validation |                    |                    |
  //   stopped   |         a)         |         a)         |         a)
  // ------------+--------------------+--------------------+--------------------
  //   attempt   |                    |                    |
  //  scheduled  |         a)         |         b)         |         d)
  // ------------+--------------------+--------------------+--------------------
  //  currently  |                    |                    |
  //   running   |         a)         |         c)         |         d)
  // ------------+--------------------+--------------------+--------------------
  //   a) reinitialize |portal_detector_| & start a network validation attempt
  //      immediately.
  //   b) reschedule the next network validation attempt to run immediately.
  //   c) reschedule another network validation attempt immediately after the
  //      current one if the result is not conclusive (the result was not
  //      kInternetConnectivity or kPortalRedirect).
  //   e) do nothing, wait for the network validation attempt scheduled next to
  //      run.
  mockable bool StartPortalDetection(ValidationReason reason);
  // Schedules the next portal detection attempt for the current network
  // validation cycle. Returns true if portal detection restarts successfully.
  // If portal detection fails to restart, it is stopped.
  mockable bool RestartPortalDetection();
  // Stops the current network validation cycle if it is still running.
  mockable void StopPortalDetection();
  // Returns true if portal detection is currently in progress.
  mockable bool IsPortalDetectionInProgress() const;
  // Returns the PortalDetector::Result from the last network validation
  // attempt that completed, or nothing if no network validation attempt
  // has completed for this network connection yet.
  const std::optional<PortalDetector::Result>& network_validation_result()
      const {
    return network_validation_result_;
  }
  void StopNetworkValidationLog();

  // Initiates connection diagnostics on this Network.
  mockable void StartConnectionDiagnostics();

  // Start a separate PortalDetector instance for the purpose of connectivity
  // test.
  mockable void StartConnectivityTest(
      PortalDetector::ProbingConfiguration probe_config);

  // TODO(b/232177767): This group of getters and setters are only exposed for
  // the purpose of refactor. New code outside Device should not use these.
  IPConfig* ipconfig() const { return ipconfig_.get(); }
  IPConfig* ip6config() const { return ip6config_.get(); }
  void set_ipconfig(std::unique_ptr<IPConfig> config) {
    ipconfig_ = std::move(config);
  }
  void set_ip6config(std::unique_ptr<IPConfig> config) {
    ip6config_ = std::move(config);
  }
  bool fixed_ip_params() const { return fixed_ip_params_; }
  const std::string& logging_tag() const { return logging_tag_; }
  void set_logging_tag(const std::string& logging_tag) {
    logging_tag_ = logging_tag;
  }

  // Returns true if the IPv4 or IPv6 gateway respectively has been observed as
  // a reachable neighbor for the current active connection. Reachability can
  // only be obsrved on WiFi and Ethernet networks.
  mockable bool ipv4_gateway_found() const { return ipv4_gateway_found_; }
  mockable bool ipv6_gateway_found() const { return ipv6_gateway_found_; }

  // Returns true if the DHCP parameters provided indicate that the Chromebook
  // is tetherd to an Android mobile device or another Chromebook over a WiFi
  // hotspot or a USB ethernet connection ("ANDROID_METERED" vendor option 43).
  mockable bool IsConnectedViaTether() const;

  // Called by the Portal Detector whenever a trial completes.  Device
  // subclasses that choose unique mappings from portal results to connected
  // states can override this method in order to do so.
  // Visibility is public for usage in unit tests.
  void OnPortalDetectorResult(const PortalDetector::Result& result);

  // Helper functions to prepare data and call corresponding NetworkApplier
  // function. Protected for manual-triggering in test.
  mockable void ApplyNetworkConfig(NetworkApplier::Area area);

  void set_fixed_ip_params_for_testing(bool val) { fixed_ip_params_ = val; }
  void set_dhcp_provider_for_testing(DHCPProvider* provider) {
    dhcp_provider_ = provider;
  }
  void set_state_for_testing(State state) { state_ = state; }
  void set_primary_family_for_testing(
      std::optional<net_base::IPFamily> family) {
    primary_family_ = family;
  }
  void set_dhcp_network_config_for_testing(
      const net_base::NetworkConfig& network_config) {
    config_.SetFromDHCP(
        std::make_unique<net_base::NetworkConfig>(network_config));
  }
  void set_dhcp_data_for_testing(const DHCPv4Config::Data data) {
    dhcp_data_ = data;
  }
  // Take ownership of an external created ProcFsStub and return the point to
  // internal proc_fs_ after move.
  ProcFsStub* set_proc_fs_for_testing(std::unique_ptr<ProcFsStub> proc_fs) {
    proc_fs_ = std::move(proc_fs);
    return proc_fs_.get();
  }
  void set_portal_detector_for_testing(PortalDetector* portal_detector) {
    portal_detector_.reset(portal_detector);
  }
  void set_ignore_link_monitoring_for_testing(bool ignore_link_monitoring) {
    ignore_link_monitoring_ = ignore_link_monitoring;
  }
  void set_portal_detector_result_for_testing(
      const PortalDetector::Result& result) {
    network_validation_result_ = result;
  }

 private:
  // TODO(b/232177767): Refactor DeviceTest to remove this dependency.
  friend class DeviceTest;
  // TODO(b/232177767): Refactor DeviceTest to remove this dependency.
  friend class DevicePortalDetectorTest;
  // TODO(b/232177767): Refactor StaticIPParametersTest to remove this
  // dependency
  friend class StaticIPParametersTest;

  // Configures (or reconfigures) the Network for |family|. If |is_slaac, the
  // address and default route configuration is skipped.
  void SetupConnection(net_base::IPFamily family, bool is_slaac);

  // Creates a SLAACController object. Isolated for unit test mock injection.
  mockable std::unique_ptr<SLAACController> CreateSLAACController();

  // Constructs and returns a PortalDetector instance. Isolate
  // this function only for unit tests, so that we can inject a mock
  // PortalDetector object easily.
  mockable std::unique_ptr<PortalDetector> CreatePortalDetector();

  // Returns the preferred IPFamily for performing network validation with
  // PortalDetector. This defaults to IPv4 if both IPv4 and IPv6 are available.
  std::optional<net_base::IPFamily> GetNetworkValidationIPFamily() const;
  // Returns the list of name servers for performing network validation with
  // PortalDetector.
  std::vector<net_base::IPAddress> GetNetworkValidationDNSServers(
      net_base::IPFamily family) const;

  // Constructs and returns a ConnectionDiagnostics instance. Isolate
  // this function only for unit tests, so that we can inject a mock
  // ConnectionDiagnostics object easily.
  mockable std::unique_ptr<ConnectionDiagnostics> CreateConnectionDiagnostics(
      const net_base::IPAddress& ip_address,
      const net_base::IPAddress& gateway,
      const std::vector<net_base::IPAddress>& dns_list);

  // Shuts down and clears all the running state of this network. If
  // |trigger_callback| is true and the Network is started, OnNetworkStopped()
  // will be invoked with |is_failure|.
  void StopInternal(bool is_failure, bool trigger_callback);
  // Stop connection diagnostics if it is running.
  void StopConnectionDiagnostics();

  void ConnectivityTestCallback(const std::string& device_logging_tag,
                                const PortalDetector::Result& result);

  // Functions for IPv4.
  // Triggers a reconfiguration on connection for an IPv4 config change.
  void OnIPv4ConfigUpdated();
  // Callback registered with DHCPController. Also see the comment for
  // DHCPController::UpdateCallback.
  void OnIPConfigUpdatedFromDHCP(const net_base::NetworkConfig& network_config,
                                 const DHCPv4Config::Data& dhcp_data,
                                 bool new_lease_acquired);
  // Callback invoked on DHCP failures and RFC 8925 voluntary stops.
  void OnDHCPDrop(bool is_voluntary);

  // Functions for IPv6.
  // Called when IPv6 configuration changes.
  void OnIPv6ConfigUpdated();

  // Callback registered with SLAACController. |update_type| indicates the
  // update type (see comment in SLAACController declaration for detail).
  void OnUpdateFromSLAAC(SLAACController::UpdateType update_type);

  void UpdateIPConfigDBusObject();

  // Enable ARP filtering on the interface. Incoming ARP requests are responded
  // to only by the interface(s) owning the address. Outgoing ARP requests will
  // contain the best local address for the target.
  void EnableARPFiltering();

  // Report the current IP type metrics (v4, v6 or dual-stack) to UMA.
  void ReportIPType();

  // Report to UMA a failure in patchpanel::NeighborLinkMonitor for a WiFi or
  // Ethernet network connection.
  void ReportNeighborLinkMonitorFailure(Technology tech,
                                        net_base::IPFamily family,
                                        patchpanel::Client::NeighborRole role);

  const int interface_index_;
  const std::string interface_name_;
  const Technology technology_;
  // A header tag to use in LOG statement for identifying the Device and Service
  // associated with a Network connection.
  std::string logging_tag_;

  // If true, IP parameters should not be modified. This should not be changed
  // after a Network object is created. Make it modifiable just for unit tests.
  bool fixed_ip_params_;

  State state_ = State::kIdle;

  // A temporary helper flag simulating the legacy SetupConnection() state. Also
  // indicates which IPConfig will be seen by legacy Service->IPConfig dbus API.
  //  std::nullopt - network configuration has not been applied.
  //  kIPv6 - IPv6 configuration has been applied, but not IPv4.
  //  kIPv4 - IPv4 configuration has been applied (IPv6 can be yes or no).
  std::optional<net_base::IPFamily> primary_family_ = std::nullopt;

  std::unique_ptr<ProcFsStub> proc_fs_;

  std::unique_ptr<DHCPController> dhcp_controller_;
  std::unique_ptr<SLAACController> slaac_controller_;
  std::unique_ptr<IPConfig> ipconfig_;
  std::unique_ptr<IPConfig> ip6config_;
  NetworkPriority priority_;

  base::RepeatingClosure current_ipconfig_change_handler_;

  CompoundNetworkConfig config_;
  std::optional<DHCPv4Config::Data> dhcp_data_;

  // Track the current same-net multi-home state.
  bool is_multi_homed_ = false;

  // Remember which flag files were previously successfully written. Only used
  // in SetIPFlag().
  std::set<std::string> written_flags_;

  // When set to true, neighbor events from link monitoring are ignored. This
  // boolean is reevaluated for every new Network connection.
  bool ignore_link_monitoring_ = false;

  // If the gateway has ever been reachable for the current connection. Reset in
  // Start().
  bool ipv4_gateway_found_ = false;
  bool ipv6_gateway_found_ = false;

  PortalDetector::ProbingConfiguration probing_configuration_;
  std::unique_ptr<PortalDetector> portal_detector_;
  std::unique_ptr<ValidationLog> network_validation_log_;
  // Only defined if PortalDetector completed at least one attempt for the
  // current network connection.
  std::optional<PortalDetector::Result> network_validation_result_;
  std::unique_ptr<ConnectionDiagnostics> connection_diagnostics_;
  // Another instance of PortalDetector used for CreateConnectivityReport.
  std::unique_ptr<PortalDetector> connectivity_test_portal_detector_;

  base::ObserverList<EventHandler> event_handlers_;

  // Other dependencies.
  ControlInterface* control_interface_;
  EventDispatcher* dispatcher_;
  Metrics* metrics_;

  // Cache singleton pointers for performance and test purposes.
  DHCPProvider* dhcp_provider_;
  net_base::RTNLHandler* rtnl_handler_;
  NetworkApplier* network_applier_;

  // All the weak pointers created by this factory will be invalidated when the
  // Network state becomes kIdle. Can be useful when the concept of a connected
  // Network is needed. Note that the "connection" in the name is not the same
  // thing with the Connection class in shill.
  base::WeakPtrFactory<Network> weak_factory_for_connection_{this};

  base::WeakPtrFactory<Network> weak_factory_{this};
};

std::ostream& operator<<(std::ostream& stream, const Network& network);
std::ostream& operator<<(std::ostream& stream,
                         Network::ValidationReason reason);

}  // namespace shill

#endif  // SHILL_NETWORK_NETWORK_H_
