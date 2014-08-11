// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unistd.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <base/bind.h>
#include <base/memory/scoped_ptr.h>
#include <base/time/time.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "update_engine/fake_clock.h"
#include "update_engine/test_utils.h"
#include "update_engine/update_manager/default_policy.h"
#include "update_engine/update_manager/fake_state.h"
#include "update_engine/update_manager/mock_policy.h"
#include "update_engine/update_manager/umtest_utils.h"
#include "update_engine/update_manager/update_manager.h"

using base::Bind;
using base::Callback;
using base::Time;
using base::TimeDelta;
using chromeos_update_engine::ErrorCode;
using chromeos_update_engine::FakeClock;
using std::pair;
using std::string;
using std::vector;
using testing::Return;
using testing::StrictMock;
using testing::_;

namespace {

// Generates a fixed timestamp for use in faking the current time.
Time FixedTime() {
  Time::Exploded now_exp;
  now_exp.year = 2014;
  now_exp.month = 3;
  now_exp.day_of_week = 2;
  now_exp.day_of_month = 18;
  now_exp.hour = 8;
  now_exp.minute = 5;
  now_exp.second = 33;
  now_exp.millisecond = 675;
  return Time::FromLocalExploded(now_exp);
}

}  // namespace

namespace chromeos_update_manager {

class UmUpdateManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fake_state_ = new FakeState();
    umut_.reset(new UpdateManager(&fake_clock_, TimeDelta::FromSeconds(5),
                                  TimeDelta::FromSeconds(1), fake_state_));
  }

  FakeState* fake_state_;  // Owned by the umut_.
  FakeClock fake_clock_;
  scoped_ptr<UpdateManager> umut_;
};

// The FailingPolicy implements a single method and make it always fail. This
// class extends the DefaultPolicy class to allow extensions of the Policy
// class without extending nor changing this test.
class FailingPolicy : public DefaultPolicy {
 public:
  explicit FailingPolicy(int* num_called_p) : num_called_p_(num_called_p) {}
  FailingPolicy() : FailingPolicy(nullptr) {}
  EvalStatus UpdateCheckAllowed(EvaluationContext* ec, State* state,
                                string* error,
                                UpdateCheckParams* result) const override {
    if (num_called_p_)
      (*num_called_p_)++;
    *error = "FailingPolicy failed.";
    return EvalStatus::kFailed;
  }

 protected:
  std::string PolicyName() const override { return "FailingPolicy"; }

 private:
  int* num_called_p_;
};

// The LazyPolicy always returns EvalStatus::kAskMeAgainLater.
class LazyPolicy : public DefaultPolicy {
  EvalStatus UpdateCheckAllowed(EvaluationContext* ec, State* state,
                                string* error,
                                UpdateCheckParams* result) const override {
    return EvalStatus::kAskMeAgainLater;
  }

 protected:
  std::string PolicyName() const override { return "LazyPolicy"; }
};

// A policy that sleeps for a predetermined amount of time, then checks for a
// wallclock-based time threshold (if given) and returns
// EvalStatus::kAskMeAgainLater if not passed; otherwise, returns
// EvalStatus::kSucceeded. Increments a counter every time it is being queried,
// if a pointer to it is provided.
class DelayPolicy : public DefaultPolicy {
 public:
  DelayPolicy(int sleep_secs, Time time_threshold, int* num_called_p)
      : sleep_secs_(sleep_secs), time_threshold_(time_threshold),
        num_called_p_(num_called_p) {}
  EvalStatus UpdateCheckAllowed(EvaluationContext* ec, State* state,
                                string* error,
                                UpdateCheckParams* result) const override {
    if (num_called_p_)
      (*num_called_p_)++;

    // Sleep for a predetermined amount of time.
    if (sleep_secs_ > 0)
      sleep(sleep_secs_);

    // Check for a time threshold. This can be used to ensure that the policy
    // has some non-constant dependency.
    if (time_threshold_ < Time::Max() &&
        ec->IsWallclockTimeGreaterThan(time_threshold_))
      return EvalStatus::kSucceeded;

    return EvalStatus::kAskMeAgainLater;
  }

 protected:
  std::string PolicyName() const override { return "DelayPolicy"; }

 private:
  int sleep_secs_;
  Time time_threshold_;
  int* num_called_p_;
};

// AccumulateCallsCallback() adds to the passed |acc| accumulator vector pairs
// of EvalStatus and T instances. This allows to create a callback that keeps
// track of when it is called and the arguments passed to it, to be used with
// the UpdateManager::AsyncPolicyRequest().
template<typename T>
static void AccumulateCallsCallback(vector<pair<EvalStatus, T>>* acc,
                                    EvalStatus status, const T& result) {
  acc->push_back(std::make_pair(status, result));
}

// Tests that policy requests are completed successfully. It is important that
// this tests cover all policy requests as defined in Policy.
TEST_F(UmUpdateManagerTest, PolicyRequestCallUpdateCheckAllowed) {
  UpdateCheckParams result;
  EXPECT_EQ(EvalStatus::kSucceeded, umut_->PolicyRequest(
      &Policy::UpdateCheckAllowed, &result));
}

TEST_F(UmUpdateManagerTest, PolicyRequestCallUpdateCanStart) {
  const UpdateState update_state = {
    FixedTime(), 1,
    vector<string>(1, "http://fake/url/"), 10, 0, 0, vector<ErrorCode>(),
    TimeDelta::FromSeconds(15), TimeDelta::FromSeconds(60),
    4, 2, 8
  };
  UpdateDownloadParams result;
  EXPECT_EQ(EvalStatus::kSucceeded,
            umut_->PolicyRequest(&Policy::UpdateCanStart, &result, true,
                                 update_state));
}

