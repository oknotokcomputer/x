/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "features/auto_framing/auto_framing_client.h"

#include <hardware/gralloc.h>
#include <libyuv.h>

#include <numeric>
#include <optional>
#include <string>
#include <utility>

#include <base/files/file_path.h>
#include <base/files/file_util.h>

#include "common/camera_hal3_helpers.h"
#include "cros-camera/common.h"

namespace cros {

namespace {

// The internal detector model input dimensions.  It saves an internal copy when
// the detector input buffer matches this size and is continuous.
constexpr uint32_t kDetectorInputWidth = 569;
constexpr uint32_t kDetectorInputHeight = 320;

constexpr char kAutoFramingGraphConfigOverridePath[] =
    "/run/camera/auto_framing_subgraph.pbtxt";

}  // namespace

bool AutoFramingClient::SetUp(const Options& options) {
  base::AutoLock lock(lock_);

  image_size_ = options.input_size;

  AutoFramingCrOS::Options auto_framing_options = {
      .frame_rate = options.frame_rate,
      .image_width = base::checked_cast<int>(options.input_size.width),
      .image_height = base::checked_cast<int>(options.input_size.height),
      .detector_input_format = AutoFramingCrOS::ImageFormat::kGRAY8,
      .detector_input_width = base::checked_cast<int>(kDetectorInputWidth),
      .detector_input_height = base::checked_cast<int>(kDetectorInputHeight),
      .target_aspect_ratio_x =
          base::checked_cast<int>(options.target_aspect_ratio_x),
      .target_aspect_ratio_y =
          base::checked_cast<int>(options.target_aspect_ratio_y),
  };
  std::string graph_config;
  std::string* graph_config_ptr = nullptr;
  if (base::ReadFileToString(
          base::FilePath(kAutoFramingGraphConfigOverridePath), &graph_config)) {
    graph_config_ptr = &graph_config;
  }
  auto_framing_ = AutoFramingCrOS::Create();
  if (!auto_framing_ || !auto_framing_->Initialize(auto_framing_options, this,
                                                   graph_config_ptr)) {
    LOGF(ERROR) << "Failed to initialize auto-framing engine";
    auto_framing_ = nullptr;
    return false;
  }

  detector_input_buffer_.resize(kDetectorInputWidth * kDetectorInputHeight);

  region_of_interest_ = std::nullopt;
  crop_window_ = NormalizeRect(
      GetCenteringFullCrop(options.input_size, options.target_aspect_ratio_x,
                           options.target_aspect_ratio_y),
      image_size_);

  return true;
}

bool AutoFramingClient::ProcessFrame(int64_t timestamp,
                                     buffer_handle_t buffer) {
  base::AutoLock lock(lock_);
  DCHECK_NE(auto_framing_, nullptr);

  VLOGF(2) << "Notify frame @" << timestamp;
  if (!auto_framing_->NotifyFrame(timestamp)) {
    LOGF(ERROR) << "Failed to notify frame @" << timestamp;
    return false;
  }

  // Skip detecting this frame if there's an inflight detection.
  if (detector_input_buffer_timestamp_.has_value()) {
    return true;
  }

  ScopedMapping mapping(buffer);
  libyuv::ScalePlane(
      mapping.plane(0).addr, mapping.plane(0).stride, mapping.width(),
      mapping.height(), detector_input_buffer_.data(), kDetectorInputWidth,
      kDetectorInputWidth, kDetectorInputHeight, libyuv::kFilterNone);

  VLOGF(2) << "Process frame @" << timestamp;
  detector_input_buffer_timestamp_ = timestamp;
  if (!auto_framing_->ProcessFrame(timestamp, detector_input_buffer_.data(),
                                   kDetectorInputWidth)) {
    LOGF(ERROR) << "Failed to detect frame @" << timestamp;
    detector_input_buffer_timestamp_ = std::nullopt;
    return false;
  }

  return true;
}

std::optional<Rect<float>> AutoFramingClient::TakeNewRegionOfInterest() {
  base::AutoLock lock(lock_);
  std::optional<Rect<float>> roi;
  roi.swap(region_of_interest_);
  return roi;
}

Rect<float> AutoFramingClient::GetCropWindow() {
  base::AutoLock lock(lock_);
  return crop_window_;
}

void AutoFramingClient::TearDown() {
  base::AutoLock lock(lock_);

  auto_framing_.reset();

  detector_input_buffer_timestamp_ = std::nullopt;
  detector_input_buffer_.clear();
}

void AutoFramingClient::OnFrameProcessed(int64_t timestamp) {
  VLOGF(2) << "Release frame @" << timestamp;

  base::AutoLock lock(lock_);
  DCHECK(detector_input_buffer_timestamp_.has_value());
  DCHECK_EQ(*detector_input_buffer_timestamp_, timestamp);
  detector_input_buffer_timestamp_ = std::nullopt;
}

void AutoFramingClient::OnNewRegionOfInterest(
    int64_t timestamp, int x_min, int y_min, int x_max, int y_max) {
  VLOGF(2) << "ROI @" << timestamp << ": " << x_min << "," << y_min << ","
           << x_max << "," << y_max;

  base::AutoLock lock(lock_);
  region_of_interest_ = NormalizeRect(
      Rect<int>(x_min, y_min, x_max - x_min + 1, y_max - y_min + 1)
          .AsRect<uint32_t>(),
      image_size_);
}

void AutoFramingClient::OnNewCropWindow(
    int64_t timestamp, int x_min, int y_min, int x_max, int y_max) {
  VLOGF(2) << "Crop window @" << timestamp << ": " << x_min << "," << y_min
           << "," << x_max << "," << y_max;

  base::AutoLock lock(lock_);
  crop_window_ = NormalizeRect(
      Rect<int>(x_min, y_min, x_max - x_min + 1, y_max - y_min + 1)
          .AsRect<uint32_t>(),
      image_size_);
}

void AutoFramingClient::OnNewAnnotatedFrame(int64_t timestamp,
                                            const uint8_t* data,
                                            int stride) {
  VLOGF(2) << "Annotated frame @" << timestamp;

  // TODO(kamesan): Draw annotated frame in debug mode.
}

}  // namespace cros
