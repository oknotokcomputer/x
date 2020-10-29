// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/cellular/cellular_capability_3gpp.h"

#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <base/bind.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <chromeos/dbus/service_constants.h>
#include <ModemManager/ModemManager.h>

#include "shill/cellular/cellular.h"
#include "shill/cellular/cellular_bearer.h"
#include "shill/cellular/cellular_service.h"
#include "shill/cellular/cellular_service_provider.h"
#include "shill/cellular/mock_cellular.h"
#include "shill/cellular/mock_cellular_service.h"
#include "shill/cellular/mock_mm1_modem_location_proxy.h"
#include "shill/cellular/mock_mm1_modem_modem3gpp_proxy.h"
#include "shill/cellular/mock_mm1_modem_proxy.h"
#include "shill/cellular/mock_mm1_modem_simple_proxy.h"
#include "shill/cellular/mock_mm1_sim_proxy.h"
#include "shill/cellular/mock_mobile_operator_info.h"
#include "shill/cellular/mock_modem_info.h"
#include "shill/cellular/mock_pending_activation_store.h"
#include "shill/error.h"
#include "shill/mock_adaptors.h"
#include "shill/mock_control.h"
#include "shill/mock_dbus_properties_proxy.h"
#include "shill/mock_event_dispatcher.h"
#include "shill/mock_manager.h"
#include "shill/mock_metrics.h"
#include "shill/mock_profile.h"
#include "shill/test_event_dispatcher.h"
#include "shill/testing.h"

using base::Bind;
using base::Unretained;
using std::string;
using std::vector;
using testing::_;
using testing::AnyNumber;
using testing::InSequence;
using testing::Invoke;
using testing::Mock;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::SaveArg;

namespace shill {

MATCHER_P(HasApn, expected_apn, "") {
  return arg.template Contains<string>(CellularCapability3gpp::kConnectApn) &&
         expected_apn ==
             arg.template Get<string>(CellularCapability3gpp::kConnectApn);
}

MATCHER(HasNoUser, "") {
  return !arg.template Contains<string>(CellularCapability3gpp::kConnectUser);
}

MATCHER_P(HasUser, expected_user, "") {
  return arg.template Contains<string>(CellularCapability3gpp::kConnectUser) &&
         expected_user ==
             arg.template Get<string>(CellularCapability3gpp::kConnectUser);
}

MATCHER(HasNoPassword, "") {
  return !arg.template Contains<string>(
      CellularCapability3gpp::kConnectPassword);
}

MATCHER_P(HasPassword, expected_password, "") {
  return arg.template Contains<string>(
             CellularCapability3gpp::kConnectPassword) &&
         expected_password ==
             arg.template Get<string>(CellularCapability3gpp::kConnectPassword);
}

MATCHER(HasNoAllowedAuth, "") {
  return !arg.template Contains<string>(
      CellularCapability3gpp::kConnectAllowedAuth);
}

MATCHER_P(HasAllowedAuth, expected_authentication, "") {
  return arg.template Contains<uint32_t>(
             CellularCapability3gpp::kConnectAllowedAuth) &&
         expected_authentication ==
             arg.template Get<uint32_t>(
                 CellularCapability3gpp::kConnectAllowedAuth);
}

MATCHER(HasNoIpType, "") {
  return !arg.template Contains<uint32_t>(
      CellularCapability3gpp::kConnectIpType);
}

MATCHER_P(HasIpType, expected_ip_type, "") {
  return arg.template Contains<uint32_t>(
             CellularCapability3gpp::kConnectIpType) &&
         expected_ip_type ==
             arg.template Get<uint32_t>(CellularCapability3gpp::kConnectIpType);
}

class CellularCapability3gppTest : public testing::TestWithParam<string> {
 public:
  explicit CellularCapability3gppTest(EventDispatcher* dispatcher)
      : dispatcher_(dispatcher),
        control_interface_(this),
        manager_(&control_interface_, dispatcher, &metrics_),
        modem_info_(&control_interface_, &manager_),
        modem_3gpp_proxy_(new NiceMock<mm1::MockModemModem3gppProxy>()),
        modem_location_proxy_(new mm1::MockModemLocationProxy()),
        modem_proxy_(new mm1::MockModemProxy()),
        modem_simple_proxy_(new mm1::MockModemSimpleProxy()),
        sim_proxy_(new mm1::MockSimProxy()),
        properties_proxy_(new MockDBusPropertiesProxy()),
        capability_(nullptr),
        device_adaptor_(nullptr),
        cellular_(new Cellular(&modem_info_,
                               "",
                               "00:01:02:03:04:05",
                               0,
                               Cellular::kType3gpp,
                               "",
                               RpcIdentifier(""))),
        service_(new MockCellularService(&manager_, cellular_)),
        mock_home_provider_info_(nullptr),
        mock_serving_operator_info_(nullptr) {
    metrics_.RegisterDevice(cellular_->interface_index(),
                            Technology::kCellular);
  }

  ~CellularCapability3gppTest() override {
    cellular_->service_ = nullptr;
    CHECK(cellular_->HasOneRef());
    cellular_ = nullptr;
    device_adaptor_ = nullptr;
  }

  void SetUp() override {
    EXPECT_CALL(*modem_proxy_, set_state_changed_callback(_))
        .Times(AnyNumber());

    cellular_->CreateCapability(&modem_info_);
    capability_ =
        static_cast<CellularCapability3gpp*>(cellular_->capability_.get());
    device_adaptor_ = static_cast<DeviceMockAdaptor*>(cellular_->adaptor());
    cellular_->service_ = service_;

    EXPECT_CALL(*service_, activation_state())
        .WillRepeatedly(ReturnRef(kActivationStateUnknown));

    // kStateUnknown leads to minimal extra work in maintaining
    // activation state.
    ON_CALL(*modem_info_.mock_pending_activation_store(),
            GetActivationState(PendingActivationStore::kIdentifierICCID, _))
        .WillByDefault(Return(PendingActivationStore::kStateUnknown));
    EXPECT_CALL(manager_, cellular_service_provider())
        .WillRepeatedly(Return(&cellular_service_provider_));

    SetMockMobileOperatorInfoObjects();
  }

  void TearDown() override {
    cellular_->DestroyCapability();
    capability_ = nullptr;
  }

  void CreateService() {
    // The following constants are never directly accessed by the tests.
    const char kFriendlyServiceName[] = "default_test_service_name";
    const char kOperatorCode[] = "10010";
    const char kOperatorName[] = "default_test_operator_name";
    const char kOperatorCountry[] = "us";

    // Simulate all the side-effects of Cellular::CreateService
    auto service =
        new CellularService(&manager_, cellular_->imsi(), cellular_->iccid(),
                            cellular_->GetSimCardId());
    service->SetFriendlyName(kFriendlyServiceName);

    Stringmap serving_operator;
    serving_operator[kOperatorCodeKey] = kOperatorCode;
    serving_operator[kOperatorNameKey] = kOperatorName;
    serving_operator[kOperatorCountryKey] = kOperatorCountry;
    service->SetServingOperator(serving_operator);
    cellular_->set_home_provider(serving_operator);
    cellular_->service_ = service;
  }

  void ExpectModemAndModem3gppProperties() {
    // Set up mock modem properties.
    KeyValueStore modem_properties;
    string operator_name = "TestOperator";
    string operator_code = "001400";

    modem_properties.Set<uint32_t>(MM_MODEM_PROPERTY_ACCESSTECHNOLOGIES,
                                   kAccessTechnologies);
    std::tuple<uint32_t, bool> signal_signal{90, true};
    modem_properties.SetVariant(MM_MODEM_PROPERTY_SIGNALQUALITY,
                                brillo::Any(signal_signal));

    // Set up mock modem 3gpp properties.
    KeyValueStore modem3gpp_properties;
    modem3gpp_properties.Set<uint32_t>(
        MM_MODEM_MODEM3GPP_PROPERTY_ENABLEDFACILITYLOCKS, 0);
    modem3gpp_properties.Set<string>(MM_MODEM_MODEM3GPP_PROPERTY_IMEI, kImei);

    EXPECT_CALL(*properties_proxy_, GetAll(MM_DBUS_INTERFACE_MODEM))
        .WillOnce(Return(modem_properties));
    EXPECT_CALL(*properties_proxy_, GetAll(MM_DBUS_INTERFACE_MODEM_MODEM3GPP))
        .WillOnce(Return(modem3gpp_properties));
  }

  void InvokeEnable(bool enable,
                    Error* error,
                    const ResultCallback& callback,
                    int timeout) {
    callback.Run(Error());
  }
  void InvokeEnableFail(bool enable,
                        Error* error,
                        const ResultCallback& callback,
                        int timeout) {
    callback.Run(Error(Error::kOperationFailed));
  }
  void InvokeEnableInWrongState(bool enable,
                                Error* error,
                                const ResultCallback& callback,
                                int timeout) {
    callback.Run(Error(Error::kWrongState));
  }
  void InvokeSetPowerState(const uint32_t& power_state,
                           Error* error,
                           const ResultCallback& callback,
                           int timeout) {
    callback.Run(Error());
  }
  void Set3gppProxy() {
    capability_->modem_3gpp_proxy_ = std::move(modem_3gpp_proxy_);
  }

  void SetSimpleProxy() {
    capability_->modem_simple_proxy_ = std::move(modem_simple_proxy_);
  }

  void SetMockMobileOperatorInfoObjects() {
    CHECK(!mock_home_provider_info_);
    CHECK(!mock_serving_operator_info_);
    mock_home_provider_info_ =
        new NiceMock<MockMobileOperatorInfo>(dispatcher_, "HomeProvider");
    mock_serving_operator_info_ =
        new NiceMock<MockMobileOperatorInfo>(dispatcher_, "ServingOperator");
    mock_home_provider_info_->Init();
    mock_serving_operator_info_->Init();
    cellular_->set_home_provider_info(mock_home_provider_info_);
    cellular_->set_serving_operator_info(mock_serving_operator_info_);
  }

  void ReleaseCapabilityProxies() {
    capability_->ReleaseProxies();
    EXPECT_EQ(nullptr, capability_->modem_3gpp_proxy_);
    EXPECT_EQ(nullptr, capability_->modem_proxy_);
    EXPECT_EQ(nullptr, capability_->modem_location_proxy_);
    EXPECT_EQ(nullptr, capability_->modem_simple_proxy_);
  }

  void SetRegistrationDroppedUpdateTimeout(int64_t timeout_milliseconds) {
    capability_->registration_dropped_update_timeout_milliseconds_ =
        timeout_milliseconds;
  }

  MOCK_METHOD(void, TestCallback, (const Error&));

  MOCK_METHOD(void, DummyCallback, ());

  void SetMockRegistrationDroppedUpdateCallback() {
    capability_->registration_dropped_update_callback_.Reset(
        Bind(&CellularCapability3gppTest::DummyCallback, Unretained(this)));
  }

 protected:
  static const char kActiveBearerPathPrefix[];
  static const char kImei[];
  static const char kInactiveBearerPathPrefix[];
  static const RpcIdentifier kSimPath;
  static const uint32_t kAccessTechnologies;
  static const char kTestMobileProviderDBPath[];

  class TestControl : public MockControl {
   public:
    explicit TestControl(CellularCapability3gppTest* test) : test_(test) {
      active_bearer_properties_.Set<bool>(MM_BEARER_PROPERTY_CONNECTED, true);
      active_bearer_properties_.Set<string>(MM_BEARER_PROPERTY_INTERFACE,
                                            "/dev/fake");

      KeyValueStore ip4config;
      ip4config.Set<uint32_t>("method", MM_BEARER_IP_METHOD_DHCP);
      active_bearer_properties_.Set<KeyValueStore>(MM_BEARER_PROPERTY_IP4CONFIG,
                                                   ip4config);

      inactive_bearer_properties_.Set<bool>(MM_BEARER_PROPERTY_CONNECTED,
                                            false);
    }

