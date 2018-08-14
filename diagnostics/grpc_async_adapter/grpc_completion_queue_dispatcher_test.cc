// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/grpc_async_adapter/grpc_completion_queue_dispatcher.h"

#include <list>
#include <memory>

#include <base/bind.h>
#include <base/callback.h>
#include <base/location.h>
#include <base/macros.h>
#include <base/message_loop/message_loop.h>
#include <base/run_loop.h>
#include <base/task_runner.h>
#include <base/time/time.h>
#include <gmock/gmock.h>
#include <grpc++/alarm.h>
#include <grpc++/grpc++.h>
#include <gtest/gtest.h>

namespace diagnostics {

namespace {

// Allows testing if a callback has been invoked, and the value of the
// grpc-specific |ok| bool parameter.
class TagAvailableCalledTester {
 public:
  TagAvailableCalledTester() = default;
  ~TagAvailableCalledTester() = default;

  GrpcCompletionQueueDispatcher::TagAvailableCallback
  GetTagAvailableCallback() {
    return base::Bind(&TagAvailableCalledTester::Callback,
                      base::Unretained(this));
  }

  // Bind this to a RegisterTag call of the
  // |GrpcCompletionQueueDispatcher|. Will check that it is invoked at
  // most once, remember the value of |ok|, and call the closure passed to
  // |CallWhenInvoked|, if any.
  void Callback(bool ok) {
    CHECK(!has_been_called_);
    has_been_called_ = true;
    value_of_ok_ = ok;

    std::list<base::Closure> callbacks_temp;
    callbacks_temp.swap(call_when_invoked_);
    for (auto& callback : callbacks_temp)
      callback.Run();
  }

  // Register |call_when_invoked| to be called when |Callback| is called.
  void CallWhenInvoked(base::Closure call_when_invoked) {
    call_when_invoked_.push_back(call_when_invoked);
  }

  // Returns true if |Callback| has been called.
  bool has_been_called() const { return has_been_called_; }

  // Only call if |has_been_called()| is returning true. Returns the value of
  // |ok| passed to |Callback|.
  bool value_of_ok() const {
    CHECK(has_been_called());
    return value_of_ok_;
  }

 private:
  bool has_been_called_ = false;
  bool value_of_ok_ = false;
  std::list<base::Closure> call_when_invoked_;

  DISALLOW_COPY_AND_ASSIGN(TagAvailableCalledTester);
};

// Allows testing if an object (owned by callback) has been destroyed. Also
// tests that this is destroyed on the same message loop it has been
// instantiated on.
class ObjectDestroyedTester {
 public:
  // Will set |*has_been_destroyed| to true when this instance is being
  // destroyed.
  explicit ObjectDestroyedTester(bool* has_been_destroyed)
      : expected_message_loop_(base::MessageLoop::current()),
        has_been_destroyed_(has_been_destroyed) {
    *has_been_destroyed_ = false;
  }

  ~ObjectDestroyedTester() {
    EXPECT_TRUE(
        expected_message_loop_->task_runner()->RunsTasksOnCurrentThread());
    *has_been_destroyed_ = true;
  }

 private:
  base::MessageLoop* const expected_message_loop_;
  bool* const has_been_destroyed_;

  DISALLOW_COPY_AND_ASSIGN(ObjectDestroyedTester);
};

// An adapter to be able to give a Callback to
// |GrpcCompletionQueueDispatcher::RegisterTag| which owns an
// |ObjectDestroyedTester|.
void ObjectDestroyedTesterAdapter(
    TagAvailableCalledTester* tag_available_called_tester,
    std::unique_ptr<ObjectDestroyedTester> object_destroyed_tester,
    bool ok) {
  tag_available_called_tester->Callback(ok);
}

gpr_timespec GprTimespecWithDeltaFromNow(base::TimeDelta delta) {
  return gpr_time_add(
      gpr_now(GPR_CLOCK_MONOTONIC),
      gpr_time_from_millis(delta.InMilliseconds(), GPR_TIMESPAN));
}

}  // namespace

class GrpcCompletionQueueDispatcherTest : public ::testing::Test {
 public:
  GrpcCompletionQueueDispatcherTest()
      : dispatcher_(&completion_queue_,
                    base::MessageLoop::current()->task_runner()) {
    dispatcher_.Start();
  }

  ~GrpcCompletionQueueDispatcherTest() override = default;

 protected:
  base::MessageLoopForIO message_loop_;
  grpc::CompletionQueue completion_queue_;

  // The dispatcher under test.
  GrpcCompletionQueueDispatcher dispatcher_;

  // Note: This can't be |const void*| because gRPC functions expect |void*|.
  void* const kTag = reinterpret_cast<void*>(1);

