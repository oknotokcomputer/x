// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/cros_healthd/delegate/utils/evdev_utils.h"

#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>

#include <memory>
#include <string>
#include <utility>

#include <base/files/file_util.h>
#include <base/strings/stringprintf.h>
#include <base/test/mock_callback.h>
#include <base/test/repeating_test_future.h>
#include <base/test/task_environment.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "diagnostics/base/file_test_utils.h"
#include "diagnostics/cros_healthd/delegate/utils/test/mock_libevdev_wrapper.h"
#include "diagnostics/mojom/public/nullable_primitives.mojom.h"

namespace diagnostics {
namespace {

namespace mojom = ::ash::cros_healthd::mojom;

using ::testing::_;
using ::testing::DoAll;
using ::testing::InvokeWithoutArgs;
using ::testing::IsEmpty;
using ::testing::Pointer;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::SetArgPointee;
using ::testing::SizeIs;
using ::testing::StrictMock;
using ::testing::WithArg;

const base::FilePath kDevInputPath = base::FilePath("/dev/input");

// Reads one byte from fd. Return whether the operation is successful.
bool ReadOneByte(int fd) {
  char buffer;
  return base::ReadFromFD(fd, &buffer, sizeof(buffer));
}

// Write one byte to fd. Return whether the operation is successful.
bool WriteOneByte(int fd) {
  return base::WriteFileDescriptor(fd, "x");
}

class MockDelegate : public EvdevUtil::Delegate {
 public:
  MockDelegate() = default;
  MockDelegate(const MockDelegate&) = delete;
  MockDelegate& operator=(const MockDelegate&) = delete;

  // EvdevUtil::Delegate overrides.
  MOCK_METHOD(bool, IsTarget, (LibevdevWrapper * dev), (override));
  MOCK_METHOD(void,
              FireEvent,
              (const input_event& event, LibevdevWrapper* dev),
              (override));
  MOCK_METHOD(void,
              InitializationFail,
              (uint32_t custom_reason, const std::string& description),
              (override));
  MOCK_METHOD(void, ReportProperties, (LibevdevWrapper * dev), (override));
};

void SetMockSlotValue(test::MockLibevdevWrapper* dev,
                      int slot,
                      int code,
                      int value) {
  EXPECT_CALL(*dev, FetchSlotValue(slot, code, _))
      .WillRepeatedly(DoAll(SetArgPointee<2>(value), Return(1)));
}

void SetMockSlotValue(test::MockLibevdevWrapper* dev,
                      int slot,
                      int code,
                      const mojom::NullableUint32Ptr& value) {
  if (value.is_null()) {
    EXPECT_CALL(*dev, FetchSlotValue(slot, code, _)).WillRepeatedly(Return(0));
  } else {
    SetMockSlotValue(dev, slot, code, value->value);
  }
}

void SetMockTouchPointInfo(test::MockLibevdevWrapper* dev,
                           int slot,
                           const mojom::TouchPointInfoPtr& info) {
  SetMockSlotValue(dev, slot, ABS_MT_TRACKING_ID, info->tracking_id);
  SetMockSlotValue(dev, slot, ABS_MT_POSITION_X, info->x);
  SetMockSlotValue(dev, slot, ABS_MT_POSITION_Y, info->y);
  SetMockSlotValue(dev, slot, ABS_MT_PRESSURE, info->pressure);
  SetMockSlotValue(dev, slot, ABS_MT_TOUCH_MAJOR, info->touch_major);
  SetMockSlotValue(dev, slot, ABS_MT_TOUCH_MINOR, info->touch_minor);
}

class EvdevUtilsTest : public ::testing::Test {
 public:
  EvdevUtilsTest(const EvdevUtilsTest&) = delete;
  EvdevUtilsTest& operator=(const EvdevUtilsTest&) = delete;

 protected:
  EvdevUtilsTest() = default;

  void SetUp() override {
    ASSERT_TRUE(base::CreateDirectory(GetRootedPath(kDevInputPath)));
    mock_delegate_ = std::make_unique<StrictMock<MockDelegate>>();
  }

  base::ScopedFD CreateAndOpenFakeEvdevNode(std::string node_name) {
    auto path = GetRootedPath(kDevInputPath.Append(node_name));
    int res = mkfifo(path.value().c_str(), 0644);
    if (res != 0) {
      LOG(ERROR) << "mkfifo failed, return value = " << res;
      return base::ScopedFD();
    }
    return base::ScopedFD(open(path.value().c_str(), O_RDWR));
  }

  void StartEvdevUtil(bool allow_multiple_devices = false) {
    CHECK(mock_delegate_);
    CHECK(!evdev_util_) << "StartEvdevUtil can only be called once";
    evdev_util_ = std::make_unique<EvdevUtil>(std::move(mock_delegate_),
                                              allow_multiple_devices,
                                              mock_factory_method_.Get());
  }

