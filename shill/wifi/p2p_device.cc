// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/wifi/p2p_device.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <chromeos/dbus/shill/dbus-constants.h>
#include <net-base/byte_utils.h>

#include "shill/control_interface.h"
#include "shill/manager.h"
#include "shill/supplicant/supplicant_group_proxy_interface.h"
#include "shill/supplicant/supplicant_interface_proxy_interface.h"
#include "shill/supplicant/supplicant_p2pdevice_proxy_interface.h"
#include "shill/supplicant/wpa_supplicant.h"

namespace shill {

namespace {
// Stop p2p device and return error if group cannot be fully configured within
// |kStartTimeout| time.
static constexpr base::TimeDelta kStartTimeout = base::Seconds(10);
// Return error if p2p group cannot be fully stopped within |kStopTimeout| time.
static constexpr base::TimeDelta kStopTimeout = base::Seconds(5);

const char* GroupInfoState(P2PDevice::P2PDeviceState state) {
  switch (state) {
    case P2PDevice::P2PDeviceState::kGOStarting:
      return kP2PGroupInfoStateStarting;
    case P2PDevice::P2PDeviceState::kGOConfiguring:
      return kP2PGroupInfoStateConfiguring;
    case P2PDevice::P2PDeviceState::kGOActive:
      return kP2PGroupInfoStateActive;
    case P2PDevice::P2PDeviceState::kGOStopping:
      return kP2PGroupInfoStateStopping;
    case P2PDevice::P2PDeviceState::kUninitialized:
    case P2PDevice::P2PDeviceState::kReady:
    case P2PDevice::P2PDeviceState::kClientAssociating:
    case P2PDevice::P2PDeviceState::kClientConfiguring:
    case P2PDevice::P2PDeviceState::kClientConnected:
    case P2PDevice::P2PDeviceState::kClientDisconnecting:
      return kP2PGroupInfoStateIdle;
  }
  NOTREACHED() << "Unhandled P2P state " << static_cast<int>(state);
  return kP2PGroupInfoStateIdle;
}

const char* ClientInfoState(P2PDevice::P2PDeviceState state) {
  switch (state) {
    case P2PDevice::P2PDeviceState::kClientAssociating:
      return kP2PClientInfoStateAssociating;
    case P2PDevice::P2PDeviceState::kClientConfiguring:
      return kP2PClientInfoStateConfiguring;
    case P2PDevice::P2PDeviceState::kClientConnected:
      return kP2PClientInfoStateConnected;
    case P2PDevice::P2PDeviceState::kClientDisconnecting:
      return kP2PClientInfoStateDisconnecting;
    case P2PDevice::P2PDeviceState::kUninitialized:
    case P2PDevice::P2PDeviceState::kReady:
    case P2PDevice::P2PDeviceState::kGOStarting:
    case P2PDevice::P2PDeviceState::kGOConfiguring:
    case P2PDevice::P2PDeviceState::kGOActive:
    case P2PDevice::P2PDeviceState::kGOStopping:
      return kP2PClientInfoStateIdle;
  }
  NOTREACHED() << "Unhandled P2P state " << static_cast<int>(state);
  return kP2PClientInfoStateIdle;
}
}  // namespace

// Constructor function
P2PDevice::P2PDevice(Manager* manager,
                     LocalDevice::IfaceType iface_type,
                     const std::string& primary_link_name,
                     uint32_t phy_index,
                     uint32_t shill_id,
                     LocalDevice::EventCallback callback)
    : LocalDevice(manager, iface_type, std::nullopt, phy_index, callback),
      primary_link_name_(primary_link_name),
      shill_id_(shill_id),
      state_(P2PDeviceState::kUninitialized) {
  // A P2PDevice with a non-P2P interface type makes no sense.
  CHECK(iface_type == LocalDevice::IfaceType::kP2PGO ||
        iface_type == LocalDevice::IfaceType::kP2PClient);
  log_name_ = (iface_type == LocalDevice::IfaceType::kP2PGO)
                  ? "p2p_go_" + std::to_string(shill_id)
                  : "p2p_client_" + std::to_string(shill_id);
  supplicant_interface_proxy_.reset();
  supplicant_interface_path_ = RpcIdentifier("");
  supplicant_p2pdevice_proxy_.reset();
  supplicant_group_proxy_.reset();
  supplicant_group_path_ = RpcIdentifier("");
  supplicant_persistent_group_path_ = RpcIdentifier("");
  group_ssid_ = "";
  group_bssid_ = "";
  group_frequency_ = 0;
  group_passphrase_ = "";
  LOG(INFO) << log_name() << ": P2PDevice created";
}

P2PDevice::~P2PDevice() {
  LOG(INFO) << log_name() << ": P2PDevice destroyed";
}

// static
const char* P2PDevice::P2PDeviceStateName(P2PDeviceState state) {
  switch (state) {
    case P2PDeviceState::kUninitialized:
      return kP2PDeviceStateUninitialized;
    case P2PDeviceState::kReady:
      return kP2PDeviceStateReady;
    case P2PDeviceState::kClientAssociating:
      return kP2PDeviceStateClientAssociating;
    case P2PDeviceState::kClientConfiguring:
      return kP2PDeviceStateClientConfiguring;
    case P2PDeviceState::kClientConnected:
      return kP2PDeviceStateClientConnected;
    case P2PDeviceState::kClientDisconnecting:
      return kP2PDeviceStateClientDisconnecting;
    case P2PDeviceState::kGOStarting:
      return kP2PDeviceStateGOStarting;
    case P2PDeviceState::kGOConfiguring:
      return kP2PDeviceStateGOConfiguring;
    case P2PDeviceState::kGOActive:
      return kP2PDeviceStateGOActive;
    case P2PDeviceState::kGOStopping:
      return kP2PDeviceStateGOStopping;
  }
  NOTREACHED() << "Unhandled P2P state " << static_cast<int>(state);
  return "Invalid";
}

Stringmaps P2PDevice::GroupInfoClients() const {
  Stringmaps clients;
  for (auto const& peer : group_peers_) {
    clients.push_back(peer.second.get()->GetPeerProperties());
  }
  return clients;
}

KeyValueStore P2PDevice::GetGroupInfo() const {
  KeyValueStore group_info;
  if (iface_type() != LocalDevice::IfaceType::kP2PGO) {
    LOG(WARNING) << log_name() << ": Tried to get group info for iface_type "
                 << iface_type();
    return group_info;
  }
  group_info.Set<uint32_t>(kP2PGroupInfoShillIDProperty, shill_id());
  group_info.Set<String>(kP2PGroupInfoStateProperty, GroupInfoState(state_));

  if (!group_ssid_.empty())
    group_info.Set<String>(kP2PGroupInfoSSIDProperty, group_ssid_);

  if (!group_bssid_.empty())
    group_info.Set<String>(kP2PGroupInfoBSSIDProperty, group_bssid_);

  if (group_frequency_)
    group_info.Set<Integer>(kP2PGroupInfoFrequencyProperty, group_frequency_);

  if (!group_passphrase_.empty())
    group_info.Set<String>(kP2PGroupInfoPassphraseProperty, group_passphrase_);

  if (link_name().has_value())
    group_info.Set<String>(kP2PGroupInfoInterfaceProperty, *link_name());

  group_info.Set<Stringmaps>(kP2PGroupInfoClientsProperty, GroupInfoClients());

  // TODO(b/299915001): retrieve IPv4/IPv6Address from patchpanel
  // TODO(b/301049348): retrieve MacAddress from wpa_supplicant
  return group_info;
}

KeyValueStore P2PDevice::GetClientInfo() const {
  KeyValueStore client_info;
  if (iface_type() != LocalDevice::IfaceType::kP2PClient) {
    LOG(WARNING) << log_name() << ": Tried to get client info for iface_type "
                 << iface_type();
    return client_info;
  }
  client_info.Set<uint32_t>(kP2PClientInfoShillIDProperty, shill_id());
  client_info.Set<String>(kP2PClientInfoStateProperty, ClientInfoState(state_));

  if (!group_ssid_.empty())
    client_info.Set<String>(kP2PClientInfoSSIDProperty, group_ssid_);

  if (!group_bssid_.empty())
    client_info.Set<String>(kP2PClientInfoGroupBSSIDProperty, group_bssid_);

  if (group_frequency_)
    client_info.Set<Integer>(kP2PClientInfoFrequencyProperty, group_frequency_);

  if (!group_passphrase_.empty()) {
    client_info.Set<String>(kP2PClientInfoPassphraseProperty,
                            group_passphrase_);
  }

  if (link_name().has_value())
    client_info.Set<String>(kP2PClientInfoInterfaceProperty, *link_name());

  // TODO(b/299915001): retrieve IPv4/IPv6Address from Shill::Network class
  // TODO(b/301049348): retrieve MacAddress from wpa_supplicant
  // TODO(b/301049348): retrieve GO properties from wpa_supplicant
  return client_info;
}

bool P2PDevice::Start() {
  SetState(P2PDeviceState::kReady);
  return true;
}

bool P2PDevice::Stop() {
  bool ret = true;
  if (InClientState()) {
    if (!Disconnect()) {
      ret = false;
    }
  } else if (InGOState()) {
    if (!RemoveGroup()) {
      ret = false;
    }
  }
  SetState(P2PDeviceState::kUninitialized);
  return ret;
}

bool P2PDevice::CreateGroup(std::unique_ptr<P2PService> service) {
  if (state_ != P2PDeviceState::kReady) {
    LOG(ERROR) << log_name() << ": Tried to create group while in state "
               << P2PDeviceStateName(state_);
    return false;
  }
  if (!service) {
    LOG(ERROR) << log_name()
               << ": Tried to create a group with an empty service.";
    return false;
  }
  if (service_) {
    LOG(ERROR) << log_name()
               << ": Attempted to create group on a device which already has a "
                  "service configured.";
    return false;
  }
  KeyValueStore properties = service->GetSupplicantConfigurationParameters();
  if (!StartSupplicantGroupForGO(properties)) {
    return false;
  }
  SetService(std::move(service));
  SetState(P2PDeviceState::kGOStarting);
  return true;
}

bool P2PDevice::Connect(std::unique_ptr<P2PService> service) {
  if (state_ != P2PDeviceState::kReady) {
    LOG(ERROR) << log_name() << ": Tried to connect while in state "
               << P2PDeviceStateName(state_);
    return false;
  }
  if (!service) {
    LOG(ERROR) << log_name() << ": Tried to connect with an empty serveice.";
    return false;
  }
  if (service_) {
    LOG(ERROR) << log_name()
               << ": Attempted to connect to group on a device which already "
                  "has a service configured.";
    return false;
  }
  KeyValueStore properties = service->GetSupplicantConfigurationParameters();
  if (!StartSupplicantGroupForClient(properties)) {
    return false;
  }
  SetService(std::move(service));
  SetState(P2PDeviceState::kClientAssociating);
  return true;
}

bool P2PDevice::RemoveGroup() {
  if (!InGOState()) {
    LOG(WARNING) << log_name() << ": Tried to remove a group while in state "
                 << P2PDeviceStateName(state_);
    return false;
  }
  FinishSupplicantGroup();
  SetState(P2PDeviceState::kGOStopping);
  // TODO(b/308081318): delete service on GroupFinished
  DeleteService();
  return true;
}

bool P2PDevice::Disconnect() {
  if (!InClientState()) {
    LOG(WARNING) << log_name() << ": Tried to disconnect while in state "
                 << P2PDeviceStateName(state_);
    return false;
  }
  FinishSupplicantGroup();
  SetState(P2PDeviceState::kClientDisconnecting);
  // TODO(b/308081318): delete service on GroupFinished
  DeleteService();
  return true;
}

bool P2PDevice::InGOState() const {
  return (state_ == P2PDeviceState::kGOStarting ||
          state_ == P2PDeviceState::kGOConfiguring ||
          state_ == P2PDeviceState::kGOActive ||
          state_ == P2PDeviceState::kGOStopping);
}

bool P2PDevice::InClientState() const {
  return (state_ == P2PDeviceState::kClientAssociating ||
          state_ == P2PDeviceState::kClientConfiguring ||
          state_ == P2PDeviceState::kClientConnected ||
          state_ == P2PDeviceState::kClientDisconnecting);
}

SupplicantP2PDeviceProxyInterface* P2PDevice::SupplicantPrimaryP2PDeviceProxy()
    const {
  return manager()
      ->wifi_provider()
      ->p2p_manager()
      ->SupplicantPrimaryP2PDeviceProxy();
}

bool P2PDevice::StartSupplicantGroupForGO(const KeyValueStore& properties) {
  if (!SupplicantPrimaryP2PDeviceProxy()) {
    LOG(ERROR) << log_name()
               << ": Tried to start group while the primary P2PDevice proxy is "
                  "not connected";
    return false;
  }
  if (!SupplicantPrimaryP2PDeviceProxy()->GroupAdd(properties)) {
    LOG(ERROR) << log_name()
               << ": Failed to GroupAdd via the primary P2PDevice proxy";
    return false;
  }
  return true;
}

bool P2PDevice::StartSupplicantGroupForClient(const KeyValueStore& properties) {
  if (!SupplicantPrimaryP2PDeviceProxy()) {
    LOG(WARNING) << log_name()
                 << ": Tried to join group while the primary "
                    "P2PDevice proxy is not connected";
    return false;
  }
  // Right now, there are no commands available in wpa_supplicant to bypass
  // P2P discovery and join an existing P2P group directly. Instead `GroupAdd`
  // with persistent group object path and role specified as client can be used
  // to join the P2P network. For client mode, even if group is specified as
  // persistent, it will still follow the GO's lead and join as a non-persistent
  // group. For GO mode, the `GroupAdd` is used directly so that it creates
  // a non-persistent group.
  if (!SupplicantPrimaryP2PDeviceProxy()->AddPersistentGroup(
          properties, &supplicant_persistent_group_path_)) {
    LOG(ERROR) << log_name()
               << ": Failed to AddPersistentGroup via the primary"
                  " P2PDevice proxy";
    return false;
  }
  if (supplicant_persistent_group_path_.value().empty()) {
    LOG(ERROR) << log_name()
               << ": Got empty persistent group path from "
                  "the primary P2PDevice proxy";
    return false;
  }
  KeyValueStore p2pgroup_args;
  p2pgroup_args.Set<RpcIdentifier>(
      WPASupplicant::kGroupAddPropertyPersistentPath,
      supplicant_persistent_group_path_);
  if (!SupplicantPrimaryP2PDeviceProxy()->GroupAdd(p2pgroup_args)) {
    LOG(ERROR) << log_name()
               << ": Failed to GroupAdd via the primary "
                  "P2PDevice proxy";
    SupplicantPrimaryP2PDeviceProxy()->RemovePersistentGroup(
        supplicant_persistent_group_path_);
    supplicant_persistent_group_path_ = RpcIdentifier("");
    return false;
  }
  return true;
}

bool P2PDevice::FinishSupplicantGroup() {
  if (!supplicant_p2pdevice_proxy_) {
    LOG(ERROR)
        << log_name()
        << ": Tried to stop group while P2PDevice proxy is not connected";
    return false;
  }
  if (!supplicant_p2pdevice_proxy_->Disconnect()) {
    LOG(ERROR) << log_name() << ": Failed to Disconnect via P2PDevice proxy";
    return false;
  }
  return true;
}

void P2PDevice::SetService(std::unique_ptr<P2PService> service) {
  service_ = std::move(service);
  service_->SetState(LocalService::LocalServiceState::kStateStarting);
}

void P2PDevice::DeleteService() {
  if (!service_) {
    return;
  }
  service_->SetState(LocalService::LocalServiceState::kStateIdle);
  service_ = nullptr;
}

void P2PDevice::SetState(P2PDeviceState state) {
  if (state_ == state)
    return;
  ResetTimersOnStateChange(state);
  LOG(INFO) << log_name() << ": State changed: " << P2PDeviceStateName(state_)
            << " -> " << P2PDeviceStateName(state);
  state_ = state;
}

bool P2PDevice::ConnectToSupplicantInterfaceProxy(
    const RpcIdentifier& object_path) {
  if (supplicant_interface_proxy_) {
    LOG(WARNING) << log_name()
                 << ": Tried to connect to the Interface proxy while it is "
                    "already connected";
    return false;
  }
  supplicant_interface_proxy_ =
      ControlInterface()->CreateSupplicantInterfaceProxy(this, object_path);
  if (!supplicant_interface_proxy_) {
    LOG(ERROR) << log_name()
               << ": Failed to connect to the Interface proxy, path: "
               << object_path.value();
    return false;
  }
  supplicant_interface_path_ = object_path;
  LOG(INFO) << log_name() << ": Interface proxy connected, path: "
            << supplicant_interface_path_.value();
  return true;
}

void P2PDevice::DisconnectFromSupplicantInterfaceProxy() {
  if (supplicant_interface_proxy_) {
    LOG(INFO) << log_name() << ": Interface proxy disconnected, path: "
              << supplicant_interface_path_.value();
  }
  supplicant_interface_path_ = RpcIdentifier("");
  supplicant_interface_proxy_.reset();
}

String P2PDevice::GetInterfaceName() const {
  String ifname;
  if (!supplicant_interface_proxy_->GetIfname(&ifname)) {
    LOG(ERROR) << log_name() << ": Failed to GetIfname via Interface proxy";
    return "";
  }
  return ifname;
}

bool P2PDevice::ConnectToSupplicantP2PDeviceProxy(
    const RpcIdentifier& interface) {
  if (supplicant_p2pdevice_proxy_) {
    LOG(WARNING)
        << log_name()
        << ": Tried to connect to P2PDevice proxy while already connected";
    return false;
  }
  supplicant_p2pdevice_proxy_ =
      ControlInterface()->CreateSupplicantP2PDeviceProxy(this, interface);
  if (!supplicant_p2pdevice_proxy_) {
    LOG(ERROR) << log_name() << ": Failed to connect to P2PDevice proxy, path: "
               << interface.value();
    return false;
  }
  LOG(INFO) << log_name()
            << ": P2PDevice proxy connected, path: " << interface.value();
  return true;
}

void P2PDevice::DisconnectFromSupplicantP2PDeviceProxy() {
  if (supplicant_p2pdevice_proxy_) {
    supplicant_p2pdevice_proxy_.reset();
    LOG(INFO) << log_name() << ": P2PDevice proxy disconnected";
  }
}

bool P2PDevice::ConnectToSupplicantGroupProxy(const RpcIdentifier& group) {
  if (supplicant_group_proxy_) {
    LOG(WARNING) << log_name()
                 << ": Tried to connect to the Group proxy while it is already "
                    "connected";
    return false;
  }
  supplicant_group_proxy_ =
      ControlInterface()->CreateSupplicantGroupProxy(this, group);
  if (!supplicant_group_proxy_) {
    LOG(ERROR) << log_name() << ": Failed to connect to the Group proxy, path: "
               << group.value();
    return false;
  }
  supplicant_group_path_ = group;
  LOG(INFO) << log_name() << ": Group proxy connected, path: "
            << supplicant_group_path_.value();
  return true;
}

void P2PDevice::DisconnectFromSupplicantGroupProxy(void) {
  if (supplicant_group_proxy_) {
    LOG(INFO) << log_name() << ": Group proxy disconnected, path: "
              << supplicant_group_path_.value();
  }
  supplicant_group_path_ = RpcIdentifier("");
  supplicant_group_proxy_.reset();
}

String P2PDevice::GetGroupSSID() const {
  ByteArray ssid;
  if (!supplicant_group_proxy_->GetSSID(&ssid)) {
    LOG(ERROR) << log_name() << ": Failed to GetSSID via Group proxy";
    return "";
  }
  return net_base::byte_utils::ByteStringFromBytes(ssid);
}

String P2PDevice::GetGroupBSSID() const {
  ByteArray bssid;
  if (!supplicant_group_proxy_->GetBSSID(&bssid)) {
    LOG(ERROR) << log_name() << ": Failed to GetBSSID via Group proxy";
    return "";
  }
  return net_base::MacAddress::CreateFromBytes(bssid)->ToString();
}

Integer P2PDevice::GetGroupFrequency() const {
  uint16_t frequency = 0;
  if (!supplicant_group_proxy_->GetFrequency(&frequency)) {
    LOG(ERROR) << log_name() << ": Failed to GetFrequency via Group proxy";
    return 0;
  }
  return frequency;
}

String P2PDevice::GetGroupPassphrase() const {
  std::string passphrase;
  if (!supplicant_group_proxy_->GetPassphrase(&passphrase)) {
    LOG(ERROR) << log_name() << ": Failed to GetPassphrase via Group proxy";
    return "";
  }
  return passphrase;
}

bool P2PDevice::SetupGroup(const KeyValueStore& properties) {
  RpcIdentifier interface_path = RpcIdentifier("");
  if (properties.Contains<RpcIdentifier>(
          WPASupplicant::kGroupStartedPropertyInterfaceObject)) {
    interface_path = properties.Get<RpcIdentifier>(
        WPASupplicant::kGroupStartedPropertyInterfaceObject);
  }
  if (interface_path.value().empty()) {
    LOG(ERROR) << log_name() << ": Failed to " << __func__
               << " without interface path";
    return false;
  }
  RpcIdentifier group_path = RpcIdentifier("");
  if (properties.Contains<RpcIdentifier>(
          WPASupplicant::kGroupStartedPropertyGroupObject)) {
    group_path = properties.Get<RpcIdentifier>(
        WPASupplicant::kGroupStartedPropertyGroupObject);
  }
  if (group_path.value().empty()) {
    LOG(ERROR) << log_name() << ": Failed to " << __func__
               << " without group path";
    return false;
  }
  if (!ConnectToSupplicantInterfaceProxy(interface_path) ||
      !ConnectToSupplicantP2PDeviceProxy(interface_path) ||
      !ConnectToSupplicantGroupProxy(group_path)) {
    TeardownGroup();
    return false;
  }

  link_name_ = GetInterfaceName();
  if (!link_name_.value().empty())
    LOG(INFO) << log_name() << ": Link name configured: " << link_name_.value();

  group_ssid_ = GetGroupSSID();
  if (!group_ssid_.empty())
    LOG(INFO) << log_name() << ": SSID configured: " << group_ssid_;

  group_bssid_ = GetGroupBSSID();
  if (!group_bssid_.empty())
    LOG(INFO) << log_name() << ": BSSID configured: " << group_bssid_;

  group_frequency_ = GetGroupFrequency();
  if (group_frequency_)
    LOG(INFO) << log_name() << ": Freqency configured: " << group_frequency_;

  group_passphrase_ = GetGroupPassphrase();
  if (!group_passphrase_.empty())
    LOG(INFO) << log_name() << ": Passphrase configured: " << group_passphrase_;

  // TODO(b/308081318): This requires HotspotDevice to be fully responsible
  // for states and events handling. Currently DeviceEvent::kLinkUp/Down events
  // are partially handled by LocalService.
  // service_->SetState(LocalService::LocalServiceState::kStateUp);
  return true;
}

void P2PDevice::TeardownGroup(const KeyValueStore& properties) {
  RpcIdentifier interface_path = RpcIdentifier("");
  if (properties.Contains<RpcIdentifier>(
          WPASupplicant::kGroupFinishedPropertyInterfaceObject)) {
    interface_path = properties.Get<RpcIdentifier>(
        WPASupplicant::kGroupFinishedPropertyInterfaceObject);
  }
  CHECK(interface_path == supplicant_interface_path_);
  RpcIdentifier group_path = RpcIdentifier("");
  if (properties.Contains<RpcIdentifier>(
          WPASupplicant::kGroupFinishedPropertyGroupObject)) {
    group_path = properties.Get<RpcIdentifier>(
        WPASupplicant::kGroupFinishedPropertyGroupObject);
  }
  if (group_path != supplicant_group_path_) {
    LOG(WARNING) << log_name() << ": " << __func__
                 << " for unknown object, path: " << group_path.value();
  }
  TeardownGroup();
}

void P2PDevice::TeardownGroup() {
  // TODO(b/322557062): Ensure that the underlying kernel interface is properly
  // torn down.
  group_ssid_ = "";
  group_bssid_ = "";
  group_frequency_ = 0;
  group_passphrase_ = "";
  group_peers_.clear();
  link_name_ = std::nullopt;

  DisconnectFromSupplicantGroupProxy();
  DisconnectFromSupplicantP2PDeviceProxy();
  DisconnectFromSupplicantInterfaceProxy();

  if (!supplicant_persistent_group_path_.value().empty()) {
    SupplicantPrimaryP2PDeviceProxy()->RemovePersistentGroup(
        supplicant_persistent_group_path_);
    supplicant_persistent_group_path_ = RpcIdentifier("");
  }
}

void P2PDevice::GroupStarted(const KeyValueStore& properties) {
  LOG(INFO) << log_name() << ": Got " << __func__ << " while in state "
            << P2PDeviceStateName(state_);
  switch (state_) {
    // Expected P2P client state for GroupStarted event
    case P2PDeviceState::kClientAssociating:
      SetupGroup(properties);
      SetState(P2PDeviceState::kClientConfiguring);
      PostDeviceEvent(DeviceEvent::kLinkUp);
      AcquireClientIP();
      break;
    // Expected P2P GO state for GroupStarted event
    case P2PDeviceState::kGOStarting:
      SetupGroup(properties);
      SetState(P2PDeviceState::kGOConfiguring);
      PostDeviceEvent(DeviceEvent::kLinkUp);
      StartGroupNetwork();
      break;
    // Common states for all roles.
    case P2PDeviceState::kUninitialized:
    case P2PDeviceState::kReady:
    // P2P client states.
    case P2PDeviceState::kClientConfiguring:
    case P2PDeviceState::kClientConnected:
    case P2PDeviceState::kClientDisconnecting:
    // P2P GO states.
    case P2PDeviceState::kGOConfiguring:
    case P2PDeviceState::kGOActive:
    case P2PDeviceState::kGOStopping:
      LOG(WARNING) << log_name() << ": Ignored " << __func__
                   << " while in state " << P2PDeviceStateName(state_);
      break;
  }
}

void P2PDevice::GroupFinished(const KeyValueStore& properties) {
  LOG(INFO) << log_name() << ": Got " << __func__ << " while in state "
            << P2PDeviceStateName(state_);
  switch (state_) {
    // Expected P2P client/GO state for GroupFinished event
    case P2PDeviceState::kClientDisconnecting:
    case P2PDeviceState::kGOStopping:
      TeardownGroup(properties);
      SetState(P2PDeviceState::kReady);
      PostDeviceEvent(DeviceEvent::kLinkDown);
      break;
    // P2P client link failure states for GroupFinished event
    case P2PDeviceState::kClientConfiguring:
    case P2PDeviceState::kClientConnected:
      LOG(WARNING) << log_name()
                   << ": Client link failure, group finished while in state "
                   << P2PDeviceStateName(state_);
      TeardownGroup(properties);
      SetState(P2PDeviceState::kClientDisconnecting);
      PostDeviceEvent(DeviceEvent::kLinkFailure);
      break;
    // P2P GO link failure states for GroupFinished event
    case P2PDeviceState::kGOConfiguring:
    case P2PDeviceState::kGOActive:
      LOG(WARNING) << log_name()
                   << ": GO link failure, group finished while in state "
                   << P2PDeviceStateName(state_);
      TeardownGroup(properties);
      SetState(P2PDeviceState::kGOStopping);
      PostDeviceEvent(DeviceEvent::kLinkFailure);
      break;
    // P2P client/GO unknown error states for GroupFinished event
    case P2PDeviceState::kClientAssociating:
    case P2PDeviceState::kGOStarting:
      LOG(ERROR) << log_name() << ": Ignored " << __func__ << " while in state "
                 << P2PDeviceStateName(state_);
      break;
    // Common states for all roles.
    case P2PDeviceState::kUninitialized:
    case P2PDeviceState::kReady:
      LOG(WARNING) << log_name() << ": Ignored " << __func__
                   << " while in state " << P2PDeviceStateName(state_);
      break;
  }
}

void P2PDevice::GroupFormationFailure(const std::string& reason) {
  LOG(WARNING) << log_name() << ": Got " << __func__ << " while in state "
               << P2PDeviceStateName(state_);
  switch (state_) {
    // Expected P2P client state for GroupFormationFailure signal
    case P2PDeviceState::kClientAssociating:
      LOG(ERROR) << log_name()
                 << ": Failed to connect Client, group formation failure";
      SetState(P2PDeviceState::kClientDisconnecting);
      PostDeviceEvent(DeviceEvent::kLinkFailure);
      break;
    // Expected P2P GO state for GroupFormationFailure signal
    case P2PDeviceState::kGOStarting:
      LOG(ERROR) << log_name()
                 << ": Failed to start GO, group formation failure";
      SetState(P2PDeviceState::kGOStopping);
      PostDeviceEvent(DeviceEvent::kLinkFailure);
      break;
    // Common states for all roles.
    case P2PDeviceState::kUninitialized:
    case P2PDeviceState::kReady:
    // P2P client states.
    case P2PDeviceState::kClientConfiguring:
    case P2PDeviceState::kClientConnected:
    case P2PDeviceState::kClientDisconnecting:
    // P2P GO states.
    case P2PDeviceState::kGOConfiguring:
    case P2PDeviceState::kGOActive:
    case P2PDeviceState::kGOStopping:
      LOG(WARNING) << log_name() << ": Ignored " << __func__
                   << " while in state " << P2PDeviceStateName(state_);
      break;
  }
}

// TODO(b/299915001): The OnClientIPAcquired handler should be called
// internally in response to events from Shill::Network.
void P2PDevice::EmulateClientIPAcquired() {
  Dispatcher()->PostTask(
      FROM_HERE,
      base::BindOnce(&P2PDevice::OnClientIPAcquired, base::Unretained(this)));
}

// TODO(b/299915001): The NetworkStarted handler should be called internally
// in response to events from patchpanel.
void P2PDevice::EmulateGroupNetworkStarted() {
  Dispatcher()->PostTask(FROM_HERE,
                         base::BindOnce(&P2PDevice::OnGroupNetworkStarted,
                                        base::Unretained(this)));
}

// TODO(b/299915001): Actually trigger IP acquisition via Shill::Network.
void P2PDevice::AcquireClientIP() {
  EmulateClientIPAcquired();
}

// TODO(b/299915001): Actually trigger network creation via patchpanel.
void P2PDevice::StartGroupNetwork() {
  EmulateGroupNetworkStarted();
}

void P2PDevice::OnClientIPAcquired() {
  LOG(INFO) << log_name() << ": Got " << __func__ << " while in state "
            << P2PDeviceStateName(state_);
  switch (state_) {
    // Expected P2P client state for OnClientIPAcquired signal
    case P2PDeviceState::kClientConfiguring:
      SetState(P2PDeviceState::kClientConnected);
      PostDeviceEvent(DeviceEvent::kNetworkUp);
      break;
    // Common states for all roles.
    case P2PDeviceState::kUninitialized:
    case P2PDeviceState::kReady:
    // P2P client states.
    case P2PDeviceState::kClientAssociating:
    case P2PDeviceState::kClientConnected:
    case P2PDeviceState::kClientDisconnecting:
    // P2P GO states.
    case P2PDeviceState::kGOStarting:
    case P2PDeviceState::kGOConfiguring:
    case P2PDeviceState::kGOActive:
    case P2PDeviceState::kGOStopping:
      LOG(WARNING) << log_name() << ": Ignored " << __func__
                   << " while in state " << P2PDeviceStateName(state_);
      break;
  }
}

void P2PDevice::OnGroupNetworkStarted() {
  LOG(INFO) << log_name() << ": Got " << __func__ << " while in state "
            << P2PDeviceStateName(state_);
  switch (state_) {
    // Expected P2P GO state for NetworkStarted signal
    case P2PDeviceState::kGOConfiguring:
      SetState(P2PDeviceState::kGOActive);
      PostDeviceEvent(DeviceEvent::kNetworkUp);
      break;
    // Common states for all roles.
    case P2PDeviceState::kUninitialized:
    case P2PDeviceState::kReady:
    // P2P client states.
    case P2PDeviceState::kClientAssociating:
    case P2PDeviceState::kClientConfiguring:
    case P2PDeviceState::kClientConnected:
    case P2PDeviceState::kClientDisconnecting:
    // P2P GO states.
    case P2PDeviceState::kGOStarting:
    case P2PDeviceState::kGOActive:
    case P2PDeviceState::kGOStopping:
      LOG(WARNING) << log_name() << ": Ignored " << __func__
                   << " while in state " << P2PDeviceStateName(state_);
      break;
  }
}

void P2PDevice::NetworkFinished() {
  LOG(INFO) << log_name() << ": Got " << __func__ << " while in state "
            << P2PDeviceStateName(state_);
  // TODO(b/308081318): teardown group/connection or ignore unexpected state
  PostDeviceEvent(DeviceEvent::kNetworkDown);
}

void P2PDevice::NetworkFailure(const std::string& reason) {
  LOG(WARNING) << log_name() << ": Got " << __func__ << " while in state "
               << P2PDeviceStateName(state_) << ", reason: " << reason;
  // TODO(b/308081318): teardown group/connection or ignore unexpected state
  PostDeviceEvent(DeviceEvent::kNetworkFailure);
}

void P2PDevice::PeerJoined(const dbus::ObjectPath& peer) {
  LOG(INFO) << log_name() << ": Got " << __func__ << " while in state "
            << P2PDeviceStateName(state_);
  if (state_ != P2PDeviceState::kGOConfiguring &&
      state_ != P2PDeviceState::kGOActive) {
    return;
  }

  if (base::Contains(group_peers_, peer)) {
    LOG(WARNING) << "Ignored " << __func__
                 << " while already connected, path: " << peer.value();
    return;
  }
  group_peers_[peer] =
      std::make_unique<P2PPeer>(this, peer, ControlInterface());
  LOG(INFO) << log_name() << ": Peer connected, path: " << peer.value();
  PostDeviceEvent(DeviceEvent::kPeerConnected);
}

void P2PDevice::PeerDisconnected(const dbus::ObjectPath& peer) {
  LOG(INFO) << log_name() << ": Got " << __func__ << " while in state "
            << P2PDeviceStateName(state_);

  if (state_ != P2PDeviceState::kGOConfiguring &&
      state_ != P2PDeviceState::kGOActive) {
    return;
  }

  if (!base::Contains(group_peers_, peer)) {
    LOG(WARNING) << "Ignored " << __func__
                 << " while not connected, path: " << peer.value();
    return;
  }
  LOG(INFO) << log_name() << ": Peer disconnected, path: " << peer.value();
  group_peers_.erase(peer);
  PostDeviceEvent(DeviceEvent::kPeerDisconnected);
}

void P2PDevice::StartingTimerExpired() {
  switch (state_) {
    // P2P client failure states for StartingTimerExpired event
    case P2PDeviceState::kClientAssociating:
      LOG(ERROR) << log_name()
                 << ": Failed to connect Client, timer expired while in state "
                 << P2PDeviceStateName(state_);
      FinishSupplicantGroup();
      SetState(P2PDeviceState::kClientDisconnecting);
      PostDeviceEvent(DeviceEvent::kLinkFailure);
      break;
    case P2PDeviceState::kClientConfiguring:
      LOG(ERROR) << log_name()
                 << ": Failed to connect Client, timer expired while in state "
                 << P2PDeviceStateName(state_);
      FinishSupplicantGroup();
      SetState(P2PDeviceState::kClientDisconnecting);
      PostDeviceEvent(DeviceEvent::kNetworkFailure);
      break;
    // P2P GO failure states for StartingTimerExpired event
    case P2PDeviceState::kGOStarting:
      LOG(ERROR) << log_name()
                 << ": Failed to start GO, timer expired while in state "
                 << P2PDeviceStateName(state_);
      FinishSupplicantGroup();
      SetState(P2PDeviceState::kGOStopping);
      PostDeviceEvent(DeviceEvent::kLinkFailure);
      break;
    case P2PDeviceState::kGOConfiguring:
      LOG(ERROR) << log_name()
                 << ": Failed to start GO, timer expired while in state "
                 << P2PDeviceStateName(state_);
      FinishSupplicantGroup();
      SetState(P2PDeviceState::kGOStopping);
      PostDeviceEvent(DeviceEvent::kNetworkFailure);
      break;
    // Common states for all roles.
    case P2PDeviceState::kUninitialized:
    case P2PDeviceState::kReady:
    // P2P client states.
    case P2PDeviceState::kClientConnected:
    case P2PDeviceState::kClientDisconnecting:
    // P2P GO states.
    case P2PDeviceState::kGOActive:
    case P2PDeviceState::kGOStopping:
      LOG(WARNING) << log_name() << ": Ignored " << __func__
                   << " while in state " << P2PDeviceStateName(state_);
      break;
  }
}

void P2PDevice::StoppingTimerExpired() {
  switch (state_) {
    // P2P client failure states for StoppingTimerExpired event
    case P2PDeviceState::kClientDisconnecting:
      LOG(WARNING)
          << log_name()
          << ": Forcing Client to disconnect, timer expired while in state "
          << P2PDeviceStateName(state_);
      TeardownGroup();
      PostDeviceEvent(DeviceEvent::kLinkDown);
      break;
    // P2P GO failure states for StoppingTimerExpired event
    case P2PDeviceState::kGOStopping:
      LOG(WARNING) << log_name()
                   << ": Forcing GO to stop, timer expired while in state "
                   << P2PDeviceStateName(state_);
      TeardownGroup();
      PostDeviceEvent(DeviceEvent::kLinkDown);
      break;
    // Common states for all roles.
    case P2PDeviceState::kUninitialized:
    case P2PDeviceState::kReady:
    // P2P client states.
    case P2PDeviceState::kClientAssociating:
    case P2PDeviceState::kClientConfiguring:
    case P2PDeviceState::kClientConnected:
    // P2P GO states.
    case P2PDeviceState::kGOStarting:
    case P2PDeviceState::kGOConfiguring:
    case P2PDeviceState::kGOActive:
      LOG(WARNING) << log_name() << ": Ignored " << __func__
                   << " while in state " << P2PDeviceStateName(state_);
      break;
  }
}

void P2PDevice::ResetTimersOnStateChange(P2PDeviceState new_state) {
  switch (new_state) {
    case P2PDeviceState::kClientAssociating:
    case P2PDeviceState::kGOStarting:
      start_timer_callback_.Reset(base::BindOnce(
          &P2PDevice::StartingTimerExpired, weak_ptr_factory_.GetWeakPtr()));
      manager()->dispatcher()->PostDelayedTask(
          FROM_HERE, start_timer_callback_.callback(), kStartTimeout);
      LOG(INFO) << log_name()
                << ": Starting timer armed, timeout: " << kStartTimeout;
      break;
    case P2PDeviceState::kClientConnected:
    case P2PDeviceState::kGOActive:
      if (!start_timer_callback_.IsCancelled()) {
        start_timer_callback_.Cancel();
        LOG(INFO) << log_name() << ": Starting timer cancelled";
      }
      break;
    case P2PDeviceState::kClientDisconnecting:
    case P2PDeviceState::kGOStopping:
      if (!start_timer_callback_.IsCancelled()) {
        start_timer_callback_.Cancel();
        LOG(INFO) << log_name() << ": Starting timer cancelled";
      }
      stop_timer_callback_.Reset(base::BindOnce(
          &P2PDevice::StoppingTimerExpired, weak_ptr_factory_.GetWeakPtr()));
      manager()->dispatcher()->PostDelayedTask(
          FROM_HERE, stop_timer_callback_.callback(), kStopTimeout);
      LOG(INFO) << log_name()
                << ": Stopping timer armed, timeout: " << kStopTimeout;
      break;
    case P2PDeviceState::kReady:
      if (!start_timer_callback_.IsCancelled()) {
        start_timer_callback_.Cancel();
        LOG(INFO) << log_name() << ": Starting timer cancelled";
      }
      if (!stop_timer_callback_.IsCancelled()) {
        stop_timer_callback_.Cancel();
        LOG(INFO) << log_name() << ": Stopping timer cancelled";
      }
      break;
    // Common states for all roles.
    case P2PDeviceState::kUninitialized:
    case P2PDeviceState::kClientConfiguring:
    case P2PDeviceState::kGOConfiguring:
      break;
  }
}

}  // namespace shill