  void ShutdownDispatcher() {
    base::RunLoop run_loop;
    dispatcher_.Shutdown(run_loop.QuitClosure());
    run_loop.Run();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(GrpcCompletionQueueDispatcherTest);
};

// Start and shutdown a dispatcher, with no tags posted to the underlying
// CompletionQueue.
TEST_F(GrpcCompletionQueueDispatcherTest, StartAndShutdownEmpty) {
  ShutdownDispatcher();
}

// Register a tag that is not passed to the CompletionQueue. Check that the
// callback is never called, but that it is properly destroyed. This also
// demonstrates that instances passed to the callback using base::Passed are
// properly destroyed in this case.
TEST_F(GrpcCompletionQueueDispatcherTest, TagNeverAvailable) {
  bool object_has_been_destroyed = false;
  auto object_destroyed_tester =
      std::make_unique<ObjectDestroyedTester>(&object_has_been_destroyed);

  TagAvailableCalledTester tag_available_called_tester;
  dispatcher_.RegisterTag(
      nullptr,
      base::Bind(&ObjectDestroyedTesterAdapter, &tag_available_called_tester,
                 base::Passed(&object_destroyed_tester)));

  ShutdownDispatcher();

  EXPECT_FALSE(tag_available_called_tester.has_been_called());
  EXPECT_TRUE(object_has_been_destroyed);
}

// Register a tag that becomes available with |ok=true|. Verify that the
// registered callback is called with |ok=true|.
TEST_F(GrpcCompletionQueueDispatcherTest,
       CompletionQueueTagAvailableWithOkTrue) {
  base::RunLoop run_loop;
  TagAvailableCalledTester tag_available_called_tester;
  tag_available_called_tester.CallWhenInvoked(run_loop.QuitClosure());

  dispatcher_.RegisterTag(
      kTag, tag_available_called_tester.GetTagAvailableCallback());

  grpc::Alarm alarm(
      &completion_queue_,
      GprTimespecWithDeltaFromNow(base::TimeDelta::FromMilliseconds(1)), kTag);
  run_loop.Run();

  EXPECT_TRUE(tag_available_called_tester.has_been_called());
  EXPECT_TRUE(tag_available_called_tester.value_of_ok());

  ShutdownDispatcher();
}

// Register a tag that becomes available with |ok=false|. Verify that the
// regitered callback is called with |ok=false|.
TEST_F(GrpcCompletionQueueDispatcherTest,
       CompletionQueueTagAvailableWithOkFalse) {
  base::RunLoop run_loop;
  TagAvailableCalledTester tag_available_called_tester;
  tag_available_called_tester.CallWhenInvoked(run_loop.QuitClosure());

  dispatcher_.RegisterTag(
      kTag, tag_available_called_tester.GetTagAvailableCallback());

  grpc::Alarm alarm(&completion_queue_,
                    GprTimespecWithDeltaFromNow(base::TimeDelta::FromHours(24)),
                    kTag);
  alarm.Cancel();
  run_loop.Run();

  EXPECT_TRUE(tag_available_called_tester.has_been_called());
  EXPECT_FALSE(tag_available_called_tester.value_of_ok());

  ShutdownDispatcher();
}

// Re-register a tag that becomes available in the context of the tag's
// callback.
TEST_F(GrpcCompletionQueueDispatcherTest, ReregisterTag) {
  base::RunLoop run_loop_1;
  TagAvailableCalledTester tag_available_called_tester_1;
  base::RunLoop run_loop_2;
  TagAvailableCalledTester tag_available_called_tester_2;

  dispatcher_.RegisterTag(
      kTag, tag_available_called_tester_1.GetTagAvailableCallback());
  auto reregister_tag_callback =
      base::Bind(&GrpcCompletionQueueDispatcher::RegisterTag,
                 base::Unretained(&dispatcher_), kTag,
                 tag_available_called_tester_2.GetTagAvailableCallback());
  tag_available_called_tester_1.CallWhenInvoked(reregister_tag_callback);
  tag_available_called_tester_1.CallWhenInvoked(run_loop_1.QuitClosure());

  tag_available_called_tester_2.CallWhenInvoked(run_loop_2.QuitClosure());

  grpc::Alarm alarm_1(
      &completion_queue_,
      GprTimespecWithDeltaFromNow(base::TimeDelta::FromMilliseconds(1)), kTag);
  run_loop_1.Run();

  grpc::Alarm alarm_2(
      &completion_queue_,
      GprTimespecWithDeltaFromNow(base::TimeDelta::FromMilliseconds(1)), kTag);
  run_loop_2.Run();

  EXPECT_TRUE(tag_available_called_tester_1.has_been_called());
  EXPECT_TRUE(tag_available_called_tester_1.value_of_ok());
  EXPECT_TRUE(tag_available_called_tester_2.has_been_called());
  EXPECT_TRUE(tag_available_called_tester_2.value_of_ok());

  ShutdownDispatcher();
}

}  // namespace diagnostics
