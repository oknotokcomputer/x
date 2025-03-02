// Copyright 2021 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/cros_healthd/events/udev_events_impl.h"

#include <string>
#include <utility>

#include <base/containers/flat_map.h>
#include <base/files/file_util.h>
#include <base/observer_list.h>
#include <base/observer_list_types.h>
#include <base/strings/string_split.h>
#include <base/test/gmock_callback_support.h>
#include <base/test/task_environment.h>
#include <base/test/test_future.h>
#include <brillo/udev/mock_udev.h>
#include <brillo/udev/mock_udev_device.h>
#include <brillo/udev/mock_udev_monitor.h>
#include <brillo/unittest_utils.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <gtest/gtest_prod.h>
#include <mojo/public/cpp/bindings/pending_receiver.h>
#include <mojo/public/cpp/bindings/receiver.h>

#include "diagnostics/base/file_test_utils.h"
#include "diagnostics/cros_healthd/events/mock_event_observer.h"
#include "diagnostics/cros_healthd/executor/mock_executor.h"
#include "diagnostics/cros_healthd/system/mock_context.h"
#include "diagnostics/cros_healthd/utils/mojo_test_utils.h"
#include "diagnostics/cros_healthd/utils/usb_utils_constants.h"
#include "diagnostics/mojom/public/cros_healthd_events.mojom.h"
#include "diagnostics/mojom/public/cros_healthd_probe.mojom.h"
#include "diagnostics/mojom/public/nullable_primitives.mojom.h"

namespace diagnostics {
namespace {

namespace mojom = ::ash::cros_healthd::mojom;

using ::testing::_;
using ::testing::ByMove;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::StrictMock;
using ::testing::WithArg;

constexpr const char kUdevActionAdd[] = "add";
constexpr const char kUdevActionRemove[] = "remove";
constexpr const char kUdevThunderboltSubSystem[] = "thunderbolt";
constexpr char kFakeThunderboltDevicePath[] =
    "sys/bus/thunderbolt/devices/domain0/";
constexpr const char kUdevActionChange[] = "change";
constexpr char kFakeThunderboltFullPath[] =
    "sys/bus/thunderbolt/devices/domain0/0-0:1-0";
constexpr char kFileThunderboltAuthorized[] = "authorized";
constexpr char kThunderboltAuthorized[] = "1";
constexpr char kThunderboltUnAuthorized[] = "0";

constexpr const char kUdevUsbSubSystem[] = "usb";
constexpr const char kUdevUsbDeviceType[] = "usb_device";
constexpr const char kFakeUsbSysPath[] = "sys/fake/dev/path";
constexpr const char kFakeUsbVendor[] = "fake_usb_vendor";
constexpr const char kFakeUsbName[] = "fake_usb_name";
constexpr const char kFakeUsbProduct[] = "47f/430c/1093";
constexpr uint16_t kFakeUsbVid = 0x47f;
constexpr uint16_t kFakeUsbPid = 0x430c;

constexpr const char kUdevExternalDisplayAction[] = "change";
constexpr const char kUdevExternalDisplaySubSystem[] = "drm";
constexpr const char kUdevExternalDisplayDeviceType[] = "drm_minor";

class MockCrosHealthdThunderboltObserver
    : public mojom::CrosHealthdThunderboltObserver {
 public:
  explicit MockCrosHealthdThunderboltObserver(
      mojo::PendingReceiver<mojom::CrosHealthdThunderboltObserver> receiver)
      : receiver_{this /* impl */, std::move(receiver)} {
    CHECK(receiver_.is_bound());
  }
  MockCrosHealthdThunderboltObserver(
      const MockCrosHealthdThunderboltObserver&) = delete;
  MockCrosHealthdThunderboltObserver& operator=(
      const MockCrosHealthdThunderboltObserver&) = delete;

  MOCK_METHOD(void, OnAdd, (), (override));
  MOCK_METHOD(void, OnRemove, (), (override));
  MOCK_METHOD(void, OnAuthorized, (), (override));
  MOCK_METHOD(void, OnUnAuthorized, (), (override));

 private:
  mojo::Receiver<mojom::CrosHealthdThunderboltObserver> receiver_;
};

class MockCrosHealthdUsbObserver : public mojom::CrosHealthdUsbObserver {
 public:
  explicit MockCrosHealthdUsbObserver(
      mojo::PendingReceiver<mojom::CrosHealthdUsbObserver> receiver)
      : receiver_{this /* impl */, std::move(receiver)} {
    CHECK(receiver_.is_bound());
  }
  MockCrosHealthdUsbObserver(const MockCrosHealthdUsbObserver&) = delete;
  MockCrosHealthdUsbObserver& operator=(const MockCrosHealthdUsbObserver&) =
      delete;