    KeyValueStore* mutable_active_bearer_properties() {
      return &active_bearer_properties_;
    }

    KeyValueStore* mutable_inactive_bearer_properties() {
      return &inactive_bearer_properties_;
    }

    std::unique_ptr<mm1::ModemLocationProxyInterface>
    CreateMM1ModemLocationProxy(const RpcIdentifier& /*path*/,
                                const std::string& /*service*/) override {
      return std::move(test_->modem_location_proxy_);
    }

    std::unique_ptr<mm1::ModemModem3gppProxyInterface>
    CreateMM1ModemModem3gppProxy(const RpcIdentifier& /*path*/,
                                 const std::string& /*service*/) override {
      return std::move(test_->modem_3gpp_proxy_);
    }

    std::unique_ptr<mm1::ModemProxyInterface> CreateMM1ModemProxy(
        const RpcIdentifier& /*path*/,
        const std::string& /*service*/) override {
      return std::move(test_->modem_proxy_);
    }

    std::unique_ptr<mm1::ModemSimpleProxyInterface> CreateMM1ModemSimpleProxy(
        const RpcIdentifier& /*path*/,
        const std::string& /*service*/) override {
      return std::move(test_->modem_simple_proxy_);
    }

    std::unique_ptr<mm1::SimProxyInterface> CreateMM1SimProxy(
        const RpcIdentifier& /*path*/,
        const std::string& /*service*/) override {
      std::unique_ptr<mm1::MockSimProxy> sim_proxy =
          std::move(test_->sim_proxy_);
      test_->sim_proxy_ = std::make_unique<mm1::MockSimProxy>();
      return sim_proxy;
    }

    std::unique_ptr<DBusPropertiesProxyInterface> CreateDBusPropertiesProxy(
        const RpcIdentifier& path, const std::string& /*service*/) override {
      std::unique_ptr<MockDBusPropertiesProxy> properties_proxy =
          std::move(test_->properties_proxy_);
      if (path.value().find(kActiveBearerPathPrefix) != std::string::npos) {
        EXPECT_CALL(*properties_proxy, GetAll(MM_DBUS_INTERFACE_BEARER))
            .Times(AnyNumber())
            .WillRepeatedly(Return(active_bearer_properties_));
      } else {
        EXPECT_CALL(*properties_proxy, GetAll(MM_DBUS_INTERFACE_BEARER))
            .Times(AnyNumber())
            .WillRepeatedly(Return(inactive_bearer_properties_));
      }
      EXPECT_CALL(*properties_proxy, set_properties_changed_callback(_))
          .Times(AnyNumber());

      test_->properties_proxy_ = std::make_unique<MockDBusPropertiesProxy>();
      return properties_proxy;
    }

   private:
    CellularCapability3gppTest* test_;
    KeyValueStore active_bearer_properties_;
    KeyValueStore inactive_bearer_properties_;
  };

  EventDispatcher* dispatcher_;
  TestControl control_interface_;
  NiceMock<MockMetrics> metrics_;
  NiceMock<MockManager> manager_;
  MockModemInfo modem_info_;
  std::unique_ptr<NiceMock<mm1::MockModemModem3gppProxy>> modem_3gpp_proxy_;
  std::unique_ptr<mm1::MockModemLocationProxy> modem_location_proxy_;
  std::unique_ptr<mm1::MockModemProxy> modem_proxy_;
  std::unique_ptr<mm1::MockModemSimpleProxy> modem_simple_proxy_;
  std::unique_ptr<mm1::MockSimProxy> sim_proxy_;
  std::unique_ptr<MockDBusPropertiesProxy> properties_proxy_;
  CellularCapability3gpp* capability_;  // Owned by |cellular_|.
  DeviceMockAdaptor* device_adaptor_;   // Owned by |cellular_|.
  CellularRefPtr cellular_;
  MockCellularService* service_;  // owned by cellular_
  CellularServiceProvider cellular_service_provider_{&manager_};

  // saved for testing connect operations.
  RpcIdentifierCallback connect_callback_;

  // Set when required and passed to |cellular_|. Owned by |cellular_|.
  MockMobileOperatorInfo* mock_home_provider_info_;
  MockMobileOperatorInfo* mock_serving_operator_info_;
};

// Most of our tests involve using a real EventDispatcher object.
class CellularCapability3gppMainTest : public CellularCapability3gppTest {
 public:
  CellularCapability3gppMainTest() : CellularCapability3gppTest(&dispatcher_) {}

 protected:
  EventDispatcherForTest dispatcher_;
};

// Tests that involve timers will (or may) use a mock of the event dispatcher
// instead of a real one.
class CellularCapability3gppTimerTest : public CellularCapability3gppTest {
 public:
  CellularCapability3gppTimerTest()
      : CellularCapability3gppTest(&mock_dispatcher_) {}

