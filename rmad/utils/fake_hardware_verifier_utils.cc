// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "rmad/utils/fake_hardware_verifier_utils.h"

#include <string>

#include <base/logging.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/strings/string_util.h>

#include "rmad/constants.h"
#include "rmad/proto_bindings/rmad.pb.h"

namespace {

constexpr char kHwVerificationResultPass[] = "1";
constexpr char kHwVerificationResultFail[] = "0";

}  // namespace

namespace rmad {
namespace fake {

FakeHardwareVerifierUtils::FakeHardwareVerifierUtils(
    const base::FilePath& working_dir_path)
    : HardwareVerifierUtils(), working_dir_path_(working_dir_path) {}

bool FakeHardwareVerifierUtils::GetHardwareVerificationResult(
    HardwareVerificationResult* result) const {
  base::FilePath result_path =
      working_dir_path_.AppendASCII(kHwVerificationResultFilePath);
  if (std::string result_str;
      base::PathExists(result_path) &&
      base::ReadFileToString(result_path, &result_str)) {
    VLOG(1) << "Found injected hardware verification result";
    base::TrimWhitespaceASCII(result_str, base::TRIM_ALL, &result_str);
    if (result_str == kHwVerificationResultPass) {
      result->set_is_compliant(true);
      result->set_error_str("fake_hardware_verifier_error_string");
      return true;
    } else if (result_str == kHwVerificationResultFail) {
      result->set_is_compliant(false);
      result->set_error_str("fake_hardware_verifier_error_string");
      return true;
    } else {
      VLOG(1) << "Invalid injected hardware verification result";
    }
  }
  return false;
}

}  // namespace fake
}  // namespace rmad
