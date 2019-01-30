/* Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "hardware_verifier/hw_verification_spec_getter_impl.h"

#include <string>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/optional.h>
#include <google/protobuf/text_format.h>
#include <vboot/crossystem.h>

#include "hardware_verifier/hardware_verifier.pb.h"
#include "hardware_verifier/log_utils.h"

namespace hardware_verifier {

namespace {

const char kTextFmtExt[] = ".prototxt";
const char kDefaultHwVerificationSpecRelPath[] =
    "etc/hardware_verifier/hw_verification_spec.prototxt";

base::Optional<HwVerificationSpec> ReadOutHwVerificationSpecFromFile(
    const base::FilePath& file_path) {
  VLOG(1) << "Try to retrieve the verification payload from file ("
          << file_path.value() << ").";
  if (file_path.Extension() != kTextFmtExt) {
    LOG(ERROR) << "The extension (" << file_path.Extension()
               << ") is unrecognizable.";
    return base::nullopt;
  }

  std::string content;
  if (!base::ReadFileToString(file_path, &content)) {
    LOG(ERROR) << "Failed to read the verification payload file.";
    return base::nullopt;
  }

  HwVerificationSpec hw_spec;
  if (!google::protobuf::TextFormat::ParseFromString(content, &hw_spec)) {
    LOG(ERROR) << "Failed to parse the verification payload in text format.";
    return base::nullopt;
  }
  VLogProtobuf(2, "HwVerificationSpec", hw_spec);
  return hw_spec;
}

}  // namespace

HwVerificationSpecGetterImpl::HwVerificationSpecGetterImpl()
    : root_("/"), check_cros_debug_flag_(true) {}

base::Optional<HwVerificationSpec> HwVerificationSpecGetterImpl::GetDefault()
    const {
  return ReadOutHwVerificationSpecFromFile(
      root_.Append(kDefaultHwVerificationSpecRelPath));
}

base::Optional<HwVerificationSpec> HwVerificationSpecGetterImpl::GetFromFile(
    const base::FilePath& file_path) const {
  if (check_cros_debug_flag_ && VbGetSystemPropertyInt("cros_debug") != 1) {
    LOG(ERROR) << "Arbitrary hardware verificaion spec is only allowed with "
                  "cros_debug=1";
    return base::nullopt;
  }
  return ReadOutHwVerificationSpecFromFile(file_path);
}

}  // namespace hardware_verifier
