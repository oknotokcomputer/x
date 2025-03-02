// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::fs::read_to_string;
use std::path::Path;
use std::path::PathBuf;
use std::str::FromStr;
use std::vec::Vec;

use anyhow::bail;
use anyhow::Context;
use anyhow::Result;
use glob::glob;
use log::error;
use log::info;

use crate::common;
use crate::common::read_from_file;
use crate::common::BatterySaverMode;
use crate::common::FullscreenVideo;
use crate::common::GameMode;
use crate::common::RTCAudioActive;
use crate::common::VmBootMode;
use crate::config::ConfigProvider;
use crate::config::CpuOfflinePreference;
use crate::config::EnergyPerformancePreference;
use crate::config::Governor;
use crate::config::PowerPreferences;
use crate::config::PowerPreferencesType;
use crate::config::PowerSourceType;
use crate::cpu_utils::hotplug_cpus;
use crate::cpu_utils::HotplugCpuAction;

const POWER_SUPPLY_PATH: &str = "sys/class/power_supply";
const POWER_SUPPLY_ONLINE: &str = "online";
const POWER_SUPPLY_STATUS: &str = "status";
const GLOBAL_ONDEMAND_PATH: &str = "sys/devices/system/cpu/cpufreq/ondemand";

pub trait PowerSourceProvider {
    /// Returns the current power source of the system.
    fn get_power_source(&self) -> Result<PowerSourceType>;
}

/// See the `POWER_SUPPLY_STATUS_` enum in the linux kernel.
/// These values are intended to describe the battery status. They are also used
/// to describe the charger status, which adds a little bit of confusion. A
/// charger will only return `Charging` or `NotCharging`.
#[derive(Copy, Clone, Debug, PartialEq)]
enum PowerSupplyStatus {
    Unknown,
    Charging,
    Discharging,
    NotCharging,
    Full,
}

impl FromStr for PowerSupplyStatus {
    type Err = anyhow::Error;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let s = s.trim_end();

        match s {
            "Unknown" => Ok(PowerSupplyStatus::Unknown),
            "Charging" => Ok(PowerSupplyStatus::Charging),
            "Discharging" => Ok(PowerSupplyStatus::Discharging),
            "Not charging" => Ok(PowerSupplyStatus::NotCharging),
            "Full" => Ok(PowerSupplyStatus::Full),
            _ => anyhow::bail!("Unknown Power Supply Status: '{}'", s),
        }
    }
}

#[derive(Clone, Debug)]
pub struct DirectoryPowerSourceProvider {
    root: PathBuf,
}

impl DirectoryPowerSourceProvider {
    pub fn new(root: PathBuf) -> Self {
        Self { root }
    }
}

impl PowerSourceProvider for DirectoryPowerSourceProvider {
    /// Iterates through all the power supplies in sysfs and looks for the `online` property.
    /// This indicates an external power source is connected (AC), but it doesn't necessarily
    /// mean it's powering the system. Tests will sometimes disable the charger to get power
    /// measurements. In order to determine if the charger is powering the system we need to
    /// look at the `status` property. If there is no charger connected and powering the system
    /// then we assume we are running off a battery (DC).
    fn get_power_source(&self) -> Result<PowerSourceType> {
        let path = self.root.join(POWER_SUPPLY_PATH);

        if !path.exists() {
            return Ok(PowerSourceType::DC);
        }

        let dirs = path
            .read_dir()
            .with_context(|| format!("Failed to enumerate power supplies in {}", path.display()))?;

        for result in dirs {
            let charger_path = result?;

            let online_path = charger_path.path().join(POWER_SUPPLY_ONLINE);

            if !online_path.exists() {
                continue;
            }

            let online: u32 = read_from_file(&online_path)
                .with_context(|| format!("Error reading online from {}", online_path.display()))?;

            if online != 1 {
                continue;
            }

            let status_path = charger_path.path().join(POWER_SUPPLY_STATUS);

            if !status_path.exists() {
                continue;
            }

            let status_string = read_to_string(&status_path)
                .with_context(|| format!("Error reading status from {}", status_path.display()))?;

            let status_result = PowerSupplyStatus::from_str(&status_string);

            let status = match status_result {
                Err(_) => {
                    info!(
                        "Failure parsing '{}' from {}",
                        status_string,
                        status_path.display()
                    );
                    continue;
                }
                Ok(status) => status,
            };

            if status != PowerSupplyStatus::Charging {
                continue;
            }

            return Ok(PowerSourceType::AC);
        }

        Ok(PowerSourceType::DC)
    }
}

pub trait PowerPreferencesManager {
    /// Chooses a [power preference](PowerPreferences) using the parameters and the
    /// system's current power source. It then applies it to the system.
    ///
    /// If more then one activity is active, the following priority list is used
    /// to determine which [power preference](PowerPreferences) to apply. If there is no
    /// power preference defined for an activity, the next activity in the list will be tried.
    ///
    /// 1) [Borealis Gaming](PowerPreferencesType::BorealisGaming)
    /// 2) [ARCVM Gaming](PowerPreferencesType::ArcvmGaming)
    /// 3) [WebRTC](PowerPreferencesType::WebRTC)
    /// 4) [Fullscreen Video](PowerPreferencesType::Fullscreen)
    /// 5) [VM boot Mode] (PowerPreferencesType::VmBoot)
    ///
    /// The [default](PowerPreferencesType::Default) preference will be applied when no
    /// activity is active.
    fn update_power_preferences(
        &self,
        rtc: common::RTCAudioActive,
        fullscreen: common::FullscreenVideo,
        game: common::GameMode,
        vmboot: common::VmBootMode,
        batterysaver: common::BatterySaverMode,
    ) -> Result<()>;
    fn get_root(&self) -> &Path;
}

fn write_to_cpu_policy_patterns(pattern: &str, new_value: &str) -> Result<()> {
    let mut applied: bool = false;
    let entries: Vec<_> = glob(pattern)?.collect();

    if entries.is_empty() {
        applied = true;
    }

    for entry in entries {
        let policy_path = entry?;
        let mut affected_cpus_path = policy_path.to_path_buf();
        affected_cpus_path.set_file_name("affected_cpus");
        // Skip the policy update if there are no CPUs can be affected by policy.
        // Otherwise, write to the scaling governor may cause error.
        if affected_cpus_path.exists() {
            if let Ok(affected_cpus) = read_to_string(affected_cpus_path) {
                if affected_cpus.trim_end_matches('\n').is_empty() {
                    applied = true;
                    continue;
                }
            }
        }

        // Allow read fail due to CPU may be offlined.
        if let Ok(current_value) = read_to_string(&policy_path) {
            if current_value.trim_end_matches('\n') != new_value {
                std::fs::write(&policy_path, new_value).with_context(|| {
                    format!(
                        "Failed to set attribute to {}, new value: {}",
                        policy_path.display(),
                        new_value
                    )
                })?;
            }
            applied = true;
        }
    }

    // Fail if there are entries in the pattern but nothing is applied
    if !applied {
        bail!("Failed to read any of the pattern {}", pattern);
    }

    Ok(())
}

