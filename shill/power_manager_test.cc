// Copyright 2018 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/power_manager.h"

#include <memory>
#include <string>
#include <utility>

#include <base/check.h>
#include <base/functional/bind.h>
#include <base/test/test_future.h>
#include <chromeos/dbus/service_constants.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "shill/mock_control.h"
#include "shill/mock_metrics.h"
#include "shill/mock_power_manager_proxy.h"
#include "shill/power_manager_proxy_interface.h"

using testing::_;
using testing::DoAll;
using testing::IgnoreResult;
using testing::Invoke;
using testing::InvokeWithoutArgs;
using testing::Mock;
using testing::Return;
using testing::SetArgPointee;
using testing::Test;

namespace shill {

namespace {

class FakeControl : public MockControl {
 public:
  FakeControl()
      : delegate_(nullptr),
        power_manager_proxy_raw_(new MockPowerManagerProxy),
        power_manager_proxy_(power_manager_proxy_raw_) {}

  std::unique_ptr<PowerManagerProxyInterface> CreatePowerManagerProxy(
      PowerManagerProxyDelegate* delegate,
      const base::RepeatingClosure& service_appeared_callback,
      const base::RepeatingClosure& service_vanished_callback) override {
    CHECK(power_manager_proxy_);
    delegate_ = delegate;
    // Passes ownership.
    return std::move(power_manager_proxy_);
  }

  PowerManagerProxyDelegate* delegate() const { return delegate_; }
  // Can not guarantee that the returned object is alive.
  MockPowerManagerProxy* power_manager_proxy() const {
    return power_manager_proxy_raw_;
  }

 private:
  PowerManagerProxyDelegate* delegate_;
  MockPowerManagerProxy* const power_manager_proxy_raw_;
  std::unique_ptr<MockPowerManagerProxy> power_manager_proxy_;
};

}  // namespace

class PowerManagerTest : public Test {
 public:
  static const char kDescription[];
  static const char kDarkDescription[];
  static const char kPowerManagerDefaultOwner[];
  static const int kSuspendId1 = 123;
  static const int kSuspendId2 = 456;
  static const int64_t kSuspendDurationUsecs = 1000000;
  static const int kDelayId = 4;
  static const int kDelayId2 = 5;

  PowerManagerTest()
      : kTimeout(base::Seconds(3)),
        power_manager_(&control_),
        power_manager_proxy_(control_.power_manager_proxy()),
        delegate_(control_.delegate()) {}

  MOCK_METHOD(void, SuspendImminentAction, ());
  MOCK_METHOD(void, SuspendDoneAction, ());
  MOCK_METHOD(void, DarkSuspendImminentAction, ());

 protected:
  void SetUp() override {
    power_manager_.Start(
        kTimeout,
        base::BindRepeating(&PowerManagerTest::SuspendImminentAction,
                            base::Unretained(this)),
        base::BindRepeating(&PowerManagerTest::SuspendDoneAction,
                            base::Unretained(this)),
        base::BindRepeating(&PowerManagerTest::DarkSuspendImminentAction,
                            base::Unretained(this)));
  }

  void TearDown() override { power_manager_.Stop(); }

  void AddProxyExpectationForRegisterSuspendDelay(std::optional<int> delay_id) {
    EXPECT_CALL(*power_manager_proxy_,
                RegisterSuspendDelay(kTimeout, kDescription, _))
        .WillOnce(Invoke(
            [delay_id](base::TimeDelta /* timeout */,
                       const std::string& /* description */,
                       base::OnceCallback<void(std::optional<int>)> callback) {
              std::move(callback).Run(delay_id);
            }));
  }

  void AddProxyExpectationForUnregisterSuspendDelay(int delay_id,
                                                    bool return_value) {
    EXPECT_CALL(*power_manager_proxy_, UnregisterSuspendDelay(delay_id))
        .WillOnce(Return(return_value));
  }

  void AddProxyExpectationForReportSuspendReadiness(int delay_id,
                                                    int suspend_id,
                                                    bool return_value) {
    EXPECT_CALL(*power_manager_proxy_,
                ReportSuspendReadiness(delay_id, suspend_id, _))
        .WillOnce(
            Invoke([return_value](int /* delay_id */, int /* suspend_id */,
                                  base::OnceCallback<void(bool)> callback) {
              std::move(callback).Run(return_value);
            }));
  }