 protected:
  ::testing::StrictMock<MockEventDispatcher> mock_dispatcher_;
};

const char CellularCapability3gppTest::kActiveBearerPathPrefix[] =
    "/bearer/active";
const char CellularCapability3gppTest::kImei[] = "999911110000";
const char CellularCapability3gppTest::kInactiveBearerPathPrefix[] =
    "/bearer/inactive";
const RpcIdentifier CellularCapability3gppTest::kSimPath =
    RpcIdentifier("/foo/sim");
const uint32_t CellularCapability3gppTest::kAccessTechnologies =
    MM_MODEM_ACCESS_TECHNOLOGY_LTE | MM_MODEM_ACCESS_TECHNOLOGY_HSPA_PLUS;
const char CellularCapability3gppTest::kTestMobileProviderDBPath[] =
    "provider_db_unittest.bfd";

TEST_F(CellularCapability3gppMainTest, StartModem) {
  ExpectModemAndModem3gppProperties();

  EXPECT_CALL(*modem_proxy_,
              Enable(true, _, _, CellularCapability::kTimeoutEnable))
      .WillOnce(Invoke(this, &CellularCapability3gppTest::InvokeEnable));

  Error error;
  EXPECT_CALL(*this, TestCallback(IsSuccess()));
  ResultCallback callback =
      Bind(&CellularCapability3gppTest::TestCallback, Unretained(this));
  capability_->StartModem(&error, callback);

  EXPECT_TRUE(error.IsOngoing());
  EXPECT_EQ(kImei, cellular_->imei());
  EXPECT_EQ(kAccessTechnologies, capability_->access_technologies_);
}

TEST_F(CellularCapability3gppMainTest, StartModemFailure) {
  EXPECT_CALL(*modem_proxy_,
              Enable(true, _, _, CellularCapability::kTimeoutEnable))
      .WillOnce(Invoke(this, &CellularCapability3gppTest::InvokeEnableFail));
  EXPECT_CALL(*properties_proxy_, GetAll(MM_DBUS_INTERFACE_MODEM)).Times(0);
  EXPECT_CALL(*properties_proxy_, GetAll(MM_DBUS_INTERFACE_MODEM_MODEM3GPP))
      .Times(0);

  Error error;
  EXPECT_CALL(*this, TestCallback(IsFailure()));
  ResultCallback callback =
      Bind(&CellularCapability3gppTest::TestCallback, Unretained(this));
  capability_->StartModem(&error, callback);
  EXPECT_TRUE(error.IsOngoing());
}

TEST_F(CellularCapability3gppMainTest, StartModemInWrongState) {
  ExpectModemAndModem3gppProperties();

  EXPECT_CALL(*modem_proxy_,
              Enable(true, _, _, CellularCapability::kTimeoutEnable))
      .WillOnce(
          Invoke(this, &CellularCapability3gppTest::InvokeEnableInWrongState))
      .WillOnce(Invoke(this, &CellularCapability3gppTest::InvokeEnable));

  Error error;
  EXPECT_CALL(*this, TestCallback(_)).Times(0);
  ResultCallback callback =
      Bind(&CellularCapability3gppTest::TestCallback, Unretained(this));
  capability_->StartModem(&error, callback);
  EXPECT_TRUE(error.IsOngoing());

  // Verify that the modem has not been enabled.
  EXPECT_TRUE(cellular_->imei().empty());
  EXPECT_EQ(0, capability_->access_technologies_);
  Mock::VerifyAndClearExpectations(this);

  // Change the state to kModemStateEnabling and verify that it still has not
  // been enabled.
  capability_->OnModemStateChanged(Cellular::kModemStateEnabling);
  EXPECT_TRUE(cellular_->imei().empty());
  EXPECT_EQ(0, capability_->access_technologies_);
  Mock::VerifyAndClearExpectations(this);

  // Change the state to kModemStateDisabling and verify that it still has not
  // been enabled.
  EXPECT_CALL(*this, TestCallback(_)).Times(0);
  capability_->OnModemStateChanged(Cellular::kModemStateDisabling);
  EXPECT_TRUE(cellular_->imei().empty());
  EXPECT_EQ(0, capability_->access_technologies_);
  Mock::VerifyAndClearExpectations(this);

  // Change the state of the modem to disabled and verify that it gets enabled.
  EXPECT_CALL(*this, TestCallback(IsSuccess()));
  capability_->OnModemStateChanged(Cellular::kModemStateDisabled);
  EXPECT_EQ(kImei, cellular_->imei());
  EXPECT_EQ(kAccessTechnologies, capability_->access_technologies_);
}

TEST_F(CellularCapability3gppMainTest, StartModemWithDeferredEnableFailure) {
  EXPECT_CALL(*modem_proxy_,
              Enable(true, _, _, CellularCapability::kTimeoutEnable))
      .Times(2)
      .WillRepeatedly(
          Invoke(this, &CellularCapability3gppTest::InvokeEnableInWrongState));
  EXPECT_CALL(*properties_proxy_, GetAll(MM_DBUS_INTERFACE_MODEM)).Times(0);
  EXPECT_CALL(*properties_proxy_, GetAll(MM_DBUS_INTERFACE_MODEM_MODEM3GPP))
      .Times(0);

  Error error;
  EXPECT_CALL(*this, TestCallback(_)).Times(0);
  ResultCallback callback =
      Bind(&CellularCapability3gppTest::TestCallback, Unretained(this));
  capability_->StartModem(&error, callback);
  EXPECT_TRUE(error.IsOngoing());
  Mock::VerifyAndClearExpectations(this);

  // Change the state of the modem to disabled but fail the deferred enable
  // operation with the WrongState error in order to verify that the deferred
  // enable operation does not trigger another deferred enable operation.
  EXPECT_CALL(*this, TestCallback(IsFailure()));
  capability_->OnModemStateChanged(Cellular::kModemStateDisabled);
}

TEST_F(CellularCapability3gppMainTest, StopModem) {
  // Save pointers to proxies before they are lost by the call to InitProxies
  mm1::MockModemProxy* modem_proxy = modem_proxy_.get();
  EXPECT_CALL(*modem_proxy, set_state_changed_callback(_));
  capability_->InitProxies();

  Error error;
  ResultCallback callback =
      Bind(&CellularCapability3gppTest::TestCallback, Unretained(this));
  capability_->StopModem(&error, callback);
  EXPECT_TRUE(error.IsSuccess());

  ResultCallback disable_callback;
  EXPECT_CALL(*modem_proxy,
              Enable(false, _, _, CellularCapability::kTimeoutEnable))
      .WillOnce(SaveArg<2>(&disable_callback));
  dispatcher_.DispatchPendingEvents();

  ResultCallback set_power_state_callback;
  EXPECT_CALL(
      *modem_proxy,
      SetPowerState(MM_MODEM_POWER_STATE_LOW, _, _,
                    CellularCapability3gpp::kSetPowerStateTimeoutMilliseconds))
      .WillOnce(SaveArg<2>(&set_power_state_callback));
  disable_callback.Run(Error(Error::kSuccess));

  EXPECT_CALL(*this, TestCallback(IsSuccess()));
  set_power_state_callback.Run(Error(Error::kSuccess));
  Mock::VerifyAndClearExpectations(this);

  // TestCallback should get called with success even if the power state
  // callback gets called with an error
  EXPECT_CALL(*this, TestCallback(IsSuccess()));
  set_power_state_callback.Run(Error(Error::kOperationFailed));
}

TEST_F(CellularCapability3gppMainTest, TerminationAction) {
  ExpectModemAndModem3gppProperties();

  {
    InSequence seq;

    EXPECT_CALL(*modem_proxy_,
                Enable(true, _, _, CellularCapability::kTimeoutEnable))
        .WillOnce(Invoke(this, &CellularCapability3gppTest::InvokeEnable));
    EXPECT_CALL(*modem_proxy_,
                Enable(false, _, _, CellularCapability::kTimeoutEnable))
        .WillOnce(Invoke(this, &CellularCapability3gppTest::InvokeEnable));
    EXPECT_CALL(*modem_proxy_,
                SetPowerState(
                    MM_MODEM_POWER_STATE_LOW, _, _,
                    CellularCapability3gpp::kSetPowerStateTimeoutMilliseconds))
        .WillOnce(
            Invoke(this, &CellularCapability3gppTest::InvokeSetPowerState));
  }
  EXPECT_CALL(*this, TestCallback(IsSuccess())).Times(2);

  EXPECT_EQ(Cellular::kStateDisabled, cellular_->state());
  EXPECT_EQ(Cellular::kModemStateUnknown, cellular_->modem_state());
  EXPECT_TRUE(manager_.termination_actions_.IsEmpty());

  // Here we mimic the modem state change from ModemManager. When the modem is
  // enabled, a termination action should be added.
  cellular_->OnModemStateChanged(Cellular::kModemStateEnabled);
  dispatcher_.DispatchPendingEvents();
  EXPECT_EQ(Cellular::kStateEnabled, cellular_->state());
  EXPECT_EQ(Cellular::kModemStateEnabled, cellular_->modem_state());
  EXPECT_FALSE(manager_.termination_actions_.IsEmpty());

  // Running the termination action should disable the modem.
  manager_.RunTerminationActions(
      Bind(&CellularCapability3gppMainTest::TestCallback, Unretained(this)));
  dispatcher_.DispatchPendingEvents();
  // Here we mimic the modem state change from ModemManager. When the modem is
  // disabled, the termination action should be removed.
  cellular_->OnModemStateChanged(Cellular::kModemStateDisabled);
  dispatcher_.DispatchPendingEvents();
  EXPECT_EQ(Cellular::kStateDisabled, cellular_->state());
  EXPECT_EQ(Cellular::kModemStateDisabled, cellular_->modem_state());
  EXPECT_TRUE(manager_.termination_actions_.IsEmpty());

  // No termination action should be called here.
  manager_.RunTerminationActions(
      Bind(&CellularCapability3gppMainTest::TestCallback, Unretained(this)));
  dispatcher_.DispatchPendingEvents();
}

TEST_F(CellularCapability3gppMainTest, TerminationActionRemovedByStopModem) {
  ExpectModemAndModem3gppProperties();

  {
    InSequence seq;

    EXPECT_CALL(*modem_proxy_,
                Enable(true, _, _, CellularCapability::kTimeoutEnable))
        .WillOnce(Invoke(this, &CellularCapability3gppTest::InvokeEnable));
    EXPECT_CALL(*modem_proxy_,
                Enable(false, _, _, CellularCapability::kTimeoutEnable))
        .WillOnce(Invoke(this, &CellularCapability3gppTest::InvokeEnable));
    EXPECT_CALL(*modem_proxy_,
                SetPowerState(
                    MM_MODEM_POWER_STATE_LOW, _, _,
                    CellularCapability3gpp::kSetPowerStateTimeoutMilliseconds))
        .WillOnce(
            Invoke(this, &CellularCapability3gppTest::InvokeSetPowerState));
  }
  EXPECT_CALL(*this, TestCallback(IsSuccess())).Times(1);

  EXPECT_EQ(Cellular::kStateDisabled, cellular_->state());
  EXPECT_EQ(Cellular::kModemStateUnknown, cellular_->modem_state());
  EXPECT_TRUE(manager_.termination_actions_.IsEmpty());

  // Here we mimic the modem state change from ModemManager. When the modem is
  // enabled, a termination action should be added.
  cellular_->OnModemStateChanged(Cellular::kModemStateEnabled);
  dispatcher_.DispatchPendingEvents();
  EXPECT_EQ(Cellular::kStateEnabled, cellular_->state());
  EXPECT_EQ(Cellular::kModemStateEnabled, cellular_->modem_state());
  EXPECT_FALSE(manager_.termination_actions_.IsEmpty());

  // Verify that the termination action is removed when the modem is disabled
  // not due to a suspend request.
  cellular_->SetEnabled(false);
  dispatcher_.DispatchPendingEvents();
  EXPECT_EQ(Cellular::kStateDisabled, cellular_->state());
  EXPECT_TRUE(manager_.termination_actions_.IsEmpty());

  // No termination action should be called here.
  manager_.RunTerminationActions(
      Bind(&CellularCapability3gppMainTest::TestCallback, Unretained(this)));
  dispatcher_.DispatchPendingEvents();
}

TEST_F(CellularCapability3gppMainTest, DisconnectModemNoBearer) {
  Error error;
  ResultCallback disconnect_callback;
  EXPECT_CALL(*modem_simple_proxy_,
              Disconnect(_, _, _, CellularCapability::kTimeoutDisconnect))
      .Times(0);
  capability_->Disconnect(&error, disconnect_callback);
}

TEST_F(CellularCapability3gppMainTest, DisconnectNoProxy) {
  Error error;
  ResultCallback disconnect_callback;
  EXPECT_CALL(*modem_simple_proxy_,
              Disconnect(_, _, _, CellularCapability::kTimeoutDisconnect))
      .Times(0);
  ReleaseCapabilityProxies();
  capability_->Disconnect(&error, disconnect_callback);
}

TEST_F(CellularCapability3gppMainTest, SimLockStatusChanged) {
  // Set up mock SIM properties
  const char kImsi[] = "310100000001";
  const char kSimIdentifier[] = "9999888";
  const char kOperatorIdentifier[] = "310240";
  const char kOperatorName[] = "Custom SPN";
  KeyValueStore sim_properties;
  sim_properties.Set<string>(MM_SIM_PROPERTY_IMSI, kImsi);
  sim_properties.Set<string>(MM_SIM_PROPERTY_SIMIDENTIFIER, kSimIdentifier);
  sim_properties.Set<string>(MM_SIM_PROPERTY_OPERATORIDENTIFIER,
                             kOperatorIdentifier);
  sim_properties.Set<string>(MM_SIM_PROPERTY_OPERATORNAME, kOperatorName);

  EXPECT_CALL(*properties_proxy_, GetAll(MM_DBUS_INTERFACE_SIM))
      .WillOnce(Return(sim_properties));
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(PendingActivationStore::kIdentifierICCID, _))
      .Times(3);
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivated))
      .Times(1);

  EXPECT_FALSE(cellular_->sim_present());
  EXPECT_EQ(nullptr, capability_->sim_proxy_);

  capability_->OnSimPathChanged(kSimPath);
  EXPECT_TRUE(cellular_->sim_present());
  EXPECT_NE(nullptr, capability_->sim_proxy_);
  EXPECT_EQ(kSimPath, capability_->sim_path_);

  cellular_->set_imsi("");
  cellular_->set_iccid("");
  capability_->spn_ = "";

  // SIM is locked.
  capability_->sim_lock_status_.lock_type = MM_MODEM_LOCK_SIM_PIN;
  capability_->OnSimLockStatusChanged();
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());

  EXPECT_EQ("", cellular_->imsi());
  EXPECT_EQ("", cellular_->iccid());
  EXPECT_EQ("", capability_->spn_);

  // SIM is unlocked.
  properties_proxy_.reset(new MockDBusPropertiesProxy());
  EXPECT_CALL(*properties_proxy_, GetAll(MM_DBUS_INTERFACE_SIM))
      .WillOnce(Return(sim_properties));
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(PendingActivationStore::kIdentifierICCID, _))
      .Times(3);
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivated))
      .Times(1);

  capability_->sim_lock_status_.lock_type = MM_MODEM_LOCK_NONE;
  capability_->OnSimLockStatusChanged();
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());

  EXPECT_EQ(kImsi, cellular_->imsi());
  EXPECT_EQ(kSimIdentifier, cellular_->iccid());
  EXPECT_EQ(kOperatorName, capability_->spn_);

  // SIM is missing and SIM path is "/".
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(PendingActivationStore::kIdentifierICCID, _))
      .Times(1);
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivated))
      .Times(1);
  capability_->OnSimPathChanged(CellularCapability3gpp::kRootPath);
  EXPECT_FALSE(cellular_->sim_present());
  EXPECT_EQ(nullptr, capability_->sim_proxy_);
  EXPECT_EQ(CellularCapability3gpp::kRootPath, capability_->sim_path_);

  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(_, _))
      .Times(0);

  capability_->OnSimLockStatusChanged();
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());

  EXPECT_EQ("", cellular_->imsi());
  EXPECT_EQ("", cellular_->iccid());
  EXPECT_EQ("", capability_->spn_);

  // SIM is missing and SIM path is empty.
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(PendingActivationStore::kIdentifierICCID, _))
      .Times(1);
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivated))
      .Times(1);
  capability_->OnSimPathChanged(RpcIdentifier(""));
  EXPECT_FALSE(cellular_->sim_present());
  EXPECT_EQ(nullptr, capability_->sim_proxy_);
  EXPECT_EQ(RpcIdentifier(""), capability_->sim_path_);

  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(_, _))
      .Times(0);
  capability_->OnSimLockStatusChanged();
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());

  EXPECT_EQ("", cellular_->imsi());
  EXPECT_EQ("", cellular_->iccid());
  EXPECT_EQ("", capability_->spn_);
}

