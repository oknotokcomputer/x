// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DIAGNOSTICS_CROS_HEALTHD_DELEGATE_UTILS_EVDEV_UTILS_H_
#define DIAGNOSTICS_CROS_HEALTHD_DELEGATE_UTILS_EVDEV_UTILS_H_

#include <memory>
#include <string>
#include <vector>

#include <base/files/file_descriptor_watcher_posix.h>
#include <base/files/file_path.h>
#include <base/files/scoped_file.h>
#include <base/functional/callback.h>
#include <mojo/public/cpp/bindings/pending_remote.h>
#include <mojo/public/cpp/bindings/remote.h>

#include "diagnostics/cros_healthd/delegate/utils/libevdev_wrapper.h"
#include "diagnostics/mojom/public/cros_healthd_events.mojom.h"

namespace diagnostics {

class EvdevUtil {
 public:
  class Delegate {
   public:
    virtual ~Delegate() = default;
    // Check if |dev| is the target device.
    virtual bool IsTarget(LibevdevWrapper* dev) = 0;
    // Deal with the events and report to the caller through observer.
    virtual void FireEvent(const input_event& event, LibevdevWrapper* dev) = 0;
    // Initialization fail. Delegate should reset the observer.
    virtual void InitializationFail(uint32_t custom_reason,
                                    const std::string& description) = 0;
    // Collect properties here and report to the caller through observer.
    virtual void ReportProperties(LibevdevWrapper* dev) = 0;
  };

  // This class manages the life cycle of an opened evdev node.
  class EvdevDevice {
   public:
    EvdevDevice(base::ScopedFD fd, std::unique_ptr<LibevdevWrapper> dev);
    EvdevDevice(const EvdevDevice& device) = delete;
    EvdevDevice(EvdevDevice&& device) = delete;
    virtual ~EvdevDevice();

    // Starts watching the readable state of |fd_| and calls |on_evdev_event|
    // when |fd_| is readable. Returns whether the monitoring starts
    // successfully.
    bool StarWatchingEvents(
        base::RepeatingCallback<void(LibevdevWrapper*)> on_evdev_event);

   private:
    // The fd of opened evdev node.
    base::ScopedFD fd_;
    // The libevdev device object.
    std::unique_ptr<LibevdevWrapper> dev_;
    // The watcher to monitor if the |fd_| is readable.
    std::unique_ptr<base::FileDescriptorWatcher::Controller> watcher_;
  };

  using LibevdevWrapperFactoryMethod =
      base::RepeatingCallback<std::unique_ptr<LibevdevWrapper>(int fd)>;

  // If |allow_multiple_devices| is true, all evdev nodes for which
  // |Delegate::IsTarget| returns true will be monitored. Otherwise, at most one
  // evdev node will be monitored.
  EvdevUtil(std::unique_ptr<Delegate> delegate,
            bool allow_multiple_devices = false);
  // Constructor that overrides |factory_method| is only for testing.
  EvdevUtil(std::unique_ptr<Delegate> delegate,
            bool allow_multiple_devices,
            LibevdevWrapperFactoryMethod factory_method);
  EvdevUtil(const EvdevUtil& oth) = delete;
  EvdevUtil(EvdevUtil&& oth) = delete;
  virtual ~EvdevUtil();

 private:
  void Initialize(LibevdevWrapperFactoryMethod factory_method);
  bool Initialize(const base::FilePath& path,
                  LibevdevWrapperFactoryMethod factory_method);

  // Called when the fd of a targeted evdev device |dev| is readable. It reads
  // events from the fd and fires events through |FireEvent|.
  void OnEvdevEvent(LibevdevWrapper* dev);

  // Whether to monitor events from multiple devices.
  const bool allow_multiple_devices_ = false;
  // The evdev devices to monitor.
  std::vector<std::unique_ptr<EvdevDevice>> devs_;
  // Delegate to implement dedicated behaviors for different evdev devices.
  std::unique_ptr<Delegate> delegate_;
};

// Returns touch points of the evdev device.
std::vector<ash::cros_healthd::mojom::TouchPointInfoPtr> FetchTouchPoints(
    LibevdevWrapper* dev);

}  // namespace diagnostics

#endif  // DIAGNOSTICS_CROS_HEALTHD_DELEGATE_UTILS_EVDEV_UTILS_H_
