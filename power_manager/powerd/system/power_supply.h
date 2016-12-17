// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWERD_SYSTEM_POWER_SUPPLY_H_
#define POWER_MANAGER_POWERD_SYSTEM_POWER_SUPPLY_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/cancelable_callback.h>
#include <base/files/file_path.h>
#include <base/macros.h>
#include <base/observer_list.h>
#include <base/time/time.h>
#include <base/timer/timer.h>

#include "power_manager/powerd/system/power_supply_observer.h"
#include "power_manager/powerd/system/rolling_average.h"
#include "power_manager/powerd/system/udev_subsystem_observer.h"
#include "power_manager/proto_bindings/power_supply_properties.pb.h"

namespace power_manager {

class Clock;
class PowerSupplyProperties;
class PrefsInterface;

namespace metrics {
enum class PowerSupplyType;
}

namespace system {

struct PowerStatus;
class UdevInterface;

// Copies fields from |status| into |proto|.
void CopyPowerStatusToProtocolBuffer(const PowerStatus& status,
                                     PowerSupplyProperties* proto);

// Returns a string describing the battery status from |status|.
std::string GetPowerStatusBatteryDebugString(const PowerStatus& status);

// Returns a metrics value corresponding to |type|, a sysfs power supply type.
metrics::PowerSupplyType GetPowerSupplyTypeMetric(const std::string& type);

// Structures used for passing power supply info.
struct PowerStatus {
  // Details about a power source.
  struct Source {
    Source(const std::string& id,
           PowerSupplyProperties::PowerSource::Port port,
           const std::string& manufacturer_id,
           const std::string& model_id,
           double max_power,
           bool active_by_default);
    ~Source();

    bool operator==(const Source& o) const;

    // Opaque ID corresponding to the power source.
    std::string id;

    // The charging port to which this power source is connected.
    PowerSupplyProperties::PowerSource::Port port;

    // Values read from |manufacturer| and |model_name|.
    std::string manufacturer_id;
    std::string model_id;

    // Maximum power this source is capable of delivering, in watts.
    double max_power;

    // True if the power source automatically provides charge when connected
    // (e.g. a dedicated charger).
    bool active_by_default;
  };

  PowerStatus();
  ~PowerStatus();

  // Is a non-battery power source connected?
  bool line_power_on;

  // String read from sysfs describing the non-battery power source.
  std::string line_power_type;

  // Line power statistics. These may be unset even if line power is connected.
  double line_power_voltage;      // In volts.
  double line_power_max_voltage;  // In volts.
  double line_power_current;      // In amperes.
  double line_power_max_current;  // In amperes.

  // Amount of energy, measured in Wh, in the battery.
  double battery_energy;

  // Amount of energy being drained from the battery, measured in W. It is a
  // positive value irrespective of the battery charging or discharging.
  double battery_energy_rate;

  // Current battery levels.
  double battery_voltage;  // In volts.
  double battery_current;  // In amperes.
  double battery_charge;   // In ampere-hours.

  // Battery full charge and design-charge levels in ampere-hours.
  double battery_charge_full;
  double battery_charge_full_design;

  // Observed rate at which the battery's charge has been changing, in amperes
  // (i.e. change in the charge per hour). Positive if the battery's charge has
  // increased, negative if it's decreased, and zero if the charge hasn't
  // changed or if the rate was not calculated because too few samples were
  // available.
  double observed_battery_charge_rate;

  // The battery voltage used in calculating time remaining.  This may or may
  // not be the same as the instantaneous voltage |battery_voltage|, as voltage
  // levels vary over the time the battery is charged or discharged.
  double nominal_voltage;

  // Set to true when we have just transitioned states and we might have both a
  // segment of charging and discharging in the calculation. This is done to
  // signal that the time value maybe inaccurate.
  bool is_calculating_battery_time;

  // Estimated time until the battery is empty (while discharging) or full
  // (while charging).
  base::TimeDelta battery_time_to_empty;
  base::TimeDelta battery_time_to_full;

  // If discharging, estimated time until the battery is at a low-enough level
  // that the system will shut down automatically. This will be less than
  // |battery_time_to_empty| if a shutdown threshold is set.
  base::TimeDelta battery_time_to_shutdown;

