// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "libhwsec/error/tpm_nvram_error.h"

#include <type_traits>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libhwsec-foundation/error/testing_helper.h>

#include "libhwsec/status.h"

namespace hwsec {

using ::hwsec_foundation::error::testing::IsOk;
using ::hwsec_foundation::error::testing::NotOk;
using ::hwsec_foundation::status::MakeStatus;
using ::hwsec_foundation::status::StatusChain;
using ::tpm_manager::NvramResult;

class TestingTPMNvramErrorTest : public ::testing::Test {
 public:
  TestingTPMNvramErrorTest() {}
  ~TestingTPMNvramErrorTest() override = default;
};

TEST_F(TestingTPMNvramErrorTest, MakeStatus) {
  Status status = MakeStatus<TPMNvramError>(NvramResult::NVRAM_RESULT_SUCCESS);
  EXPECT_THAT(status, IsOk());

  status = MakeStatus<TPMNvramError>(NvramResult::NVRAM_RESULT_ACCESS_DENIED);
  EXPECT_THAT(status, NotOk());
}

TEST_F(TestingTPMNvramErrorTest, TPMRetryAction) {
  Status status =
      MakeStatus<TPMNvramError>(NvramResult::NVRAM_RESULT_IPC_ERROR);
  EXPECT_EQ(status->ToTPMRetryAction(), TPMRetryAction::kCommunication);

  Status status2 = MakeStatus<TPMError>("OuO*").Wrap(std::move(status));
  EXPECT_EQ("OuO*: NVRAM result 100 (NVRAM_RESULT_IPC_ERROR)",
            status2.ToFullString());
  EXPECT_EQ(status2->ToTPMRetryAction(), TPMRetryAction::kCommunication);

  EXPECT_EQ(MakeStatus<TPMNvramError>(NvramResult::NVRAM_RESULT_DEVICE_ERROR)
                ->ToTPMRetryAction(),
            TPMRetryAction::kReboot);
}

}  // namespace hwsec
