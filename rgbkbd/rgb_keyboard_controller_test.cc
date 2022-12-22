// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "rgbkbd/constants.h"
#include "rgbkbd/keyboard_backlight_logger.h"
#include "rgbkbd/rgb_keyboard_controller_impl.h"

#include "base/containers/contains.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/strcat.h"

namespace rgbkbd {
namespace {
// Path to the temporary log file created by `keyboard_backlight_logger`.
const base::FilePath kTempLogFilePath("/tmp/rgbkbd_log");
}  // namespace

class RgbKeyboardControllerTest : public testing::Test {
 public:
  RgbKeyboardControllerTest() {
    // Default to RgbKeyboardCapabilities::kIndividualKey
    logger_ = std::make_unique<KeyboardBacklightLogger>(
        kTempLogFilePath, RgbKeyboardCapabilities::kIndividualKey);

    controller_ = std::make_unique<RgbKeyboardControllerImpl>(logger_.get());
  }

  RgbKeyboardControllerTest(const RgbKeyboardControllerTest&) = delete;
  RgbKeyboardControllerTest& operator=(const RgbKeyboardControllerTest&) =
      delete;
  ~RgbKeyboardControllerTest() override = default;

 protected:
  std::unique_ptr<RgbKeyboardControllerImpl> controller_;
  std::unique_ptr<KeyboardBacklightLogger> logger_;
};

TEST_F(RgbKeyboardControllerTest, SetCapabilityIndividualKey) {
  controller_->SetKeyboardCapabilityForTesting(
      RgbKeyboardCapabilities::kIndividualKey);

  EXPECT_EQ(static_cast<uint32_t>(RgbKeyboardCapabilities::kIndividualKey),
            controller_->GetRgbKeyboardCapabilities());
}

TEST_F(RgbKeyboardControllerTest, SetCapabilityFourZoneFortyLed) {
  controller_->SetKeyboardCapabilityForTesting(
      RgbKeyboardCapabilities::kFourZoneFortyLed);

  EXPECT_EQ(static_cast<uint32_t>(RgbKeyboardCapabilities::kFourZoneFortyLed),
            controller_->GetRgbKeyboardCapabilities());
}

}  // namespace rgbkbd