TEST_F(CellularCapability3gppMainTest, PropertiesChanged) {
  // Set up mock modem properties
  KeyValueStore modem_properties;
  modem_properties.Set<uint32_t>(MM_MODEM_PROPERTY_ACCESSTECHNOLOGIES,
                                 kAccessTechnologies);
  modem_properties.Set<RpcIdentifier>(MM_MODEM_PROPERTY_SIM, kSimPath);

  // Set up mock modem 3gpp properties
  KeyValueStore modem3gpp_properties;
  modem3gpp_properties.Set<uint32_t>(
      MM_MODEM_MODEM3GPP_PROPERTY_ENABLEDFACILITYLOCKS, 0);
  modem3gpp_properties.Set<string>(MM_MODEM_MODEM3GPP_PROPERTY_IMEI, kImei);

  // Set up mock modem sim properties
  KeyValueStore sim_properties;

  EXPECT_CALL(*properties_proxy_, GetAll(MM_DBUS_INTERFACE_SIM))
      .WillOnce(Return(sim_properties));

  EXPECT_EQ("", cellular_->imei());
  EXPECT_EQ(MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN,
            capability_->access_technologies_);
  EXPECT_EQ(nullptr, capability_->sim_proxy_);
  EXPECT_CALL(*device_adaptor_, EmitStringChanged(kTechnologyFamilyProperty,
                                                  kTechnologyFamilyGsm));
  EXPECT_CALL(*device_adaptor_, EmitStringChanged(kImeiProperty, kImei));
  capability_->OnPropertiesChanged(MM_DBUS_INTERFACE_MODEM, modem_properties,
                                   vector<string>());
  EXPECT_EQ(kAccessTechnologies, capability_->access_technologies_);
  EXPECT_EQ(kSimPath, capability_->sim_path_);
  EXPECT_NE(nullptr, capability_->sim_proxy_);

  // Changing properties on wrong interface will not have an effect
  capability_->OnPropertiesChanged(MM_DBUS_INTERFACE_MODEM,
                                   modem3gpp_properties, vector<string>());
  EXPECT_EQ("", cellular_->imei());

  // Changing properties on the right interface gets reflected in the
  // capabilities object
  capability_->OnPropertiesChanged(MM_DBUS_INTERFACE_MODEM_MODEM3GPP,
                                   modem3gpp_properties, vector<string>());
  EXPECT_EQ(kImei, cellular_->imei());
  Mock::VerifyAndClearExpectations(device_adaptor_);

  // Expect to see changes when the family changes
  modem_properties.Clear();
  modem_properties.Set<uint32_t>(MM_MODEM_PROPERTY_ACCESSTECHNOLOGIES,
                                 MM_MODEM_ACCESS_TECHNOLOGY_1XRTT);
  EXPECT_CALL(*device_adaptor_, EmitStringChanged(kTechnologyFamilyProperty,
                                                  kTechnologyFamilyCdma))
      .Times(1);
  capability_->OnPropertiesChanged(MM_DBUS_INTERFACE_MODEM, modem_properties,
                                   vector<string>());
  Mock::VerifyAndClearExpectations(device_adaptor_);

  // Back to LTE
  modem_properties.Clear();
  modem_properties.Set<uint32_t>(MM_MODEM_PROPERTY_ACCESSTECHNOLOGIES,
                                 MM_MODEM_ACCESS_TECHNOLOGY_LTE);
  EXPECT_CALL(*device_adaptor_, EmitStringChanged(kTechnologyFamilyProperty,
                                                  kTechnologyFamilyGsm))
      .Times(1);
  capability_->OnPropertiesChanged(MM_DBUS_INTERFACE_MODEM, modem_properties,
                                   vector<string>());
  Mock::VerifyAndClearExpectations(device_adaptor_);

  // LTE & CDMA - the device adaptor should not be called!
  modem_properties.Clear();
  modem_properties.Set<uint32_t>(
      MM_MODEM_PROPERTY_ACCESSTECHNOLOGIES,
      MM_MODEM_ACCESS_TECHNOLOGY_LTE | MM_MODEM_ACCESS_TECHNOLOGY_1XRTT);
  EXPECT_CALL(*device_adaptor_, EmitStringChanged(_, _)).Times(0);
  capability_->OnPropertiesChanged(MM_DBUS_INTERFACE_MODEM, modem_properties,
                                   vector<string>());
}

TEST_F(CellularCapability3gppMainTest, UpdateRegistrationState) {
  capability_->InitProxies();

  CreateService();
  cellular_->set_imsi("310240123456789");
  cellular_->set_modem_state(Cellular::kModemStateConnected);
  SetRegistrationDroppedUpdateTimeout(0);

  const Stringmap& home_provider_map = cellular_->home_provider();
  ASSERT_NE(home_provider_map.end(), home_provider_map.find(kOperatorNameKey));
  string home_provider = home_provider_map.find(kOperatorNameKey)->second;
  string ota_name = cellular_->service_->friendly_name();

  // Home --> Roaming should be effective immediately.
  capability_->On3gppRegistrationChanged(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
                                         home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
            capability_->registration_state_);
  capability_->On3gppRegistrationChanged(
      MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING, home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING,
            capability_->registration_state_);

  // Idle --> Roaming should be effective immediately.
  capability_->On3gppRegistrationChanged(MM_MODEM_3GPP_REGISTRATION_STATE_IDLE,
                                         home_provider, ota_name);
  dispatcher_.DispatchPendingEvents();
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_IDLE,
            capability_->registration_state_);
  capability_->On3gppRegistrationChanged(
      MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING, home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING,
            capability_->registration_state_);

  // Idle --> Searching should be effective immediately.
  capability_->On3gppRegistrationChanged(MM_MODEM_3GPP_REGISTRATION_STATE_IDLE,
                                         home_provider, ota_name);
  dispatcher_.DispatchPendingEvents();
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_IDLE,
            capability_->registration_state_);
  capability_->On3gppRegistrationChanged(
      MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING, home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING,
            capability_->registration_state_);

  // Home --> Searching --> Home should never see Searching.
  EXPECT_CALL(metrics_, Notify3GPPRegistrationDelayedDropPosted());
  EXPECT_CALL(metrics_, Notify3GPPRegistrationDelayedDropCanceled());

  capability_->On3gppRegistrationChanged(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
                                         home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
            capability_->registration_state_);
  capability_->On3gppRegistrationChanged(
      MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING, home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
            capability_->registration_state_);
  capability_->On3gppRegistrationChanged(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
                                         home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
            capability_->registration_state_);
  dispatcher_.DispatchPendingEvents();
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
            capability_->registration_state_);
  Mock::VerifyAndClearExpectations(&metrics_);

  // Home --> Searching --> wait till dispatch should see Searching
  EXPECT_CALL(metrics_, Notify3GPPRegistrationDelayedDropPosted());
  capability_->On3gppRegistrationChanged(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
                                         home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
            capability_->registration_state_);
  capability_->On3gppRegistrationChanged(
      MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING, home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
            capability_->registration_state_);
  dispatcher_.DispatchPendingEvents();
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING,
            capability_->registration_state_);
  Mock::VerifyAndClearExpectations(&metrics_);

  // Home --> Searching --> Searching --> wait till dispatch should see
  // Searching *and* the first callback should be cancelled.
  EXPECT_CALL(*this, DummyCallback()).Times(0);
  EXPECT_CALL(metrics_, Notify3GPPRegistrationDelayedDropPosted());

  capability_->On3gppRegistrationChanged(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
                                         home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
            capability_->registration_state_);
  capability_->On3gppRegistrationChanged(
      MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING, home_provider, ota_name);
  SetMockRegistrationDroppedUpdateCallback();
  capability_->On3gppRegistrationChanged(
      MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING, home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
            capability_->registration_state_);
  dispatcher_.DispatchPendingEvents();
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING,
            capability_->registration_state_);
}

TEST_F(CellularCapability3gppMainTest, IsRegistered) {
  capability_->registration_state_ = MM_MODEM_3GPP_REGISTRATION_STATE_IDLE;
  EXPECT_FALSE(capability_->IsRegistered());

  capability_->registration_state_ = MM_MODEM_3GPP_REGISTRATION_STATE_HOME;
  EXPECT_TRUE(capability_->IsRegistered());

  capability_->registration_state_ = MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING;
  EXPECT_FALSE(capability_->IsRegistered());

  capability_->registration_state_ = MM_MODEM_3GPP_REGISTRATION_STATE_DENIED;
  EXPECT_FALSE(capability_->IsRegistered());

  capability_->registration_state_ = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
  EXPECT_FALSE(capability_->IsRegistered());

  capability_->registration_state_ = MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING;
  EXPECT_TRUE(capability_->IsRegistered());
}

TEST_F(CellularCapability3gppMainTest,
       UpdateRegistrationStateModemNotConnected) {
  capability_->InitProxies();
  CreateService();

  cellular_->set_imsi("310240123456789");
  cellular_->set_modem_state(Cellular::kModemStateRegistered);
  SetRegistrationDroppedUpdateTimeout(0);

  const Stringmap& home_provider_map = cellular_->home_provider();
  ASSERT_NE(home_provider_map.end(), home_provider_map.find(kOperatorNameKey));
  string home_provider = home_provider_map.find(kOperatorNameKey)->second;
  string ota_name = cellular_->service_->friendly_name();

  // Home --> Searching should be effective immediately.
  capability_->On3gppRegistrationChanged(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
                                         home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_HOME,
            capability_->registration_state_);
  capability_->On3gppRegistrationChanged(
      MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING, home_provider, ota_name);
  EXPECT_EQ(MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING,
            capability_->registration_state_);
}

TEST_F(CellularCapability3gppMainTest, IsValidSimPath) {
  // Invalid paths
  EXPECT_FALSE(capability_->IsValidSimPath(RpcIdentifier("")));
  EXPECT_FALSE(capability_->IsValidSimPath(RpcIdentifier("/")));

  // A valid path
  EXPECT_TRUE(capability_->IsValidSimPath(
      RpcIdentifier("/org/freedesktop/ModemManager1/SIM/0")));

  // Note that any string that is not one of the above invalid paths is
  // currently regarded as valid, since the ModemManager spec doesn't impose
  // a strict format on the path. The validity of this is subject to change.
  EXPECT_TRUE(capability_->IsValidSimPath(RpcIdentifier("path")));
}

TEST_F(CellularCapability3gppMainTest, NormalizeMdn) {
  EXPECT_EQ("", capability_->NormalizeMdn(""));
  EXPECT_EQ("12345678901", capability_->NormalizeMdn("12345678901"));
  EXPECT_EQ("12345678901", capability_->NormalizeMdn("+1 234 567 8901"));
  EXPECT_EQ("12345678901", capability_->NormalizeMdn("+1-234-567-8901"));
  EXPECT_EQ("12345678901", capability_->NormalizeMdn("+1 (234) 567-8901"));
  EXPECT_EQ("12345678901", capability_->NormalizeMdn("1 234  567 8901 "));
  EXPECT_EQ("2345678901", capability_->NormalizeMdn("(234) 567-8901"));
}