  void AddProxyExpectationForRecordDarkResumeWakeReason(
      const std::string& wake_reason, bool return_value) {
    EXPECT_CALL(*power_manager_proxy_, RecordDarkResumeWakeReason(wake_reason))
        .WillOnce(Return(return_value));
  }

  void AddProxyExpectationForRegisterDarkSuspendDelay(
      std::optional<int> delay_id) {
    EXPECT_CALL(*power_manager_proxy_,
                RegisterDarkSuspendDelay(kTimeout, kDarkDescription, _))
        .WillOnce(Invoke(
            [delay_id](base::TimeDelta /* timeout */,
                       const std::string& /* description */,
                       base::OnceCallback<void(std::optional<int>)> callback) {
              std::move(callback).Run(delay_id);
            }));
  }

  void AddProxyExpectationForReportDarkSuspendReadiness(int delay_id,
                                                        int suspend_id,
                                                        bool return_value) {
    EXPECT_CALL(*power_manager_proxy_,
                ReportDarkSuspendReadiness(delay_id, suspend_id, _))
        .WillOnce(
            Invoke([return_value](int /* delay_id */, int /* suspend_id */,
                                  base::OnceCallback<void(bool)> callback) {
              std::move(callback).Run(return_value);
            }));
  }

  void AddProxyExpectationForUnregisterDarkSuspendDelay(int delay_id,
                                                        bool return_value) {
    EXPECT_CALL(*power_manager_proxy_, UnregisterDarkSuspendDelay(delay_id))
        .WillOnce(Return(return_value));
  }

  void AddProxyExpectationForChangeRegDomain(
      power_manager::WifiRegDomainDbus domain, bool return_value) {
    EXPECT_CALL(*power_manager_proxy_, ChangeRegDomain(domain));
  }

  void RegisterSuspendDelays() {
    AddProxyExpectationForRegisterSuspendDelay(kDelayId);
    AddProxyExpectationForRegisterDarkSuspendDelay(kDelayId);
    OnPowerManagerAppeared();
    Mock::VerifyAndClearExpectations(power_manager_proxy_);
  }

  bool ReportSuspendReadiness() {
    base::test::TestFuture<bool> future;
    power_manager_.ReportSuspendReadiness(future.GetCallback());
    return future.Get();
  }

  bool ReportDarkSuspendReadiness() {
    base::test::TestFuture<bool> future;
    power_manager_.ReportDarkSuspendReadiness(future.GetCallback());
    return future.Get();
  }

  void OnSuspendImminent(int suspend_id) {
    control_.delegate()->OnSuspendImminent(suspend_id);
    EXPECT_TRUE(power_manager_.suspending());
  }

  void OnSuspendDone(int suspend_id, int64_t suspend_duration_us) {
    control_.delegate()->OnSuspendDone(suspend_id, suspend_duration_us);
  }

  void OnDarkSuspendImminent(int suspend_id) {
    control_.delegate()->OnDarkSuspendImminent(suspend_id);
  }

  void OnPowerManagerAppeared() { power_manager_.OnPowerManagerAppeared(); }

  void OnPowerManagerVanished() { power_manager_.OnPowerManagerVanished(); }

  // This is non-static since it's a non-POD type.
  const base::TimeDelta kTimeout;

