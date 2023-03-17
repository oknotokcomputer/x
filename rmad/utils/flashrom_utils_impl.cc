// Copyright 2021 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <rmad/utils/flashrom_utils_impl.h>

#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <base/files/file_path.h>
#include <base/files/scoped_temp_dir.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_util.h>
#include <re2/re2.h>

#include "rmad/utils/cmd_utils_impl.h"

namespace {

constexpr char kFlashromCmd[] = "/usr/sbin/flashrom";
constexpr char kFlashromWriteProtectDisabledStr[] =
    "WP: write protect is disabled";

constexpr char kFutilityCmd[] = "/usr/bin/futility";
constexpr char kFutilityWriteProtectDisabledStr[] = "WP status: disabled";

}  // namespace

namespace rmad {

FlashromUtilsImpl::FlashromUtilsImpl() : FlashromUtils() {
  cmd_utils_ = std::make_unique<CmdUtilsImpl>();
}

FlashromUtilsImpl::FlashromUtilsImpl(std::unique_ptr<CmdUtils> cmd_utils)
    : FlashromUtils(), cmd_utils_(std::move(cmd_utils)) {}

bool FlashromUtilsImpl::GetApWriteProtectionStatus(bool* enabled) {
  std::string futility_output;
  // Get WP status output string.
  if (!cmd_utils_->GetOutput({kFutilityCmd, "flash", "--wp-status"},
                             &futility_output)) {
    return false;
  }

  // Check if WP is disabled.
  *enabled = (futility_output.find(kFutilityWriteProtectDisabledStr) ==
              std::string::npos);
  return true;
}

bool FlashromUtilsImpl::GetEcWriteProtectionStatus(bool* enabled) {
  std::string flashrom_output;
  // Get WP status output string.
  if (!cmd_utils_->GetOutput({kFlashromCmd, "-p", "ec", "--wp-status"},
                             &flashrom_output)) {
    return false;
  }
  // Check if WP is disabled.
  *enabled = (flashrom_output.find(kFlashromWriteProtectDisabledStr) ==
              std::string::npos);
  return true;
}

bool FlashromUtilsImpl::EnableApSoftwareWriteProtection() {
  // Enable AP WP.
  if (std::string output;
      !cmd_utils_->GetOutput({kFutilityCmd, "flash", "--wp-enable"}, &output)) {
    LOG(ERROR) << "Failed to enable AP SWWP";
    LOG(ERROR) << output;
    return false;
  }

  return true;
}

bool FlashromUtilsImpl::DisableSoftwareWriteProtection() {
  // Disable AP WP.
  if (std::string output; !cmd_utils_->GetOutput(
          {kFutilityCmd, "flash", "--wp-disable"}, &output)) {
    LOG(ERROR) << "Failed to disable AP SWWP";
    LOG(ERROR) << output;
    return false;
  }
  // Disable EC WP.
  if (std::string output; !cmd_utils_->GetOutput(
          {kFlashromCmd, "-p", "ec", "--wp-disable"}, &output)) {
    LOG(ERROR) << "Failed to disable EC SWWP";
    LOG(ERROR) << output;
    return false;
  }

  return true;
}

}  // namespace rmad
