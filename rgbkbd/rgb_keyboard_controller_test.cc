// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "rgbkbd/keyboard_backlight_logger.h"
#include "rgbkbd/rgb_keyboard_controller_impl.h"

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/strcat.h"

namespace rgbkbd {
namespace {

// Path to the temporary log file created by `keyboard_backlight_logger`.
const base::FilePath kTempLogFilePath("/tmp/rgbkbd_log");

// TODO(michaelcheco): Move to shared test util.
const std::string CreateSetKeyColorLogEntry(const KeyColor& key_color) {
  return base::StrCat({"RGB::SetKeyColor - ", std::to_string(key_color.key),
                       ",", std::to_string(key_color.color.r), ",",
                       std::to_string(key_color.color.g), ",",
                       std::to_string(key_color.color.b), "\n"});
}

const std::string CreateSetAllKeyColorsLogEntry(const Color& color) {
  return base::StrCat({"RGB::SetAllKeyColors - ", std::to_string(color.r), ",",
                       std::to_string(color.g), ",", std::to_string(color.b),
                       "\n"});
}

void ValidateLog(const std::string& expected) {
  std::string file_contents;
  EXPECT_TRUE(base::ReadFileToString(kTempLogFilePath, &file_contents) &&
              expected == file_contents);
}

void ValidateLog(base::span<const KeyColor> expected) {
  std::string expected_string;
  for (const auto& key_color : expected) {
    expected_string += CreateSetKeyColorLogEntry(key_color);
  }

  ValidateLog(expected_string);
}

}  // namespace

class RgbKeyboardControllerTest : public testing::Test {
 public:
  RgbKeyboardControllerTest() {
    logger_ = std::make_unique<KeyboardBacklightLogger>(kTempLogFilePath);

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

// TODO(michaelcheco): Update when we are able to test the real implementation.
TEST_F(RgbKeyboardControllerTest, GetRgbKeyboardCapabilitiesReturnsFiveZone) {
  EXPECT_EQ(controller_->GetRgbKeyboardCapabilities(),
            static_cast<uint32_t>(RgbKeyboardCapabilities::kFiveZone));
}

TEST_F(RgbKeyboardControllerTest, SetCapabilityIndividualKey) {
  controller_->SetKeyboardCapabilityForTesting(
      RgbKeyboardCapabilities::kIndividualKey);

  EXPECT_EQ(static_cast<uint32_t>(RgbKeyboardCapabilities::kIndividualKey),
            controller_->GetRgbKeyboardCapabilities());
}

TEST_F(RgbKeyboardControllerTest, SetCapabilityFiveZone) {
  controller_->SetKeyboardCapabilityForTesting(
      RgbKeyboardCapabilities::kFiveZone);

  EXPECT_EQ(static_cast<uint32_t>(RgbKeyboardCapabilities::kFiveZone),
            controller_->GetRgbKeyboardCapabilities());
}

TEST_F(RgbKeyboardControllerTest, SetCapsLockStateWithDefaultHighlight) {
  EXPECT_FALSE(controller_->IsCapsLockEnabledForTesting());
  // Set the background color to something other than |kWhiteBackgroundColor|
  // to ensure the default caps lock highlight color is selected.
  const auto expected_color = Color(/*r=*/100, /*g=*/150, /*b=*/200);
  controller_->SetStaticBackgroundColor(expected_color.r, expected_color.g,
                                        expected_color.b);
  EXPECT_TRUE(logger_->ResetLog());
  controller_->SetCapsLockState(/*enabled=*/true);
  EXPECT_TRUE(controller_->IsCapsLockEnabledForTesting());
  const std::vector<KeyColor> caps_lock_colors = {
      {kLeftShiftKey, kCapsLockHighlightDefault},
      {kRightShiftKey, kCapsLockHighlightDefault}};
  ValidateLog(std::move(caps_lock_colors));

  // Disable caps lock and verify that the background color is restored.
  EXPECT_TRUE(logger_->ResetLog());
  controller_->SetCapsLockState(/*enabled=*/false);
  const std::vector<KeyColor> default_colors = {
      {kLeftShiftKey, expected_color}, {kRightShiftKey, expected_color}};

  EXPECT_FALSE(controller_->IsCapsLockEnabledForTesting());
  ValidateLog(std::move(default_colors));
}

TEST_F(RgbKeyboardControllerTest, SetCapsLockStateWithAlternateHighlight) {
  EXPECT_FALSE(controller_->IsCapsLockEnabledForTesting());
  EXPECT_TRUE(logger_->ResetLog());
  controller_->SetCapsLockState(/*enabled=*/true);
  EXPECT_TRUE(controller_->IsCapsLockEnabledForTesting());
  // Background color defaults to kWhiteBackgroundColor so expect the alternate
  // caps lock highlight color to be used.
  const std::vector<KeyColor> caps_lock_colors = {
      {kLeftShiftKey, kCapsLockHighlightAlternate},
      {kRightShiftKey, kCapsLockHighlightAlternate}};
  ValidateLog(std::move(caps_lock_colors));
}

TEST_F(RgbKeyboardControllerTest, SetRainbowModeFiveZone) {
  controller_->SetCapabilitiesForTesting(RgbKeyboardCapabilities::kFiveZone);
  controller_->SetRainbowMode();
  ValidateLog(kRainbowModeFiveZone);
}

TEST_F(RgbKeyboardControllerTest, SetRainbowModeIndividualKey) {
  controller_->SetCapabilitiesForTesting(
      RgbKeyboardCapabilities::kIndividualKey);
  controller_->SetRainbowMode();
  ValidateLog(kRainbowModeIndividualKey);
}

TEST_F(RgbKeyboardControllerTest, SetRainbowModeCapsLockEnabled) {
  controller_->SetCapabilitiesForTesting(
      RgbKeyboardCapabilities::kIndividualKey);
  controller_->SetCapsLockState(/*enabled=*/true);
  EXPECT_TRUE(logger_->ResetLog());
  controller_->SetRainbowMode();
  ValidateLog(controller_->GetRainbowModeColorsWithoutShiftKeysForTesting());
}

TEST_F(RgbKeyboardControllerTest, SetRainbowModeWithCapsLock) {
  controller_->SetCapabilitiesForTesting(
      RgbKeyboardCapabilities::kIndividualKey);
  // Simulate enabling caps lock.
  controller_->SetCapsLockState(/*enabled=*/true);
  EXPECT_TRUE(logger_->ResetLog());

  // Set rainbow mode.
  controller_->SetRainbowMode();

  ValidateLog(controller_->GetRainbowModeColorsWithoutShiftKeysForTesting());
  EXPECT_TRUE(logger_->ResetLog());

  // Disable caps lock.
  controller_->SetCapsLockState(/*enabled=*/false);
  // Since rainbow mode was set, expect disabling caps lock reverts to the
  // correct color
  ValidateLog(
      CreateSetKeyColorLogEntry({kLeftShiftKey, kCapsLockHighlightDefault}) +
      CreateSetKeyColorLogEntry({kRightShiftKey, kCapsLockHighlightDefault}));
}

TEST_F(RgbKeyboardControllerTest, SetStaticBackgroundColor) {
  const Color expected_color(/*r=*/100, /*g=*/150, /*b=*/200);

  controller_->SetStaticBackgroundColor(expected_color.r, expected_color.g,
                                        expected_color.b);

  const std::string expected_log =
      CreateSetAllKeyColorsLogEntry(expected_color);
  ValidateLog(expected_log);
}

TEST_F(RgbKeyboardControllerTest, SetStaticBackgroundColorWithCapsLock) {
  // Simulate enabling Capslock.
  EXPECT_FALSE(controller_->IsCapsLockEnabledForTesting());
  controller_->SetCapsLockState(/*enabled=*/true);
  EXPECT_TRUE(controller_->IsCapsLockEnabledForTesting());

  std::string shift_key_logs =
      CreateSetKeyColorLogEntry({kLeftShiftKey, kCapsLockHighlightAlternate}) +
      CreateSetKeyColorLogEntry({kRightShiftKey, kCapsLockHighlightAlternate});

  ValidateLog(shift_key_logs);
  EXPECT_TRUE(logger_->ResetLog());

  // Set static background color.
  const Color expected_color(/*r=*/100, /*g=*/150, /*b=*/200);
  controller_->SetStaticBackgroundColor(expected_color.r, expected_color.g,
                                        expected_color.b);

  // Since Capslock was enabled, it is re-highlighted when the background is
  // set. Capslock is not set to the default highlight color since the
  // background is no longer the default white color.
  shift_key_logs =
      CreateSetKeyColorLogEntry({kLeftShiftKey, kCapsLockHighlightDefault}) +
      CreateSetKeyColorLogEntry({kRightShiftKey, kCapsLockHighlightDefault});
  const std::string background_log =
      CreateSetAllKeyColorsLogEntry(expected_color);
  ValidateLog(background_log + shift_key_logs);
  EXPECT_TRUE(logger_->ResetLog());

  // Disable Capslock.
  EXPECT_TRUE(controller_->IsCapsLockEnabledForTesting());
  controller_->SetCapsLockState(/*enabled=*/false);
  EXPECT_FALSE(controller_->IsCapsLockEnabledForTesting());

  // Since background was set, expect disabling Capslock reverts to the set
  // background color.
  ValidateLog(CreateSetKeyColorLogEntry({kLeftShiftKey, expected_color}) +
              CreateSetKeyColorLogEntry({kRightShiftKey, expected_color}));
}
}  // namespace rgbkbd