#[derive(Clone, Debug)]
/// Applies [power preferences](PowerPreferences) to the system by writing to
/// the system's sysfs nodes.
///
/// This struct is using generics for the [ConfigProvider](ConfigProvider) and
/// [PowerSourceProvider] to make unit testing easier.
pub struct DirectoryPowerPreferencesManager<P: PowerSourceProvider> {
    root: PathBuf,
    config_provider: ConfigProvider,
    power_source_provider: P,
}

impl<P: PowerSourceProvider> DirectoryPowerPreferencesManager<P> {
    // The global ondemand parameters are in /sys/devices/system/cpu/cpufreq/ondemand/.
    fn set_global_ondemand_governor_value(&self, attr: &str, value: u32) -> Result<()> {
        let path = self.root.join(GLOBAL_ONDEMAND_PATH).join(attr);

        let current_value_str = read_to_string(&path)
            .with_context(|| format!("Error reading ondemand parameter from {}", path.display()))?;
        let current_value = current_value_str.trim_end_matches('\n').parse::<u32>()?;

        // Check current value before writing to avoid permission error when the new value and
        // current value are the same but resourced didn't own the parameter file.
        if current_value != value {
            std::fs::write(&path, value.to_string()).with_context(|| {
                format!("Error writing {} {} to {}", attr, value, path.display())
            })?;

            info!("Updating ondemand {} to {}", attr, value);
        }

        Ok(())
    }

    // The per-policy ondemand parameters are in /sys/devices/system/cpu/cpufreq/policy*/ondemand/.
    fn set_per_policy_ondemand_governor_value(&self, attr: &str, value: u32) -> Result<()> {
        const ONDEMAND_PATTERN: &str = "sys/devices/system/cpu/cpufreq/policy*/ondemand";
        let pattern = self
            .root
            .join(ONDEMAND_PATTERN)
            .join(attr)
            .to_str()
            .context("Cannot convert ondemand path to string")?
            .to_owned();
        write_to_cpu_policy_patterns(&pattern, &value.to_string())
    }

    fn set_scaling_governor(&self, new_governor: &str) -> Result<()> {
        const GOVERNOR_PATTERN: &str = "sys/devices/system/cpu/cpufreq/policy*/scaling_governor";
        let pattern = self
            .root
            .join(GOVERNOR_PATTERN)
            .to_str()
            .context("Cannot convert scaling_governor path to string")?
            .to_owned();

        write_to_cpu_policy_patterns(&pattern, new_governor)
    }

    fn apply_governor_preferences(&self, governor: Governor) -> Result<()> {
        self.set_scaling_governor(governor.name())?;

        if let Governor::Ondemand {
            powersave_bias,
            sampling_rate,
        } = governor
        {
            let global_path = self.root.join(GLOBAL_ONDEMAND_PATH);

            // There are 2 use cases now:
            // 1. on guybrush, the scaling_governor is always ondemand, the ondemand directory is
            //    chowned to resourced so resourced can set the ondemand parameters.
            // 2. on herobrine, resourced only changes the scaling_governor, so the permission of
            //    the new governor's sysfs nodes doesn't matter.
            // TODO: support changing both the scaling_governor and the governor's parameters.

            // The ondemand tunable could be global (system-wide) or per-policy, depending on the
            // scaling driver in use [1]. The global ondemand tunable is in
            // /sys/devices/system/cpu/cpufreq/ondemand. The per-policy ondemand tunable is in
            // /sys/devices/system/cpu/cpufreq/policy*/ondemand.
            //
            // [1]: https://www.kernel.org/doc/html/latest/admin-guide/pm/cpufreq.html
            if global_path.exists() {
                self.set_global_ondemand_governor_value("powersave_bias", powersave_bias)?;

                if let Some(sampling_rate) = sampling_rate {
                    self.set_global_ondemand_governor_value("sampling_rate", sampling_rate)?;
                }
            } else {
                self.set_per_policy_ondemand_governor_value("powersave_bias", powersave_bias)?;
                if let Some(sampling_rate) = sampling_rate {
                    self.set_per_policy_ondemand_governor_value("sampling_rate", sampling_rate)?;
                }
            }
        }

        Ok(())
    }

    fn has_epp(&self) -> Result<bool> {
        const CPU0_EPP_PATH: &str =
            "sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference";
        let pattern = self
            .root
            .join(CPU0_EPP_PATH)
            .to_str()
            .context("Cannot convert cpu0 epp path to string")?
            .to_owned();
        Ok(Path::new(&pattern).exists())
    }

    fn set_epp(&self, epp: EnergyPerformancePreference) -> Result<()> {
        const EPP_PATTERN: &str =
            "sys/devices/system/cpu/cpufreq/policy*/energy_performance_preference";
        if self.has_epp()? {
            let pattern = self
                .root
                .join(EPP_PATTERN)
                .to_str()
                .context("Cannot convert epp path to string")?
                .to_owned();
            return write_to_cpu_policy_patterns(&pattern, epp.name());
        }
        Ok(())
    }

    fn apply_power_preferences(&self, preferences: PowerPreferences) -> Result<()> {
        if let Some(epp) = preferences.epp {
            self.set_epp(epp)?
        }
        if let Some(governor) = preferences.governor {
            self.apply_governor_preferences(governor)?
        }

        Ok(())
    }

    fn apply_cpu_hotplug(&self, preferences: PowerPreferences) -> Result<()> {
        match preferences.cpu_offline {
            Some(CpuOfflinePreference::Smt { min_active_threads }) => hotplug_cpus(
                self.get_root(),
                HotplugCpuAction::OfflineSMT { min_active_threads },
            )?,
            Some(CpuOfflinePreference::Half { min_active_threads }) => hotplug_cpus(
                self.get_root(),
                HotplugCpuAction::OfflineHalf { min_active_threads },
            )?,
            Some(CpuOfflinePreference::SmallCore { min_active_threads }) => hotplug_cpus(
                self.get_root(),
                HotplugCpuAction::OfflineSmallCore { min_active_threads },
            )?,
            None => {
                hotplug_cpus(self.get_root(), HotplugCpuAction::OnlineAll)?;
            }
        }

        Ok(())
    }
}