  FakeControl control_;
  PowerManager power_manager_;
  MockPowerManagerProxy* const power_manager_proxy_;
  PowerManagerProxyDelegate* const delegate_;
};

const char PowerManagerTest::kDescription[] = "shill";
const char PowerManagerTest::kDarkDescription[] = "shill";
const char PowerManagerTest::kPowerManagerDefaultOwner[] =
    "PowerManagerDefaultOwner";

TEST_F(PowerManagerTest, SuspendingState) {
  RegisterSuspendDelays();
  EXPECT_FALSE(power_manager_.suspending());
  OnSuspendImminent(kSuspendId1);
  EXPECT_TRUE(power_manager_.suspending());
  EXPECT_EQ(0, power_manager_.suspend_duration_us());
  AddProxyExpectationForReportSuspendReadiness(kDelayId, kSuspendId1, true);
  EXPECT_TRUE(ReportSuspendReadiness());
  OnSuspendDone(kSuspendId1, kSuspendDurationUsecs);
  EXPECT_FALSE(power_manager_.suspending());
  EXPECT_TRUE(power_manager_.suspend_duration_us() == kSuspendDurationUsecs);
}

TEST_F(PowerManagerTest, SuspendDoneBeforeReady) {
  RegisterSuspendDelays();

  EXPECT_FALSE(power_manager_.suspending());
  EXPECT_CALL(*this, SuspendImminentAction()).Times(1);
  EXPECT_CALL(*this, SuspendDoneAction()).Times(0);
  OnSuspendImminent(kSuspendId1);
  Mock::VerifyAndClearExpectations(this);
  EXPECT_TRUE(power_manager_.suspending());

  // If SuspendDone is received before SuspendReadiness is reported,
  // SuspendDoneAction should be deferred.
  EXPECT_CALL(*this, SuspendDoneAction()).Times(0);
  OnSuspendDone(kSuspendId1, kSuspendDurationUsecs);
  Mock::VerifyAndClearExpectations(this);
  EXPECT_TRUE(power_manager_.suspending());

  // When it's about to report readiness to suspend, the deferred
  // SuspendDoneAction should be taken and ReportSuspendReadiness should be
  // skipped.
  EXPECT_CALL(*this, SuspendDoneAction()).Times(1);
  EXPECT_CALL(*power_manager_proxy_, ReportSuspendReadiness(_, _, _)).Times(0);
  EXPECT_FALSE(ReportSuspendReadiness());
  Mock::VerifyAndClearExpectations(this);
  Mock::VerifyAndClearExpectations(power_manager_proxy_);
  EXPECT_FALSE(power_manager_.suspending());
}

TEST_F(PowerManagerTest, SuspendDoneThenImminentBeforeReady) {
  RegisterSuspendDelays();

  EXPECT_FALSE(power_manager_.suspending());
  EXPECT_CALL(*this, SuspendImminentAction()).Times(1);
  EXPECT_CALL(*this, SuspendDoneAction()).Times(0);
  OnSuspendImminent(kSuspendId1);
  Mock::VerifyAndClearExpectations(this);
  EXPECT_TRUE(power_manager_.suspending());

  // If SuspendDone is received before SuspendReadiness is reported,
  // SuspendDoneAction should be deferred.
  EXPECT_CALL(*this, SuspendDoneAction()).Times(0);
  OnSuspendDone(kSuspendId1, kSuspendDurationUsecs);
  Mock::VerifyAndClearExpectations(this);
  EXPECT_TRUE(power_manager_.suspending());

  // If another SuspendImminent is received before SuspendReadiness is reported,
  // SuspendImminentAction shouldn't be called again.
  EXPECT_CALL(*this, SuspendImminentAction()).Times(0);
  EXPECT_CALL(*this, SuspendDoneAction()).Times(0);
  OnSuspendImminent(kSuspendId2);
  Mock::VerifyAndClearExpectations(this);
  EXPECT_TRUE(power_manager_.suspending());

  // If SuspendDone for the second SuspendImminent is received after
  // SuspendReadiness is reported, SuspendDoneAction is taken after SuspendDone
  // is received.
  EXPECT_CALL(*this, SuspendDoneAction()).Times(0);
  AddProxyExpectationForReportSuspendReadiness(kDelayId, kSuspendId2, true);
  EXPECT_TRUE(ReportSuspendReadiness());
  Mock::VerifyAndClearExpectations(this);
  Mock::VerifyAndClearExpectations(power_manager_proxy_);
  EXPECT_TRUE(power_manager_.suspending());

  EXPECT_CALL(*this, SuspendDoneAction()).Times(1);
  OnSuspendDone(kSuspendId2, kSuspendDurationUsecs);
  Mock::VerifyAndClearExpectations(this);
  Mock::VerifyAndClearExpectations(power_manager_proxy_);
  EXPECT_FALSE(power_manager_.suspending());
}

TEST_F(PowerManagerTest, SuspendDoneThenImminentThenDoneBeforeReady) {
  RegisterSuspendDelays();

  EXPECT_FALSE(power_manager_.suspending());
  EXPECT_CALL(*this, SuspendImminentAction()).Times(1);
  EXPECT_CALL(*this, SuspendDoneAction()).Times(0);
  OnSuspendImminent(kSuspendId1);
  Mock::VerifyAndClearExpectations(this);
  EXPECT_TRUE(power_manager_.suspending());

  // If SuspendDone is received before SuspendReadiness is reported,
  // SuspendDoneAction should be deferred.
  EXPECT_CALL(*this, SuspendDoneAction()).Times(0);
  OnSuspendDone(kSuspendId1, kSuspendDurationUsecs);
  Mock::VerifyAndClearExpectations(this);
  EXPECT_TRUE(power_manager_.suspending());

  // If another SuspendImminent is received before SuspendReadiness is
  // reported, SuspendImminentAction shouldn't be called again.
  EXPECT_CALL(*this, SuspendImminentAction()).Times(0);
  EXPECT_CALL(*this, SuspendDoneAction()).Times(0);
  OnSuspendImminent(kSuspendId2);
  Mock::VerifyAndClearExpectations(this);
  EXPECT_TRUE(power_manager_.suspending());

  // If SuspendDone for the second SuspendImminent is received before
  // SuspendReadiness is reported, SuspendDoneAction should be deferred.
  EXPECT_CALL(*this, SuspendDoneAction()).Times(0);
  OnSuspendDone(kSuspendId2, kSuspendDurationUsecs);
  Mock::VerifyAndClearExpectations(this);
  EXPECT_TRUE(power_manager_.suspending());

  // When it's about to report readiness to suspend, the deferred
  // SuspendDoneAction should be taken and ReportSuspendReadiness should be
  // skipped.
  EXPECT_CALL(*this, SuspendDoneAction()).Times(1);
  EXPECT_CALL(*power_manager_proxy_, ReportSuspendReadiness(_, _, _)).Times(0);
  EXPECT_FALSE(ReportSuspendReadiness());
  Mock::VerifyAndClearExpectations(this);
  Mock::VerifyAndClearExpectations(power_manager_proxy_);
  EXPECT_FALSE(power_manager_.suspending());
}

TEST_F(PowerManagerTest, RegisterSuspendDelayFailure) {
  AddProxyExpectationForRegisterSuspendDelay(std::nullopt);
  OnPowerManagerAppeared();
  Mock::VerifyAndClearExpectations(power_manager_proxy_);

  // Outstanding shill callbacks should still be invoked.
  // - suspend_done_callback: If powerd died in the middle of a suspend
  //   we want to wake shill up with suspend_done_action, so this callback
  //   should be invoked anyway.
  //   See PowerManagerTest::PowerManagerDiedInSuspend and
  //   PowerManagerTest::PowerManagerReappearedInSuspend.
  EXPECT_CALL(*this, SuspendDoneAction());
  // - suspend_imminent_callback: The only case this can happen is if this
  //   callback was put on the queue, and then powerd reappeared, but we failed
  //   to registered a suspend delay with it.
  //   It is safe to go through the suspend_imminent -> timeout -> suspend_done
  //   path in this black swan case.
  EXPECT_CALL(*this, SuspendImminentAction());
  OnSuspendImminent(kSuspendId1);
  EXPECT_FALSE(ReportSuspendReadiness());
  OnSuspendDone(kSuspendId1, kSuspendDurationUsecs);
  EXPECT_FALSE(power_manager_.suspending());
}

TEST_F(PowerManagerTest, RegisterDarkSuspendDelayFailure) {
  AddProxyExpectationForRegisterDarkSuspendDelay(std::nullopt);
  OnPowerManagerAppeared();
  Mock::VerifyAndClearExpectations(power_manager_proxy_);

  // Outstanding dark suspend imminent signal should be ignored, since we
  // probably won't have time to cleanly do dark resume actions. Might as well
  // ignore the signal.
  EXPECT_CALL(*this, DarkSuspendImminentAction()).Times(0);
  OnDarkSuspendImminent(kSuspendId1);
}

TEST_F(PowerManagerTest, OnPowerManagerAppearedCalledTwice) {
  EXPECT_CALL(*power_manager_proxy_,
              RegisterSuspendDelay(kTimeout, kDescription, _))
      .Times(1);
  EXPECT_CALL(*power_manager_proxy_,
              RegisterDarkSuspendDelay(kTimeout, kDarkDescription, _))
      .Times(1);
  OnPowerManagerAppeared();
  OnPowerManagerAppeared();
}

TEST_F(PowerManagerTest, ReportSuspendReadinessFailure) {
  RegisterSuspendDelays();
  EXPECT_CALL(*this, SuspendImminentAction());
  OnSuspendImminent(kSuspendId1);
  AddProxyExpectationForReportSuspendReadiness(kDelayId, kSuspendId1, false);
  EXPECT_FALSE(ReportSuspendReadiness());
}

TEST_F(PowerManagerTest, RecordDarkResumeWakeReasonFailure) {
  const std::string kWakeReason = "WiFi.Disconnect";
  RegisterSuspendDelays();
  EXPECT_CALL(*this, DarkSuspendImminentAction());
  OnDarkSuspendImminent(kSuspendId1);
  AddProxyExpectationForRecordDarkResumeWakeReason(kWakeReason, false);
  EXPECT_FALSE(power_manager_.RecordDarkResumeWakeReason(kWakeReason));
}

TEST_F(PowerManagerTest, RecordDarkResumeWakeReasonSuccess) {
  const std::string kWakeReason = "WiFi.Disconnect";
  RegisterSuspendDelays();
  EXPECT_CALL(*this, DarkSuspendImminentAction());
  OnDarkSuspendImminent(kSuspendId1);
  AddProxyExpectationForRecordDarkResumeWakeReason(kWakeReason, true);
  EXPECT_TRUE(power_manager_.RecordDarkResumeWakeReason(kWakeReason));
}

TEST_F(PowerManagerTest, ReportDarkSuspendReadinessFailure) {
  RegisterSuspendDelays();
  EXPECT_CALL(*this, DarkSuspendImminentAction());
  OnDarkSuspendImminent(kSuspendId1);
  AddProxyExpectationForReportDarkSuspendReadiness(kDelayId, kSuspendId1,
                                                   false);
  EXPECT_FALSE(ReportDarkSuspendReadiness());
}

TEST_F(PowerManagerTest, ReportSuspendReadinessFailsOutsideSuspend) {
  RegisterSuspendDelays();
  EXPECT_CALL(*power_manager_proxy_, ReportSuspendReadiness(_, _, _)).Times(0);
  EXPECT_FALSE(ReportSuspendReadiness());
}

TEST_F(PowerManagerTest, ReportSuspendReadinessSynchronous) {
  // Verifies that a synchronous ReportSuspendReadiness call by shill on a
  // SuspendImminent callback is routed back to powerd.
  RegisterSuspendDelays();
  AddProxyExpectationForReportSuspendReadiness(kDelayId, kSuspendId1, true);
  base::test::TestFuture<bool> future;
  EXPECT_CALL(*this, SuspendImminentAction()).WillOnce(Invoke([this, &future] {
    power_manager_.ReportSuspendReadiness(future.GetCallback());
  }));
  OnSuspendImminent(kSuspendId1);
  EXPECT_TRUE(future.Get());
}

TEST_F(PowerManagerTest, ReportDarkSuspendReadinessSynchronous) {
  // Verifies that a synchronous ReportDarkSuspendReadiness call by shill on a
  // DarkSuspendImminent callback is routed back to powerd.
  RegisterSuspendDelays();
  AddProxyExpectationForReportDarkSuspendReadiness(kDelayId, kSuspendId1, true);
  base::test::TestFuture<bool> future;
  EXPECT_CALL(*this, DarkSuspendImminentAction())
      .WillOnce(Invoke([this, &future] {
        power_manager_.ReportDarkSuspendReadiness(future.GetCallback());
      }));
  OnDarkSuspendImminent(kSuspendId1);
  EXPECT_TRUE(future.Get());
}

TEST_F(PowerManagerTest, Stop) {
  RegisterSuspendDelays();
  AddProxyExpectationForUnregisterSuspendDelay(kDelayId, true);
  AddProxyExpectationForUnregisterDarkSuspendDelay(kDelayId, true);
  power_manager_.Stop();
}

TEST_F(PowerManagerTest, StopFailure) {
  RegisterSuspendDelays();

  AddProxyExpectationForUnregisterSuspendDelay(kDelayId, false);
  power_manager_.Stop();
  Mock::VerifyAndClearExpectations(power_manager_proxy_);

  // PowerManager::Stop() nullifies |PowerManager::power_manager_proxy_|, so no
  // further SuspendImminent or SuspendDone notification is expected.
}

TEST_F(PowerManagerTest, OnPowerManagerReappeared) {
  RegisterSuspendDelays();

  // Check that we re-register suspend delay on powerd restart.
  AddProxyExpectationForRegisterSuspendDelay(kDelayId2);
  AddProxyExpectationForRegisterDarkSuspendDelay(kDelayId2);
  // Check that we resend current reg domain on powerd restart.
  power_manager_.ChangeRegDomain(NL80211_DFS_FCC);
  AddProxyExpectationForChangeRegDomain(power_manager::WIFI_REG_DOMAIN_FCC,
                                        true);
  OnPowerManagerVanished();
  OnPowerManagerAppeared();
  Mock::VerifyAndClearExpectations(power_manager_proxy_);

  // Check that a |ReportSuspendReadiness| message is sent with the new delay
  // id.
  EXPECT_CALL(*this, SuspendImminentAction());
  OnSuspendImminent(kSuspendId1);
  AddProxyExpectationForReportSuspendReadiness(kDelayId2, kSuspendId1, true);
  EXPECT_TRUE(ReportSuspendReadiness());
  Mock::VerifyAndClearExpectations(power_manager_proxy_);

  // Check that a |ReportDarkSuspendReadiness| message is sent with the new
  // delay id.
  EXPECT_CALL(*this, DarkSuspendImminentAction());
  OnDarkSuspendImminent(kSuspendId1);
  AddProxyExpectationForReportDarkSuspendReadiness(kDelayId2, kSuspendId1,
                                                   true);
  EXPECT_TRUE(ReportDarkSuspendReadiness());
}

TEST_F(PowerManagerTest, PowerManagerDiedInSuspend) {
  RegisterSuspendDelays();
  EXPECT_CALL(*this, SuspendImminentAction());
  OnSuspendImminent(kSuspendId1);
  Mock::VerifyAndClearExpectations(this);

  EXPECT_CALL(*this, SuspendDoneAction());
  OnPowerManagerVanished();
  EXPECT_FALSE(power_manager_.suspending());
}

TEST_F(PowerManagerTest, PowerManagerReappearedInSuspend) {
  RegisterSuspendDelays();
  EXPECT_CALL(*this, SuspendImminentAction());
  OnSuspendImminent(kSuspendId1);
  Mock::VerifyAndClearExpectations(this);

  AddProxyExpectationForRegisterSuspendDelay(kDelayId2);
  AddProxyExpectationForRegisterDarkSuspendDelay(kDelayId2);
  EXPECT_CALL(*this, SuspendDoneAction());
  OnPowerManagerVanished();
  OnPowerManagerAppeared();
  EXPECT_FALSE(power_manager_.suspending());
  Mock::VerifyAndClearExpectations(this);

  // Let's check a normal suspend request after the fact.
  EXPECT_CALL(*this, SuspendImminentAction());
  OnSuspendImminent(kSuspendId2);
}

TEST_F(PowerManagerTest, OnChangeRegDomain) {
  // Revert to default reg domain for this test.
  power_manager_.ChangeRegDomain(NL80211_DFS_UNSET);
  // Multiple calls to ChangeRegDomain with the same dfs region should only
  // trigger a single proxy call.
  AddProxyExpectationForChangeRegDomain(power_manager::WIFI_REG_DOMAIN_FCC,
                                        true);
  power_manager_.ChangeRegDomain(NL80211_DFS_FCC);
  power_manager_.ChangeRegDomain(NL80211_DFS_FCC);

  AddProxyExpectationForChangeRegDomain(power_manager::WIFI_REG_DOMAIN_EU,
                                        true);
  power_manager_.ChangeRegDomain(NL80211_DFS_ETSI);
  power_manager_.ChangeRegDomain(NL80211_DFS_ETSI);

  AddProxyExpectationForChangeRegDomain(
      power_manager::WIFI_REG_DOMAIN_REST_OF_WORLD, true);
  power_manager_.ChangeRegDomain(NL80211_DFS_JP);
  power_manager_.ChangeRegDomain(NL80211_DFS_JP);

  AddProxyExpectationForChangeRegDomain(power_manager::WIFI_REG_DOMAIN_NONE,
                                        true);
  power_manager_.ChangeRegDomain(NL80211_DFS_UNSET);
  power_manager_.ChangeRegDomain(NL80211_DFS_UNSET);
}

TEST_F(PowerManagerTest, ChangeRegDomainAfterStop) {
  // This shouldn't crash the process.
  power_manager_.Stop();
  power_manager_.ChangeRegDomain(NL80211_DFS_FCC);
}

}  // namespace shill
