// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "missive/util/disconnectable_client.h"

#include <memory>
#include <utility>

#include <base/test/task_environment.h>
#include <base/threading/sequenced_task_runner_handle.h>
#include <base/time/time.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "missive/util/status.h"
#include "missive/util/statusor.h"
#include "missive/util/test_support_callbacks.h"

using ::testing::Eq;

namespace reporting {

class MockDelegate : public DisconnectableClient::Delegate {
 public:
  MockDelegate(int64_t input,
               base::TimeDelta delay,
               base::OnceCallback<void(StatusOr<int64_t>)> completion_cb)
      : input_(input),
        delay_(delay),
        completion_cb_(std::move(completion_cb)) {}
  MockDelegate(const MockDelegate& other) = delete;
  MockDelegate& operator=(const MockDelegate& other) = delete;
  ~MockDelegate() override = default;

  void DoCall(base::OnceClosure cb) override {
    base::SequencedTaskRunnerHandle::Get()->PostDelayedTask(
        FROM_HERE, std::move(cb), delay_);
  }

  void Respond(Status status) override {
    DCHECK(completion_cb_);
    if (!status.ok()) {
      std::move(completion_cb_).Run(status);
      return;
    }
    std::move(completion_cb_).Run(input_ * 2);
  }

 private:
  const int64_t input_;
  const base::TimeDelta delay_;
  base::OnceCallback<void(StatusOr<int64_t>)> completion_cb_;
};

class FailDelegate : public DisconnectableClient::Delegate {
 public:
  FailDelegate(base::TimeDelta delay,
               base::OnceCallback<void(StatusOr<int64_t>)> completion_cb)
      : delay_(delay), completion_cb_(std::move(completion_cb)) {}
  FailDelegate(const FailDelegate& other) = delete;
  FailDelegate& operator=(const FailDelegate& other) = delete;
  ~FailDelegate() override = default;

  void DoCall(base::OnceClosure cb) override {
    base::SequencedTaskRunnerHandle::Get()->PostDelayedTask(
        FROM_HERE, std::move(cb), delay_);
  }

  void Respond(Status status) override {
    DCHECK(completion_cb_);
    if (!status.ok()) {
      std::move(completion_cb_).Run(status);
      return;
    }
    std::move(completion_cb_).Run(Status(error::CANCELLED, "Failed in test"));
  }

 private:
  const base::TimeDelta delay_;
  base::OnceCallback<void(StatusOr<int64_t>)> completion_cb_;
};

class DisconnectableClientTest : public ::testing::Test {
 protected:
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};

  DisconnectableClient client_{base::SequencedTaskRunnerHandle::Get()};
};

TEST_F(DisconnectableClientTest, NormalConnection) {
  client_.SetAvailability(/*is_available=*/true);

  test::TestEvent<StatusOr<int64_t>> res1;
  test::TestEvent<StatusOr<int64_t>> res2;
  client_.MaybeMakeCall(
      std::make_unique<MockDelegate>(111, base::TimeDelta(), res1.cb()));
  client_.MaybeMakeCall(
      std::make_unique<MockDelegate>(222, base::TimeDelta(), res2.cb()));

  auto result = res1.result();
  ASSERT_OK(result) << result.status();
  EXPECT_THAT(result.ValueOrDie(), Eq(222));
  result = res2.result();
  ASSERT_OK(result) << result.status();
  EXPECT_THAT(result.ValueOrDie(), Eq(444));
}

TEST_F(DisconnectableClientTest, NoConnection) {
  test::TestEvent<StatusOr<int64_t>> res;
  client_.MaybeMakeCall(
      std::make_unique<MockDelegate>(111, base::TimeDelta(), res.cb()));

  auto result = res.result();
  ASSERT_FALSE(result.ok());
  ASSERT_THAT(result.status().error_code(), Eq(error::UNAVAILABLE))
      << result.status();
}