  MOCK_METHOD(void, OnAdd, (mojom::UsbEventInfoPtr), (override));
  MOCK_METHOD(void, OnRemove, (mojom::UsbEventInfoPtr), (override));

 private:
  mojo::Receiver<mojom::CrosHealthdUsbObserver> receiver_;
};

class UdevEventsImplTest : public BaseFileTest {
 public:
  UdevEventsImpl* udev_events_impl() { return &udev_events_impl_; }

 protected:
  MockContext mock_context_;
  UdevEventsImpl udev_events_impl_{&mock_context_};
};

class ThunderboltEventTest : public UdevEventsImplTest {
 public:
  ThunderboltEventTest()
      : task_environment_(
            base::test::TaskEnvironment::MainThreadType::IO,
            base::test::TaskEnvironment::ThreadPoolExecutionMode::ASYNC) {}

  void SetUp() override {
    mojo::PendingRemote<mojom::CrosHealthdThunderboltObserver> observer;
    mojo::PendingReceiver<mojom::CrosHealthdThunderboltObserver>
        observer_receiver(observer.InitWithNewPipeAndPassReceiver());
    observer_ =
        std::make_unique<StrictMock<MockCrosHealthdThunderboltObserver>>(
            std::move(observer_receiver));
    udev_events_impl_.AddThunderboltObserver(std::move(observer));
  }

  MockCrosHealthdThunderboltObserver* mock_observer() {
    return observer_.get();
  }

  void DestroyMojoObserver() {
    observer_.reset();
    task_environment_.RunUntilIdle();
  }

  void SetUpSysfsFile(const std::string& val) {
    const auto dir = kFakeThunderboltDevicePath;
    const auto dev_file = "0-0:1-0";
    SetFile({dir, dev_file, kFileThunderboltAuthorized}, val);
  }

  void TriggerUdevEvent(const char* action, const char* authorized) {
    auto path = GetRootDir().Append(kFakeThunderboltFullPath);
    auto monitor = mock_context_.mock_udev_monitor();
    auto device = std::make_unique<brillo::MockUdevDevice>();
    EXPECT_CALL(*device, GetAction()).WillOnce(Return(action));
    EXPECT_CALL(*device, GetSubsystem())
        .WillOnce(Return(kUdevThunderboltSubSystem));
    if (authorized) {
      SetUpSysfsFile(std::string(authorized));
      EXPECT_CALL(*device, GetSysPath()).WillOnce(Return(path.value().c_str()));
    }
    EXPECT_CALL(*monitor, ReceiveDevice())
        .WillOnce(Return(ByMove(std::move(device))));
    udev_events_impl()->OnUdevEvent();
  }

 protected:
  base::test::TaskEnvironment task_environment_;
  std::unique_ptr<brillo::MockUdevDevice> device_;
  std::unique_ptr<brillo::UdevMonitor> monitor_;
  std::unique_ptr<StrictMock<MockCrosHealthdThunderboltObserver>> observer_;
};

class UsbEventTest : public UdevEventsImplTest {
 public:
  UsbEventTest()
      : task_environment_(
            base::test::TaskEnvironment::MainThreadType::IO,
            base::test::TaskEnvironment::ThreadPoolExecutionMode::ASYNC) {}

  void SetUp() override {
    mojo::PendingRemote<mojom::CrosHealthdUsbObserver> observer;
    mojo::PendingReceiver<mojom::CrosHealthdUsbObserver> observer_receiver(
        observer.InitWithNewPipeAndPassReceiver());
    observer_ = std::make_unique<StrictMock<MockCrosHealthdUsbObserver>>(
        std::move(observer_receiver));
    udev_events_impl_.AddUsbObserver(std::move(observer));
  }