impl<P: PowerSourceProvider> PowerPreferencesManager for DirectoryPowerPreferencesManager<P> {
    fn update_power_preferences(
        &self,
        rtc: RTCAudioActive,
        fullscreen: FullscreenVideo,
        game: GameMode,
        vmboot: VmBootMode,
        batterysaver: BatterySaverMode,
    ) -> Result<()> {
        let mut preferences: Option<PowerPreferences> = None;

        let power_source = self.power_source_provider.get_power_source()?;

        info!("Power source {:?}", power_source);

        if batterysaver == BatterySaverMode::Active {
            preferences = self
                .config_provider
                .read_power_preferences(power_source, PowerPreferencesType::BatterySaver)?;
        } else if game == GameMode::Borealis {
            preferences = self
                .config_provider
                .read_power_preferences(power_source, PowerPreferencesType::BorealisGaming)?;
        } else if game == GameMode::Arc {
            preferences = self
                .config_provider
                .read_power_preferences(power_source, PowerPreferencesType::ArcvmGaming)?;
        }

        if preferences.is_none() && rtc == RTCAudioActive::Active {
            preferences = self
                .config_provider
                .read_power_preferences(power_source, PowerPreferencesType::WebRTC)?;
        }

        if preferences.is_none() && fullscreen == FullscreenVideo::Active {
            preferences = self
                .config_provider
                .read_power_preferences(power_source, PowerPreferencesType::Fullscreen)?;
        }

        if preferences.is_none() && vmboot == VmBootMode::Active {
            preferences = self
                .config_provider
                .read_power_preferences(power_source, PowerPreferencesType::VmBoot)?;
        }

        if preferences.is_none() {
            preferences = self
                .config_provider
                .read_power_preferences(power_source, PowerPreferencesType::Default)?;
        }

        if let Some(preferences) = preferences {
            self.apply_cpu_hotplug(preferences)?;
            self.apply_power_preferences(preferences)?
        }

        if power_source == PowerSourceType::DC
            && (rtc == RTCAudioActive::Active || fullscreen == FullscreenVideo::Active)
        {
            if let Err(err) = self.set_epp(EnergyPerformancePreference::BalancePower) {
                error!("Failed to set energy performance preference: {:#}", err);
            }
        } else {
            self.set_epp(EnergyPerformancePreference::BalancePerformance)?;
            // Default EPP
        }

        Ok(())
    }

    fn get_root(&self) -> &Path {
        self.root.as_path()
    }
}

