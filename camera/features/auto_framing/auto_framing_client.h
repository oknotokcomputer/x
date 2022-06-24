/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CAMERA_FEATURES_AUTO_FRAMING_AUTO_FRAMING_CLIENT_H_
#define CAMERA_FEATURES_AUTO_FRAMING_AUTO_FRAMING_CLIENT_H_

#include <cutils/native_handle.h>

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <base/callback.h>
#include <base/synchronization/condition_variable.h>
#include <base/synchronization/lock.h>
#include <base/time/time.h>
#include <base/timer/elapsed_timer.h>

#include "cros-camera/auto_framing_cros.h"
#include "cros-camera/common_types.h"

namespace cros {

// This class interfaces with the Google3 auto-framing library:
// http://google3/chromeos/camera/lib/auto_framing/auto_framing_cros.h
class AutoFramingClient : public AutoFramingCrOS::Client {
 public:
  struct Options {
    Size input_size;
    double frame_rate = 0.0;
    uint32_t target_aspect_ratio_x = 0;
    uint32_t target_aspect_ratio_y = 0;
    float detection_rate = 0.0f;
  };

  AutoFramingClient() : crop_window_received_cv_(&lock_) {}

  // Set up the pipeline.
  bool SetUp(const Options& options);

  // Process one frame.  |buffer| is only used during this function call.
  bool ProcessFrame(int64_t timestamp, buffer_handle_t buffer);

  // Return the stored ROI if a new detection is available, or nullopt if not.
  // After this call the stored ROI is cleared, waiting for another new
  // detection to fill it.
  std::optional<Rect<float>> TakeNewRegionOfInterest();

  // Gets the crop window calculated by the full auto-framing pipeline.
  Rect<float> GetCropWindow(int64_t timestamp);

  // Resets the timer that controls detection rate. This forces detecting the
  // next frame when the pipeline is not queued.
  void ResetDetectionTimer();

  // Tear down the pipeline and clear states.
  void TearDown();

  // Implementations of AutoFramingCrOS::Client.
  void OnFrameProcessed(int64_t timestamp) override;
  void OnNewRegionOfInterest(
      int64_t timestamp, int x_min, int y_min, int x_max, int y_max) override;
  void OnNewCropWindow(
      int64_t timestamp, int x_min, int y_min, int x_max, int y_max) override;
  void OnNewAnnotatedFrame(int64_t timestamp,
                           const uint8_t* data,
                           int stride) override;

 private:
  base::Lock lock_;
  base::ConditionVariable crop_window_received_cv_ GUARDED_BY(lock_);
  Size image_size_ GUARDED_BY(lock_);
  Rect<float> full_crop_ GUARDED_BY(lock_);
  std::unique_ptr<AutoFramingCrOS> auto_framing_ GUARDED_BY(lock_);
  std::vector<uint8_t> detector_input_buffer_ GUARDED_BY(lock_);
  std::optional<int64_t> detector_input_buffer_timestamp_ GUARDED_BY(lock_);
  std::optional<Rect<float>> region_of_interest_ GUARDED_BY(lock_);
  std::map<int64_t, Rect<float>> crop_windows_ GUARDED_BY(lock_);
  base::TimeDelta min_detection_interval_ GUARDED_BY(lock_);
  std::optional<base::ElapsedTimer> detection_timer_ GUARDED_BY(lock_);
};

}  // namespace cros

#endif  // CAMERA_FEATURES_AUTO_FRAMING_AUTO_FRAMING_CLIENT_H_