  // Battery charge in the range [0.0, 100.0], i.e. |battery_charge| /
  // |battery_charge_full| * 100.0.
  double battery_percentage;

  // Battery charge in the range [0.0, 100.0] that should be displayed to
  // the user. This takes other factors into consideration, such as the
  // percentage at which point we shut down the device and the "full
  // factor".
  double display_battery_percentage;

  // Does the system have a battery?
  bool battery_is_present;

  // Is the battery level so low that the machine should be shut down?
  bool battery_below_shutdown_threshold;

  PowerSupplyProperties::ExternalPower external_power;
  PowerSupplyProperties::BatteryState battery_state;

  // ID of the active source from |available_external_power_sources|.
  std::string external_power_source_id;

  // Connected external power sources.
  std::vector<Source> available_external_power_sources;

  // True if it is possible for some connected devices to function as either
  // sources or sinks (i.e. to either deliver or receive charge).
  bool supports_dual_role_devices;

  // /sys paths from which the line power and battery information was read.
  std::string line_power_path;
  std::string battery_path;

  // Additional information about the battery.
  std::string battery_vendor;
  std::string battery_model_name;
  std::string battery_serial;
  std::string battery_technology;
};

// Fetches the system's power status, e.g. whether on AC or battery, charge and
// voltage level, current, etc.
class PowerSupplyInterface {
 public:
  PowerSupplyInterface() {}
  virtual ~PowerSupplyInterface() {}

  // Adds or removes an observer.
  virtual void AddObserver(PowerSupplyObserver* observer) = 0;
  virtual void RemoveObserver(PowerSupplyObserver* observer) = 0;

  // Returns the last-read status.
  virtual PowerStatus GetPowerStatus() const = 0;

  // Updates the status synchronously, returning true on success. If successful,
  // observers will be notified asynchronously.
  virtual bool RefreshImmediately() = 0;

  // On suspend, stops polling. On resume, updates the status immediately,
  // notifies observers asynchronously, and schedules a poll for the near
  // future.
  virtual void SetSuspended(bool suspended) = 0;

  // Handles a request to use the PowerStatus::Source described by |id|,
  // returning true on success.
  virtual bool SetPowerSource(const std::string& id) = 0;
};

// Real implementation of PowerSupplyInterface that reads from sysfs.
class PowerSupply : public PowerSupplyInterface, public UdevSubsystemObserver {
 public:
  // Helper class for testing PowerSupply.
  class TestApi {
   public:
    explicit TestApi(PowerSupply* power_supply) : power_supply_(power_supply) {}
    ~TestApi() {}

    base::TimeDelta current_poll_delay() const {
      return power_supply_->current_poll_delay_for_testing_;
    }

    // Returns the time that will be used as "now".
    base::TimeTicks GetCurrentTime() const;

    // Sets the time that will be used as "now".
    void SetCurrentTime(base::TimeTicks now);

    // Advances the time by |interval|.
    void AdvanceTime(base::TimeDelta interval);

    // If |poll_timer_| was running, calls HandlePollTimeout() and returns true.
    // Returns false otherwise.
    bool TriggerPollTimeout() WARN_UNUSED_RESULT;

   private:
    PowerSupply* power_supply_;  // weak

    DISALLOW_COPY_AND_ASSIGN(TestApi);
  };

  // Power supply subsystem for udev events.
  static const char kUdevSubsystem[];

  // File within a sysfs device directory that can be used to request that the
  // device be used to deliver power to the system.
  static const char kChargeControlLimitMaxFile[];

  // Minimum duration of samples that need to be present in |charge_samples_|
  // for the observed battery charge rate to be calculated.
  static const int kObservedBatteryChargeRateMinMs;

  // Additional time beyond |battery_stabilized_after_*_delay_| to wait before
  // updating the status, in milliseconds. This just ensures that the timer
  // doesn't fire before it's safe to calculate the battery time.
  static const int kBatteryStabilizedSlackMs;

  // To reduce the risk of shutting down prematurely due to a bad battery
  // time-to-empty estimate, avoid shutting down when
  // |low_battery_shutdown_time_| is set if the battery percent is not also
  // equal to or less than this threshold (in the range [0.0, 100.0)).
  static const double kLowBatteryShutdownSafetyPercent;

