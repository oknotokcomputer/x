// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWERD_SYSTEM_AUDIO_CLIENT_H_
#define POWER_MANAGER_POWERD_SYSTEM_AUDIO_CLIENT_H_

#include "base/basictypes.h"
#include "base/observer_list.h"
#include "base/time.h"
#include "power_manager/common/signal_callback.h"

typedef int gboolean;
typedef unsigned int guint;

namespace power_manager {
namespace system {

class AudioObserver;

// AudioClient monitors audio activity as reported by CRAS, the Chrome OS
// audio server.
class AudioClient {
 public:
  AudioClient();
  ~AudioClient();

  bool headphone_jack_plugged() const { return headphone_jack_plugged_; }
  bool hdmi_active() const { return hdmi_active_; }

  // Adds or removes an observer.
  void AddObserver(AudioObserver* observer);
  void RemoveObserver(AudioObserver* observer);

  // Mutes the system.
  void MuteSystem();

  // Restores the muted state the system had before the call to MuteSystem.
  // Multiple calls to MuteSystem do not stack.
  void RestoreMutedState();

  // Calls Update*() to load the initial state from CRAS.
  void LoadInitialState();

  // Updates the client's view of connected audio devices.
  void UpdateDevices();

  // Updates |num_active_streams_|. Starts or stops
  // |notify_observers_timeout_id_| on changes between zero and nonzero counts.
  void UpdateNumActiveStreams();

 private:
  // Called periodically while |num_active_streams_| is nonzero to notify
  // |observers_| about audio playback.
  SIGNAL_CALLBACK_0(AudioClient, gboolean, NotifyObservers);

  // Number of audio streams (either input or output) currently active.
  int num_active_streams_;

  // Is something plugged in to a headphone jack?
  bool headphone_jack_plugged_;

  // Is an HDMI output active?
  bool hdmi_active_;

  // Indicates whether the muted state was successfully stored by a call to
  // MuteSystem().
  bool mute_stored_;

  // The state the system was in before the call to MuteSystem().
  bool originally_muted_;

  // GLib timeout ID for running NotifyObservers(), or 0 if unset.
  guint notify_observers_timeout_id_;

  ObserverList<AudioObserver> observers_;

  DISALLOW_COPY_AND_ASSIGN(AudioClient);
};

}  // namespace system
}  // namespace power_manager

#endif  // POWER_MANAGER_POWERD_SYSTEM_AUDIO_CLIENT_H_