  MockCrosHealthdUsbObserver* mock_observer() { return observer_.get(); }

  void DestroyMojoObserver() {
    observer_.reset();
    task_environment_.RunUntilIdle();
  }

  void SetInterfacesType() {
    // Human Interface Device.
    SetFile({kFakeUsbSysPath, "1-1.2:1.0", "bInterfaceClass"}, "03");
    // Video.
    SetFile({kFakeUsbSysPath, "1-1.2:1.1", "bInterfaceClass"}, "0E");
    // Wireless.
    SetFile({kFakeUsbSysPath, "1-1.2:1.2", "bInterfaceClass"}, "E0");
  }

  void SetSysfsFiles() {
    auto product_tokens =
        base::SplitString(std::string(kFakeUsbProduct), "/",
                          base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    EXPECT_EQ(product_tokens.size(), 3);
    SetFile({kFakeUsbSysPath, kFileUsbVendor}, product_tokens[0]);
    SetFile({kFakeUsbSysPath, kFileUsbProduct}, product_tokens[1]);
  }

  void TriggerUdevEvent(const char* action) {
    auto path = GetRootDir().Append(kFakeUsbSysPath);
    auto monitor = mock_context_.mock_udev_monitor();
    auto device = std::make_unique<brillo::MockUdevDevice>();
    EXPECT_CALL(*device, GetAction()).WillOnce(Return(action));
    EXPECT_CALL(*device, GetSubsystem()).WillOnce(Return(kUdevUsbSubSystem));
    EXPECT_CALL(*device, GetDeviceType()).WillOnce(Return(kUdevUsbDeviceType));
    EXPECT_CALL(*device, GetPropertyValue(kPropertieVendorFromDB))
        .WillOnce(Return(kFakeUsbVendor));
    EXPECT_CALL(*device, GetPropertyValue(kPropertieModelFromDB))
        .WillOnce(Return(kFakeUsbName));
    EXPECT_CALL(*device, GetPropertyValue(kPropertieProduct))
        .WillOnce(Return(kFakeUsbProduct));
    EXPECT_CALL(*device, GetSysPath())
        .WillRepeatedly(Return(path.value().c_str()));
    EXPECT_CALL(*monitor, ReceiveDevice())
        .WillOnce(Return(ByMove(std::move(device))));
    SetInterfacesType();
    SetSysfsFiles();

    udev_events_impl()->OnUdevEvent();
  }

 protected:
  base::test::TaskEnvironment task_environment_;
  std::unique_ptr<brillo::MockUdevDevice> device_;
  std::unique_ptr<brillo::UdevMonitor> monitor_;
  std::unique_ptr<StrictMock<MockCrosHealthdUsbObserver>> observer_;
};

// Tests for the External Display event.
class ExternalDisplayEventsImplTest : public testing::Test {
 public:
  ExternalDisplayEventsImplTest(const ExternalDisplayEventsImplTest&) = delete;
  ExternalDisplayEventsImplTest& operator=(
      const ExternalDisplayEventsImplTest&) = delete;

 protected:
  ExternalDisplayEventsImplTest() = default;

  void SetUp() override {
    udev_events_impl_ = std::make_unique<UdevEventsImpl>(&mock_context_);
  }

  MockEventObserver* mock_event_observer() { return event_observer_.get(); }
  MockExecutor* mock_executor() { return mock_context_.mock_executor(); }

  void InitializeObserver() {
    mojo::PendingRemote<mojom::EventObserver> external_display_observer;
    mojo::PendingReceiver<mojom::EventObserver> observer_receiver(
        external_display_observer.InitWithNewPipeAndPassReceiver());
    event_observer_ = std::make_unique<StrictMock<MockEventObserver>>(
        std::move(observer_receiver));
    udev_events_impl_->AddExternalDisplayObserver(
        std::move(external_display_observer));
  }

  void SetExecutorGetExternalDisplay(
      base::flat_map<uint32_t, mojom::ExternalDisplayInfoPtr> connectors,
      base::OnceClosure on_finish = base::DoNothing()) {
    EXPECT_CALL(*mock_executor(), GetConnectedExternalDisplayConnectors(_, _))
        .WillOnce(DoAll(base::test::RunOnceClosure(std::move(on_finish)),
                        base::test::RunOnceCallback<1>(std::move(connectors),
                                                       std::nullopt)));
  }