pub fn new_directory_power_preferences_manager(
    root: &Path,
    config_provider: ConfigProvider,
) -> DirectoryPowerPreferencesManager<DirectoryPowerSourceProvider> {
    let root = root.to_path_buf();
    DirectoryPowerPreferencesManager {
        root: root.clone(),
        config_provider,
        power_source_provider: DirectoryPowerSourceProvider::new(root),
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::Path;

    use tempfile::tempdir;

    use super::*;
    use crate::test_utils::*;

    #[test]
    fn test_parse_power_supply_status() {
        assert_eq!(
            PowerSupplyStatus::from_str("Unknown\n").unwrap(),
            PowerSupplyStatus::Unknown
        );
        assert_eq!(
            PowerSupplyStatus::from_str("Charging\n").unwrap(),
            PowerSupplyStatus::Charging
        );
        assert_eq!(
            PowerSupplyStatus::from_str("Discharging\n").unwrap(),
            PowerSupplyStatus::Discharging
        );
        assert_eq!(
            PowerSupplyStatus::from_str("Not charging\n").unwrap(),
            PowerSupplyStatus::NotCharging
        );
        assert_eq!(
            PowerSupplyStatus::from_str("Full\n").unwrap(),
            PowerSupplyStatus::Full
        );

        assert!(PowerSupplyStatus::from_str("").is_err());
        assert!(PowerSupplyStatus::from_str("abc").is_err());
    }

    #[test]
    fn test_power_source_provider_empty_root() {
        let root = tempdir().unwrap();

        let provider = DirectoryPowerSourceProvider {
            root: root.path().to_path_buf(),
        };

        let power_source = provider.get_power_source().unwrap();

        assert_eq!(power_source, PowerSourceType::DC);
    }

    const POWER_SUPPLY_PATH: &str = "sys/class/power_supply";

    #[test]
    fn test_power_source_provider_empty_path() {
        let root = tempdir().unwrap();

        let path = root.path().join(POWER_SUPPLY_PATH);
        fs::create_dir_all(path).unwrap();

        let provider = DirectoryPowerSourceProvider {
            root: root.path().to_path_buf(),
        };

        let power_source = provider.get_power_source().unwrap();

        assert_eq!(power_source, PowerSourceType::DC);
    }

    /// Tests that the `DirectoryPowerSourceProvider` can parse the charger sysfs
    /// `online` and `status` attributes.
    #[test]
    fn test_power_source_provider_disconnected_then_connected() {
        let root = tempdir().unwrap();

        let path = root.path().join(POWER_SUPPLY_PATH);
        fs::create_dir_all(&path).unwrap();

        let provider = DirectoryPowerSourceProvider {
            root: root.path().to_path_buf(),
        };

        let charger = path.join("charger-1");
        fs::create_dir_all(&charger).unwrap();
        let online = charger.join("online");

        fs::write(&online, b"0").unwrap();
        let power_source = provider.get_power_source().unwrap();
        assert_eq!(power_source, PowerSourceType::DC);

        let status = charger.join("status");
        fs::write(&online, b"1").unwrap();
        fs::write(&status, b"Charging\n").unwrap();
        let power_source = provider.get_power_source().unwrap();
        assert_eq!(power_source, PowerSourceType::AC);

        fs::write(&online, b"1").unwrap();
        fs::write(&status, b"Not Charging\n").unwrap();
        let power_source = provider.get_power_source().unwrap();
        assert_eq!(power_source, PowerSourceType::DC);
    }

    struct FakePowerSourceProvider {
        power_source: PowerSourceType,
    }

    impl PowerSourceProvider for FakePowerSourceProvider {
        fn get_power_source(&self) -> Result<PowerSourceType> {
            Ok(self.power_source)
        }
    }

    fn write_global_powersave_bias(root: &Path, value: u32) -> Result<()> {
        let ondemand_path = root.join("sys/devices/system/cpu/cpufreq/ondemand");
        fs::create_dir_all(&ondemand_path)?;

        std::fs::write(
            ondemand_path.join("powersave_bias"),
            value.to_string() + "\n",
        )?;

        Ok(())
    }

    fn read_global_powersave_bias(root: &Path) -> Result<String> {
        let powersave_bias_path = root
            .join("sys/devices/system/cpu/cpufreq/ondemand")
            .join("powersave_bias");

        let mut powersave_bias = std::fs::read_to_string(powersave_bias_path)?;
        if powersave_bias.ends_with('\n') {
            powersave_bias.pop();
        }

        Ok(powersave_bias)
    }

    fn write_global_sampling_rate(root: &Path, value: u32) -> Result<()> {
        let ondemand_path = root.join("sys/devices/system/cpu/cpufreq/ondemand");
        fs::create_dir_all(&ondemand_path)?;

        std::fs::write(ondemand_path.join("sampling_rate"), value.to_string())?;

        Ok(())
    }

    fn read_global_sampling_rate(root: &Path) -> Result<String> {
        let sampling_rate_path = root
            .join("sys/devices/system/cpu/cpufreq/ondemand")
            .join("sampling_rate");

        let mut sampling_rate = std::fs::read_to_string(sampling_rate_path)?;
        if sampling_rate.ends_with('\n') {
            sampling_rate.pop();
        }

        Ok(sampling_rate)
    }

    // In the following per policy access functions, there are 2 cpufreq policies: policy0 and
    // policy1.

    const TEST_CPUFREQ_POLICIES: &[&str] = &[
        "sys/devices/system/cpu/cpufreq/policy0",
        "sys/devices/system/cpu/cpufreq/policy1",
    ];
    const SCALING_GOVERNOR_FILENAME: &str = "scaling_governor";
    const ONDEMAND_DIRECTORY: &str = "ondemand";
    const POWERSAVE_BIAS_FILENAME: &str = "powersave_bias";
    const SAMPLING_RATE_FILENAME: &str = "sampling_rate";
    const AFFECTED_CPUS_NAME: &str = "affected_cpus";
    const AFFECTED_CPU_NONE: &str = "";
    const AFFECTED_CPU0: &str = "0";
    const AFFECTED_CPU1: &str = "1";

    struct PolicyConfigs<'a> {
        policy_path: &'a str,
        governor: &'a Governor,
        affected_cpus: &'a str,
    }
    // Instead of returning an error, crash/assert immediately in a test utility function makes it
    // easier to debug an unittest.
    fn write_per_policy_scaling_governor(root: &Path, policies: Vec<PolicyConfigs>) {
        for policy in policies {
            let policy_path = root.join(policy.policy_path);
            fs::create_dir_all(&policy_path).unwrap();
            std::fs::write(
                policy_path.join(SCALING_GOVERNOR_FILENAME),
                policy.governor.name().to_string() + "\n",
            )
            .unwrap();
            std::fs::write(
                policy_path.join(AFFECTED_CPUS_NAME),
                policy.affected_cpus.to_owned() + "\n",
            )
            .unwrap();
        }
    }

    fn check_per_policy_scaling_governor(root: &Path, expected: Vec<Governor>) {
        for (i, policy) in TEST_CPUFREQ_POLICIES.iter().enumerate() {
            let governor_path = root.join(policy).join(SCALING_GOVERNOR_FILENAME);
            let scaling_governor = std::fs::read_to_string(governor_path).unwrap();
            assert_eq!(scaling_governor.trim_end_matches('\n'), expected[i].name());
        }
    }

    fn write_per_policy_powersave_bias(root: &Path, value: u32) {
        for policy in TEST_CPUFREQ_POLICIES {
            let ondemand_path = root.join(policy).join(ONDEMAND_DIRECTORY);
            println!("ondemand_path: {}", ondemand_path.display());
            fs::create_dir_all(&ondemand_path).unwrap();
            std::fs::write(
                ondemand_path.join(POWERSAVE_BIAS_FILENAME),
                value.to_string() + "\n",
            )
            .unwrap();
        }
    }

    fn check_per_policy_powersave_bias(root: &Path, expected: u32) {
        for policy in TEST_CPUFREQ_POLICIES {
            let powersave_bias_path = root
                .join(policy)
                .join(ONDEMAND_DIRECTORY)
                .join(POWERSAVE_BIAS_FILENAME);
            let powersave_bias = std::fs::read_to_string(powersave_bias_path).unwrap();
            assert_eq!(powersave_bias.trim_end_matches('\n'), expected.to_string());
        }
    }

    fn write_per_policy_sampling_rate(root: &Path, value: u32) {
        for policy in TEST_CPUFREQ_POLICIES {
            let ondemand_path = root.join(policy).join(ONDEMAND_DIRECTORY);
            fs::create_dir_all(&ondemand_path).unwrap();
            std::fs::write(
                ondemand_path.join(SAMPLING_RATE_FILENAME),
                value.to_string(),
            )
            .unwrap();
        }
    }

    fn check_per_policy_sampling_rate(root: &Path, expected: u32) {
        for policy in TEST_CPUFREQ_POLICIES {
            let sampling_rate_path = root
                .join(policy)
                .join(ONDEMAND_DIRECTORY)
                .join(SAMPLING_RATE_FILENAME);
            let sampling_rate = std::fs::read_to_string(sampling_rate_path).unwrap();
            assert_eq!(sampling_rate, expected.to_string());
        }
    }

    fn write_epp(root: &Path, value: &str, affected_cpus: &str) -> Result<()> {
        let policy_path = root.join("sys/devices/system/cpu/cpufreq/policy0");
        fs::create_dir_all(&policy_path)?;

        std::fs::write(policy_path.join("energy_performance_preference"), value)?;
        std::fs::write(policy_path.join("affected_cpus"), affected_cpus)?;

        Ok(())
    }

    fn read_epp(root: &Path) -> Result<String> {
        let epp_path = root
            .join("sys/devices/system/cpu/cpufreq/policy0/")
            .join("energy_performance_preference");

        let epp = std::fs::read_to_string(epp_path)?;

        Ok(epp)
    }

    #[test]
    fn test_power_update_power_preferences_wrong_governor() {
        let root = tempdir().unwrap();

        test_write_cpuset_root_cpus(root.path(), "0-3");
        let power_source_provider = FakePowerSourceProvider {
            power_source: PowerSourceType::AC,
        };

        let fake_config = FakeConfig::new();
        let config_provider = fake_config.provider();

        let manager = DirectoryPowerPreferencesManager {
            root: root.path().to_path_buf(),
            config_provider,
            power_source_provider,
        };

        manager
            .update_power_preferences(
                common::RTCAudioActive::Inactive,
                common::FullscreenVideo::Inactive,
                common::GameMode::Off,
                common::VmBootMode::Inactive,
                common::BatterySaverMode::Inactive,
            )
            .unwrap();

        // We shouldn't have written anything.
        let powersave_bias = read_global_powersave_bias(root.path());
        assert!(powersave_bias.is_err());
    }

    #[test]
    fn test_power_update_power_preferences_none() {
        let root = tempdir().unwrap();

        write_global_powersave_bias(root.path(), 0).unwrap();
        write_global_sampling_rate(root.path(), 2000).unwrap();
        test_write_cpuset_root_cpus(root.path(), "0-3");

        let power_source_provider = FakePowerSourceProvider {
            power_source: PowerSourceType::AC,
        };

        let fake_config = FakeConfig::new();
        let config_provider = fake_config.provider();

        let manager = DirectoryPowerPreferencesManager {
            root: root.path().to_path_buf(),
            config_provider,
            power_source_provider,
        };

        manager
            .update_power_preferences(
                common::RTCAudioActive::Inactive,
                common::FullscreenVideo::Inactive,
                common::GameMode::Off,
                common::VmBootMode::Inactive,
                common::BatterySaverMode::Inactive,
            )
            .unwrap();

        let powersave_bias = read_global_powersave_bias(root.path()).unwrap();
        assert_eq!(powersave_bias, "0");

        let sampling_rate = read_global_sampling_rate(root.path()).unwrap();
        assert_eq!(sampling_rate, "2000");
    }

    #[test]
    fn test_power_update_power_preferences_default_ac() {
        let root = tempdir().unwrap();

        write_global_powersave_bias(root.path(), 0).unwrap();
        write_global_sampling_rate(root.path(), 2000).unwrap();
        test_write_cpuset_root_cpus(root.path(), "0-3");

        let power_source_provider = FakePowerSourceProvider {
            power_source: PowerSourceType::AC,
        };

        let mut fake_config = FakeConfig::new();
        fake_config.write_power_preference(
            PowerSourceType::AC,
            PowerPreferencesType::Default,
            &PowerPreferences {
                governor: Some(Governor::Ondemand {
                    powersave_bias: 200,
                    sampling_rate: Some(16000),
                }),
                epp: None,
                cpu_offline: None,
            },
        );
        let config_provider = fake_config.provider();

        let manager = DirectoryPowerPreferencesManager {
            root: root.path().to_path_buf(),
            config_provider,
            power_source_provider,
        };

        manager
            .update_power_preferences(
                common::RTCAudioActive::Inactive,
                common::FullscreenVideo::Inactive,
                common::GameMode::Off,
                common::VmBootMode::Inactive,
                common::BatterySaverMode::Inactive,
            )
            .unwrap();

        let powersave_bias = read_global_powersave_bias(root.path()).unwrap();
        assert_eq!(powersave_bias, "200");

        let sampling_rate = read_global_sampling_rate(root.path()).unwrap();
        assert_eq!(sampling_rate, "16000");
    }

    #[test]
    fn test_power_update_power_preferences_default_dc() {
        let root = tempdir().unwrap();

        write_global_powersave_bias(root.path(), 0).unwrap();
        write_global_sampling_rate(root.path(), 2000).unwrap();
        test_write_cpuset_root_cpus(root.path(), "0-3");

        let power_source_provider = FakePowerSourceProvider {
            power_source: PowerSourceType::DC,
        };

        let mut fake_config = FakeConfig::new();
        fake_config.write_power_preference(
            PowerSourceType::DC,
            PowerPreferencesType::Default,
            &PowerPreferences {
                governor: Some(Governor::Ondemand {
                    powersave_bias: 200,
                    sampling_rate: None,
                }),
                epp: None,
                cpu_offline: None,
            },
        );
        let config_provider = fake_config.provider();

        let manager = DirectoryPowerPreferencesManager {
            root: root.path().to_path_buf(),
            config_provider,
            power_source_provider,
        };

        manager
            .update_power_preferences(
                common::RTCAudioActive::Inactive,
                common::FullscreenVideo::Inactive,
                common::GameMode::Off,
                common::VmBootMode::Inactive,
                common::BatterySaverMode::Inactive,
            )
            .unwrap();

        let powersave_bias = read_global_powersave_bias(root.path()).unwrap();
        assert_eq!(powersave_bias, "200");

        let sampling_rate = read_global_sampling_rate(root.path()).unwrap();
        assert_eq!(sampling_rate, "2000");
    }

    #[test]
    fn test_power_update_power_preferences_default_rtc_active() {
        let root = tempdir().unwrap();

        write_global_powersave_bias(root.path(), 0).unwrap();
        write_global_sampling_rate(root.path(), 2000).unwrap();
        test_write_cpuset_root_cpus(root.path(), "0-3");

        let power_source_provider = FakePowerSourceProvider {
            power_source: PowerSourceType::AC,
        };

        let mut fake_config = FakeConfig::new();
        fake_config.write_power_preference(
            PowerSourceType::AC,
            PowerPreferencesType::Default,
            &PowerPreferences {
                governor: Some(Governor::Ondemand {
                    powersave_bias: 200,
                    sampling_rate: Some(4000),
                }),
                epp: None,
                cpu_offline: None,
            },
        );
        let config_provider = fake_config.provider();

        let manager = DirectoryPowerPreferencesManager {
            root: root.path().to_path_buf(),
            config_provider,
            power_source_provider,
        };

        manager
            .update_power_preferences(
                common::RTCAudioActive::Active,
                common::FullscreenVideo::Inactive,
                common::GameMode::Off,
                common::VmBootMode::Inactive,
                common::BatterySaverMode::Inactive,
            )
            .unwrap();

        let powersave_bias = read_global_powersave_bias(root.path()).unwrap();
        assert_eq!(powersave_bias, "200");

        let sampling_rate = read_global_sampling_rate(root.path()).unwrap();
        assert_eq!(sampling_rate, "4000");
    }

    #[test]
    fn test_power_update_power_preferences_rtc_active() {
        let root = tempdir().unwrap();

        write_global_powersave_bias(root.path(), 0).unwrap();
        write_global_sampling_rate(root.path(), 2000).unwrap();
        test_write_cpuset_root_cpus(root.path(), "0-3");

        let power_source_provider = FakePowerSourceProvider {
            power_source: PowerSourceType::AC,
        };

        let mut fake_config = FakeConfig::new();
        fake_config.write_power_preference(
            PowerSourceType::AC,
            PowerPreferencesType::WebRTC,
            &PowerPreferences {
                governor: Some(Governor::Ondemand {
                    powersave_bias: 200,
                    sampling_rate: Some(16000),
                }),
                epp: None,
                cpu_offline: None,
            },
        );
        let config_provider = fake_config.provider();

        let manager = DirectoryPowerPreferencesManager {
            root: root.path().to_path_buf(),
            config_provider,
            power_source_provider,
        };

        manager
            .update_power_preferences(
                common::RTCAudioActive::Active,
                common::FullscreenVideo::Inactive,
                common::GameMode::Off,
                common::VmBootMode::Inactive,
                common::BatterySaverMode::Inactive,
            )
            .unwrap();

        let powersave_bias = read_global_powersave_bias(root.path()).unwrap();
        assert_eq!(powersave_bias, "200");

        let sampling_rate = read_global_sampling_rate(root.path()).unwrap();
        assert_eq!(sampling_rate, "16000");
    }

    #[test]
    /// Tests default battery saver mode
    fn test_power_update_power_preferences_battery_saver_active() {
        let temp_dir = tempdir().unwrap();
        let root = temp_dir.path();

        test_write_cpuset_root_cpus(root, "0-3");

        let power_source_provider = FakePowerSourceProvider {
            power_source: PowerSourceType::AC,
        };

        let mut fake_config = FakeConfig::new();
        fake_config.write_power_preference(
            PowerSourceType::AC,
            PowerPreferencesType::ArcvmGaming,
            &PowerPreferences {
                governor: Some(Governor::Schedutil),
                epp: None,
                cpu_offline: None,
            },
        );
        fake_config.write_power_preference(
            PowerSourceType::AC,
            PowerPreferencesType::BatterySaver,
            &PowerPreferences {
                governor: Some(Governor::Conservative),
                epp: None,
                cpu_offline: None,
            },
        );
        fake_config.write_power_preference(
            PowerSourceType::AC,
            PowerPreferencesType::Default,
            &PowerPreferences {
                governor: Some(Governor::Schedutil),
                epp: None,
                cpu_offline: None,
            },
        );
        let config_provider = fake_config.provider();

        let manager = DirectoryPowerPreferencesManager {
            root: root.to_path_buf(),
            config_provider,
            power_source_provider,
        };

        let tests = [
            (
                BatterySaverMode::Active,
                "balance_performance",
                Governor::Conservative,
                AFFECTED_CPU0, // policy0 affected_cpus
                AFFECTED_CPU1, // policy1 affected_cpus
            ),
            (
                BatterySaverMode::Inactive,
                "balance_performance",
                Governor::Schedutil,
                AFFECTED_CPU0, // policy0 affected_cpus
                AFFECTED_CPU1, // policy1 affected_cpus
            ),
            (
                BatterySaverMode::Active,
                "balance_performance",
                Governor::Conservative,
                AFFECTED_CPU_NONE, // policy0 affected_cpus, which has no affected cpus
                AFFECTED_CPU1,     // policy1 affected_cpus
            ),
        ];

        // Test device without EPP path
        for test in tests {
            let orig_governor = Governor::Performance;
            let policy0 = PolicyConfigs {
                policy_path: TEST_CPUFREQ_POLICIES[0],
                governor: &orig_governor,
                affected_cpus: test.3,
            };
            let policy1 = PolicyConfigs {
                policy_path: TEST_CPUFREQ_POLICIES[1],
                governor: &orig_governor,
                affected_cpus: test.4,
            };
            let policies = vec![policy0, policy1];
            write_per_policy_scaling_governor(root, policies);
            manager
                .update_power_preferences(
                    common::RTCAudioActive::Inactive,
                    common::FullscreenVideo::Inactive,
                    common::GameMode::Arc,
                    common::VmBootMode::Inactive,
                    test.0,
                )
                .unwrap();

            let mut expected_governors = vec![test.2, test.2];
            if test.3.is_empty() {
                expected_governors[0] = orig_governor;
            }
            check_per_policy_scaling_governor(root, expected_governors);
        }

        // Test device with EPP path
        let orig_epp = "balance_performance";
        for test in tests {
            write_epp(root, orig_epp, test.3).unwrap();
            manager
                .update_power_preferences(
                    common::RTCAudioActive::Inactive,
                    common::FullscreenVideo::Inactive,
                    common::GameMode::Arc,
                    common::VmBootMode::Inactive,
                    test.0,
                )
                .unwrap();

            let epp = read_epp(root).unwrap();
            let mut expected_epp = test.1;
            if test.3.is_empty() {
                expected_epp = orig_epp;
            }
            assert_eq!(epp, expected_epp);
        }
    }

    #[test]
    fn test_apply_hotplug_cpus() {
        struct Test<'a> {
            cpus: &'a str,
            big_little: bool,
            cluster1_state: [&'a str; 2],
            cluster2_state: [&'a str; 2],
            cluster1_freq: [u32; 2],
            cluster2_freq: [u32; 2],
            preferences: PowerPreferences,
            smt_offlined: bool,
            smt_orig_state: &'a str,
            cluster1_expected_state: [&'a str; 2],
            cluster2_expected_state: [&'a str; 2],
            smt_expected_state: &'a str,
        }

        let tests = [
            // Test offline small core
            Test {
                cpus: "0-3",
                big_little: true,
                cluster1_state: ["1"; 2],
                cluster2_state: ["1"; 2],
                cluster1_freq: [2400000; 2],
                cluster2_freq: [1800000; 2],
                preferences: PowerPreferences {
                    governor: Some(Governor::Conservative),
                    epp: None,
                    cpu_offline: Some(CpuOfflinePreference::SmallCore {
                        min_active_threads: 2,
                    }),
                },
                smt_offlined: false,
                smt_orig_state: "on",
                cluster1_expected_state: ["1"; 2],
                cluster2_expected_state: ["0"; 2],
                smt_expected_state: "",
            },
            // Test offline SMT
            Test {
                cpus: "0-3",
                big_little: false,
                cluster1_state: ["1"; 2],
                cluster2_state: ["1"; 2],
                cluster1_freq: [2400000; 2],
                cluster2_freq: [2400000; 2],
                preferences: PowerPreferences {
                    governor: None,
                    epp: None,
                    cpu_offline: Some(CpuOfflinePreference::Smt {
                        min_active_threads: 2,
                    }),
                },
                smt_offlined: true,
                smt_orig_state: "on",
                cluster1_expected_state: ["1"; 2],
                cluster2_expected_state: ["0"; 2],
                smt_expected_state: "off",
            },
            // Test offline half
            Test {
                cpus: "0-3",
                big_little: false,
                cluster1_state: ["1"; 2],
                cluster2_state: ["1"; 2],
                cluster1_freq: [2400000; 2],
                cluster2_freq: [2400000; 2],
                preferences: PowerPreferences {
                    governor: Some(Governor::Conservative),
                    epp: None,
                    cpu_offline: Some(CpuOfflinePreference::Half {
                        min_active_threads: 2,
                    }),
                },
                smt_offlined: false,
                smt_orig_state: "on",
                cluster1_expected_state: ["1"; 2],
                cluster2_expected_state: ["0"; 2],
                smt_expected_state: "off",
            },
            // Test online all
            Test {
                cpus: "0-3",
                big_little: false,
                cluster1_state: ["1"; 2],
                cluster2_state: ["0"; 2],
                cluster1_freq: [2400000; 2],
                cluster2_freq: [2400000; 2],
                preferences: PowerPreferences {
                    governor: None,
                    epp: None,
                    cpu_offline: None,
                },
                smt_offlined: false,
                smt_orig_state: "on",
                cluster1_expected_state: ["1"; 2],
                cluster2_expected_state: ["1"; 2],
                smt_expected_state: "off",
            },
        ];

        for test in tests {
            //Setup
            let temp_dir = tempdir().unwrap();
            let root = temp_dir.path();
            let fake_config = FakeConfig::new();
            let config_provider = fake_config.provider();
            let manager = DirectoryPowerPreferencesManager {
                root: root.to_path_buf(),
                config_provider,
                power_source_provider: FakePowerSourceProvider {
                    power_source: PowerSourceType::DC,
                },
            };
            test_write_cpuset_root_cpus(root, test.cpus);
            test_write_smt_control(root, test.smt_orig_state);
            // Setup core cpus list for two physical cores and two virtual cores
            test_write_core_cpus_list(root, 0, "0,2");
            test_write_core_cpus_list(root, 1, "1,3");
            test_write_core_cpus_list(root, 2, "0,2");
            test_write_core_cpus_list(root, 3, "1,3");

            if test.big_little {
                test_write_ui_use_flags(root, "big_little");
            }

            for (i, freq) in test.cluster1_freq.iter().enumerate() {
                test_write_online_cpu(root, i.try_into().unwrap(), test.cluster1_state[i]);
                test_write_cpu_max_freq(root, i.try_into().unwrap(), *freq);
            }
            for (i, freq) in test.cluster2_freq.iter().enumerate() {
                test_write_online_cpu(
                    root,
                    (test.cluster1_freq.len() + i).try_into().unwrap(),
                    test.cluster2_state[i],
                );
                test_write_cpu_max_freq(
                    root,
                    (test.cluster1_freq.len() + i).try_into().unwrap(),
                    *freq,
                );
            }

            // Call function to test
            manager.apply_cpu_hotplug(test.preferences).unwrap();

            // Check result.
            if test.smt_offlined {
                // The mock sysfs cannot offline the SMT CPUs, here to check the smt control state
                test_check_smt_control(root, test.smt_expected_state);
                continue;
            }

            for (i, state) in test.cluster1_expected_state.iter().enumerate() {
                test_check_online_cpu(root, i.try_into().unwrap(), state);
            }

            for (i, state) in test.cluster2_expected_state.iter().enumerate() {
                test_check_online_cpu(
                    root,
                    (test.cluster1_expected_state.len() + i).try_into().unwrap(),
                    state,
                );
            }
        }
    }

    #[test]
    /// Tests the various EPP permutations
    fn test_power_update_power_preferences_epp() {
        let root = tempdir().unwrap();

        write_epp(root.path(), "balance_performance", AFFECTED_CPU0).unwrap();
        test_write_cpuset_root_cpus(root.path(), "0-3");

        let tests = [
            (
                FakePowerSourceProvider {
                    power_source: PowerSourceType::DC,
                },
                RTCAudioActive::Active,
                FullscreenVideo::Inactive,
                "balance_power",
            ),
            (
                FakePowerSourceProvider {
                    power_source: PowerSourceType::DC,
                },
                RTCAudioActive::Inactive,
                FullscreenVideo::Active,
                "balance_power",
            ),
            (
                FakePowerSourceProvider {
                    power_source: PowerSourceType::DC,
                },
                RTCAudioActive::Active,
                FullscreenVideo::Active,
                "balance_power",
            ),
            (
                FakePowerSourceProvider {
                    power_source: PowerSourceType::DC,
                },
                RTCAudioActive::Inactive,
                FullscreenVideo::Inactive,
                "balance_performance",
            ),
            (
                FakePowerSourceProvider {
                    power_source: PowerSourceType::AC,
                },
                RTCAudioActive::Inactive,
                FullscreenVideo::Active,
                "balance_performance",
            ),
            (
                FakePowerSourceProvider {
                    power_source: PowerSourceType::AC,
                },
                RTCAudioActive::Active,
                FullscreenVideo::Active,
                "balance_performance",
            ),
            (
                FakePowerSourceProvider {
                    power_source: PowerSourceType::AC,
                },
                RTCAudioActive::Active,
                FullscreenVideo::Inactive,
                "balance_performance",
            ),
        ];

        for test in tests {
            let mut fake_config = FakeConfig::new();
            fake_config.write_power_preference(
                PowerSourceType::AC,
                PowerPreferencesType::Default,
                &PowerPreferences {
                    governor: Some(Governor::Schedutil),
                    epp: None,
                    cpu_offline: None,
                },
            );
            let config_provider = fake_config.provider();

            let manager = DirectoryPowerPreferencesManager {
                root: root.path().to_path_buf(),
                config_provider,
                power_source_provider: test.0,
            };

            manager
                .update_power_preferences(
                    test.1,
                    test.2,
                    common::GameMode::Off,
                    common::VmBootMode::Inactive,
                    common::BatterySaverMode::Inactive,
                )
                .unwrap();

            let epp = read_epp(root.path()).unwrap();

            assert_eq!(epp, test.3);
        }
    }

    #[test]
    fn test_power_update_power_preferences_fullscreen_active() {
        let root = tempdir().unwrap();

        write_global_powersave_bias(root.path(), 0).unwrap();
        write_global_sampling_rate(root.path(), 2000).unwrap();
        test_write_cpuset_root_cpus(root.path(), "0-3");

        let power_source_provider = FakePowerSourceProvider {
            power_source: PowerSourceType::AC,
        };

        let mut fake_config = FakeConfig::new();
        fake_config.write_power_preference(
            PowerSourceType::AC,
            PowerPreferencesType::Fullscreen,
            &PowerPreferences {
                governor: Some(Governor::Ondemand {
                    powersave_bias: 200,
                    sampling_rate: Some(16000),
                }),
                epp: None,
                cpu_offline: None,
            },
        );
        let config_provider = fake_config.provider();

        let manager = DirectoryPowerPreferencesManager {
            root: root.path().to_path_buf(),
            config_provider,
            power_source_provider,
        };

        manager
            .update_power_preferences(
                common::RTCAudioActive::Inactive,
                common::FullscreenVideo::Active,
                common::GameMode::Off,
                common::VmBootMode::Inactive,
                common::BatterySaverMode::Inactive,
            )
            .unwrap();

        let powersave_bias = read_global_powersave_bias(root.path()).unwrap();
        assert_eq!(powersave_bias, "200");

        let sampling_rate = read_global_sampling_rate(root.path()).unwrap();
        assert_eq!(sampling_rate, "16000");
    }

    #[test]
    fn test_power_update_power_preferences_borealis_gaming_active() {
        let root = tempdir().unwrap();

        write_global_powersave_bias(root.path(), 0).unwrap();
        write_global_sampling_rate(root.path(), 2000).unwrap();
        test_write_cpuset_root_cpus(root.path(), "0-3");

        let power_source_provider = FakePowerSourceProvider {
            power_source: PowerSourceType::AC,
        };

        let mut fake_config = FakeConfig::new();
        fake_config.write_power_preference(
            PowerSourceType::AC,
            PowerPreferencesType::BorealisGaming,
            &PowerPreferences {
                governor: Some(Governor::Ondemand {
                    powersave_bias: 200,
                    sampling_rate: Some(16000),
                }),
                epp: None,
                cpu_offline: None,
            },
        );
        let config_provider = fake_config.provider();

        let manager = DirectoryPowerPreferencesManager {
            root: root.path().to_path_buf(),
            config_provider,
            power_source_provider,
        };

        manager
            .update_power_preferences(
                common::RTCAudioActive::Inactive,
                common::FullscreenVideo::Inactive,
                common::GameMode::Borealis,
                common::VmBootMode::Inactive,
                common::BatterySaverMode::Inactive,
            )
            .unwrap();

        let powersave_bias = read_global_powersave_bias(root.path()).unwrap();
        assert_eq!(powersave_bias, "200");

        let sampling_rate = read_global_sampling_rate(root.path()).unwrap();
        assert_eq!(sampling_rate, "16000");
    }

    #[test]
    fn test_power_update_power_preferences_arcvm_gaming_active() {
        let root = tempdir().unwrap();

        write_global_powersave_bias(root.path(), 0).unwrap();
        write_global_sampling_rate(root.path(), 2000).unwrap();
        test_write_cpuset_root_cpus(root.path(), "0-3");

        let power_source_provider = FakePowerSourceProvider {
            power_source: PowerSourceType::AC,
        };

        let mut fake_config = FakeConfig::new();
        fake_config.write_power_preference(
            PowerSourceType::AC,
            PowerPreferencesType::ArcvmGaming,
            &PowerPreferences {
                governor: Some(Governor::Ondemand {
                    powersave_bias: 200,
                    sampling_rate: Some(16000),
                }),
                epp: None,
                cpu_offline: None,
            },
        );
        let config_provider = fake_config.provider();

        let manager = DirectoryPowerPreferencesManager {
            root: root.path().to_path_buf(),
            config_provider,
            power_source_provider,
        };
        manager
            .update_power_preferences(
                common::RTCAudioActive::Inactive,
                common::FullscreenVideo::Inactive,
                common::GameMode::Arc,
                common::VmBootMode::Inactive,
                common::BatterySaverMode::Inactive,
            )
            .unwrap();

        let powersave_bias = read_global_powersave_bias(root.path()).unwrap();
        assert_eq!(powersave_bias, "200");

        let sampling_rate = read_global_sampling_rate(root.path()).unwrap();
        assert_eq!(sampling_rate, "16000");
    }

    #[test]
    fn test_per_policy_ondemand_governor() {
        let temp_dir = tempdir().unwrap();
        let root = temp_dir.path();

        const INIT_POWERSAVE_BIAS: u32 = 0;
        const INIT_SAMPLING_RATE: u32 = 2000;
        const CONFIG_POWERSAVE_BIAS: u32 = 200;
        const CONFIG_SAMPLING_RATE: u32 = 16000;

        let ondemand = Governor::Ondemand {
            powersave_bias: INIT_POWERSAVE_BIAS,
            sampling_rate: Some(INIT_SAMPLING_RATE),
        };
        let policy0 = PolicyConfigs {
            policy_path: TEST_CPUFREQ_POLICIES[0],
            governor: &ondemand,
            affected_cpus: "0",
        };
        let policy1 = PolicyConfigs {
            policy_path: TEST_CPUFREQ_POLICIES[1],
            governor: &ondemand,
            affected_cpus: "1",
        };
        let policies = vec![policy0, policy1];
        write_per_policy_scaling_governor(root, policies);
        write_per_policy_powersave_bias(root, INIT_POWERSAVE_BIAS);
        write_per_policy_sampling_rate(root, INIT_SAMPLING_RATE);
        test_write_cpuset_root_cpus(root, "0-3");

        let power_source_provider = FakePowerSourceProvider {
            power_source: PowerSourceType::AC,
        };

        let mut fake_config = FakeConfig::new();
        fake_config.write_power_preference(
            PowerSourceType::AC,
            PowerPreferencesType::ArcvmGaming,
            &PowerPreferences {
                governor: Some(Governor::Ondemand {
                    powersave_bias: CONFIG_POWERSAVE_BIAS,
                    sampling_rate: Some(CONFIG_SAMPLING_RATE),
                }),
                epp: None,
                cpu_offline: None,
            },
        );
        let config_provider = fake_config.provider();

        let manager = DirectoryPowerPreferencesManager {
            root: root.to_path_buf(),
            config_provider,
            power_source_provider,
        };
        manager
            .update_power_preferences(
                common::RTCAudioActive::Inactive,
                common::FullscreenVideo::Inactive,
                common::GameMode::Arc,
                common::VmBootMode::Inactive,
                common::BatterySaverMode::Inactive,
            )
            .unwrap();
        check_per_policy_scaling_governor(root, vec![ondemand, ondemand]);
        check_per_policy_powersave_bias(root, CONFIG_POWERSAVE_BIAS);
        check_per_policy_sampling_rate(root, CONFIG_SAMPLING_RATE);
    }

    #[test]
    fn test_scaling_governors() {
        let temp_dir = tempdir().unwrap();
        let root = temp_dir.path();

        test_write_cpuset_root_cpus(root, "0-3");
        const INIT_POWERSAVE_BIAS: u32 = 0;
        const INIT_SAMPLING_RATE: u32 = 2000;

        let ondemand = Governor::Ondemand {
            powersave_bias: INIT_POWERSAVE_BIAS,
            sampling_rate: Some(INIT_SAMPLING_RATE),
        };
        let policy0 = PolicyConfigs {
            policy_path: TEST_CPUFREQ_POLICIES[0],
            governor: &ondemand,
            affected_cpus: AFFECTED_CPU0,
        };
        let policy1 = PolicyConfigs {
            policy_path: TEST_CPUFREQ_POLICIES[1],
            governor: &ondemand,
            affected_cpus: AFFECTED_CPU1,
        };
        let policies = vec![policy0, policy1];
        write_per_policy_scaling_governor(root, policies);

        let governors = [
            Governor::Conservative,
            Governor::Performance,
            Governor::Powersave,
            Governor::Schedutil,
            Governor::Userspace,
        ];

        for governor in governors {
            let power_source_provider = FakePowerSourceProvider {
                power_source: PowerSourceType::AC,
            };
            let mut fake_config = FakeConfig::new();
            fake_config.write_power_preference(
                PowerSourceType::AC,
                PowerPreferencesType::ArcvmGaming,
                &PowerPreferences {
                    governor: Some(governor),
                    epp: None,
                    cpu_offline: None,
                },
            );
            let config_provider = fake_config.provider();
            let manager = DirectoryPowerPreferencesManager {
                root: root.to_path_buf(),
                config_provider,
                power_source_provider,
            };

            manager
                .update_power_preferences(
                    common::RTCAudioActive::Inactive,
                    common::FullscreenVideo::Inactive,
                    common::GameMode::Arc,
                    common::VmBootMode::Inactive,
                    common::BatterySaverMode::Inactive,
                )
                .unwrap();

            check_per_policy_scaling_governor(root, vec![governor, governor]);
        }
    }
}