  void ExpectEventFromNode(int fd, input_event fake_event) {
    auto libevdev_wrapper =
        std::make_unique<StrictMock<test::MockLibevdevWrapper>>();
    // Save the pointer to verify the later accesses are against this instance.
    LibevdevWrapper* const libevdev_wrapper_ptr = libevdev_wrapper.get();
    EXPECT_CALL(*libevdev_wrapper, NextEvent)
        .WillOnce(DoAll(SetArgPointee<1>(fake_event),
                        Return(LIBEVDEV_READ_STATUS_SUCCESS)))
        .WillRepeatedly(Return(-EAGAIN));

    EXPECT_CALL(mock_factory_method_, Run)
        .WillOnce(Return(std::move(libevdev_wrapper)))
        .RetiresOnSaturation();

    EXPECT_CALL(mock_delegate(), IsTarget(Pointer(libevdev_wrapper_ptr)))
        .WillOnce(Return(true));
    EXPECT_CALL(mock_delegate(),
                ReportProperties(Pointer(libevdev_wrapper_ptr)))
        .Times(1);
    EXPECT_CALL(mock_delegate(), FireEvent(_, Pointer(libevdev_wrapper_ptr)))
        .WillOnce(
            DoAll(WithArg<0>([&](auto event) { event_future.AddValue(event); }),
                  // Read data to make reading file blocked again.
                  InvokeWithoutArgs([=]() { ASSERT_TRUE(ReadOneByte(fd)); })));
  }

  MockDelegate& mock_delegate() {
    CHECK(mock_delegate_);
    return *mock_delegate_;
  }

  StrictMock<base::MockCallback<EvdevUtil::LibevdevWrapperFactoryMethod>>
      mock_factory_method_;
  base::test::RepeatingTestFuture<input_event> event_future;