  PowerSupply();
  virtual ~PowerSupply();

  base::TimeTicks battery_stabilized_timestamp() const {
    return battery_stabilized_timestamp_;
  }

  // Initializes the object and begins polling. Ownership of |prefs| remains
  // with the caller. If |log_shutdown_thresholds| is true, logs details about
  // shutdown thresholds that are needed by power_LoadTest.
  void Init(const base::FilePath& power_supply_path,
            PrefsInterface *prefs,
            UdevInterface* udev,
            bool log_shutdown_thresholds);

  // PowerSupplyInterface implementation:
  void AddObserver(PowerSupplyObserver* observer) override;
  void RemoveObserver(PowerSupplyObserver* observer) override;
  PowerStatus GetPowerStatus() const override;
  bool RefreshImmediately() override;
  void SetSuspended(bool suspended) override;
  bool SetPowerSource(const std::string& id) override;

  // UdevSubsystemObserver implementation:
  void OnUdevEvent(const std::string& subsystem,
                   const std::string& sysname,
                   UdevAction action) override;

 private:
  // Specifies when UpdatePowerStatus() should update |power_status_|.
  enum class UpdatePolicy {
    // Update the status after any successful refresh.
    UNCONDITIONALLY,
    // Update the status only if the new state (i.e. the connected power sources
    // or the battery state) differs from the current state.
    ONLY_IF_STATE_CHANGED,
  };

  // Specifies how PerformUpdate() should notify observers.
  enum class NotifyPolicy {
    // Call NotifyObservers() directly.
    SYNCHRONOUSLY,
    // Post |notify_observers_task_| to call NotifyObservers() asynchronously.
    ASYNCHRONOUSLY,
  };

  std::string GetIdForPath(const base::FilePath& path) const;
  base::FilePath GetPathForId(const std::string& id) const;

  // Returns the value of |pref_name|, an int64_t pref containing a
  // millisecond-based duration. |default_duration_ms| is returned if the pref
  // is unset.
  base::TimeDelta GetMsPref(const std::string& pref_name,
                            int64_t default_duration_ms) const;

  // Sets |battery_stabilized_timestamp_| so that the current and charge won't
  // be sampled again until at least |stabilized_delay| in the future.
  void DeferBatterySampling(base::TimeDelta stabilized_delay);

  // Reads data from |power_supply_path_| and updates |power_status_|. Returns
  // false if an error is encountered that prevents the status from being
  // initialized or if |policy| was UPDATE_ONLY_IF_SOURCES_CHANGED but the
  // connected power sources have not changed.
  bool UpdatePowerStatus(UpdatePolicy policy);

  // Helper method for UpdatePowerStatus() that reads |path|, a directory under
  // |power_supply_path_| corresponding to a line power source (e.g. anything
  // that isn't a battery), and updates |status|.
  void ReadLinePowerDirectory(const base::FilePath& path,
                              PowerStatus* status);

  // Helper method for UpdatePowerStatus() that reads |path|, a directory under
  // |power_supply_path_| corresponding to a battery, and updates |status|.
  // Returns false if an error is encountered.
  bool ReadBatteryDirectory(const base::FilePath& path, PowerStatus* status);

  // Updates |status|'s time-to-full and time-to-empty estimates or returns
  // false if estimates can't be calculated yet. Negative values are used
  // if the estimates would otherwise be extremely large (due to a very low
  // current).
  //
  // The |battery_state|, |battery_charge|, |battery_charge_full|,
  // |nominal_voltage|, and |battery_voltage| fields must already be
  // initialized.
  bool UpdateBatteryTimeEstimates(PowerStatus* status);

  // Calculates and stores the observed (based on periodic sampling) rate at
  // which the battery's reported charge is changing.
  void UpdateObservedBatteryChargeRate(PowerStatus* status) const;

  // Returns true if |status|'s battery level is so low that the system
  // should be shut down.  |status|'s |battery_percentage|,
  // |battery_time_to_*|, and |line_power_on| fields must already be set.
  bool IsBatteryBelowShutdownThreshold(const PowerStatus& status) const;