TEST_F(CellularCapability3gppMainTest, SimPathChanged) {
  // Set up mock modem SIM properties
  const char kImsi[] = "310100000001";
  const char kSimIdentifier[] = "9999888";
  const char kOperatorIdentifier[] = "310240";
  const char kOperatorName[] = "Custom SPN";
  KeyValueStore sim_properties;
  sim_properties.Set<string>(MM_SIM_PROPERTY_IMSI, kImsi);
  sim_properties.Set<string>(MM_SIM_PROPERTY_SIMIDENTIFIER, kSimIdentifier);
  sim_properties.Set<string>(MM_SIM_PROPERTY_OPERATORIDENTIFIER,
                             kOperatorIdentifier);
  sim_properties.Set<string>(MM_SIM_PROPERTY_OPERATORNAME, kOperatorName);

  EXPECT_CALL(*properties_proxy_, GetAll(MM_DBUS_INTERFACE_SIM))
      .Times(1)
      .WillOnce(Return(sim_properties));
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(PendingActivationStore::kIdentifierICCID, _))
      .Times(4);
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivated))
      .Times(2);

  EXPECT_FALSE(cellular_->sim_present());
  EXPECT_EQ(nullptr, capability_->sim_proxy_);
  EXPECT_EQ(RpcIdentifier(""), capability_->sim_path_);
  EXPECT_EQ("", cellular_->imsi());
  EXPECT_EQ("", cellular_->iccid());
  EXPECT_EQ("", capability_->spn_);

  capability_->OnSimPathChanged(kSimPath);
  EXPECT_TRUE(cellular_->sim_present());
  EXPECT_NE(nullptr, capability_->sim_proxy_);
  EXPECT_EQ(kSimPath, capability_->sim_path_);
  EXPECT_EQ(kImsi, cellular_->imsi());
  EXPECT_EQ(kSimIdentifier, cellular_->iccid());
  EXPECT_EQ(kOperatorName, capability_->spn_);

  // Changing to the same SIM path should be a no-op.
  capability_->OnSimPathChanged(kSimPath);
  EXPECT_TRUE(cellular_->sim_present());
  EXPECT_NE(nullptr, capability_->sim_proxy_);
  EXPECT_EQ(kSimPath, capability_->sim_path_);
  EXPECT_EQ(kImsi, cellular_->imsi());
  EXPECT_EQ(kSimIdentifier, cellular_->iccid());
  EXPECT_EQ(kOperatorName, capability_->spn_);

  capability_->OnSimPathChanged(RpcIdentifier(""));
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());
  Mock::VerifyAndClearExpectations(properties_proxy_.get());
  EXPECT_FALSE(cellular_->sim_present());
  EXPECT_EQ(nullptr, capability_->sim_proxy_);
  EXPECT_EQ(RpcIdentifier(""), capability_->sim_path_);
  EXPECT_EQ("", cellular_->imsi());
  EXPECT_EQ("", cellular_->iccid());
  EXPECT_EQ("", capability_->spn_);

  EXPECT_CALL(*properties_proxy_, GetAll(MM_DBUS_INTERFACE_SIM))
      .Times(1)
      .WillOnce(Return(sim_properties));
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(PendingActivationStore::kIdentifierICCID, _))
      .Times(4);
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivated))
      .Times(2);

  capability_->OnSimPathChanged(kSimPath);
  EXPECT_TRUE(cellular_->sim_present());
  EXPECT_NE(nullptr, capability_->sim_proxy_);
  EXPECT_EQ(kSimPath, capability_->sim_path_);
  EXPECT_EQ(kImsi, cellular_->imsi());
  EXPECT_EQ(kSimIdentifier, cellular_->iccid());
  EXPECT_EQ(kOperatorName, capability_->spn_);

  capability_->OnSimPathChanged(RpcIdentifier("/"));
  EXPECT_FALSE(cellular_->sim_present());
  EXPECT_EQ(nullptr, capability_->sim_proxy_);
  EXPECT_EQ(RpcIdentifier("/"), capability_->sim_path_);
  EXPECT_EQ("", cellular_->imsi());
  EXPECT_EQ("", cellular_->iccid());
  EXPECT_EQ("", capability_->spn_);
}

TEST_F(CellularCapability3gppMainTest, SimPropertiesChanged) {
  // Set up mock modem properties
  KeyValueStore modem_properties;
  modem_properties.Set<RpcIdentifier>(MM_MODEM_PROPERTY_SIM, kSimPath);

  // Set up mock modem sim properties
  const char kImsi[] = "310100000001";
  KeyValueStore sim_properties;
  sim_properties.Set<string>(MM_SIM_PROPERTY_IMSI, kImsi);

  EXPECT_CALL(*properties_proxy_, GetAll(MM_DBUS_INTERFACE_SIM))
      .WillOnce(Return(sim_properties));
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(PendingActivationStore::kIdentifierICCID, _))
      .Times(0);
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivated))
      .Times(2);

  EXPECT_EQ(nullptr, capability_->sim_proxy_);
  capability_->OnPropertiesChanged(MM_DBUS_INTERFACE_MODEM, modem_properties,
                                   vector<string>());
  EXPECT_EQ(kSimPath, capability_->sim_path_);
  EXPECT_NE(nullptr, capability_->sim_proxy_);
  EXPECT_EQ(kImsi, cellular_->imsi());
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());

  // Updating the SIM
  KeyValueStore new_properties;
  const char kNewImsi[] = "310240123456789";
  const char kSimIdentifier[] = "9999888";
  const char kOperatorIdentifier[] = "310240";
  const char kOperatorName[] = "Custom SPN";
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(PendingActivationStore::kIdentifierICCID, _))
      .Times(6);
  EXPECT_CALL(*mock_home_provider_info_, UpdateIMSI(kNewImsi)).Times(2);
  new_properties.Set<string>(MM_SIM_PROPERTY_IMSI, kNewImsi);
  new_properties.Set<string>(MM_SIM_PROPERTY_SIMIDENTIFIER, kSimIdentifier);
  new_properties.Set<string>(MM_SIM_PROPERTY_OPERATORIDENTIFIER,
                             kOperatorIdentifier);
  capability_->OnPropertiesChanged(MM_DBUS_INTERFACE_SIM, new_properties,
                                   vector<string>());
  EXPECT_EQ(kNewImsi, cellular_->imsi());
  EXPECT_EQ(kSimIdentifier, cellular_->iccid());
  EXPECT_EQ("", capability_->spn_);

  new_properties.Set<string>(MM_SIM_PROPERTY_OPERATORNAME, kOperatorName);
  capability_->OnPropertiesChanged(MM_DBUS_INTERFACE_SIM, new_properties,
                                   vector<string>());
  EXPECT_EQ(kOperatorName, capability_->spn_);
}

TEST_F(CellularCapability3gppMainTest, Reset) {
  // Save pointers to proxies before they are lost by the call to InitProxies
  mm1::MockModemProxy* modem_proxy = modem_proxy_.get();
  EXPECT_CALL(*modem_proxy, set_state_changed_callback(_));
  capability_->InitProxies();

  Error error;
  ResultCallback reset_callback;

  EXPECT_CALL(*modem_proxy, Reset(_, _, CellularCapability::kTimeoutReset))
      .WillOnce(SaveArg<1>(&reset_callback));

  capability_->Reset(&error, ResultCallback());
  EXPECT_TRUE(capability_->resetting_);
  reset_callback.Run(error);
  EXPECT_FALSE(capability_->resetting_);
}

TEST_F(CellularCapability3gppMainTest, UpdateActiveBearer) {
  // Common resources.
  const size_t kPathCount = 3;
  RpcIdentifier active_paths[kPathCount], inactive_paths[kPathCount];
  for (size_t i = 0; i < kPathCount; ++i) {
    active_paths[i] =
        RpcIdentifier(base::StringPrintf("%s/%zu", kActiveBearerPathPrefix, i));
    inactive_paths[i] = RpcIdentifier(
        base::StringPrintf("%s/%zu", kInactiveBearerPathPrefix, i));
  }

  EXPECT_EQ(nullptr, capability_->GetActiveBearer());

  // Check that |active_bearer_| is set correctly when an active bearer is
  // returned.
  capability_->OnBearersChanged({inactive_paths[0], inactive_paths[1],
                                 active_paths[2], inactive_paths[1],
                                 inactive_paths[2]});
  capability_->UpdateActiveBearer();
  ASSERT_NE(nullptr, capability_->GetActiveBearer());
  EXPECT_EQ(active_paths[2], capability_->GetActiveBearer()->dbus_path());

  // Check that |active_bearer_| is nullptr if no active bearers are returned.
  capability_->OnBearersChanged({inactive_paths[0], inactive_paths[1],
                                 inactive_paths[2], inactive_paths[1]});
  capability_->UpdateActiveBearer();
  EXPECT_EQ(nullptr, capability_->GetActiveBearer());

  // Check that returning multiple bearers causes death.
  capability_->OnBearersChanged({active_paths[0], inactive_paths[1],
                                 inactive_paths[2], active_paths[1],
                                 inactive_paths[1]});
  EXPECT_DEATH(capability_->UpdateActiveBearer(),
               "Found more than one active bearer.");

  capability_->OnBearersChanged({});
  capability_->UpdateActiveBearer();
  EXPECT_EQ(nullptr, capability_->GetActiveBearer());
}

TEST_F(CellularCapability3gppMainTest, SetInitialEpsBearer) {
  constexpr char kTestApn[] = "test_apn";
  KeyValueStore properties;
  Error error;
  ResultCallback callback =
      Bind(&CellularCapability3gppTest::TestCallback, Unretained(this));

  ResultCallback set_callback;
  EXPECT_CALL(*modem_3gpp_proxy_,
              SetInitialEpsBearerSettings(
                  _, _, _, CellularCapability::kTimeoutSetInitialEpsBearer))
      .Times(1)
      .WillOnce(SaveArg<2>(&set_callback));
  EXPECT_CALL(*this, TestCallback(IsSuccess()));
  properties.Set<string>(CellularCapability3gpp::kConnectApn, kTestApn);
  capability_->InitProxies();
  capability_->SetInitialEpsBearer(properties, &error, callback);
  set_callback.Run(Error(Error::kSuccess));
}

