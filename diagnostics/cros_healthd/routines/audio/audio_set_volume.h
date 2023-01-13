// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DIAGNOSTICS_CROS_HEALTHD_ROUTINES_AUDIO_AUDIO_SET_VOLUME_H_
#define DIAGNOSTICS_CROS_HEALTHD_ROUTINES_AUDIO_AUDIO_SET_VOLUME_H_

#include <string>

#include "diagnostics/cros_healthd/routines/diag_routine_with_status.h"
#include "diagnostics/cros_healthd/system/context.h"

namespace diagnostics {

class AudioSetVolumeRoutine final : public DiagnosticRoutineWithStatus {
 public:
  explicit AudioSetVolumeRoutine(Context* context,
                                 uint64_t node_id,
                                 uint8_t volume,
                                 bool mute_on);
  AudioSetVolumeRoutine(const AudioSetVolumeRoutine&) = delete;
  AudioSetVolumeRoutine& operator=(const AudioSetVolumeRoutine&) = delete;

  // DiagnosticRoutine overrides:
  ~AudioSetVolumeRoutine() override;
  void Start() override;
  void Resume() override;
  void Cancel() override;
  void PopulateStatusUpdate(ash::cros_healthd::mojom::RoutineUpdate* response,
                            bool include_output) override;

 private:
  // Tartget node id.
  uint64_t node_id_;

  // Target volume value.
  uint8_t volume_;

  // Mute the device or not.
  bool mute_on_;

  // Context object used to communicate with the executor.
  Context* context_;
};

}  // namespace diagnostics

#endif  // DIAGNOSTICS_CROS_HEALTHD_ROUTINES_AUDIO_AUDIO_SET_VOLUME_H_