  void TriggerExternalDisplayEvent() {
    auto monitor = mock_context_.mock_udev_monitor();
    auto device = std::make_unique<brillo::MockUdevDevice>();
    EXPECT_CALL(*device, GetAction())
        .WillOnce(Return(kUdevExternalDisplayAction));
    EXPECT_CALL(*device, GetSubsystem())
        .WillOnce(Return(kUdevExternalDisplaySubSystem));
    EXPECT_CALL(*device, GetDeviceType())
        .WillOnce(Return(kUdevExternalDisplayDeviceType));
    EXPECT_CALL(*monitor, ReceiveDevice())
        .WillOnce(Return(ByMove(std::move(device))));

    udev_events_impl_->OnUdevEvent();
  }

  mojom::ExternalDisplayInfoPtr GenerateExternalDisplayInfo(
      const std::string& name) {
    auto display = mojom::ExternalDisplayInfo::New();
    display->display_width = mojom::NullableUint32::New(1);
    display->display_height = mojom::NullableUint32::New(1);
    display->resolution_horizontal = mojom::NullableUint32::New(1);
    display->resolution_vertical = mojom::NullableUint32::New(1);
    display->refresh_rate = mojom::NullableDouble::New(1);
    display->manufacturer = "manufacturer";
    display->model_id = mojom::NullableUint16::New(1);
    display->serial_number = mojom::NullableUint32::New(1);
    display->manufacture_week = mojom::NullableUint8::New(1);
    display->manufacture_year = mojom::NullableUint16::New(1);
    display->edid_version = "1";
    display->display_name = name;
    display->input_type = mojom::DisplayInputType::kAnalog;
    return display;
  }

  UdevEventsImpl* udev_events_impl() { return udev_events_impl_.get(); }