  // Calls UpdatePowerStatus() and SchedulePoll() and notifies observers
  // according to |notify_policy| on success.
  bool PerformUpdate(UpdatePolicy update_policy, NotifyPolicy notify_policy);

  // Schedules |poll_timer_| to call HandlePollTimeout().
  void SchedulePoll();

  // Handles |poll_timer_| firing. Updates |power_status_| and reschedules the
  // timer.
  void HandlePollTimeout();

  // Notifies |observers_| that |power_status_| has been updated.
  void NotifyObservers();

  PrefsInterface* prefs_;  // non-owned
  UdevInterface* udev_;  // non-owned

  std::unique_ptr<Clock> clock_;

  base::ObserverList<PowerSupplyObserver> observers_;

  // Most-recently-computed status.
  PowerStatus power_status_;

  // True after |power_status_| has been successfully updated at least once.
  bool power_status_initialized_;

  // Base sysfs directory containing subdirectories corresponding to power
  // supplies.
  base::FilePath power_supply_path_;

  // Remaining battery time at which the system will shut down automatically.
  // Empty if unset.
  base::TimeDelta low_battery_shutdown_time_;

  // Remaining battery charge (as a percentage of |battery_charge_full| in the
  // range [0.0, 100.0]) at which the system will shut down automatically. 0.0
  // if unset. If both |low_battery_shutdown_time_| and this setting are
  // supplied, only |low_battery_shutdown_percent_| will take effect.
  double low_battery_shutdown_percent_;

  // Minimum maximally-available power in watts that must be reported by a USB
  // power source in order for it to be classified as an AC power source. Read
  // from kUsbMinAcWattsPref.
  double usb_min_ac_watts_;

  bool is_suspended_;

  // Amount of time to wait after startup, a power source change, or a
  // resume event before assuming that the current can be used in battery
  // time estimates and the charge is accurate.
  base::TimeDelta battery_stabilized_after_startup_delay_;
  base::TimeDelta battery_stabilized_after_line_power_connected_delay_;
  base::TimeDelta battery_stabilized_after_line_power_disconnected_delay_;
  base::TimeDelta battery_stabilized_after_resume_delay_;

  // Time at which the reported current and charge are expected to have
  // stabilized to the point where they can be recorded in
  // |current_samples_on_*_power_| and |charge_samples_| and the battery's
  // time-to-full or time-to-empty estimates can be updated.
  base::TimeTicks battery_stabilized_timestamp_;

  // A collection of recent current readings (in amperes) used to calculate
  // time-to-full and time-to-empty estimates collected while on line or
  // battery power. Values are positive when the battery is charging and
  // negative when it's discharging.
  std::unique_ptr<RollingAverage> current_samples_on_line_power_;
  std::unique_ptr<RollingAverage> current_samples_on_battery_power_;

  // A collection of recent charge readings (in ampere-hours) used to measure
  // the rate at which the battery is charging or discharging. Reset when the
  // system resumes from suspend or the power source changes.
  std::unique_ptr<RollingAverage> charge_samples_;

  // The fraction of the full charge at which the battery is considered "full",
  // in the range (0.0, 1.0]. Initialized from kPowerSupplyFullFactorPref.
  double full_factor_;

  // Amount of time to wait before updating |power_status_| again after an
  // update.
  base::TimeDelta poll_delay_;

  // Calls HandlePollTimeout().
  base::OneShotTimer poll_timer_;

  // Delay used when |poll_timer_| was last started.
  base::TimeDelta current_poll_delay_for_testing_;

  // Calls NotifyObservers().
  base::CancelableClosure notify_observers_task_;

  // Maps from sysfs line power subdirectory basenames (e.g.
  // "CROS_USB_PD_CHARGER0") to enum values describing the corresponding
  // charging ports' positions. Loaded from kChargingPortsPref.
  std::map<std::string, PowerSupplyProperties::PowerSource::Port> port_names_;

  DISALLOW_COPY_AND_ASSIGN(PowerSupply);
};

}  // namespace system
}  // namespace power_manager

#endif  // POWER_MANAGER_POWERD_SYSTEM_POWER_SUPPLY_H_