// Validates FillConnectPropertyMap
TEST_F(CellularCapability3gppMainTest, FillConnectPropertyMap) {
  constexpr char kTestApn[] = "test_apn";
  constexpr char kTestUser[] = "test_user";
  constexpr char kTestPassword[] = "test_password";

  KeyValueStore properties;
  Stringmap apn;
  apn[kApnProperty] = kTestApn;
  capability_->apn_try_list_ = {apn};
  capability_->FillConnectPropertyMap(&properties);
  EXPECT_THAT(properties, HasApn(kTestApn));
  EXPECT_THAT(properties, HasNoUser());
  EXPECT_THAT(properties, HasNoPassword());
  EXPECT_THAT(properties, HasNoAllowedAuth());
  EXPECT_THAT(properties, HasNoIpType());

  apn[kApnUsernameProperty] = kTestUser;
  capability_->apn_try_list_ = {apn};
  capability_->FillConnectPropertyMap(&properties);
  EXPECT_THAT(properties, HasApn(kTestApn));
  EXPECT_THAT(properties, HasUser(kTestUser));
  EXPECT_THAT(properties, HasNoPassword());
  EXPECT_THAT(properties, HasNoAllowedAuth());
  EXPECT_THAT(properties, HasNoIpType());

  apn[kApnPasswordProperty] = kTestPassword;
  capability_->apn_try_list_ = {apn};
  capability_->FillConnectPropertyMap(&properties);
  EXPECT_THAT(properties, HasApn(kTestApn));
  EXPECT_THAT(properties, HasUser(kTestUser));
  EXPECT_THAT(properties, HasPassword(kTestPassword));
  EXPECT_THAT(properties, HasNoAllowedAuth());
  EXPECT_THAT(properties, HasNoIpType());

  apn[kApnAuthenticationProperty] = kApnAuthenticationPap;
  capability_->apn_try_list_ = {apn};
  capability_->FillConnectPropertyMap(&properties);
  EXPECT_THAT(properties, HasApn(kTestApn));
  EXPECT_THAT(properties, HasUser(kTestUser));
  EXPECT_THAT(properties, HasPassword(kTestPassword));
  EXPECT_THAT(properties, HasAllowedAuth(MM_BEARER_ALLOWED_AUTH_PAP));
  EXPECT_THAT(properties, HasNoIpType());

  apn[kApnAuthenticationProperty] = kApnAuthenticationChap;
  capability_->apn_try_list_ = {apn};
  capability_->FillConnectPropertyMap(&properties);
  EXPECT_THAT(properties, HasApn(kTestApn));
  EXPECT_THAT(properties, HasUser(kTestUser));
  EXPECT_THAT(properties, HasPassword(kTestPassword));
  EXPECT_THAT(properties, HasAllowedAuth(MM_BEARER_ALLOWED_AUTH_CHAP));
  EXPECT_THAT(properties, HasNoIpType());

  apn[kApnAuthenticationProperty] = "something";
  capability_->apn_try_list_ = {apn};
  capability_->FillConnectPropertyMap(&properties);
  EXPECT_THAT(properties, HasApn(kTestApn));
  EXPECT_THAT(properties, HasUser(kTestUser));
  EXPECT_THAT(properties, HasPassword(kTestPassword));
  EXPECT_THAT(properties, HasNoAllowedAuth());
  EXPECT_THAT(properties, HasNoIpType());

  apn[kApnAuthenticationProperty] = "";
  capability_->apn_try_list_ = {apn};
  capability_->FillConnectPropertyMap(&properties);
  EXPECT_THAT(properties, HasApn(kTestApn));
  EXPECT_THAT(properties, HasUser(kTestUser));
  EXPECT_THAT(properties, HasPassword(kTestPassword));
  EXPECT_THAT(properties, HasNoAllowedAuth());
  EXPECT_THAT(properties, HasNoIpType());

  apn[kApnIpTypeProperty] = kApnIpTypeV4;
  capability_->apn_try_list_ = {apn};
  capability_->FillConnectPropertyMap(&properties);
  EXPECT_THAT(properties, HasApn(kTestApn));
  EXPECT_THAT(properties, HasUser(kTestUser));
  EXPECT_THAT(properties, HasPassword(kTestPassword));
  EXPECT_THAT(properties, HasNoAllowedAuth());
  EXPECT_THAT(properties, HasIpType(MM_BEARER_IP_FAMILY_IPV4));

  apn[kApnIpTypeProperty] = kApnIpTypeV6;
  capability_->apn_try_list_ = {apn};
  capability_->FillConnectPropertyMap(&properties);
  EXPECT_THAT(properties, HasApn(kTestApn));
  EXPECT_THAT(properties, HasUser(kTestUser));
  EXPECT_THAT(properties, HasPassword(kTestPassword));
  EXPECT_THAT(properties, HasNoAllowedAuth());
  EXPECT_THAT(properties, HasIpType(MM_BEARER_IP_FAMILY_IPV6));

  apn[kApnIpTypeProperty] = kApnIpTypeV4V6;
  capability_->apn_try_list_ = {apn};
  capability_->FillConnectPropertyMap(&properties);
  EXPECT_THAT(properties, HasApn(kTestApn));
  EXPECT_THAT(properties, HasUser(kTestUser));
  EXPECT_THAT(properties, HasPassword(kTestPassword));
  EXPECT_THAT(properties, HasNoAllowedAuth());
  EXPECT_THAT(properties, HasIpType(MM_BEARER_IP_FAMILY_IPV4V6));

  // IP type defaults to v4 if something unsupported is specified.
  apn[kApnIpTypeProperty] = "orekid";
  capability_->apn_try_list_ = {apn};
  capability_->FillConnectPropertyMap(&properties);
  EXPECT_THAT(properties, HasApn(kTestApn));
  EXPECT_THAT(properties, HasUser(kTestUser));
  EXPECT_THAT(properties, HasPassword(kTestPassword));
  EXPECT_THAT(properties, HasNoAllowedAuth());
  EXPECT_THAT(properties, HasIpType(MM_BEARER_IP_FAMILY_IPV4));
}

// Validates expected behavior of Connect function
TEST_F(CellularCapability3gppMainTest, Connect) {
  mm1::MockModemSimpleProxy* modem_simple_proxy = modem_simple_proxy_.get();
  SetSimpleProxy();
  Error error;
  KeyValueStore properties;
  capability_->apn_try_list_.clear();
  ResultCallback callback =
      Bind(&CellularCapability3gppTest::TestCallback, Unretained(this));
  RpcIdentifier bearer("/foo");

  // Test connect failures
  EXPECT_CALL(*modem_simple_proxy, Connect(_, _, _, _))
      .WillRepeatedly(SaveArg<2>(&connect_callback_));
  capability_->Connect(properties, &error, callback);
  EXPECT_TRUE(error.IsSuccess());
  EXPECT_CALL(*this, TestCallback(IsFailure()));
  EXPECT_CALL(*service_, ClearLastGoodApn());
  connect_callback_.Run(bearer, Error(Error::kOperationFailed));
  Mock::VerifyAndClearExpectations(this);

  // Test connect success
  capability_->Connect(properties, &error, callback);
  EXPECT_TRUE(error.IsSuccess());
  EXPECT_CALL(*this, TestCallback(IsSuccess()));
  connect_callback_.Run(bearer, Error(Error::kSuccess));
  Mock::VerifyAndClearExpectations(this);

  // Test connect failures without a service.  Make sure that shill
  // does not crash if the connect failed and there is no
  // CellularService object.  This can happen if the modem is enabled
  // and then quickly disabled.
  cellular_->service_ = nullptr;
  EXPECT_FALSE(capability_->cellular()->service());
  capability_->Connect(properties, &error, callback);
  EXPECT_TRUE(error.IsSuccess());
  EXPECT_CALL(*this, TestCallback(IsFailure()));
  connect_callback_.Run(bearer, Error(Error::kOperationFailed));
}

// Validates Connect iterates over APNs
TEST_F(CellularCapability3gppMainTest, ConnectApns) {
  mm1::MockModemSimpleProxy* modem_simple_proxy = modem_simple_proxy_.get();
  SetSimpleProxy();
  Error error;
  KeyValueStore properties;
  ResultCallback callback =
      Bind(&CellularCapability3gppTest::TestCallback, Unretained(this));
  RpcIdentifier bearer("/bearer0");

  const char apn_name_foo[] = "foo";
  const char apn_name_bar[] = "bar";
  EXPECT_CALL(*modem_simple_proxy, Connect(HasApn(apn_name_foo), _, _, _))
      .WillOnce(SaveArg<2>(&connect_callback_));
  Stringmap apn1;
  apn1[kApnProperty] = apn_name_foo;
  Stringmap apn2;
  apn2[kApnProperty] = apn_name_bar;
  capability_->apn_try_list_ = {apn1, apn2};
  capability_->FillConnectPropertyMap(&properties);
  capability_->Connect(properties, &error, callback);
  EXPECT_TRUE(error.IsSuccess());
  Mock::VerifyAndClearExpectations(modem_simple_proxy);

  EXPECT_CALL(*modem_simple_proxy, Connect(HasApn(apn_name_bar), _, _, _))
      .WillOnce(SaveArg<2>(&connect_callback_));
  EXPECT_CALL(*service_, ClearLastGoodApn());
  connect_callback_.Run(bearer, Error(Error::kInvalidApn));

  EXPECT_CALL(*service_, SetLastGoodApn(apn2));
  EXPECT_CALL(*this, TestCallback(IsSuccess()));
  connect_callback_.Run(bearer, Error(Error::kSuccess));
}

// Validates GetTypeString and AccessTechnologyToTechnologyFamily
TEST_F(CellularCapability3gppMainTest, GetTypeString) {
  static const uint32_t kGsmTechnologies[] = {
      MM_MODEM_ACCESS_TECHNOLOGY_LTE,
      MM_MODEM_ACCESS_TECHNOLOGY_HSPA_PLUS,
      MM_MODEM_ACCESS_TECHNOLOGY_HSPA,
      MM_MODEM_ACCESS_TECHNOLOGY_HSUPA,
      MM_MODEM_ACCESS_TECHNOLOGY_HSDPA,
      MM_MODEM_ACCESS_TECHNOLOGY_UMTS,
      MM_MODEM_ACCESS_TECHNOLOGY_EDGE,
      MM_MODEM_ACCESS_TECHNOLOGY_GPRS,
      MM_MODEM_ACCESS_TECHNOLOGY_GSM_COMPACT,
      MM_MODEM_ACCESS_TECHNOLOGY_GSM,
      MM_MODEM_ACCESS_TECHNOLOGY_LTE | MM_MODEM_ACCESS_TECHNOLOGY_EVDO0,
      MM_MODEM_ACCESS_TECHNOLOGY_GSM | MM_MODEM_ACCESS_TECHNOLOGY_EVDO0,
      MM_MODEM_ACCESS_TECHNOLOGY_LTE | MM_MODEM_ACCESS_TECHNOLOGY_EVDOA,
      MM_MODEM_ACCESS_TECHNOLOGY_GSM | MM_MODEM_ACCESS_TECHNOLOGY_EVDOA,
      MM_MODEM_ACCESS_TECHNOLOGY_LTE | MM_MODEM_ACCESS_TECHNOLOGY_EVDOB,
      MM_MODEM_ACCESS_TECHNOLOGY_GSM | MM_MODEM_ACCESS_TECHNOLOGY_EVDOB,
      MM_MODEM_ACCESS_TECHNOLOGY_GSM | MM_MODEM_ACCESS_TECHNOLOGY_1XRTT,
  };
  for (auto gsm_technology : kGsmTechnologies) {
    capability_->access_technologies_ = gsm_technology;
    ASSERT_EQ(capability_->GetTypeString(), kTechnologyFamilyGsm);
  }
  static const uint32_t kCdmaTechnologies[] = {
      MM_MODEM_ACCESS_TECHNOLOGY_EVDO0,
      MM_MODEM_ACCESS_TECHNOLOGY_EVDOA,
      MM_MODEM_ACCESS_TECHNOLOGY_EVDOA | MM_MODEM_ACCESS_TECHNOLOGY_EVDO0,
      MM_MODEM_ACCESS_TECHNOLOGY_EVDOB,
      MM_MODEM_ACCESS_TECHNOLOGY_EVDOB | MM_MODEM_ACCESS_TECHNOLOGY_EVDO0,
      MM_MODEM_ACCESS_TECHNOLOGY_1XRTT,
  };
  for (auto cdma_technology : kCdmaTechnologies) {
    capability_->access_technologies_ = cdma_technology;
    ASSERT_EQ(capability_->GetTypeString(), kTechnologyFamilyCdma);
  }
  capability_->access_technologies_ = MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
  ASSERT_EQ(capability_->GetTypeString(), "");
}

TEST_F(CellularCapability3gppMainTest, GetMdnForOLP) {
  const string kVzwUUID = "c83d6597-dc91-4d48-a3a7-d86b80123751";
  const string kFooUUID = "foo";
  MockMobileOperatorInfo mock_operator_info(&dispatcher_, "MobileOperatorInfo");

  EXPECT_CALL(mock_operator_info, IsMobileNetworkOperatorKnown())
      .WillRepeatedly(Return(true));
  EXPECT_CALL(mock_operator_info, uuid()).WillRepeatedly(ReturnRef(kVzwUUID));
  capability_->subscription_state_ = SubscriptionState::kUnknown;

  cellular_->set_mdn("");
  EXPECT_EQ("0000000000", capability_->GetMdnForOLP(&mock_operator_info));
  cellular_->set_mdn("0123456789");
  EXPECT_EQ("0123456789", capability_->GetMdnForOLP(&mock_operator_info));
  cellular_->set_mdn("10123456789");
  EXPECT_EQ("0123456789", capability_->GetMdnForOLP(&mock_operator_info));

  cellular_->set_mdn("1021232333");
  capability_->subscription_state_ = SubscriptionState::kUnprovisioned;
  EXPECT_EQ("0000000000", capability_->GetMdnForOLP(&mock_operator_info));
  Mock::VerifyAndClearExpectations(&mock_operator_info);

  EXPECT_CALL(mock_operator_info, IsMobileNetworkOperatorKnown())
      .WillRepeatedly(Return(true));
  EXPECT_CALL(mock_operator_info, uuid()).WillRepeatedly(ReturnRef(kFooUUID));

  cellular_->set_mdn("");
  EXPECT_EQ("", capability_->GetMdnForOLP(&mock_operator_info));
  cellular_->set_mdn("0123456789");
  EXPECT_EQ("0123456789", capability_->GetMdnForOLP(&mock_operator_info));
  cellular_->set_mdn("10123456789");
  EXPECT_EQ("10123456789", capability_->GetMdnForOLP(&mock_operator_info));
}