 private:
  base::test::TaskEnvironment task_environment_;
  MockContext mock_context_;
  std::unique_ptr<StrictMock<MockEventObserver>> event_observer_;
  std::unique_ptr<UdevEventsImpl> udev_events_impl_;
};

TEST_F(ThunderboltEventTest, TestThunderboltAddEvent) {
  base::test::TestFuture<void> future;
  EXPECT_CALL(*mock_observer(), OnAdd())
      .WillOnce(base::test::RunOnceClosure(future.GetCallback()));

  TriggerUdevEvent(kUdevActionAdd, nullptr);

  EXPECT_TRUE(future.Wait());
}

TEST_F(ThunderboltEventTest, TestThunderboltRemoveEvent) {
  base::test::TestFuture<void> future;
  EXPECT_CALL(*mock_observer(), OnRemove())
      .WillOnce(base::test::RunOnceClosure(future.GetCallback()));

  TriggerUdevEvent(kUdevActionRemove, nullptr);

  EXPECT_TRUE(future.Wait());
}

TEST_F(ThunderboltEventTest, TestThunderboltAuthorizedEvent) {
  base::test::TestFuture<void> future;
  EXPECT_CALL(*mock_observer(), OnAuthorized())
      .WillOnce(base::test::RunOnceClosure(future.GetCallback()));

  TriggerUdevEvent(kUdevActionChange, kThunderboltAuthorized);

  EXPECT_TRUE(future.Wait());
}

TEST_F(ThunderboltEventTest, TestThunderboltUnAuthorizedEvent) {
  base::test::TestFuture<void> future;
  EXPECT_CALL(*mock_observer(), OnUnAuthorized())
      .WillOnce(base::test::RunOnceClosure(future.GetCallback()));

  TriggerUdevEvent(kUdevActionChange, kThunderboltUnAuthorized);

  EXPECT_TRUE(future.Wait());
}

TEST_F(UsbEventTest, TestUsbAddEvent) {
  base::test::TestFuture<void> future;
  mojom::UsbEventInfoPtr info;
  EXPECT_CALL(*mock_observer(), OnAdd(_))
      .WillOnce(DoAll(SaveMojomArg<0>(&info),
                      base::test::RunOnceClosure(future.GetCallback())));

  TriggerUdevEvent(kUdevActionAdd);

  EXPECT_TRUE(future.Wait());
  EXPECT_EQ(info->vendor, kFakeUsbVendor);
  EXPECT_EQ(info->name, kFakeUsbName);
  EXPECT_EQ(info->vid, kFakeUsbVid);
  EXPECT_EQ(info->pid, kFakeUsbPid);
  EXPECT_THAT(info->categories,
              testing::UnorderedElementsAreArray(
                  {"Wireless", "Human Interface Device", "Video"}));
}

TEST_F(UsbEventTest, TestUsbRemoveEvent) {
  base::test::TestFuture<void> future;
  mojom::UsbEventInfoPtr info;
  EXPECT_CALL(*mock_observer(), OnRemove(_))
      .WillOnce(DoAll(SaveMojomArg<0>(&info),
                      base::test::RunOnceClosure(future.GetCallback())));

  TriggerUdevEvent(kUdevActionRemove);

  EXPECT_TRUE(future.Wait());
  EXPECT_EQ(info->vendor, kFakeUsbVendor);
  EXPECT_EQ(info->name, kFakeUsbName);
  EXPECT_EQ(info->vid, kFakeUsbVid);
  EXPECT_EQ(info->pid, kFakeUsbPid);
  EXPECT_THAT(info->categories,
              testing::UnorderedElementsAreArray(
                  {"Wireless", "Human Interface Device", "Video"}));
}

TEST_F(ExternalDisplayEventsImplTest, TestExternalDisplayAddEvent) {
  {
    // We did not call UdevEventsImpl::Initialize() function due to the
    // difficulty of setting up udev_monitor dependency. Here we manually set up
    // the starting state through triggering a external_display event before
    // initializing observer.
    base::test::TestFuture<void> future;
    SetExecutorGetExternalDisplay({}, future.GetCallback());
    TriggerExternalDisplayEvent();
    EXPECT_TRUE(future.Wait());
  }
  InitializeObserver();
  {
    base::test::TestFuture<void> future;
    mojom::EventInfoPtr recv_info;
    base::flat_map<uint32_t, mojom::ExternalDisplayInfoPtr> connectors;
    connectors[1] = GenerateExternalDisplayInfo("display1");
    SetExecutorGetExternalDisplay(std::move(connectors));
    EXPECT_CALL(*mock_event_observer(), OnEvent(_))
        .WillOnce(DoAll(SaveMojomArg<0>(&recv_info),
                        base::test::RunOnceClosure(future.GetCallback())));
    TriggerExternalDisplayEvent();
    EXPECT_TRUE(future.Wait());
    EXPECT_TRUE(recv_info->is_external_display_event_info());
    EXPECT_EQ(recv_info->get_external_display_event_info()->state,
              mojom::ExternalDisplayEventInfo::State::kAdd);
    EXPECT_EQ(recv_info->get_external_display_event_info()->display_info,
              GenerateExternalDisplayInfo("display1"));
  }
}

TEST_F(ExternalDisplayEventsImplTest, TestExternalDisplayRemoveEvent) {
  {
    // We did not call UdevEventsImpl::Initialize() function due to the
    // difficulty of setting up udev_monitor dependency. Here we manually set up
    // the starting state through triggering a external_display event before
    // initializing observer.
    base::test::TestFuture<void> future;
    base::flat_map<uint32_t, mojom::ExternalDisplayInfoPtr> connectors;
    connectors[1] = GenerateExternalDisplayInfo("display1");
    SetExecutorGetExternalDisplay(std::move(connectors), future.GetCallback());
    TriggerExternalDisplayEvent();
    EXPECT_TRUE(future.Wait());
  }
  InitializeObserver();
  {
    base::test::TestFuture<void> future;
    mojom::EventInfoPtr recv_info;
    SetExecutorGetExternalDisplay({});
    EXPECT_CALL(*mock_event_observer(), OnEvent(_))
        .WillOnce(DoAll(SaveMojomArg<0>(&recv_info),
                        base::test::RunOnceClosure(future.GetCallback())));
    TriggerExternalDisplayEvent();
    EXPECT_TRUE(future.Wait());
    EXPECT_TRUE(recv_info->is_external_display_event_info());
    EXPECT_EQ(recv_info->get_external_display_event_info()->state,
              mojom::ExternalDisplayEventInfo::State::kRemove);
    EXPECT_EQ(recv_info->get_external_display_event_info()->display_info,
              GenerateExternalDisplayInfo("display1"));
  }
}

TEST_F(ExternalDisplayEventsImplTest, TestDuplicateExternalDisplayConnectorId) {
  {
    // We did not call UdevEventsImpl::Initialize() function due to the
    // difficulty of setting up udev_monitor dependency. Here we manually set up
    // the starting state through triggering a external_display event before
    // initializing observer.
    base::test::TestFuture<void> future;
    base::flat_map<uint32_t, mojom::ExternalDisplayInfoPtr> connectors;
    SetExecutorGetExternalDisplay(std::move(connectors), future.GetCallback());
    TriggerExternalDisplayEvent();
    EXPECT_TRUE(future.Wait());
  }
  InitializeObserver();
  {
    base::test::TestFuture<void> future;
    base::flat_map<uint32_t, mojom::ExternalDisplayInfoPtr> connectors;
    connectors[1] = GenerateExternalDisplayInfo("display1");
    SetExecutorGetExternalDisplay(std::move(connectors));
    EXPECT_CALL(*mock_event_observer(), OnEvent(_))
        .WillOnce(base::test::RunOnceClosure(future.GetCallback()));
    TriggerExternalDisplayEvent();
    EXPECT_TRUE(future.Wait());
  }
  {
    base::test::TestFuture<void> future;
    base::flat_map<uint32_t, mojom::ExternalDisplayInfoPtr> connectors;
    SetExecutorGetExternalDisplay(std::move(connectors));
    EXPECT_CALL(*mock_event_observer(), OnEvent(_))
        .WillOnce(base::test::RunOnceClosure(future.GetCallback()));
    TriggerExternalDisplayEvent();
    EXPECT_TRUE(future.Wait());
  }
  {
    base::test::TestFuture<void> future;
    mojom::EventInfoPtr recv_info;
    base::flat_map<uint32_t, mojom::ExternalDisplayInfoPtr> connectors;
    connectors[1] = GenerateExternalDisplayInfo("display2");
    SetExecutorGetExternalDisplay(std::move(connectors));
    EXPECT_CALL(*mock_event_observer(), OnEvent(_))
        .WillOnce(DoAll(SaveMojomArg<0>(&recv_info),
                        base::test::RunOnceClosure(future.GetCallback())));
    TriggerExternalDisplayEvent();
    EXPECT_TRUE(future.Wait());
    EXPECT_TRUE(recv_info->is_external_display_event_info());
    EXPECT_EQ(recv_info->get_external_display_event_info()->state,
              mojom::ExternalDisplayEventInfo::State::kAdd);
    EXPECT_EQ(recv_info->get_external_display_event_info()->display_info,
              GenerateExternalDisplayInfo("display2"));
  }
}

TEST_F(ExternalDisplayEventsImplTest, TestExternalDisplayAddMultipleDisplay) {
  {
    // We did not call UdevEventsImpl::Initialize() function due to the
    // difficulty of setting up udev_monitor dependency. Here we manually set up
    // the starting state through triggering a external_display event before
    // initializing observer.
    base::test::TestFuture<void> future;
    SetExecutorGetExternalDisplay({}, future.GetCallback());
    TriggerExternalDisplayEvent();
    EXPECT_TRUE(future.Wait());
  }
  InitializeObserver();
  {
    base::test::TestFuture<void> future;
    mojom::EventInfoPtr recv_info_1;
    mojom::EventInfoPtr recv_info_2;
    base::flat_map<uint32_t, mojom::ExternalDisplayInfoPtr> connectors;
    connectors[1] = GenerateExternalDisplayInfo("display1");
    connectors[2] = GenerateExternalDisplayInfo("display2");
    SetExecutorGetExternalDisplay(std::move(connectors));
    EXPECT_CALL(*mock_event_observer(), OnEvent(_))
        .WillOnce(SaveMojomArg<0>(&recv_info_1))
        .WillOnce(DoAll(SaveMojomArg<0>(&recv_info_2),
                        base::test::RunOnceClosure(future.GetCallback())));

    TriggerExternalDisplayEvent();
    EXPECT_TRUE(future.Wait());

    EXPECT_TRUE(recv_info_1->is_external_display_event_info());
    EXPECT_EQ(recv_info_1->get_external_display_event_info()->state,
              mojom::ExternalDisplayEventInfo::State::kAdd);
    EXPECT_EQ(recv_info_1->get_external_display_event_info()->display_info,
              GenerateExternalDisplayInfo("display1"));

    EXPECT_TRUE(recv_info_2->is_external_display_event_info());
    EXPECT_EQ(recv_info_2->get_external_display_event_info()->state,
              mojom::ExternalDisplayEventInfo::State::kAdd);
    EXPECT_EQ(recv_info_2->get_external_display_event_info()->display_info,
              GenerateExternalDisplayInfo("display2"));
  }
}

TEST_F(ExternalDisplayEventsImplTest, TestExternalDisplayMultipleObservers) {
  {
    // We did not call UdevEventsImpl::Initialize() function due to the
    // difficulty of setting up udev_monitor dependency. Here we manually set up
    // the starting state through triggering a external_display event before
    // initializing observer.
    base::test::TestFuture<void> future;
    SetExecutorGetExternalDisplay({}, future.GetCallback());
    TriggerExternalDisplayEvent();
    EXPECT_TRUE(future.Wait());
  }
  mojo::PendingRemote<mojom::EventObserver> external_display_observer_1;
  mojo::PendingReceiver<mojom::EventObserver> observer_receiver_1(
      external_display_observer_1.InitWithNewPipeAndPassReceiver());
  auto event_observer_1 = std::make_unique<StrictMock<MockEventObserver>>(
      std::move(observer_receiver_1));
  udev_events_impl()->AddExternalDisplayObserver(
      std::move(external_display_observer_1));

  mojo::PendingRemote<mojom::EventObserver> external_display_observer_2;
  mojo::PendingReceiver<mojom::EventObserver> observer_receiver_2(
      external_display_observer_2.InitWithNewPipeAndPassReceiver());
  auto event_observer_2 = std::make_unique<StrictMock<MockEventObserver>>(
      std::move(observer_receiver_2));
  udev_events_impl()->AddExternalDisplayObserver(
      std::move(external_display_observer_2));

  {
    base::test::TestFuture<void> future_1;
    base::test::TestFuture<void> future_2;
    mojom::EventInfoPtr recv_info_1;
    mojom::EventInfoPtr recv_info_2;
    base::flat_map<uint32_t, mojom::ExternalDisplayInfoPtr> connectors;
    connectors[1] = GenerateExternalDisplayInfo("display1");
    SetExecutorGetExternalDisplay(std::move(connectors));
    EXPECT_CALL(*event_observer_1.get(), OnEvent(_))
        .WillOnce(DoAll(SaveMojomArg<0>(&recv_info_1),
                        base::test::RunOnceClosure(future_1.GetCallback())));
    EXPECT_CALL(*event_observer_2.get(), OnEvent(_))
        .WillOnce(DoAll(SaveMojomArg<0>(&recv_info_2),
                        base::test::RunOnceClosure(future_2.GetCallback())));
    TriggerExternalDisplayEvent();
    EXPECT_TRUE(future_1.Wait());
    EXPECT_TRUE(future_2.Wait());
    EXPECT_TRUE(recv_info_1->is_external_display_event_info());
    EXPECT_EQ(recv_info_1->get_external_display_event_info()->state,
              mojom::ExternalDisplayEventInfo::State::kAdd);
    EXPECT_EQ(recv_info_1->get_external_display_event_info()->display_info,
              GenerateExternalDisplayInfo("display1"));
    EXPECT_TRUE(recv_info_2->is_external_display_event_info());
    EXPECT_EQ(recv_info_2->get_external_display_event_info()->state,
              mojom::ExternalDisplayEventInfo::State::kAdd);
    EXPECT_EQ(recv_info_2->get_external_display_event_info()->display_info,
              GenerateExternalDisplayInfo("display1"));
  }
}

}  // namespace
}  // namespace diagnostics
