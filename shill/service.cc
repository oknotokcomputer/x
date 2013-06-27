// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/service.h"

#include <time.h>
#include <stdio.h>

#include <map>
#include <string>
#include <vector>

#include <base/memory/scoped_ptr.h>
#include <base/string_number_conversions.h>
#include <base/stringprintf.h>
#include <chromeos/dbus/service_constants.h>

#include "shill/connection.h"
#include "shill/control_interface.h"
#include "shill/diagnostics_reporter.h"
#include "shill/eap_credentials.h"
#include "shill/error.h"
#include "shill/http_proxy.h"
#include "shill/logging.h"
#include "shill/manager.h"
#include "shill/metrics.h"
#include "shill/profile.h"
#include "shill/property_accessor.h"
#include "shill/refptr_types.h"
#include "shill/service_dbus_adaptor.h"
#include "shill/sockets.h"
#include "shill/store_interface.h"

using base::Bind;
using std::deque;
using std::map;
using std::string;
using std::vector;

namespace shill {

const char Service::kAutoConnBusy[] = "busy";
const char Service::kAutoConnConnected[] = "connected";
const char Service::kAutoConnConnecting[] = "connecting";
const char Service::kAutoConnExplicitDisconnect[] = "explicitly disconnected";
const char Service::kAutoConnNotConnectable[] = "not connectable";
const char Service::kAutoConnOffline[] = "offline";
const char Service::kAutoConnThrottled[] = "throttled";

const size_t Service::kEAPMaxCertificationElements = 10;

const char Service::kCheckPortalAuto[] = "auto";
const char Service::kCheckPortalFalse[] = "false";
const char Service::kCheckPortalTrue[] = "true";

const char Service::kErrorDetailsNone[] = "";

const int Service::kPriorityNone = 0;

const char Service::kServiceSortAutoConnect[] = "AutoConnect";
const char Service::kServiceSortConnectable[] = "Connectable";
const char Service::kServiceSortDependency[] = "Dependency";
const char Service::kServiceSortFavorite[] = "Favorite";
const char Service::kServiceSortIsConnected[] = "IsConnected";
const char Service::kServiceSortIsConnecting[] = "IsConnecting";
const char Service::kServiceSortIsFailed[] = "IsFailed";
const char Service::kServiceSortIsPortalled[] = "IsPortal";
const char Service::kServiceSortPriority[] = "Priority";
const char Service::kServiceSortSecurityEtc[] = "SecurityEtc";
const char Service::kServiceSortTechnology[] = "Technology";
const char Service::kServiceSortUniqueName[] = "UniqueName";

const char Service::kStorageAutoConnect[] = "AutoConnect";
const char Service::kStorageCheckPortal[] = "CheckPortal";
const char Service::kStorageError[] = "Error";
const char Service::kStorageFavorite[] = "Favorite";
const char Service::kStorageGUID[] = "GUID";
const char Service::kStorageHasEverConnected[] = "HasEverConnected";
const char Service::kStorageName[] = "Name";
const char Service::kStoragePriority[] = "Priority";
const char Service::kStorageProxyConfig[] = "ProxyConfig";
const char Service::kStorageSaveCredentials[] = "SaveCredentials";
const char Service::kStorageType[] = "Type";
const char Service::kStorageUIData[] = "UIData";

const uint8 Service::kStrengthMax = 100;
const uint8 Service::kStrengthMin = 0;

const uint64 Service::kMaxAutoConnectCooldownTimeMilliseconds = 30 * 60 * 1000;
const uint64 Service::kMinAutoConnectCooldownTimeMilliseconds = 1000;
const uint64 Service::kAutoConnectCooldownBackoffFactor = 2;

const int Service::kDisconnectsMonitorSeconds = 5 * 60;
const int Service::kMisconnectsMonitorSeconds = 5 * 60;
const int Service::kReportDisconnectsThreshold = 2;
const int Service::kReportMisconnectsThreshold = 3;
const int Service::kMaxDisconnectEventHistory = 20;

// static
unsigned int Service::serial_number_ = 0;

Service::Service(ControlInterface *control_interface,
                 EventDispatcher *dispatcher,
                 Metrics *metrics,
                 Manager *manager,
                 Technology::Identifier technology)
    : weak_ptr_factory_(this),
      state_(kStateIdle),
      previous_state_(kStateIdle),
      failure_(kFailureUnknown),
      auto_connect_(false),
      check_portal_(kCheckPortalAuto),
      connectable_(false),
      error_(ConnectFailureToString(failure_)),
      error_details_(kErrorDetailsNone),
      explicitly_disconnected_(false),
      favorite_(false),
      priority_(kPriorityNone),
      crypto_algorithm_(kCryptoNone),
      key_rotation_(false),
      endpoint_auth_(false),
      strength_(0),
      save_credentials_(true),
      technology_(technology),
      failed_time_(0),
      has_ever_connected_(false),
      auto_connect_cooldown_milliseconds_(0),
      store_(PropertyStore::PropertyChangeCallback(
          base::Bind(&Service::OnPropertyChanged,
                     weak_ptr_factory_.GetWeakPtr()))),
      dispatcher_(dispatcher),
      unique_name_(base::UintToString(serial_number_++)),
      friendly_name_(unique_name_),
      adaptor_(control_interface->CreateServiceAdaptor(this)),
      metrics_(metrics),
      manager_(manager),
      sockets_(new Sockets()),
      time_(Time::GetInstance()),
      diagnostics_reporter_(DiagnosticsReporter::GetInstance()) {
  HelpRegisterDerivedBool(flimflam::kAutoConnectProperty,
                          &Service::GetAutoConnect,
                          &Service::SetAutoConnectFull);

  // flimflam::kActivationStateProperty: Registered in CellularService
  // flimflam::kCellularApnProperty: Registered in CellularService
  // flimflam::kCellularLastGoodApnProperty: Registered in CellularService
  // flimflam::kNetworkTechnologyProperty: Registered in CellularService
  // flimflam::kOperatorNameProperty: DEPRECATED
  // flimflam::kOperatorCodeProperty: DEPRECATED
  // flimflam::kRoamingStateProperty: Registered in CellularService
  // flimflam::kServingOperatorProperty: Registered in CellularService
  // flimflam::kPaymentURLProperty: Registered in CellularService

  HelpRegisterDerivedString(flimflam::kCheckPortalProperty,
                            &Service::GetCheckPortal,
                            &Service::SetCheckPortal);
  store_.RegisterConstBool(flimflam::kConnectableProperty, &connectable_);
  HelpRegisterConstDerivedRpcIdentifier(flimflam::kDeviceProperty,
                                        &Service::GetDeviceRpcId);
  store_.RegisterConstStrings(kEapRemoteCertificationProperty,
                              &remote_certification_);
  HelpRegisterDerivedString(flimflam::kGuidProperty,
                            &Service::GetGuid,
                            &Service::SetGuid);

  // TODO(ers): in flimflam clearing Error has the side-effect of
  // setting the service state to IDLE. Is this important? I could
  // see an autotest depending on it.
  store_.RegisterConstString(flimflam::kErrorProperty, &error_);
  store_.RegisterConstString(shill::kErrorDetailsProperty, &error_details_);
  store_.RegisterConstBool(flimflam::kFavoriteProperty, &favorite_);
  HelpRegisterConstDerivedUint16(shill::kHTTPProxyPortProperty,
                                 &Service::GetHTTPProxyPort);
  HelpRegisterConstDerivedRpcIdentifier(shill::kIPConfigProperty,
                                        &Service::GetIPConfigRpcIdentifier);
  HelpRegisterDerivedBool(flimflam::kIsActiveProperty,
                          &Service::IsActive,
                          NULL);
  // flimflam::kModeProperty: Registered in WiFiService

  HelpRegisterDerivedString(flimflam::kNameProperty,
                            &Service::GetNameProperty,
                            &Service::SetNameProperty);
  // flimflam::kPassphraseProperty: Registered in WiFiService
  // flimflam::kPassphraseRequiredProperty: Registered in WiFiService
  HelpRegisterDerivedInt32(flimflam::kPriorityProperty,
                           &Service::GetPriority,
                           &Service::SetPriority);
  HelpRegisterDerivedString(flimflam::kProfileProperty,
                            &Service::GetProfileRpcId,
                            &Service::SetProfileRpcId);
  HelpRegisterDerivedString(flimflam::kProxyConfigProperty,
                            &Service::GetProxyConfig,
                            &Service::SetProxyConfig);
  store_.RegisterBool(flimflam::kSaveCredentialsProperty, &save_credentials_);
  HelpRegisterDerivedString(flimflam::kTypeProperty,
                            &Service::CalculateTechnology,
                            NULL);
  // flimflam::kSecurityProperty: Registered in WiFiService
  HelpRegisterDerivedString(flimflam::kStateProperty,
                            &Service::CalculateState,
                            NULL);
  store_.RegisterConstUint8(flimflam::kSignalStrengthProperty, &strength_);
  store_.RegisterString(flimflam::kUIDataProperty, &ui_data_);
  HelpRegisterConstDerivedStrings(shill::kDiagnosticsDisconnectsProperty,
                                  &Service::GetDisconnectsProperty);
  HelpRegisterConstDerivedStrings(shill::kDiagnosticsMisconnectsProperty,
                                  &Service::GetMisconnectsProperty);
  metrics_->RegisterService(this);

  static_ip_parameters_.PlumbPropertyStore(&store_);

  IgnoreParameterForConfigure(flimflam::kTypeProperty);
  IgnoreParameterForConfigure(flimflam::kProfileProperty);

  LOG(INFO) << Technology::NameFromIdentifier(technology) << " service "
            << unique_name_ << " constructed.";
}

Service::~Service() {
  LOG(INFO) << "Service " << unique_name_ << " destroyed.";
  metrics_->DeregisterService(this);
}

void Service::AutoConnect() {
  const char *reason = NULL;
  if (IsAutoConnectable(&reason)) {
    Error error;
    LOG(INFO) << "Auto-connecting to service " << unique_name_;
    ThrottleFutureAutoConnects();
    Connect(&error, __func__);
  } else {
    if (reason == kAutoConnConnected || reason == kAutoConnBusy) {
      SLOG(Service, 1)
          << "Suppressed autoconnect to service " << unique_name_ << " "
          << "(" << reason << ")";
    } else {
      LOG(INFO) << "Suppressed autoconnect to service " << unique_name_ << " "
                << "(" << reason << ")";
    }
  }
}

void Service::Connect(Error */*error*/, const char *reason) {
  LOG(INFO) << "Connect to service " << unique_name() <<": " << reason;
  explicitly_disconnected_ = false;
  // clear any failure state from a previous connect attempt
  SetState(kStateIdle);
}

void Service::Disconnect(Error */*error*/) {
  LOG(INFO) << "Disconnecting from service " << unique_name_;
  MemoryLog::GetInstance()->FlushToDisk();
}

void Service::DisconnectWithFailure(ConnectFailure failure, Error *error) {
  Disconnect(error);
  SetFailure(failure);
}

void Service::UserInitiatedDisconnect(Error *error) {
  Disconnect(error);
  explicitly_disconnected_ = true;
}

void Service::ActivateCellularModem(const string &/*carrier*/,
                                    Error *error,
                                    const ResultCallback &/*callback*/) {
  Error::PopulateAndLog(error, Error::kNotSupported,
                        "Service doesn't support cellular modem activation.");
}

void Service::CompleteCellularActivation(Error *error) {
  Error::PopulateAndLog(
      error, Error::kNotSupported,
      "Service doesn't support cellular activation completion.");
}

bool Service::IsActive(Error */*error*/) {
  return state() != kStateUnknown &&
    state() != kStateIdle &&
    state() != kStateFailure;
}

// static
bool Service::IsConnectedState(ConnectState state) {
  return (state == kStateConnected ||
          state == kStatePortal ||
          state == kStateOnline);
}

// static
bool Service::IsConnectingState(ConnectState state) {
  return (state == kStateAssociating ||
          state == kStateConfiguring);
}

bool Service::IsConnected() const {
  return IsConnectedState(state());
}

bool Service::IsConnecting() const {
  return IsConnectingState(state());
}

void Service::SetState(ConnectState state) {
  if (state == state_) {
    return;
  }

  LOG(INFO) << "Service " << unique_name_ << ": state "
            << ConnectStateToString(state_) << " -> "
            << ConnectStateToString(state);

  if (state == kStateFailure) {
    NoteDisconnectEvent();
  }

  previous_state_ = state_;
  state_ = state;
  if (state != kStateFailure) {
    failure_ = kFailureUnknown;
    SetErrorDetails(kErrorDetailsNone);
  }
  if (state == kStateConnected) {
    failed_time_ = 0;
    has_ever_connected_ = true;
    SaveToProfile();
    // When we succeed in connecting, forget that connects failed in the past.
    // Give services one chance at a fast autoconnect retry by resetting the
    // cooldown to 0 to indicate that the last connect was successful.
    auto_connect_cooldown_milliseconds_  = 0;
    reenable_auto_connect_task_.Cancel();
  }
  UpdateErrorProperty();
  manager_->UpdateService(this);
  metrics_->NotifyServiceStateChanged(this, state);
  adaptor_->EmitStringChanged(flimflam::kStateProperty, GetStateString());
}

void Service::ReEnableAutoConnectTask() {
  // Kill the thing blocking AutoConnect().
  reenable_auto_connect_task_.Cancel();
  // Post to the manager, giving it an opportunity to AutoConnect again.
  manager_->UpdateService(this);
}

void Service::ThrottleFutureAutoConnects() {
  if (auto_connect_cooldown_milliseconds_ > 0) {
    LOG(INFO) << "Throttling future autoconnects to service " << unique_name_
              << ". Next autoconnect in "
              << auto_connect_cooldown_milliseconds_ << " milliseconds.";
    reenable_auto_connect_task_.Reset(Bind(&Service::ReEnableAutoConnectTask,
                                           weak_ptr_factory_.GetWeakPtr()));
    dispatcher_->PostDelayedTask(reenable_auto_connect_task_.callback(),
                                 auto_connect_cooldown_milliseconds_);
  }
  auto_connect_cooldown_milliseconds_ =
      std::min(kMaxAutoConnectCooldownTimeMilliseconds,
               std::max(kMinAutoConnectCooldownTimeMilliseconds,
                        auto_connect_cooldown_milliseconds_ *
                        kAutoConnectCooldownBackoffFactor));
}

void Service::SetFailure(ConnectFailure failure) {
  failure_ = failure;
  failed_time_ = time(NULL);
  UpdateErrorProperty();
  SetState(kStateFailure);
}

void Service::SetFailureSilent(ConnectFailure failure) {
  NoteDisconnectEvent();
  // Note that order matters here, since SetState modifies |failure_| and
  // |failed_time_|.
  SetState(kStateIdle);
  failure_ = failure;
  UpdateErrorProperty();
  failed_time_ = time(NULL);
}

string Service::GetRpcIdentifier() const {
  return adaptor_->GetRpcIdentifier();
}

string Service::GetLoadableStorageIdentifier(
    const StoreInterface &storage) const {
  return IsLoadableFrom(storage) ? GetStorageIdentifier() : "";
}

bool Service::IsLoadableFrom(const StoreInterface &storage) const {
  return storage.ContainsGroup(GetStorageIdentifier());
}

bool Service::Load(StoreInterface *storage) {
  const string id = GetStorageIdentifier();
  if (!storage->ContainsGroup(id)) {
    LOG(WARNING) << "Service is not available in the persistent store: " << id;
    return false;
  }
  storage->GetBool(id, kStorageAutoConnect, &auto_connect_);
  storage->GetString(id, kStorageCheckPortal, &check_portal_);
  storage->GetBool(id, kStorageFavorite, &favorite_);
  storage->GetString(id, kStorageGUID, &guid_);
  storage->GetBool(id, kStorageHasEverConnected, &has_ever_connected_);
  storage->GetInt(id, kStoragePriority, &priority_);
  storage->GetString(id, kStorageProxyConfig, &proxy_config_);
  storage->GetBool(id, kStorageSaveCredentials, &save_credentials_);
  storage->GetString(id, kStorageUIData, &ui_data_);

  static_ip_parameters_.Load(storage, id);

  if (mutable_eap()) {
    mutable_eap()->Load(storage, id);
    OnEapCredentialsChanged();
  }

  explicitly_disconnected_ = false;
  favorite_ = true;

  return true;
}

bool Service::Unload() {
  auto_connect_ = IsAutoConnectByDefault();
  check_portal_ = kCheckPortalAuto;
  explicitly_disconnected_ = false;
  favorite_ = false;
  guid_ = "";
  has_ever_connected_ = false;
  priority_ = kPriorityNone;
  proxy_config_ = "";
  save_credentials_ = true;
  ui_data_ = "";
  if (mutable_eap()) {
    mutable_eap()->Reset();
  }
  ClearEAPCertification();

  Error error;  // Ignored.
  Disconnect(&error);
  return false;
}

void Service::Remove(Error */*error*/) {
  Unload();
}

bool Service::Save(StoreInterface *storage) {
  const string id = GetStorageIdentifier();

  storage->SetString(id, kStorageType, GetTechnologyString());

  storage->SetBool(id, kStorageAutoConnect, auto_connect_);
  if (check_portal_ == kCheckPortalAuto) {
    storage->DeleteKey(id, kStorageCheckPortal);
  } else {
    storage->SetString(id, kStorageCheckPortal, check_portal_);
  }
  storage->SetBool(id, kStorageFavorite, favorite_);
  SaveString(storage, id, kStorageGUID, guid_, false, true);
  storage->SetBool(id, kStorageHasEverConnected, has_ever_connected_);
  storage->SetString(id, kStorageName, friendly_name_);
  if (priority_ != kPriorityNone) {
    storage->SetInt(id, kStoragePriority, priority_);
  } else {
    storage->DeleteKey(id, kStoragePriority);
  }
  SaveString(storage, id, kStorageProxyConfig, proxy_config_, false, true);
  storage->SetBool(id, kStorageSaveCredentials, save_credentials_);
  SaveString(storage, id, kStorageUIData, ui_data_, false, true);

  static_ip_parameters_.Save(storage, id);
  if (eap()) {
    eap()->Save(storage, id, save_credentials_);
  }
  return true;
}

void Service::SaveToCurrentProfile() {
  // Some unittests do not specify a manager.
  if (manager()) {
    manager()->SaveServiceToProfile(this);
  }
}

void Service::Configure(const KeyValueStore &args, Error *error) {
  SLOG(Service, 5) << "Configuring bool properties:";
  for (const auto &bool_it : args.bool_properties()) {
    if (ContainsKey(parameters_ignored_for_configure_, bool_it.first)) {
      continue;
    }
    SLOG(Service, 5) << "   " << bool_it.first;
    Error set_error;
    store_.SetBoolProperty(bool_it.first, bool_it.second, &set_error);
    OnPropertyChanged(bool_it.first);
    if (error->IsSuccess() && set_error.IsFailure()) {
      error->CopyFrom(set_error);
    }
  }
  SLOG(Service, 5) << "Configuring int32 properties:";
  for (const auto &int_it : args.int_properties()) {
    if (ContainsKey(parameters_ignored_for_configure_, int_it.first)) {
      continue;
    }
    SLOG(Service, 5) << "   " << int_it.first;
    Error set_error;
    store_.SetInt32Property(int_it.first, int_it.second, &set_error);
    OnPropertyChanged(int_it.first);
    if (error->IsSuccess() && set_error.IsFailure()) {
      error->CopyFrom(set_error);
    }
  }
  SLOG(Service, 5) << "Configuring string properties:";
  for (const auto &string_it : args.string_properties()) {
    if (ContainsKey(parameters_ignored_for_configure_, string_it.first)) {
      continue;
    }
    SLOG(Service, 5) << "   " << string_it.first;
    Error set_error;
    store_.SetStringProperty(string_it.first, string_it.second, &set_error);
    OnPropertyChanged(string_it.first);
    if (error->IsSuccess() && set_error.IsFailure()) {
      error->CopyFrom(set_error);
    }
  }
  SLOG(Service, 5) << "Configuring string array properties:";
  for (const auto &strings_it : args.strings_properties()) {
    if (ContainsKey(parameters_ignored_for_configure_, strings_it.first)) {
      continue;
    }
    SLOG(Service, 5) << "   " << strings_it.first;
    Error set_error;
    store_.SetStringsProperty(strings_it.first, strings_it.second, &set_error);
    OnPropertyChanged(strings_it.first);
    if (error->IsSuccess() && set_error.IsFailure()) {
      error->CopyFrom(set_error);
    }
  }
}

bool Service::DoPropertiesMatch(const KeyValueStore &args) const {
  SLOG(Service, 5) << "Checking bool properties:";
  for (const auto &bool_it: args.bool_properties()) {
    SLOG(Service, 5) << "   " << bool_it.first;
    Error get_error;
    bool value;
    if (!store_.GetBoolProperty(bool_it.first, &value, &get_error) ||
        value != bool_it.second) {
      return false;
    }
  }
  SLOG(Service, 5) << "Checking int32 properties:";
  for (const auto &int_it : args.int_properties()) {
    SLOG(Service, 5) << "   " << int_it.first;
    Error get_error;
    int32 value;
    if (!store_.GetInt32Property(int_it.first, &value, &get_error) ||
        value != int_it.second) {
      return false;
    }
  }
  SLOG(Service, 5) << "Checking string properties:";
  for (const auto &string_it : args.string_properties()) {
    SLOG(Service, 5) << "   " << string_it.first;
    Error get_error;
    string value;
    if (!store_.GetStringProperty(string_it.first, &value, &get_error) ||
        value != string_it.second) {
      return false;
    }
  }
  SLOG(Service, 5) << "Checking string array properties:";
  for (const auto &strings_it : args.strings_properties()) {
    SLOG(Service, 5) << "   " << strings_it.first;
    Error get_error;
    vector<string> value;
    if (!store_.GetStringsProperty(strings_it.first, &value, &get_error) ||
        value != strings_it.second) {
      return false;
    }
  }
  return true;
}

bool Service::IsRemembered() const {
  return profile_ && !manager_->IsServiceEphemeral(this);
}

bool Service::IsDependentOn(const ServiceRefPtr &b) const {
  if (!connection_ || !b || !b->connection()) {
    return false;
  }
  return connection_->GetLowerConnection() == b->connection();
}

void Service::MakeFavorite() {
  if (favorite_) {
    // We do not want to clobber the value of auto_connect_ (it may
    // be user-set). So return early.
    return;
  }

  MarkAsFavorite();
  SetAutoConnect(true);
}

void Service::SetConnection(const ConnectionRefPtr &connection) {
  if (connection.get()) {
    // TODO(pstew): Make this function testable by using a factory here.
    // http://crosbug.com/34528
    http_proxy_.reset(new HTTPProxy(connection));
    http_proxy_->Start(dispatcher_, sockets_.get());
  } else {
    http_proxy_.reset();
    static_ip_parameters_.ClearSavedParameters();
  }
  connection_ = connection;
  Error error;
  string ipconfig = GetIPConfigRpcIdentifier(&error);
  if (error.IsSuccess()) {
    adaptor_->EmitRpcIdentifierChanged(shill::kIPConfigProperty, ipconfig);
  }
}

bool Service::Is8021xConnectable() const {
  return eap() && eap()->IsConnectable();
}

bool Service::AddEAPCertification(const string &name, size_t depth) {
  if (depth >= kEAPMaxCertificationElements) {
    LOG(WARNING) << "Ignoring certification " << name
                 << " because depth " << depth
                 << " exceeds our maximum of "
                 << kEAPMaxCertificationElements;
    return false;
  }

  if (depth >= remote_certification_.size()) {
    remote_certification_.resize(depth + 1);
  } else if (name == remote_certification_[depth]) {
    return true;
  }

  remote_certification_[depth] = name;
  LOG(INFO) << "Received certification for "
            << name
            << " at depth "
            << depth;
  return true;
}

void Service::ClearEAPCertification() {
  remote_certification_.clear();
}

void Service::SetAutoConnect(bool connect) {
  if (auto_connect() == connect) {
    return;
  }
  auto_connect_ = connect;
  adaptor_->EmitBoolChanged(flimflam::kAutoConnectProperty, auto_connect());
}

void Service::SetEapCredentials(EapCredentials *eap) {
  // This operation must be done at most once for the lifetime of the service.
  CHECK(eap && !eap_);

  eap_.reset(eap);
  eap_->InitPropertyStore(mutable_store());
}

// static
const char *Service::ConnectFailureToString(const ConnectFailure &state) {
  switch (state) {
    case kFailureUnknown:
      return "Unknown";
    case kFailureAAA:
      return flimflam::kErrorAaaFailed;
    case kFailureActivation:
      return flimflam::kErrorActivationFailed;
    case kFailureBadPassphrase:
      return flimflam::kErrorBadPassphrase;
    case kFailureBadWEPKey:
      return flimflam::kErrorBadWEPKey;
    case kFailureConnect:
      return flimflam::kErrorConnectFailed;
    case kFailureDNSLookup:
      return flimflam::kErrorDNSLookupFailed;
    case kFailureDHCP:
      return flimflam::kErrorDhcpFailed;
    case kFailureEAPAuthentication:
      return shill::kErrorEapAuthenticationFailed;
    case kFailureEAPLocalTLS:
      return shill::kErrorEapLocalTlsFailed;
    case kFailureEAPRemoteTLS:
      return shill::kErrorEapRemoteTlsFailed;
    case kFailureHTTPGet:
      return flimflam::kErrorHTTPGetFailed;
    case kFailureInternal:
      return flimflam::kErrorInternal;
    case kFailureIPSecCertAuth:
      return flimflam::kErrorIpsecCertAuthFailed;
    case kFailureIPSecPSKAuth:
      return flimflam::kErrorIpsecPskAuthFailed;
    case kFailureNeedEVDO:
      return flimflam::kErrorNeedEvdo;
    case kFailureNeedHomeNetwork:
      return flimflam::kErrorNeedHomeNetwork;
    case kFailureOTASP:
      return flimflam::kErrorOtaspFailed;
    case kFailureOutOfRange:
      return flimflam::kErrorOutOfRange;
    case kFailurePinMissing:
      return flimflam::kErrorPinMissing;
    case kFailurePPPAuth:
      return flimflam::kErrorPppAuthFailed;
    case kFailureMax:
      NOTREACHED();
  }
  return "Invalid";
}

// static
const char *Service::ConnectStateToString(const ConnectState &state) {
  switch (state) {
    case kStateUnknown:
      return "Unknown";
    case kStateIdle:
      return "Idle";
    case kStateAssociating:
      return "Associating";
    case kStateConfiguring:
      return "Configuring";
    case kStateConnected:
      return "Connected";
    case kStatePortal:
      return "Portal";
    case kStateFailure:
      return "Failure";
    case kStateOnline:
      return "Online";
  }
  return "Invalid";
}

string Service::GetTechnologyString() const {
  return Technology::NameFromIdentifier(technology());
}

string Service::CalculateTechnology(Error */*error*/) {
  return GetTechnologyString();
}

// static
void Service::ExpireEventsBefore(
  int seconds_ago, const Timestamp &now, std::deque<Timestamp> *events) {
  struct timeval period = (const struct timeval){ seconds_ago };
  while (!events->empty()) {
    if (events->size() < static_cast<size_t>(kMaxDisconnectEventHistory)) {
      struct timeval elapsed = (const struct timeval){ 0 };
      timersub(&now.monotonic, &events->front().monotonic, &elapsed);
      if (timercmp(&elapsed, &period, <)) {
        break;
      }
    }
    events->pop_front();
  }
}

void Service::NoteDisconnectEvent() {
  SLOG(Service, 2) << __func__;
  // Ignore the event if it's user-initiated explicit disconnect.
  if (explicitly_disconnected_) {
    SLOG(Service, 2) << "Explicit disconnect ignored.";
    return;
  }
  // Ignore the event if manager is not running (e.g., service disconnects on
  // shutdown).
  if (!manager_->running()) {
    SLOG(Service, 2) << "Disconnect while manager stopped ignored.";
    return;
  }
  // Ignore the event if the power state is not on (e.g., when suspending).
  PowerManager *power_manager = manager_->power_manager();
  if (!power_manager ||
      (power_manager->power_state() != PowerManager::kOn &&
       power_manager->power_state() != PowerManager::kUnknown)) {
    SLOG(Service, 2) << "Disconnect in transitional power state ignored.";
    return;
  }
  int period = 0;
  size_t threshold = 0;
  deque<Timestamp> *events = NULL;
  // Sometimes services transition to Idle before going into a failed state so
  // take into account the last non-idle state.
  ConnectState state = state_ == kStateIdle ? previous_state_ : state_;
  if (IsConnectedState(state)) {
    LOG(INFO) << "Noting an unexpected connection drop.";
    period = kDisconnectsMonitorSeconds;
    threshold = kReportDisconnectsThreshold;
    events = &disconnects_;
  } else if (IsConnectingState(state)) {
    LOG(INFO) << "Noting an unexpected failure to connect.";
    period = kMisconnectsMonitorSeconds;
    threshold = kReportMisconnectsThreshold;
    events = &misconnects_;
  } else {
    SLOG(Service, 2)
        << "Not connected or connecting, state transition ignored.";
    return;
  }
  Timestamp now = time_->GetNow();
  // Discard old events first.
  ExpireEventsBefore(period, now, events);
  events->push_back(now);
  if (events->size() >= threshold) {
    diagnostics_reporter_->OnConnectivityEvent();
  }
}

bool Service::HasRecentConnectionIssues() {
  Timestamp now = time_->GetNow();
  ExpireEventsBefore(kDisconnectsMonitorSeconds, now, &disconnects_);
  ExpireEventsBefore(kMisconnectsMonitorSeconds, now, &misconnects_);
  return !disconnects_.empty() || !misconnects_.empty();
}

// static
bool Service::DecideBetween(int a, int b, bool *decision) {
  if (a == b)
    return false;
  *decision = (a > b);
  return true;
}

uint16 Service::SecurityLevel() {
  return (crypto_algorithm_ << 2) | (key_rotation_ << 1) | endpoint_auth_;
}

// static
bool Service::Compare(ServiceRefPtr a,
                      ServiceRefPtr b,
                      bool compare_connectivity_state,
                      const vector<Technology::Identifier> &tech_order,
                      const char **reason) {
  bool ret;

  if (compare_connectivity_state && a->state() != b->state()) {
    if (DecideBetween(a->IsConnected(), b->IsConnected(), &ret)) {
      *reason = kServiceSortIsConnected;
      return ret;
    }

    if (DecideBetween(!a->IsPortalled(), !b->IsPortalled(), &ret)) {
      *reason = kServiceSortIsPortalled;
      return ret;
    }

    if (DecideBetween(a->IsConnecting(), b->IsConnecting(), &ret)) {
      *reason = kServiceSortIsConnecting;
      return ret;
    }

    if (DecideBetween(!a->IsFailed(), !b->IsFailed(), &ret)) {
      *reason = kServiceSortIsFailed;
      return ret;
    }
  }

  if (DecideBetween(a->connectable(), b->connectable(), &ret)) {
    *reason = kServiceSortConnectable;
    return ret;
  }

  if (DecideBetween(a->IsDependentOn(b), b->IsDependentOn(a), &ret)) {
    *reason = kServiceSortDependency;
    return ret;
  }

  // Ignore the auto-connect property if both services are connected
  // already. This allows connected non-autoconnectable VPN services to be
  // sorted higher than other connected services based on technology order.
  if (!a->IsConnected() &&
      DecideBetween(a->auto_connect(), b->auto_connect(), &ret)) {
    *reason = kServiceSortAutoConnect;
    return ret;
  }

  if (DecideBetween(a->favorite(), b->favorite(), &ret)) {
    *reason = kServiceSortFavorite;
    return ret;
  }

  if (DecideBetween(a->priority(), b->priority(), &ret)) {
    *reason = kServiceSortPriority;
    return ret;
  }

  // TODO(pstew): Below this point we are making value judgements on
  // services that are not related to anything intrinsic or
  // user-specified.  These heuristics should be richer (contain
  // historical information, for example) and be subject to user
  // customization.
  for (vector<Technology::Identifier>::const_iterator it = tech_order.begin();
       it != tech_order.end();
       ++it) {
    if (DecideBetween(a->technology() == *it, b->technology() == *it, &ret)) {
      *reason = kServiceSortTechnology;
      return ret;
    }
  }

  if (DecideBetween(a->SecurityLevel(), b->SecurityLevel(), &ret) ||
      DecideBetween(a->strength(), b->strength(), &ret)) {
    *reason = kServiceSortSecurityEtc;
    return ret;
  }

  *reason = kServiceSortUniqueName;
  return a->unique_name() < b->unique_name();
}

const ProfileRefPtr &Service::profile() const { return profile_; }

void Service::set_profile(const ProfileRefPtr &p) { profile_ = p; }

void Service::SetProfile(const ProfileRefPtr &p) {
  SLOG(Service, 2) << "SetProfile from "
                   << (profile_ ? profile_->GetFriendlyName() : "")
                   << " to " << (p ? p->GetFriendlyName() : "");
  if (profile_ == p) {
    return;
  }
  profile_ = p;
  Error error;
  string profile_rpc_id = GetProfileRpcId(&error);
  if (!error.IsSuccess()) {
    return;
  }
  adaptor_->EmitStringChanged(flimflam::kProfileProperty, profile_rpc_id);
}

void Service::OnPropertyChanged(const string &property) {
  if (Is8021x() && EapCredentials::IsEapAuthenticationProperty(property)) {
    OnEapCredentialsChanged();
  }
  SaveToProfile();
  if ((property == flimflam::kCheckPortalProperty ||
       property == flimflam::kProxyConfigProperty) &&
      (state_ == kStateConnected ||
       state_ == kStatePortal ||
       state_ == kStateOnline)) {
    manager_->RecheckPortalOnService(this);
  }
}

void Service::OnAfterResume() {
  // Forget old autoconnect failures across suspend/resume.
  auto_connect_cooldown_milliseconds_  = 0;
  reenable_auto_connect_task_.Cancel();
  // Forget if the user disconnected us, we might be able to connect now.
  explicitly_disconnected_ = false;
}

string Service::GetIPConfigRpcIdentifier(Error *error) {
  if (!connection_) {
    error->Populate(Error::kNotFound);
    return DBusAdaptor::kNullPath;
  }

  string id = connection_->ipconfig_rpc_identifier();

  if (id.empty()) {
    // Do not return an empty IPConfig.
    error->Populate(Error::kNotFound);
    return DBusAdaptor::kNullPath;
  }

  return id;
}

void Service::SetConnectable(bool connectable) {
  if (connectable_ == connectable)
    return;
  connectable_ = connectable;
  adaptor_->EmitBoolChanged(flimflam::kConnectableProperty, connectable_);
}

void Service::SetConnectableFull(bool connectable) {
  if (connectable_ == connectable) {
    return;
  }
  SetConnectable(connectable);
  if (manager_->HasService(this)) {
    manager_->UpdateService(this);
  }
}

string Service::GetStateString() const {
  switch (state_) {
    case kStateIdle:
      return flimflam::kStateIdle;
    case kStateAssociating:
      return flimflam::kStateAssociation;
    case kStateConfiguring:
      return flimflam::kStateConfiguration;
    case kStateConnected:
      return flimflam::kStateReady;
    case kStateFailure:
      return flimflam::kStateFailure;
    case kStatePortal:
      return flimflam::kStatePortal;
    case kStateOnline:
      return flimflam::kStateOnline;
    case kStateUnknown:
    default:
      return "";
  }
}

string Service::CalculateState(Error */*error*/) {
  return GetStateString();
}

bool Service::IsAutoConnectable(const char **reason) const {
  if (!connectable()) {
    *reason = kAutoConnNotConnectable;
    return false;
  }

  if (IsConnected()) {
    *reason = kAutoConnConnected;
    return false;
  }

  if (IsConnecting()) {
    *reason = kAutoConnConnecting;
    return false;
  }

  if (explicitly_disconnected_) {
    *reason = kAutoConnExplicitDisconnect;
    return false;
  }

  if (!reenable_auto_connect_task_.IsCancelled()) {
    *reason = kAutoConnThrottled;
    return false;
  }

  if (!Technology::IsPrimaryConnectivityTechnology(technology_) &&
      !manager_->IsOnline()) {
    *reason = kAutoConnOffline;
    return false;
  }

  return true;
}

bool Service::IsPortalDetectionDisabled() const {
  return check_portal_ == kCheckPortalFalse;
}

bool Service::IsPortalDetectionAuto() const {
  return check_portal_ == kCheckPortalAuto;
}

void Service::HelpRegisterDerivedBool(
    const string &name,
    bool(Service::*get)(Error *),
    bool(Service::*set)(const bool&, Error *)) {
  store_.RegisterDerivedBool(
      name,
      BoolAccessor(new CustomAccessor<Service, bool>(this, get, set)));
}

void Service::HelpRegisterDerivedInt32(
    const string &name,
    int32(Service::*get)(Error *),
    bool(Service::*set)(const int32&, Error *)) {
  store_.RegisterDerivedInt32(
      name,
      Int32Accessor(new CustomAccessor<Service, int32>(this, get, set)));
}

void Service::HelpRegisterDerivedString(
    const string &name,
    string(Service::*get)(Error *),
    bool(Service::*set)(const string&, Error *)) {
  store_.RegisterDerivedString(
      name,
      StringAccessor(new CustomAccessor<Service, string>(this, get, set)));
}

void Service::HelpRegisterConstDerivedRpcIdentifier(
    const string &name,
    RpcIdentifier(Service::*get)(Error *)) {
  store_.RegisterDerivedRpcIdentifier(
      name,
      RpcIdentifierAccessor(new CustomAccessor<Service, RpcIdentifier>(
          this, get, NULL)));
}

void Service::HelpRegisterConstDerivedUint16(
    const string &name,
    uint16(Service::*get)(Error *)) {
  store_.RegisterDerivedUint16(
      name,
      Uint16Accessor(new CustomAccessor<Service, uint16>(this, get, NULL)));
}

void Service::HelpRegisterConstDerivedStrings(
    const string &name, Strings(Service::*get)(Error *error)) {
  store_.RegisterDerivedStrings(
      name,
      StringsAccessor(new CustomAccessor<Service, Strings>(this, get, NULL)));
}

// static
void Service::SaveString(StoreInterface *storage,
                         const string &id,
                         const string &key,
                         const string &value,
                         bool crypted,
                         bool save) {
  if (value.empty() || !save) {
    storage->DeleteKey(id, key);
    return;
  }
  if (crypted) {
    storage->SetCryptedString(id, key, value);
    return;
  }
  storage->SetString(id, key, value);
}

map<string, string> Service::GetLoadableProfileEntries() {
  return manager_->GetLoadableProfileEntriesForService(this);
}

void Service::IgnoreParameterForConfigure(const string &parameter) {
  parameters_ignored_for_configure_.insert(parameter);
}

const string &Service::GetEAPKeyManagement() const {
  CHECK(eap());
  return eap()->key_management();
}

void Service::SetEAPKeyManagement(const string &key_management) {
  CHECK(mutable_eap());
  mutable_eap()->SetKeyManagement(key_management, NULL);
}

bool Service::GetAutoConnect(Error */*error*/) {
  return auto_connect();
}

bool Service::SetAutoConnectFull(const bool &connect, Error */*error*/) {
  LOG(INFO) << "Service " << unique_name() << ": AutoConnect="
            << auto_connect() << "->" << connect;
  if (auto_connect() == connect) {
    return false;
  }
  SetAutoConnect(connect);
  manager_->UpdateService(this);
  return true;
}

string Service::GetCheckPortal(Error *error) {
  return check_portal_;
}

bool Service::SetCheckPortal(const string &check_portal, Error *error) {
  if (check_portal != kCheckPortalFalse &&
      check_portal != kCheckPortalTrue &&
      check_portal != kCheckPortalAuto) {
    Error::PopulateAndLog(error, Error::kInvalidArguments,
                          base::StringPrintf(
                              "Invalid Service CheckPortal property value: %s",
                              check_portal.c_str()));
    return false;
  }
  if (check_portal == check_portal_) {
    return false;
  }
  check_portal_ = check_portal;
  return true;
}

string Service::GetGuid(Error *error) {
  return guid_;
}

bool Service::SetGuid(const string &guid, Error */*error*/) {
  if (guid_ == guid) {
    return false;
  }
  guid_ = guid;
  adaptor_->EmitStringChanged(flimflam::kGuidProperty, guid_);
  return true;
}

void Service::MarkAsFavorite() {
  favorite_ = true;
  adaptor_->EmitBoolChanged(flimflam::kFavoriteProperty, favorite_);
}

void Service::SetSecurity(CryptoAlgorithm crypto_algorithm, bool key_rotation,
                          bool endpoint_auth) {
  crypto_algorithm_ = crypto_algorithm;
  key_rotation_ = key_rotation;
  endpoint_auth_ = endpoint_auth;
}

string Service::GetNameProperty(Error */*error*/) {
  return friendly_name_;
}

bool Service::SetNameProperty(const string &name, Error *error) {
  if (name != friendly_name_) {
    Error::PopulateAndLog(error, Error::kInvalidArguments,
                          base::StringPrintf(
                              "Service %s Name property cannot be modified.",
                              unique_name_.c_str()));
    return false;
  }
  return false;
}

int32 Service::GetPriority(Error *error) {
  return priority_;
}

bool Service::SetPriority(const int32 &priority, Error *error) {
  if (priority_ == priority) {
    return false;
  }
  priority_ = priority;
  adaptor_->EmitIntChanged(flimflam::kPriorityProperty, priority_);
  return true;
}

string Service::GetProfileRpcId(Error *error) {
  if (!profile_) {
    // This happens in some unit tests where profile_ is not set.
    error->Populate(Error::kNotFound);
    return "";
  }
  return profile_->GetRpcIdentifier();
}

bool Service::SetProfileRpcId(const string &profile, Error *error) {
  if (profile_ && profile_->GetRpcIdentifier() == profile) {
    return false;
  }
  ProfileConstRefPtr old_profile = profile_;
  // No need to Emit afterwards, since SetProfileForService will call
  // into SetProfile (if the profile actually changes).
  manager_->SetProfileForService(this, profile, error);
  // Can't just use error.IsSuccess(), because that also requires saving
  // the profile to succeed. (See Profile::AdoptService)
  return (profile_ != old_profile);
}

uint16 Service::GetHTTPProxyPort(Error */*error*/) {
  if (http_proxy_.get()) {
    return static_cast<uint16>(http_proxy_->proxy_port());
  }
  return 0;
}

string Service::GetProxyConfig(Error *error) {
  return proxy_config_;
}

bool Service::SetProxyConfig(const string &proxy_config, Error *error) {
  if (proxy_config_ == proxy_config)
    return false;
  proxy_config_ = proxy_config;
  adaptor_->EmitStringChanged(flimflam::kProxyConfigProperty, proxy_config_);
  return true;
}

// static
Strings Service::ExtractWallClockToStrings(
    const deque<Timestamp> &timestamps) {
  Strings strings;
  for (deque<Timestamp>::const_iterator it = timestamps.begin();
       it != timestamps.end(); ++it) {
    strings.push_back(it->wall_clock);
  }
  return strings;
}

Strings Service::GetDisconnectsProperty(Error */*error*/) {
  return ExtractWallClockToStrings(disconnects_);
}

Strings Service::GetMisconnectsProperty(Error */*error*/) {
  return ExtractWallClockToStrings(misconnects_);
}

void Service::SaveToProfile() {
  if (profile_.get() && profile_->GetConstStorage()) {
    profile_->UpdateService(this);
  }
}

void Service::SetFriendlyName(const string &friendly_name) {
  if (friendly_name == friendly_name_)
    return;
  friendly_name_ = friendly_name;
  adaptor()->EmitStringChanged(flimflam::kNameProperty, friendly_name_);
}

void Service::SetStrength(uint8 strength) {
  if (strength == strength_) {
    return;
  }
  strength_ = strength;
  adaptor_->EmitUint8Changed(flimflam::kSignalStrengthProperty, strength);
}

void Service::SetErrorDetails(const string &details) {
  if (error_details_ == details) {
    return;
  }
  error_details_ = details;
  adaptor_->EmitStringChanged(shill::kErrorDetailsProperty, error_details_);
}

void Service::UpdateErrorProperty() {
  const string error(ConnectFailureToString(failure_));
  if (error == error_) {
    return;
  }
  error_ = error;
  adaptor_->EmitStringChanged(flimflam::kErrorProperty, error);
}

}  // namespace shill
