// Copyright 2021 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <utility>

#include "libstorage/storage_container/ephemeral_container.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libstorage/platform/mock_platform.h>

namespace libstorage {

using ::testing::_;
using ::testing::Eq;
using ::testing::NiceMock;
using ::testing::Return;

namespace {
constexpr char kDevice[] = "/dev/fake_loop";
}  // namespace

class EphemeralContainerTest : public ::testing::Test {
 public:
  EphemeralContainerTest()
      : backing_device_(std::make_unique<FakeBackingDevice>(
            BackingDeviceType::kRamdiskDevice, base::FilePath(kDevice))) {}

 protected:
  NiceMock<MockPlatform> platform_;
  std::unique_ptr<FakeBackingDevice> backing_device_;

  std::unique_ptr<EphemeralContainer> CreateContainer() {
    return std::unique_ptr<EphemeralContainer>(
        new EphemeralContainer(std::move(backing_device_), &platform_));
  }
};

namespace {

TEST_F(EphemeralContainerTest, Construct) {
  auto container_ = CreateContainer();
  EXPECT_FALSE(container_->Exists());
  EXPECT_THAT(container_->GetBackingLocation(), Eq(base::FilePath()));
  EXPECT_TRUE(container_->Setup(FileSystemKey()));
  EXPECT_TRUE(container_->Exists());
  EXPECT_THAT(container_->GetBackingLocation(), Eq(base::FilePath(kDevice)));
  EXPECT_TRUE(container_->Teardown());
  EXPECT_FALSE(container_->Exists());
  EXPECT_THAT(container_->GetBackingLocation(), Eq(base::FilePath()));
  EXPECT_FALSE(container_->Purge());  // false for ephemeral teardown does purge
  EXPECT_FALSE(container_->Exists());
  EXPECT_THAT(container_->GetBackingLocation(), Eq(base::FilePath()));
}

}  // namespace

}  // namespace libstorage