TEST_F(CellularCapability3gppMainTest, UpdateServiceOLP) {
  const MobileOperatorInfo::OnlinePortal kOlp{
      "http://testurl", "POST",
      "imei=${imei}&imsi=${imsi}&mdn=${mdn}&min=${min}&iccid=${iccid}"};
  const vector<MobileOperatorInfo::OnlinePortal> kOlpList{kOlp};
  const string kUuidVzw = "c83d6597-dc91-4d48-a3a7-d86b80123751";
  const string kUuidFoo = "foo";

  cellular_->set_imei("1");
  cellular_->set_imsi("2");
  cellular_->set_mdn("10123456789");
  cellular_->set_min("5");
  cellular_->set_iccid("6");

  EXPECT_CALL(*mock_home_provider_info_, IsMobileNetworkOperatorKnown())
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_home_provider_info_, olp_list())
      .WillRepeatedly(ReturnRef(kOlpList));
  EXPECT_CALL(*mock_home_provider_info_, uuid())
      .WillRepeatedly(ReturnRef(kUuidVzw));
  CreateService();
  capability_->UpdateServiceOLP();
  // Copy to simplify assertions below.
  Stringmap vzw_olp = cellular_->service()->olp();
  EXPECT_EQ("http://testurl", vzw_olp[kPaymentPortalURL]);
  EXPECT_EQ("POST", vzw_olp[kPaymentPortalMethod]);
  EXPECT_EQ("imei=1&imsi=2&mdn=0123456789&min=5&iccid=6",
            vzw_olp[kPaymentPortalPostData]);
  Mock::VerifyAndClearExpectations(mock_home_provider_info_);

  EXPECT_CALL(*mock_home_provider_info_, IsMobileNetworkOperatorKnown())
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_home_provider_info_, olp_list())
      .WillRepeatedly(ReturnRef(kOlpList));
  EXPECT_CALL(*mock_home_provider_info_, uuid())
      .WillRepeatedly(ReturnRef(kUuidFoo));
  capability_->UpdateServiceOLP();
  // Copy to simplify assertions below.
  Stringmap olp = cellular_->service()->olp();
  EXPECT_EQ("http://testurl", olp[kPaymentPortalURL]);
  EXPECT_EQ("POST", olp[kPaymentPortalMethod]);
  EXPECT_EQ("imei=1&imsi=2&mdn=10123456789&min=5&iccid=6",
            olp[kPaymentPortalPostData]);
}

TEST_F(CellularCapability3gppMainTest, IsMdnValid) {
  cellular_->set_mdn("");
  EXPECT_FALSE(capability_->IsMdnValid());
  cellular_->set_mdn("0000000");
  EXPECT_FALSE(capability_->IsMdnValid());
  cellular_->set_mdn("0000001");
  EXPECT_TRUE(capability_->IsMdnValid());
  cellular_->set_mdn("1231223");
  EXPECT_TRUE(capability_->IsMdnValid());
}

TEST_F(CellularCapability3gppTimerTest, CompleteActivation) {
  const char kIccid[] = "1234567";

  cellular_->set_iccid(kIccid);
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              SetActivationState(PendingActivationStore::kIdentifierICCID,
                                 kIccid, PendingActivationStore::kStatePending))
      .Times(1);
  EXPECT_CALL(
      *modem_info_.mock_pending_activation_store(),
      GetActivationState(PendingActivationStore::kIdentifierICCID, kIccid))
      .WillOnce(Return(PendingActivationStore::kStatePending));
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivating))
      .Times(1);
  EXPECT_CALL(*modem_proxy_, Reset(_, _, _)).Times(1);
  Error error;
  capability_->InitProxies();
  capability_->CompleteActivation(&error);
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());
  Mock::VerifyAndClearExpectations(service_);
  Mock::VerifyAndClearExpectations(&mock_dispatcher_);
}

TEST_F(CellularCapability3gppMainTest, UpdateServiceActivationState) {
  const char kIccid[] = "1234567";
  const vector<MobileOperatorInfo::OnlinePortal> olp_list{
      {"some@url", "some_method", "some_post_data"}};

  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(PendingActivationStore::kIdentifierICCID, _))
      .WillRepeatedly(Return(PendingActivationStore::kStateUnknown));

  capability_->subscription_state_ = SubscriptionState::kUnprovisioned;
  cellular_->set_iccid("");
  cellular_->set_mdn("0000000000");
  EXPECT_CALL(*mock_home_provider_info_, IsMobileNetworkOperatorKnown())
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_home_provider_info_, olp_list())
      .WillRepeatedly(ReturnRef(olp_list));

  EXPECT_CALL(*service_, SetActivationState(kActivationStateNotActivated))
      .Times(1);
  capability_->UpdateServiceActivationState();
  Mock::VerifyAndClearExpectations(service_);

  cellular_->set_mdn("1231231122");
  capability_->subscription_state_ = SubscriptionState::kUnknown;
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivated))
      .Times(1);
  capability_->UpdateServiceActivationState();
  Mock::VerifyAndClearExpectations(service_);

  cellular_->set_mdn("0000000000");
  cellular_->set_iccid(kIccid);
  EXPECT_CALL(
      *modem_info_.mock_pending_activation_store(),
      GetActivationState(PendingActivationStore::kIdentifierICCID, kIccid))
      .Times(1)
      .WillRepeatedly(Return(PendingActivationStore::kStatePending));
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivating))
      .Times(1);
  capability_->UpdateServiceActivationState();
  Mock::VerifyAndClearExpectations(service_);
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());

  EXPECT_CALL(
      *modem_info_.mock_pending_activation_store(),
      GetActivationState(PendingActivationStore::kIdentifierICCID, kIccid))
      .Times(2)
      .WillRepeatedly(Return(PendingActivationStore::kStateActivated));
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivated))
      .Times(1);
  capability_->UpdateServiceActivationState();
  Mock::VerifyAndClearExpectations(service_);
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());

  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(PendingActivationStore::kIdentifierICCID, _))
      .WillRepeatedly(Return(PendingActivationStore::kStateUnknown));

  // SubscriptionStateUnprovisioned overrides valid MDN.
  capability_->subscription_state_ = SubscriptionState::kUnprovisioned;
  cellular_->set_mdn("1231231122");
  cellular_->set_iccid("");
  EXPECT_CALL(*service_, SetActivationState(kActivationStateNotActivated))
      .Times(1);
  capability_->UpdateServiceActivationState();
  Mock::VerifyAndClearExpectations(service_);

  // SubscriptionStateProvisioned overrides invalid MDN.
  capability_->subscription_state_ = SubscriptionState::kProvisioned;
  cellular_->set_mdn("0000000000");
  cellular_->set_iccid("");
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivated))
      .Times(1);
  capability_->UpdateServiceActivationState();
  Mock::VerifyAndClearExpectations(service_);
}

TEST_F(CellularCapability3gppMainTest, UpdatePendingActivationState) {
  const char kIccid[] = "1234567";

  capability_->InitProxies();
  capability_->registration_state_ = MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING;

  // No MDN, no ICCID.
  cellular_->set_mdn("0000000");
  capability_->subscription_state_ = SubscriptionState::kUnknown;
  cellular_->set_iccid("");
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(PendingActivationStore::kIdentifierICCID, _))
      .Times(0);
  capability_->UpdatePendingActivationState();
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());

  // Valid MDN, but subsciption_state_ Unprovisioned
  cellular_->set_mdn("1234567");
  capability_->subscription_state_ = SubscriptionState::kUnprovisioned;
  cellular_->set_iccid("");
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              GetActivationState(PendingActivationStore::kIdentifierICCID, _))
      .Times(0);
  capability_->UpdatePendingActivationState();
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());

  // ICCID known.
  cellular_->set_iccid(kIccid);

  // After the modem has reset.
  capability_->reset_done_ = true;
  EXPECT_CALL(
      *modem_info_.mock_pending_activation_store(),
      GetActivationState(PendingActivationStore::kIdentifierICCID, kIccid))
      .Times(1)
      .WillOnce(Return(PendingActivationStore::kStatePending));
  EXPECT_CALL(
      *modem_info_.mock_pending_activation_store(),
      SetActivationState(PendingActivationStore::kIdentifierICCID, kIccid,
                         PendingActivationStore::kStateActivated))
      .Times(1);
  EXPECT_CALL(*service_, SetActivationState(kActivationStateActivating))
      .Times(1);
  EXPECT_CALL(*service_, activation_state())
      .Times(2)
      .WillRepeatedly(ReturnRef(kActivationStateUnknown));
  capability_->UpdatePendingActivationState();
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());

  // Not registered.
  capability_->registration_state_ = MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING;
  EXPECT_CALL(
      *modem_info_.mock_pending_activation_store(),
      GetActivationState(PendingActivationStore::kIdentifierICCID, kIccid))
      .Times(2)
      .WillRepeatedly(Return(PendingActivationStore::kStateActivated));
  EXPECT_CALL(*service_, AutoConnect()).Times(0);
  capability_->UpdatePendingActivationState();
  Mock::VerifyAndClearExpectations(service_);

  // Service, registered.
  capability_->registration_state_ = MM_MODEM_3GPP_REGISTRATION_STATE_HOME;
  EXPECT_CALL(*service_, AutoConnect()).Times(1);
  EXPECT_CALL(*service_, activation_state())
      .WillOnce(ReturnRef(kActivationStateUnknown));
  capability_->UpdatePendingActivationState();

  cellular_->service_->activation_state_ = kActivationStateNotActivated;

  Mock::VerifyAndClearExpectations(service_);
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());

  EXPECT_CALL(*service_, activation_state())
      .WillRepeatedly(ReturnRef(kActivationStateUnknown));
  EXPECT_CALL(
      *modem_info_.mock_pending_activation_store(),
      GetActivationState(PendingActivationStore::kIdentifierICCID, kIccid))
      .WillRepeatedly(Return(PendingActivationStore::kStateUnknown));

  // Device is connected.
  cellular_->state_ = Cellular::kStateConnected;
  capability_->UpdatePendingActivationState();

  // Device is linked.
  cellular_->state_ = Cellular::kStateLinked;
  capability_->UpdatePendingActivationState();

  // Got valid MDN, subscription_state_ is SubscriptionState::kUnknown
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              RemoveEntry(PendingActivationStore::kIdentifierICCID, kIccid));
  cellular_->state_ = Cellular::kStateRegistered;
  cellular_->set_mdn("1020304");
  capability_->subscription_state_ = SubscriptionState::kUnknown;
  capability_->UpdatePendingActivationState();
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());

  EXPECT_CALL(*service_, activation_state())
      .WillRepeatedly(ReturnRef(kActivationStateUnknown));
  EXPECT_CALL(
      *modem_info_.mock_pending_activation_store(),
      GetActivationState(PendingActivationStore::kIdentifierICCID, kIccid))
      .WillRepeatedly(Return(PendingActivationStore::kStateUnknown));

  // Got invalid MDN, subscription_state_ is SubscriptionState::kProvisioned
  EXPECT_CALL(*modem_info_.mock_pending_activation_store(),
              RemoveEntry(PendingActivationStore::kIdentifierICCID, kIccid));
  cellular_->state_ = Cellular::kStateRegistered;
  cellular_->set_mdn("0000000");
  capability_->subscription_state_ = SubscriptionState::kProvisioned;
  capability_->UpdatePendingActivationState();
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());
}