TEST_F(DisconnectableClientTest, FailedCallOnNormalConnection) {
  client_.SetAvailability(/*is_available=*/true);

  test::TestEvent<StatusOr<int64_t>> res1;
  test::TestEvent<StatusOr<int64_t>> res2;
  test::TestEvent<StatusOr<int64_t>> res3;
  client_.MaybeMakeCall(std::make_unique<MockDelegate>(
      111, base::TimeDelta::FromSeconds(1), res1.cb()));
  client_.MaybeMakeCall(std::make_unique<FailDelegate>(
      base::TimeDelta::FromSeconds(2), res2.cb()));
  client_.MaybeMakeCall(std::make_unique<MockDelegate>(
      222, base::TimeDelta::FromSeconds(3), res3.cb()));

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(1));

  auto result = res1.result();
  ASSERT_OK(result) << result.status();
  EXPECT_THAT(result.ValueOrDie(), Eq(222));

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(1));

  result = res2.result();
  ASSERT_FALSE(result.ok());
  ASSERT_THAT(result.status().error_code(), Eq(error::CANCELLED))
      << result.status();

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(1));

  result = res3.result();
  ASSERT_OK(result) << result.status();
  EXPECT_THAT(result.ValueOrDie(), Eq(444));
}

TEST_F(DisconnectableClientTest, DroppedConnection) {
  client_.SetAvailability(/*is_available=*/true);

  test::TestEvent<StatusOr<int64_t>> res1;
  test::TestEvent<StatusOr<int64_t>> res2;
  client_.MaybeMakeCall(std::make_unique<MockDelegate>(
      111, base::TimeDelta::FromSeconds(1), res1.cb()));
  client_.MaybeMakeCall(std::make_unique<MockDelegate>(
      222, base::TimeDelta::FromSeconds(2), res2.cb()));

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(1));

  auto result = res1.result();
  ASSERT_OK(result) << result.status();
  EXPECT_THAT(result.ValueOrDie(), Eq(222));

  client_.SetAvailability(/*is_available=*/false);

  result = res2.result();
  ASSERT_FALSE(result.ok());
  ASSERT_THAT(result.status().error_code(), Eq(error::UNAVAILABLE))
      << result.status();
}

TEST_F(DisconnectableClientTest, FailedCallOnDroppedConnection) {
  client_.SetAvailability(/*is_available=*/true);

  test::TestEvent<StatusOr<int64_t>> res1;
  test::TestEvent<StatusOr<int64_t>> res2;
  test::TestEvent<StatusOr<int64_t>> res3;
  client_.MaybeMakeCall(std::make_unique<MockDelegate>(
      111, base::TimeDelta::FromSeconds(1), res1.cb()));
  client_.MaybeMakeCall(std::make_unique<FailDelegate>(
      base::TimeDelta::FromSeconds(2), res2.cb()));
  client_.MaybeMakeCall(std::make_unique<MockDelegate>(
      222, base::TimeDelta::FromSeconds(3), res3.cb()));

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(1));

  auto result = res1.result();
  ASSERT_OK(result) << result.status();
  EXPECT_THAT(result.ValueOrDie(), Eq(222));

  client_.SetAvailability(/*is_available=*/false);

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(1));

  result = res2.result();
  ASSERT_FALSE(result.ok());
  ASSERT_THAT(result.status().error_code(), Eq(error::UNAVAILABLE))
      << result.status();

  result = res3.result();
  ASSERT_FALSE(result.ok());
  ASSERT_THAT(result.status().error_code(), Eq(error::UNAVAILABLE))
      << result.status();
}

TEST_F(DisconnectableClientTest, ConnectionDroppedThenRestored) {
  client_.SetAvailability(/*is_available=*/true);

  test::TestEvent<StatusOr<int64_t>> res1;
  test::TestEvent<StatusOr<int64_t>> res2;
  test::TestEvent<StatusOr<int64_t>> res3;
  client_.MaybeMakeCall(std::make_unique<MockDelegate>(
      111, base::TimeDelta::FromSeconds(1), res1.cb()));
  client_.MaybeMakeCall(std::make_unique<MockDelegate>(
      222, base::TimeDelta::FromSeconds(2), res2.cb()));

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(1));

  auto result = res1.result();
  ASSERT_OK(result) << result.status();
  EXPECT_THAT(result.ValueOrDie(), Eq(222));

  client_.SetAvailability(/*is_available=*/false);

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(1));

  result = res2.result();
  ASSERT_FALSE(result.ok());
  ASSERT_THAT(result.status().error_code(), Eq(error::UNAVAILABLE))
      << result.status();

  client_.SetAvailability(/*is_available=*/true);

  client_.MaybeMakeCall(std::make_unique<MockDelegate>(
      333, base::TimeDelta::FromSeconds(1), res3.cb()));

  task_environment_.FastForwardBy(base::TimeDelta::FromSeconds(1));

  result = res3.result();
  ASSERT_OK(result) << result.status();
  EXPECT_THAT(result.ValueOrDie(), Eq(666));
}

}  // namespace reporting