TEST_F(UmUpdateManagerTest, PolicyRequestCallsDefaultOnError) {
  umut_->set_policy(new FailingPolicy());

  // Tests that the DefaultPolicy instance is called when the method fails,
  // which will set this as true.
  UpdateCheckParams result;
  result.updates_enabled = false;
  EvalStatus status = umut_->PolicyRequest(
      &Policy::UpdateCheckAllowed, &result);
  EXPECT_EQ(EvalStatus::kSucceeded, status);
  EXPECT_TRUE(result.updates_enabled);
}

// This test only applies to debug builds where DCHECK is enabled.
#if DCHECK_IS_ON
TEST_F(UmUpdateManagerTest, PolicyRequestDoesntBlockDeathTest) {
  // The update manager should die (DCHECK) if a policy called synchronously
  // returns a kAskMeAgainLater value.
  UpdateCheckParams result;
  umut_->set_policy(new LazyPolicy());
  EXPECT_DEATH(umut_->PolicyRequest(&Policy::UpdateCheckAllowed, &result), "");
}
#endif  // DCHECK_IS_ON

TEST_F(UmUpdateManagerTest, AsyncPolicyRequestDelaysEvaluation) {
  // To avoid differences in code execution order between an AsyncPolicyRequest
  // call on a policy that returns AskMeAgainLater the first time and one that
  // succeeds the first time, we ensure that the passed callback is called from
  // the main loop in both cases even when we could evaluate it right now.
  umut_->set_policy(new FailingPolicy());

  vector<pair<EvalStatus, UpdateCheckParams>> calls;
  Callback<void(EvalStatus, const UpdateCheckParams&)> callback = Bind(
      AccumulateCallsCallback<UpdateCheckParams>, &calls);

  umut_->AsyncPolicyRequest(callback, &Policy::UpdateCheckAllowed);
  // The callback should wait until we run the main loop for it to be executed.
  EXPECT_EQ(0, calls.size());
  chromeos_update_engine::RunGMainLoopMaxIterations(100);
  EXPECT_EQ(1, calls.size());
}

TEST_F(UmUpdateManagerTest, AsyncPolicyRequestTimeoutDoesNotFire) {
  // Set up an async policy call to return immediately, then wait a little and
  // ensure that the timeout event does not fire.
  int num_called = 0;
  umut_->set_policy(new FailingPolicy(&num_called));

  vector<pair<EvalStatus, UpdateCheckParams>> calls;
  Callback<void(EvalStatus, const UpdateCheckParams&)> callback =
      Bind(AccumulateCallsCallback<UpdateCheckParams>, &calls);

  umut_->AsyncPolicyRequest(callback, &Policy::UpdateCheckAllowed);
  // Run the main loop, ensure that policy was attempted once before deferring
  // to the default.
  chromeos_update_engine::RunGMainLoopMaxIterations(100);
  EXPECT_EQ(1, num_called);
  ASSERT_EQ(1, calls.size());
  EXPECT_EQ(EvalStatus::kSucceeded, calls[0].first);
  // Wait for the timeout to expire, run the main loop again, ensure that
  // nothing happened.
  sleep(2);
  chromeos_update_engine::RunGMainLoopMaxIterations(10);
  EXPECT_EQ(1, num_called);
  EXPECT_EQ(1, calls.size());
}

TEST_F(UmUpdateManagerTest, AsyncPolicyRequestTimesOut) {
  // Set up an async policy call to exceed its expiration timeout, make sure
  // that the default policy was not used (no callback) and that evaluation is
  // reattempted.
  int num_called = 0;
  umut_->set_policy(new DelayPolicy(
          0, fake_clock_.GetWallclockTime() + TimeDelta::FromSeconds(3),
          &num_called));

  vector<pair<EvalStatus, UpdateCheckParams>> calls;
  Callback<void(EvalStatus, const UpdateCheckParams&)> callback =
      Bind(AccumulateCallsCallback<UpdateCheckParams>, &calls);

  umut_->AsyncPolicyRequest(callback, &Policy::UpdateCheckAllowed);
  // Run the main loop, ensure that policy was attempted once but the callback
  // was not invoked.
  chromeos_update_engine::RunGMainLoopMaxIterations(100);
  EXPECT_EQ(1, num_called);
  EXPECT_EQ(0, calls.size());
  // Wait for the expiration timeout to expire, run the main loop again,
  // ensure that reevaluation occurred but callback was not invoked (i.e.
  // default policy was not consulted).
  sleep(2);
  fake_clock_.SetWallclockTime(fake_clock_.GetWallclockTime() +
                               TimeDelta::FromSeconds(2));
  chromeos_update_engine::RunGMainLoopMaxIterations(10);
  EXPECT_EQ(2, num_called);
  EXPECT_EQ(0, calls.size());
  // Wait for reevaluation due to delay to happen, ensure that it occurs and
  // that the callback is invoked.
  sleep(2);
  fake_clock_.SetWallclockTime(fake_clock_.GetWallclockTime() +
                               TimeDelta::FromSeconds(2));
  chromeos_update_engine::RunGMainLoopMaxIterations(10);
  EXPECT_EQ(3, num_called);
  ASSERT_EQ(1, calls.size());
  EXPECT_EQ(EvalStatus::kSucceeded, calls[0].first);
}

}  // namespace chromeos_update_manager