TEST_F(CellularCapability3gppMainTest, IsServiceActivationRequired) {
  const vector<MobileOperatorInfo::OnlinePortal> empty_list;
  const vector<MobileOperatorInfo::OnlinePortal> olp_list{
      {"some@url", "some_method", "some_post_data"}};

  capability_->subscription_state_ = SubscriptionState::kProvisioned;
  EXPECT_FALSE(capability_->IsServiceActivationRequired());

  capability_->subscription_state_ = SubscriptionState::kUnprovisioned;
  EXPECT_TRUE(capability_->IsServiceActivationRequired());

  capability_->subscription_state_ = SubscriptionState::kUnknown;
  cellular_->set_mdn("0000000000");
  EXPECT_FALSE(capability_->IsServiceActivationRequired());

  EXPECT_CALL(*mock_home_provider_info_, IsMobileNetworkOperatorKnown())
      .WillRepeatedly(Return(false));
  EXPECT_FALSE(capability_->IsServiceActivationRequired());
  Mock::VerifyAndClearExpectations(mock_home_provider_info_);

  EXPECT_CALL(*mock_home_provider_info_, IsMobileNetworkOperatorKnown())
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_home_provider_info_, olp_list())
      .WillRepeatedly(ReturnRef(empty_list));
  EXPECT_FALSE(capability_->IsServiceActivationRequired());
  Mock::VerifyAndClearExpectations(mock_home_provider_info_);

  // Set expectations for all subsequent cases.
  EXPECT_CALL(*mock_home_provider_info_, IsMobileNetworkOperatorKnown())
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_home_provider_info_, olp_list())
      .WillRepeatedly(ReturnRef(olp_list));

  cellular_->set_mdn("");
  EXPECT_TRUE(capability_->IsServiceActivationRequired());
  cellular_->set_mdn("1234567890");
  EXPECT_FALSE(capability_->IsServiceActivationRequired());
  cellular_->set_mdn("0000000000");
  EXPECT_TRUE(capability_->IsServiceActivationRequired());

  const char kIccid[] = "1234567890";
  cellular_->set_iccid(kIccid);
  EXPECT_CALL(
      *modem_info_.mock_pending_activation_store(),
      GetActivationState(PendingActivationStore::kIdentifierICCID, kIccid))
      .WillOnce(Return(PendingActivationStore::kStateActivated))
      .WillOnce(Return(PendingActivationStore::kStatePending))
      .WillOnce(Return(PendingActivationStore::kStateUnknown));
  EXPECT_FALSE(capability_->IsServiceActivationRequired());
  EXPECT_FALSE(capability_->IsServiceActivationRequired());
  EXPECT_TRUE(capability_->IsServiceActivationRequired());
  Mock::VerifyAndClearExpectations(modem_info_.mock_pending_activation_store());
}

TEST_F(CellularCapability3gppMainTest, OnModemCurrentCapabilitiesChanged) {
  EXPECT_FALSE(cellular_->scanning_supported());
  capability_->OnModemCurrentCapabilitiesChanged(MM_MODEM_CAPABILITY_LTE);
  EXPECT_FALSE(cellular_->scanning_supported());
  capability_->OnModemCurrentCapabilitiesChanged(MM_MODEM_CAPABILITY_CDMA_EVDO);
  EXPECT_FALSE(cellular_->scanning_supported());
  capability_->OnModemCurrentCapabilitiesChanged(MM_MODEM_CAPABILITY_GSM_UMTS);
  EXPECT_TRUE(cellular_->scanning_supported());
  capability_->OnModemCurrentCapabilitiesChanged(MM_MODEM_CAPABILITY_GSM_UMTS |
                                                 MM_MODEM_CAPABILITY_CDMA_EVDO);
  EXPECT_TRUE(cellular_->scanning_supported());
}

TEST_F(CellularCapability3gppMainTest, SimLockStatusToProperty) {
  Error error;
  KeyValueStore store = capability_->SimLockStatusToProperty(&error);
  EXPECT_FALSE(store.Get<bool>(kSIMLockEnabledProperty));
  EXPECT_TRUE(store.Get<string>(kSIMLockTypeProperty).empty());
  EXPECT_EQ(0, store.Get<int32_t>(kSIMLockRetriesLeftProperty));

  capability_->sim_lock_status_.enabled = true;
  capability_->sim_lock_status_.retries_left = 3;
  capability_->sim_lock_status_.lock_type = MM_MODEM_LOCK_SIM_PIN;
  store = capability_->SimLockStatusToProperty(&error);
  EXPECT_TRUE(store.Get<bool>(kSIMLockEnabledProperty));
  EXPECT_EQ("sim-pin", store.Get<string>(kSIMLockTypeProperty));
  EXPECT_EQ(3, store.Get<int32_t>(kSIMLockRetriesLeftProperty));

  capability_->sim_lock_status_.lock_type = MM_MODEM_LOCK_SIM_PUK;
  store = capability_->SimLockStatusToProperty(&error);
  EXPECT_EQ("sim-puk", store.Get<string>(kSIMLockTypeProperty));

  capability_->sim_lock_status_.lock_type = MM_MODEM_LOCK_SIM_PIN2;
  store = capability_->SimLockStatusToProperty(&error);
  EXPECT_TRUE(store.Get<string>(kSIMLockTypeProperty).empty());

  capability_->sim_lock_status_.lock_type = MM_MODEM_LOCK_SIM_PUK2;
  store = capability_->SimLockStatusToProperty(&error);
  EXPECT_TRUE(store.Get<string>(kSIMLockTypeProperty).empty());
}

TEST_F(CellularCapability3gppMainTest, OnLockRetriesChanged) {
  CellularCapability3gpp::LockRetryData data;

  capability_->OnLockRetriesChanged(data);
  EXPECT_EQ(CellularCapability3gpp::kUnknownLockRetriesLeft,
            capability_->sim_lock_status_.retries_left);

  data[MM_MODEM_LOCK_SIM_PIN] = 3;
  data[MM_MODEM_LOCK_SIM_PIN2] = 5;
  data[MM_MODEM_LOCK_SIM_PUK] = 10;
  capability_->OnLockRetriesChanged(data);
  EXPECT_EQ(3, capability_->sim_lock_status_.retries_left);

  capability_->sim_lock_status_.lock_type = MM_MODEM_LOCK_SIM_PUK;
  capability_->OnLockRetriesChanged(data);
  EXPECT_EQ(10, capability_->sim_lock_status_.retries_left);

  capability_->sim_lock_status_.lock_type = MM_MODEM_LOCK_SIM_PIN;
  capability_->OnLockRetriesChanged(data);
  EXPECT_EQ(3, capability_->sim_lock_status_.retries_left);

  capability_->sim_lock_status_.lock_type = MM_MODEM_LOCK_SIM_PIN2;
  capability_->OnLockRetriesChanged(data);
  // retries_left should always indicate the number of SIM_PIN retries if the
  // lock is not SIM_PUK
  EXPECT_EQ(3, capability_->sim_lock_status_.retries_left);

  data.clear();
  capability_->OnLockRetriesChanged(data);
  EXPECT_EQ(CellularCapability3gpp::kUnknownLockRetriesLeft,
            capability_->sim_lock_status_.retries_left);
}

TEST_F(CellularCapability3gppMainTest, OnLockTypeChanged) {
  EXPECT_EQ(MM_MODEM_LOCK_UNKNOWN, capability_->sim_lock_status_.lock_type);

  capability_->OnLockTypeChanged(MM_MODEM_LOCK_NONE);
  EXPECT_EQ(MM_MODEM_LOCK_NONE, capability_->sim_lock_status_.lock_type);
  EXPECT_FALSE(capability_->sim_lock_status_.enabled);

  capability_->OnLockTypeChanged(MM_MODEM_LOCK_SIM_PIN);
  EXPECT_EQ(MM_MODEM_LOCK_SIM_PIN, capability_->sim_lock_status_.lock_type);
  EXPECT_TRUE(capability_->sim_lock_status_.enabled);

  capability_->sim_lock_status_.enabled = false;
  capability_->OnLockTypeChanged(MM_MODEM_LOCK_SIM_PUK);
  EXPECT_EQ(MM_MODEM_LOCK_SIM_PUK, capability_->sim_lock_status_.lock_type);
  EXPECT_TRUE(capability_->sim_lock_status_.enabled);
}

TEST_F(CellularCapability3gppMainTest, OnSimLockPropertiesChanged) {
  EXPECT_EQ(MM_MODEM_LOCK_UNKNOWN, capability_->sim_lock_status_.lock_type);
  EXPECT_EQ(0, capability_->sim_lock_status_.retries_left);

  KeyValueStore changed;
  vector<string> invalidated;

  capability_->OnModemPropertiesChanged(changed, invalidated);
  EXPECT_EQ(MM_MODEM_LOCK_UNKNOWN, capability_->sim_lock_status_.lock_type);
  EXPECT_EQ(0, capability_->sim_lock_status_.retries_left);

  // Unlock retries changed, but the SIM wasn't locked.
  CellularCapability3gpp::LockRetryData retry_data;
  retry_data[MM_MODEM_LOCK_SIM_PIN] = 3;
  changed.SetVariant(MM_MODEM_PROPERTY_UNLOCKRETRIES, brillo::Any(retry_data));

  capability_->OnModemPropertiesChanged(changed, invalidated);
  EXPECT_EQ(MM_MODEM_LOCK_UNKNOWN, capability_->sim_lock_status_.lock_type);
  EXPECT_EQ(3, capability_->sim_lock_status_.retries_left);

  // Unlock retries changed and the SIM got locked.
  changed.Set<uint32_t>(MM_MODEM_PROPERTY_UNLOCKREQUIRED,
                        static_cast<uint32_t>(MM_MODEM_LOCK_SIM_PIN));
  capability_->OnModemPropertiesChanged(changed, invalidated);
  EXPECT_EQ(MM_MODEM_LOCK_SIM_PIN, capability_->sim_lock_status_.lock_type);
  EXPECT_EQ(3, capability_->sim_lock_status_.retries_left);

  // Only unlock retries changed.
  changed.Remove(MM_MODEM_PROPERTY_UNLOCKREQUIRED);
  retry_data[MM_MODEM_LOCK_SIM_PIN] = 2;
  changed.SetVariant(MM_MODEM_PROPERTY_UNLOCKRETRIES, brillo::Any(retry_data));
  capability_->OnModemPropertiesChanged(changed, invalidated);
  EXPECT_EQ(MM_MODEM_LOCK_SIM_PIN, capability_->sim_lock_status_.lock_type);
  EXPECT_EQ(2, capability_->sim_lock_status_.retries_left);

  // Unlock retries changed with a value that doesn't match the current
  // lock type. Default to unknown if PIN1 is unavailable.
  retry_data.clear();
  retry_data[MM_MODEM_LOCK_SIM_PIN2] = 2;
  changed.SetVariant(MM_MODEM_PROPERTY_UNLOCKRETRIES, brillo::Any(retry_data));
  capability_->OnModemPropertiesChanged(changed, invalidated);
  EXPECT_EQ(MM_MODEM_LOCK_SIM_PIN, capability_->sim_lock_status_.lock_type);
  EXPECT_EQ(CellularCapability3gpp::kUnknownLockRetriesLeft,
            capability_->sim_lock_status_.retries_left);
}

}  // namespace shill
