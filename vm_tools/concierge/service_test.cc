// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vm_tools/concierge/service.h"

#include <unistd.h>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <vm_concierge/concierge_service.pb.h>

#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "dbus/mock_bus.h"
#include "dbus/mock_exported_object.h"
#include "dbus/mock_object_proxy.h"
#include "dbus/object_path.h"
#include "dbus/vm_concierge/dbus-constants.h"
#include "featured/feature_library.h"

using ::testing::_;
using ::testing::Eq;
using ::testing::Invoke;
using ::testing::Return;

namespace vm_tools::concierge {

namespace {

dbus::Bus::Options GetDbusOptions() {
  dbus::Bus::Options options;
  options.bus_type = dbus::Bus::SYSTEM;
  return options;
}

class ServiceTest : public testing::Test {
 public:
  ServiceTest() {
    EXPECT_CALL(*mock_bus_, IsConnected()).WillRepeatedly(Return(true));

    EXPECT_CALL(*mock_bus_, HasDBusThread()).WillRepeatedly(Return(true));
    EXPECT_CALL(*mock_bus_, GetDBusTaskRunner())
        .WillRepeatedly(Return(task_runner_.get()));
    EXPECT_CALL(*mock_bus_, GetExportedObject(Eq(concierge_path_)))
        .WillRepeatedly(Return(mock_concierge_obj_.get()));
    EXPECT_CALL(*mock_bus_, GetObjectProxy(_, _))
        .WillRepeatedly(Return(mock_proxy_.get()));

    EXPECT_CALL(*mock_concierge_obj_, ExportMethod(_, _, _, _))
        .WillRepeatedly(Invoke(
            [](const std::string& interface_name,
               const std::string& method_name,
               const dbus::ExportedObject::MethodCallCallback&
                   method_call_callback,
               dbus::ExportedObject::OnExportedCallback on_exported_callback) {
              std::move(on_exported_callback)
                  .Run(interface_name, method_name, /*success=*/true);
            }));

    // Force an error response here because the default-constructed one is
    // expected(nullptr), which is not handled well (see b/314684498).
    EXPECT_CALL(*mock_proxy_, CallMethodAndBlock(_, _))
        .WillRepeatedly(Invoke([]() {
          return base::unexpected(
              dbus::Error("test.error", "test error message"));
        }));
  }

  ~ServiceTest() override {
    // PlatformFeatures stores a copy of the bus globally, so we have to
    // manually shut it down.
    feature::PlatformFeatures::ShutdownForTesting();
  }

 protected:
  base::test::TaskEnvironment task_env_;
  scoped_refptr<base::SequencedTaskRunner> task_runner_{
      base::ThreadPool::CreateSequencedTaskRunner({})};

  dbus::ObjectPath concierge_path_{kVmConciergeServicePath};
  scoped_refptr<dbus::MockBus> mock_bus_ =
      base::MakeRefCounted<testing::NiceMock<dbus::MockBus>>(GetDbusOptions());
  scoped_refptr<dbus::MockExportedObject> mock_concierge_obj_ =
      base::MakeRefCounted<testing::NiceMock<dbus::MockExportedObject>>(
          mock_bus_.get(), concierge_path_);
  scoped_refptr<dbus::MockObjectProxy> mock_proxy_ =
      base::MakeRefCounted<testing::NiceMock<dbus::MockObjectProxy>>(
          mock_bus_.get(),
          "fake.service.name",
          dbus::ObjectPath("/fake/object/path"));
};

}  // namespace

TEST_F(ServiceTest, InitializationSuccess) {
  EXPECT_CALL(*mock_bus_, RequestOwnership(Eq(kVmConciergeInterface), _, _))
      .WillOnce(
          Invoke([](const std::string& service_name,
                    dbus::Bus::ServiceOwnershipOptions options,
                    dbus::Bus::OnOwnershipCallback on_ownership_callback) {
            std::move(on_ownership_callback)
                .Run(service_name, /*success=*/true);
          }));

  base::RunLoop loop;
  Service::CreateAndHost(
      mock_bus_.get(), -1,
      base::BindLambdaForTesting([&](std::unique_ptr<Service> service) {
        EXPECT_TRUE(service);
        loop.Quit();
      }));
  loop.Run();
}

TEST_F(ServiceTest, InitializationFailureToOwnInterface) {
  EXPECT_CALL(*mock_bus_, RequestOwnership(Eq(kVmConciergeInterface), _, _))
      .WillOnce(
          Invoke([](const std::string& service_name,
                    dbus::Bus::ServiceOwnershipOptions options,
                    dbus::Bus::OnOwnershipCallback on_ownership_callback) {
            std::move(on_ownership_callback)
                .Run(service_name, /*success=*/false);
          }));

  base::RunLoop loop;
  Service::CreateAndHost(
      mock_bus_.get(), -1,
      base::BindLambdaForTesting([&](std::unique_ptr<Service> service) {
        EXPECT_FALSE(service);
        loop.Quit();
      }));
  loop.Run();
}

TEST(ServiceInternalTest, TestGetVmStartImageFds2) {
  google::protobuf::RepeatedField<int> fds;
  fds.Add(StartVmRequest_FdType_KERNEL);
  fds.Add(StartVmRequest_FdType_ROOTFS);
  int kernel_pipe[2], rootfs_pipe[2];
  ASSERT_EQ(0, pipe(kernel_pipe));
  ASSERT_EQ(0, pipe(rootfs_pipe));
  std::vector<base::ScopedFD> file_handles;
  file_handles.push_back(base::ScopedFD(kernel_pipe[0]));
  file_handles.push_back(base::ScopedFD(rootfs_pipe[0]));
  base::ScopedFD deleter1(kernel_pipe[1]);
  base::ScopedFD deleter2(rootfs_pipe[1]);

  std::optional<internal::VmStartImageFds> vm_start_image_fds =
      internal::GetVmStartImageFds2(fds, file_handles);
  ASSERT_EQ(7, write(kernel_pipe[1], "kernel", 7));
  ASSERT_EQ(7, write(rootfs_pipe[1], "rootfs", 7));
  char buf[7];
  ASSERT_TRUE(vm_start_image_fds.has_value());
  ASSERT_TRUE(vm_start_image_fds->kernel_fd.has_value());
  ASSERT_TRUE(vm_start_image_fds->rootfs_fd.has_value());
  ASSERT_EQ(7, read(vm_start_image_fds->kernel_fd->get(), buf, 7));
  EXPECT_STREQ(buf, "kernel");
  ASSERT_EQ(7, read(vm_start_image_fds->rootfs_fd->get(), buf, 7));
  EXPECT_STREQ(buf, "rootfs");
}

}  // namespace vm_tools::concierge