 private:
  // |MainThreadType::IO| is required by FileDescriptorWatcher.
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO};
  ScopedRootDirOverrides root_overrides_;
  std::unique_ptr<StrictMock<MockDelegate>> mock_delegate_;
  std::unique_ptr<EvdevUtil> evdev_util_;
};

// Create an evdev node, set a fake event and verify the received event.
TEST_F(EvdevUtilsTest, ReceiveEventsSuccessfully) {
  auto scoped_fd = CreateAndOpenFakeEvdevNode("event0");
  ASSERT_TRUE(scoped_fd.is_valid());

  const input_event fake_event{.type = 1, .code = 2, .value = 3};
  ExpectEventFromNode(scoped_fd.get(), fake_event);

  StartEvdevUtil();

  // Write data to make the file readable without blocking.
  ASSERT_TRUE(WriteOneByte(scoped_fd.get()));

  auto received_event = event_future.Take();
  EXPECT_EQ(received_event.type, 1);
  EXPECT_EQ(received_event.code, 2);
  EXPECT_EQ(received_event.value, 3);
}

TEST_F(EvdevUtilsTest, InitializationFailIfNoEvdevNodes) {
  EXPECT_CALL(mock_delegate(), InitializationFail).Times(1);

  StartEvdevUtil();
}

TEST_F(EvdevUtilsTest, InitializationFailIfNoTargetDevices) {
  auto scoped_fd = CreateAndOpenFakeEvdevNode("event0");
  ASSERT_TRUE(scoped_fd.is_valid());

  EXPECT_CALL(mock_factory_method_, Run)
      .WillOnce(Return(std::make_unique<test::MockLibevdevWrapper>()));

  EXPECT_CALL(mock_delegate(), IsTarget).WillOnce(Return(false));
  EXPECT_CALL(mock_delegate(), InitializationFail).Times(1);

  StartEvdevUtil();
}

TEST_F(EvdevUtilsTest, InitializationFailIfLibevdevCreationFailed) {
  auto scoped_fd = CreateAndOpenFakeEvdevNode("event0");
  ASSERT_TRUE(scoped_fd.is_valid());

  EXPECT_CALL(mock_factory_method_, Run).WillOnce(Return(nullptr));

  EXPECT_CALL(mock_delegate(), InitializationFail).Times(1);

  StartEvdevUtil();
}

class EvdevUtilsAllowMultipleDeviceTest
    : public EvdevUtilsTest,
      public ::testing::WithParamInterface<int> {
 protected:
  int evdev_node_count() { return GetParam(); }
};

// Create evdev nodes, set fake events and verify the received events.
TEST_P(EvdevUtilsAllowMultipleDeviceTest, ReceiveEventsSuccessfully) {
  std::vector<base::ScopedFD> fds;

  for (int i = 0; i < evdev_node_count(); ++i) {
    const auto event_file_name = base::StringPrintf("event%d", i);
    auto scoped_fd = CreateAndOpenFakeEvdevNode(event_file_name);
    ASSERT_TRUE(scoped_fd.is_valid());

    const input_event fake_event{.type = 1, .code = 2, .value = 3};
    ExpectEventFromNode(scoped_fd.get(), fake_event);
    fds.push_back(std::move(scoped_fd));
  }

  StartEvdevUtil(/*allow_multiple_devices*/ true);

  // Write data to make the file readable without blocking.
  for (int i = 0; i < evdev_node_count(); ++i) {
    ASSERT_TRUE(WriteOneByte(fds[i].get()));
  }
  for (int i = 0; i < evdev_node_count(); ++i) {
    auto received_event = event_future.Take();
    EXPECT_EQ(received_event.type, 1);
    EXPECT_EQ(received_event.code, 2);
    EXPECT_EQ(received_event.value, 3);
  }
}

INSTANTIATE_TEST_SUITE_P(DifferentNumberOfEvdevNodes,
                         EvdevUtilsAllowMultipleDeviceTest,
                         testing::Values(1, 2, 3));

TEST(EvdevUtilsTouchPointTest,
     FetchTouchPointsReturnsEmptyListIfNumberOfSlotsIsInvalid) {
  StrictMock<test::MockLibevdevWrapper> libevdev_wrapper;
  EXPECT_CALL(libevdev_wrapper, GetNumSlots).WillRepeatedly(Return(-1));

  auto res = FetchTouchPoints(&libevdev_wrapper);
  EXPECT_THAT(res, IsEmpty());
}

TEST(EvdevUtilsTouchPointTest, FetchTouchPointsReturnsEmptyListIfNoSlots) {
  StrictMock<test::MockLibevdevWrapper> libevdev_wrapper;
  EXPECT_CALL(libevdev_wrapper, GetNumSlots).WillRepeatedly(Return(0));

  auto res = FetchTouchPoints(&libevdev_wrapper);
  EXPECT_THAT(res, IsEmpty());
}

TEST(EvdevUtilsTouchPointTest, FetchSingleTouchPointsSuccessfully) {
  StrictMock<test::MockLibevdevWrapper> libevdev_wrapper;

  auto expected = mojom::TouchPointInfo::New();
  expected->tracking_id = 1;
  expected->x = 2;
  expected->y = 3;
  expected->pressure = mojom::NullableUint32::New(4);
  expected->touch_major = mojom::NullableUint32::New(5);
  expected->touch_minor = mojom::NullableUint32::New(6);

  EXPECT_CALL(libevdev_wrapper, GetNumSlots()).WillRepeatedly(Return(1));
  SetMockTouchPointInfo(&libevdev_wrapper, 0, expected);

  auto res = FetchTouchPoints(&libevdev_wrapper);
  ASSERT_THAT(res, SizeIs(1));

  const auto& got = res[0];
  EXPECT_EQ(got, expected);
}

TEST(EvdevUtilsTouchPointTest, FetchMultipleTouchPointsSuccessfully) {
  constexpr int kNumberOfSlots = 5;
  StrictMock<test::MockLibevdevWrapper> libevdev_wrapper;

  std::vector<mojom::TouchPointInfoPtr> expected_points;
  for (int i = 0; i < kNumberOfSlots; ++i) {
    auto info = mojom::TouchPointInfo::New();
    info->tracking_id = i;
    expected_points.push_back(std::move(info));
  }

  EXPECT_CALL(libevdev_wrapper, GetNumSlots())
      .WillRepeatedly(Return(expected_points.size()));
  for (int i = 0; i < expected_points.size(); ++i) {
    SetMockTouchPointInfo(&libevdev_wrapper, i, expected_points[i]);
  }

  auto res = FetchTouchPoints(&libevdev_wrapper);
  EXPECT_EQ(res, expected_points);
}

// Negative tracking IDs indicate non-contact points.
TEST(EvdevUtilsTouchPointTest, FetchTouchPointsIgnoresNegativeTrackingIds) {
  StrictMock<test::MockLibevdevWrapper> libevdev_wrapper;

  auto non_contact_point = mojom::TouchPointInfo::New();
  non_contact_point->tracking_id = -1;

  EXPECT_CALL(libevdev_wrapper, GetNumSlots()).WillRepeatedly(Return(1));
  SetMockTouchPointInfo(&libevdev_wrapper, 0, non_contact_point);

  auto res = FetchTouchPoints(&libevdev_wrapper);
  EXPECT_THAT(res, IsEmpty());
}

}  // namespace
}  // namespace diagnostics
