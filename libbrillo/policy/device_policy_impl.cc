// Copyright 2012 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "policy/device_policy_impl.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <base/check.h>
#include <base/containers/adapters.h>
#include <base/files/file_util.h>
#include <base/json/json_reader.h>
#include <base/logging.h>
#include <base/memory/ptr_util.h>
#include <base/strings/string_number_conversions.h>
#include <base/time/time.h>
#include <base/values.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include "bindings/chrome_device_policy.pb.h"
#include "bindings/device_management_backend.pb.h"
#include "policy/device_local_account_policy_util.h"
#include "policy/device_policy.h"
#include "policy/policy_util.h"
#include "policy/resilient_policy_util.h"

namespace em = enterprise_management;

namespace policy {

// TODO(crbug.com/984789): Remove once support for OpenSSL <1.1 is dropped.
#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define EVP_MD_CTX_new EVP_MD_CTX_create
#define EVP_MD_CTX_free EVP_MD_CTX_destroy
#endif

// Maximum value of RollbackAllowedMilestones policy.
const int kMaxRollbackAllowedMilestones = 4;

namespace {
const char kPolicyPath[] = "/var/lib/devicesettings/policy";
const char kPublicKeyPath[] = "/var/lib/devicesettings/owner.key";

// Reads the public key used to sign the policy from |key_file| and stores it
// in |public_key|. Returns true on success.
bool ReadPublicKeyFromFile(const base::FilePath& key_file,
                           std::string* public_key) {
  if (!base::PathExists(key_file))
    return false;
  public_key->clear();
  if (!base::ReadFileToString(key_file, public_key) || public_key->empty()) {
    LOG(ERROR) << "Could not read public key off disk";
    return false;
  }
  return true;
}

// Verifies that the |signed_data| has correct |signature| with |public_key|
// against |signature_type|.
// |signature_type| of em::PolicyFetchRequest::NONE is rejected.
bool VerifySignature(
    const std::string& signed_data,
    const std::string& signature,
    const std::string& public_key,
    const em::PolicyFetchRequest::SignatureType signature_type) {
  std::unique_ptr<EVP_MD_CTX, void (*)(EVP_MD_CTX*)> ctx(EVP_MD_CTX_new(),
                                                         EVP_MD_CTX_free);
  if (!ctx)
    return false;

  const EVP_MD* digest_type = nullptr;
  switch (signature_type) {
    case em::PolicyFetchRequest::SHA256_RSA:
      digest_type = EVP_sha256();
      break;
    case em::PolicyFetchRequest::SHA1_RSA:
      digest_type = EVP_sha1();
      break;
    default:
      // Treat `signature_type` of `em::PolicyFetchRequest::NONE` as unsigned,
      // which is not supported.
      LOG(ERROR) << "Unexpected signature_type: " << signature_type;
      return false;
  }

  char* key = const_cast<char*>(public_key.data());
  BIO* bio = BIO_new_mem_buf(key, public_key.length());
  if (!bio)
    return false;

  EVP_PKEY* public_key_ssl = d2i_PUBKEY_bio(bio, nullptr);
  if (!public_key_ssl) {
    BIO_free_all(bio);
    return false;
  }

  const unsigned char* sig =
      reinterpret_cast<const unsigned char*>(signature.data());
  int rv = EVP_VerifyInit_ex(ctx.get(), digest_type, nullptr);
  if (rv == 1) {
    EVP_VerifyUpdate(ctx.get(), signed_data.data(), signed_data.length());
    rv = EVP_VerifyFinal(ctx.get(), sig, signature.length(), public_key_ssl);
  }

  EVP_PKEY_free(public_key_ssl);
  BIO_free_all(bio);

  return rv == 1;
}

// Decodes the connection type enum from the device settings protobuf to string
// representations. The strings must match the connection manager definitions.
std::string DecodeConnectionType(int type) {
  static const char* const kConnectionTypes[] = {
      "ethernet", "wifi", "wimax", "bluetooth", "cellular",
  };

  if (type < 0 || type >= static_cast<int>(std::size(kConnectionTypes)))
    return std::string();

  return kConnectionTypes[type];
}

std::optional<int> ConvertDayOfWeekStringToInt(
    const std::string& day_of_week_str) {
  if (day_of_week_str == "Sunday")
    return 0;
  if (day_of_week_str == "Monday")
    return 1;
  if (day_of_week_str == "Tuesday")
    return 2;
  if (day_of_week_str == "Wednesday")
    return 3;
  if (day_of_week_str == "Thursday")
    return 4;
  if (day_of_week_str == "Friday")
    return 5;
  if (day_of_week_str == "Saturday")
    return 6;
  return std::nullopt;
}

bool DecodeWeeklyTimeFromValue(const base::Value::Dict& dict_value,
                               int* day_of_week_out,
                               base::TimeDelta* time_out) {
  const std::string* day_of_week_str = dict_value.FindString("day_of_week");
  if (!day_of_week_str) {
    LOG(ERROR) << "Day of the week is absent.";
    return false;
  }

  std::optional<int> day_of_week =
      ConvertDayOfWeekStringToInt(*day_of_week_str);
  if (!day_of_week.has_value()) {
    LOG(ERROR) << "Undefined day of the week: " << *day_of_week_str;
    return false;
  }
  *day_of_week_out = *day_of_week;

  const std::optional<int> hours = dict_value.FindInt("hours");
  if (!hours.has_value() || hours < 0 || hours > 23) {
    LOG(ERROR) << "Hours are absent or are outside of the range [0, 24).";
    return false;
  }

  const std::optional<int> minutes = dict_value.FindInt("minutes");
  if (!minutes.has_value() || minutes < 0 || minutes > 59) {
    LOG(ERROR) << "Minutes are absent or are outside the range [0, 60)";
    return false;
  }

  *time_out = base::Minutes(*minutes) + base::Hours(*hours);
  return true;
}

std::optional<base::Value> DecodeListValueFromJSON(
    const std::string& json_string) {
  auto decoded_json = base::JSONReader::ReadAndReturnValueWithError(
      json_string, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!decoded_json.has_value()) {
    LOG(ERROR) << "Invalid JSON string: " << decoded_json.error().message;
    return std::nullopt;
  }

  if (!decoded_json->is_list()) {
    LOG(ERROR) << "JSON string is not a list";
    return std::nullopt;
  }

  return std::move(*decoded_json);
}

std::optional<base::Value> DecodeDictValueFromJSON(
    const std::string& json_string, const std::string& entry_name) {
  auto decoded_json = base::JSONReader::ReadAndReturnValueWithError(
      json_string, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!decoded_json.has_value()) {
    LOG(ERROR) << "Invalid JSON string in " << entry_name << ": "
               << decoded_json.error().message;
    return std::nullopt;
  }

  if (!decoded_json->is_dict()) {
    LOG(ERROR) << "Invalid JSON string in " << entry_name << ": "
               << "JSON string is not a dictionary";
    return std::nullopt;
  }

  return std::move(*decoded_json);
}
}  // namespace

DevicePolicyImpl::DevicePolicyImpl()
    : policy_path_(kPolicyPath),
      keyfile_path_(kPublicKeyPath),
      policy_(std::make_unique<enterprise_management::PolicyFetchResponse>()),
      policy_data_(std::make_unique<enterprise_management::PolicyData>()),
      device_policy_(std::make_unique<
                     enterprise_management::ChromeDeviceSettingsProto>()) {}

DevicePolicyImpl::~DevicePolicyImpl() {}

bool DevicePolicyImpl::LoadPolicy(bool delete_invalid_files) {
  std::map<int, base::FilePath> sorted_policy_file_paths =
      policy::GetSortedResilientPolicyFilePaths(policy_path_);
  number_of_policy_files_ = sorted_policy_file_paths.size();
  number_of_invalid_files_ = 0;
  if (sorted_policy_file_paths.empty())
    return false;

  // Try to load the existent policy files one by one in reverse order of their
  // index until we succeed. The default policy, if present, appears as index 0
  // in the map and is loaded the last. This is intentional as that file is the
  // oldest.
  bool policy_loaded = false;
  for (const auto& map_pair : base::Reversed(sorted_policy_file_paths)) {
    const base::FilePath& policy_path = map_pair.second;
    if (LoadPolicyFromFile(policy_path)) {
      policy_loaded = true;
      break;
    }
    if (delete_invalid_files) {
      LOG(ERROR) << "Invalid device policy file: " << policy_path.value();
      base::DeleteFile(policy_path);
    }
    number_of_invalid_files_++;
  }

  return policy_loaded;
}

bool DevicePolicyImpl::IsEnterpriseEnrolled() const {
  DCHECK(install_attributes_reader_);
  if (!install_attributes_reader_->IsLocked())
    return false;

  const std::string& device_mode = install_attributes_reader_->GetAttribute(
      InstallAttributesReader::kAttrMode);
  return device_mode == InstallAttributesReader::kDeviceModeEnterprise;
}

std::optional<int> DevicePolicyImpl::GetPolicyRefreshRate() const {
  if (!device_policy_->has_device_policy_refresh_rate())
    return std::nullopt;
  return static_cast<int>(device_policy_->device_policy_refresh_rate()
                              .device_policy_refresh_rate());
}

std::optional<bool> DevicePolicyImpl::GetMetricsEnabled() const {
  if (!device_policy_->has_metrics_enabled()) {
    // Default for enterprise managed devices is true, cf. https://crbug/456186.
    if (IsEnterpriseManaged()) {
      return true;
    }
    return std::nullopt;
  }
  return device_policy_->metrics_enabled().metrics_enabled();
}

std::optional<bool> DevicePolicyImpl::GetUnenrolledHwDataUsageEnabled() const {
  if (!device_policy_->has_hardware_data_usage_enabled())
    return std::nullopt;

  const em::RevenDeviceHWDataUsageEnabledProto& proto =
      device_policy_->hardware_data_usage_enabled();
  if (!proto.has_hardware_data_usage_enabled())
    return std::nullopt;

  return proto.hardware_data_usage_enabled();
}

std::optional<bool> DevicePolicyImpl::GetEnrolledHwDataUsageEnabled() const {
  // This policy only applies to enrolled devices.
  if (!IsEnterpriseEnrolled())
    return std::nullopt;

  // The default for this policy is supposed to be 'true', but the `default`
  // key in the policy definition doesn't make that happen for CrOS device
  // policies. Instead we need to enforce it ourselves, here.
  // Only return false if we can read the policy and it's disabled; ignore it
  // if the proto is missing.
  if (!device_policy_
           ->has_device_flex_hw_data_for_product_improvement_enabled())
    return true;

  const em::DeviceFlexHwDataForProductImprovementEnabledProto& proto =
      device_policy_->device_flex_hw_data_for_product_improvement_enabled();
  if (!proto.has_enabled())
    return true;

  return proto.enabled();
}

bool DevicePolicyImpl::GetEphemeralSettings(
    EphemeralSettings* ephemeral_settings) const {
  if (!device_policy_->has_ephemeral_users_enabled() &&
      !device_policy_->has_device_local_accounts())
    return false;

  ephemeral_settings->global_ephemeral_users_enabled = false;
  ephemeral_settings->specific_ephemeral_users.clear();
  ephemeral_settings->specific_nonephemeral_users.clear();

  if (device_policy_->has_device_local_accounts()) {
    const em::DeviceLocalAccountsProto& local_accounts =
        device_policy_->device_local_accounts();

    for (const em::DeviceLocalAccountInfoProto& account :
         local_accounts.account()) {
      if (!account.has_ephemeral_mode()) {
        continue;
      }

      if (account.ephemeral_mode() ==
          em::DeviceLocalAccountInfoProto::EPHEMERAL_MODE_DISABLE) {
        ephemeral_settings->specific_nonephemeral_users.push_back(
            GenerateDeviceLocalAccountUserId(account.account_id(),
                                             account.type()));
      } else if (account.ephemeral_mode() ==
                 em::DeviceLocalAccountInfoProto::EPHEMERAL_MODE_ENABLE) {
        ephemeral_settings->specific_ephemeral_users.push_back(
            GenerateDeviceLocalAccountUserId(account.account_id(),
                                             account.type()));
      }
    }
  }
  if (device_policy_->has_ephemeral_users_enabled()) {
    ephemeral_settings->global_ephemeral_users_enabled =
        device_policy_->ephemeral_users_enabled().ephemeral_users_enabled();
  }

  return true;
}

std::optional<bool> DevicePolicyImpl::GetDeviceExtendedAutoUpdateEnabled()
    const {
  if (!device_policy_->has_deviceextendedautoupdateenabled()) {
    return std::nullopt;
  }

  const em::BooleanPolicyProto& proto(
      device_policy_->deviceextendedautoupdateenabled());
  if (!proto.has_value()) {
    return std::nullopt;
  }

  return proto.value();
}

bool DevicePolicyImpl::GetReleaseChannel(std::string* release_channel) const {
  if (!device_policy_->has_release_channel())
    return false;

  const em::ReleaseChannelProto& proto = device_policy_->release_channel();
  if (!proto.has_release_channel())
    return false;

  *release_channel = proto.release_channel();
  return true;
}

bool DevicePolicyImpl::GetReleaseChannelDelegated(
    bool* release_channel_delegated) const {
  if (!device_policy_->has_release_channel())
    return false;

  const em::ReleaseChannelProto& proto = device_policy_->release_channel();
  if (!proto.has_release_channel_delegated())
    return false;

  *release_channel_delegated = proto.release_channel_delegated();
  return true;
}

bool DevicePolicyImpl::GetReleaseLtsTag(std::string* lts_tag) const {
  if (!device_policy_->has_release_channel())
    return false;

  const em::ReleaseChannelProto& proto = device_policy_->release_channel();
  if (!proto.has_release_lts_tag())
    return false;

  *lts_tag = proto.release_lts_tag();
  return true;
}

bool DevicePolicyImpl::GetUpdateDisabled(bool* update_disabled) const {
  if (!IsEnterpriseEnrolled())
    return false;

  if (!device_policy_->has_auto_update_settings())
    return false;

  const em::AutoUpdateSettingsProto& proto =
      device_policy_->auto_update_settings();
  if (!proto.has_update_disabled())
    return false;

  *update_disabled = proto.update_disabled();
  return true;
}

bool DevicePolicyImpl::GetTargetVersionPrefix(
    std::string* target_version_prefix) const {
  if (!IsEnterpriseEnrolled())
    return false;

  if (!device_policy_->has_auto_update_settings())
    return false;

  const em::AutoUpdateSettingsProto& proto =
      device_policy_->auto_update_settings();
  if (!proto.has_target_version_prefix())
    return false;

  *target_version_prefix = proto.target_version_prefix();
  return true;
}

bool DevicePolicyImpl::GetRollbackToTargetVersion(
    int* rollback_to_target_version) const {
  if (!IsEnterpriseEnrolled())
    return false;

  if (!device_policy_->has_auto_update_settings())
    return false;

  const em::AutoUpdateSettingsProto& proto =
      device_policy_->auto_update_settings();
  if (!proto.has_rollback_to_target_version())
    return false;

  // TODO(b:273305614): Allow to enable enterprise rollback on Flex with a flag.
  if (USE_ENTERPRISE_ROLLBACK_REVEN) {
    LOG(INFO) << "Enterprise Rollback disabled for Flex, setting policy to "
                 "undefined.";
    return false;
  }

  *rollback_to_target_version = proto.rollback_to_target_version();
  return true;
}

bool DevicePolicyImpl::GetRollbackAllowedMilestones(
    int* rollback_allowed_milestones) const {
  // This policy can be only set for devices which are enterprise enrolled.
  if (!IsEnterpriseEnrolled())
    return false;

  if (device_policy_->has_auto_update_settings()) {
    const em::AutoUpdateSettingsProto& proto =
        device_policy_->auto_update_settings();
    if (proto.has_rollback_allowed_milestones()) {
      // Policy is set, enforce minimum and maximum constraints.
      *rollback_allowed_milestones = proto.rollback_allowed_milestones();
      if (*rollback_allowed_milestones < 0)
        *rollback_allowed_milestones = 0;
      if (*rollback_allowed_milestones > kMaxRollbackAllowedMilestones)
        *rollback_allowed_milestones = kMaxRollbackAllowedMilestones;
      return true;
    }
  }
  // Policy is not present, use default for enterprise devices.
  VLOG(1) << "RollbackAllowedMilestones policy is not set, using default "
          << kMaxRollbackAllowedMilestones << ".";
  *rollback_allowed_milestones = kMaxRollbackAllowedMilestones;
  return true;
}

bool DevicePolicyImpl::GetScatterFactorInSeconds(
    int64_t* scatter_factor_in_seconds) const {
  if (!device_policy_->has_auto_update_settings())
    return false;

  const em::AutoUpdateSettingsProto& proto =
      device_policy_->auto_update_settings();
  if (!proto.has_scatter_factor_in_seconds())
    return false;

  *scatter_factor_in_seconds = proto.scatter_factor_in_seconds();
  return true;
}

bool DevicePolicyImpl::GetAllowedConnectionTypesForUpdate(
    std::set<std::string>* connection_types) const {
  if (!IsEnterpriseEnrolled())
    return false;

  if (!device_policy_->has_auto_update_settings())
    return false;

  const em::AutoUpdateSettingsProto& proto =
      device_policy_->auto_update_settings();
  if (proto.allowed_connection_types_size() <= 0)
    return false;

  for (int i = 0; i < proto.allowed_connection_types_size(); i++) {
    std::string type = DecodeConnectionType(proto.allowed_connection_types(i));
    if (!type.empty())
      connection_types->insert(type);
  }
  return true;
}

bool DevicePolicyImpl::GetOwner(std::string* owner) const {
  if (IsEnterpriseManaged()) {
    *owner = "";
    return true;
  }

  if (!policy_data_->has_username())
    return false;
  *owner = policy_data_->username();
  return true;
}

bool DevicePolicyImpl::GetHttpDownloadsEnabled(
    bool* http_downloads_enabled) const {
  if (!device_policy_->has_auto_update_settings())
    return false;

  const em::AutoUpdateSettingsProto& proto =
      device_policy_->auto_update_settings();

  if (!proto.has_http_downloads_enabled())
    return false;

  *http_downloads_enabled = proto.http_downloads_enabled();
  return true;
}

bool DevicePolicyImpl::GetAuP2PEnabled(bool* au_p2p_enabled) const {
  if (!device_policy_->has_auto_update_settings())
    return false;

  const em::AutoUpdateSettingsProto& proto =
      device_policy_->auto_update_settings();

  if (!proto.has_p2p_enabled())
    return false;

  *au_p2p_enabled = proto.p2p_enabled();
  return true;
}

bool DevicePolicyImpl::GetAllowKioskAppControlChromeVersion(
    bool* allow_kiosk_app_control_chrome_version) const {
  if (!device_policy_->has_allow_kiosk_app_control_chrome_version())
    return false;

  const em::AllowKioskAppControlChromeVersionProto& proto =
      device_policy_->allow_kiosk_app_control_chrome_version();

  if (!proto.has_allow_kiosk_app_control_chrome_version())
    return false;

  *allow_kiosk_app_control_chrome_version =
      proto.allow_kiosk_app_control_chrome_version();
  return true;
}

bool DevicePolicyImpl::GetUsbDetachableWhitelist(
    std::vector<UsbDeviceId>* usb_whitelist) const {
  const bool has_allowlist =
      device_policy_->has_usb_detachable_allowlist() &&
      device_policy_->usb_detachable_allowlist().id_size() != 0;
  const bool has_whitelist =
      device_policy_->has_usb_detachable_whitelist() &&
      device_policy_->usb_detachable_whitelist().id_size() != 0;

  if (!has_allowlist && !has_whitelist)
    return false;

  usb_whitelist->clear();
  if (has_allowlist) {
    const em::UsbDetachableAllowlistProto& proto =
        device_policy_->usb_detachable_allowlist();
    for (int i = 0; i < proto.id_size(); i++) {
      const em::UsbDeviceIdInclusiveProto& id = proto.id(i);
      UsbDeviceId dev_id;
      dev_id.vendor_id = id.has_vendor_id() ? id.vendor_id() : 0;
      dev_id.product_id = id.has_product_id() ? id.product_id() : 0;
      usb_whitelist->push_back(dev_id);
    }
  } else {
    const em::UsbDetachableWhitelistProto& proto =
        device_policy_->usb_detachable_whitelist();
    for (int i = 0; i < proto.id_size(); i++) {
      const em::UsbDeviceIdProto& id = proto.id(i);
      UsbDeviceId dev_id;
      dev_id.vendor_id = id.has_vendor_id() ? id.vendor_id() : 0;
      dev_id.product_id = id.has_product_id() ? id.product_id() : 0;
      usb_whitelist->push_back(dev_id);
    }
  }
  return true;
}

bool DevicePolicyImpl::GetDeviceUpdateStagingSchedule(
    std::vector<DayPercentagePair>* staging_schedule_out) const {
  staging_schedule_out->clear();

  if (!device_policy_->has_auto_update_settings())
    return false;

  const em::AutoUpdateSettingsProto& proto =
      device_policy_->auto_update_settings();

  if (!proto.has_staging_schedule())
    return false;

  std::optional<base::Value> list_val =
      DecodeListValueFromJSON(proto.staging_schedule());
  if (!list_val)
    return false;

  for (const auto& pair_value : list_val->GetList()) {
    if (!pair_value.is_dict())
      return false;
    auto& dict = pair_value.GetDict();
    std::optional<int> days = dict.FindInt("days");
    std::optional<int> percentage = dict.FindInt("percentage");
    if (!days.has_value() || !percentage.has_value())
      return false;
    // Limit the percentage to [0, 100] and days to [1, 28];
    staging_schedule_out->push_back({std::max(std::min(*days, 28), 1),
                                     std::max(std::min(*percentage, 100), 0)});
  }

  return true;
}

bool DevicePolicyImpl::IsEnterpriseManaged() const {
  if (policy_data_->has_management_mode())
    return policy_data_->management_mode() ==
           em::PolicyData::ENTERPRISE_MANAGED;
  // Fall back to checking the request token, see management_mode documentation
  // in device_management_backend.proto.
  return policy_data_->has_request_token();
}

bool DevicePolicyImpl::GetSecondFactorAuthenticationMode(int* mode_out) const {
  if (!device_policy_->has_device_second_factor_authentication())
    return false;

  const em::DeviceSecondFactorAuthenticationProto& proto =
      device_policy_->device_second_factor_authentication();

  if (!proto.has_mode())
    return false;

  *mode_out = proto.mode();
  return true;
}

std::optional<bool> DevicePolicyImpl::GetRunAutomaticCleanupOnLogin() const {
  // Only runs on enterprise devices.
  if (!IsEnterpriseEnrolled())
    return {};

  if (!device_policy_->has_device_run_automatic_cleanup_on_login())
    return {};

  const em::BooleanPolicyProto& proto =
      device_policy_->device_run_automatic_cleanup_on_login();

  if (!proto.has_value())
    return {};

  return proto.value();
}

bool DevicePolicyImpl::GetDisallowedTimeIntervals(
    std::vector<WeeklyTimeInterval>* intervals_out) const {
  intervals_out->clear();
  if (!IsEnterpriseEnrolled())
    return false;

  if (!device_policy_->has_auto_update_settings()) {
    return false;
  }

  const em::AutoUpdateSettingsProto& proto =
      device_policy_->auto_update_settings();

  if (!proto.has_disallowed_time_intervals()) {
    return false;
  }

  std::optional<base::Value> list_val =
      DecodeListValueFromJSON(proto.disallowed_time_intervals());
  if (!list_val)
    return false;

  for (const auto& interval_value : list_val->GetList()) {
    if (!interval_value.is_dict()) {
      LOG(ERROR) << "Invalid JSON string given. Interval is not a dict.";
      return false;
    }

    const base::Value::Dict& interval_value_dict = interval_value.GetDict();
    const base::Value::Dict* start = interval_value_dict.FindDict("start");
    const base::Value::Dict* end = interval_value_dict.FindDict("end");
    if (!start || !end) {
      LOG(ERROR) << "Interval is missing start/end.";
      return false;
    }
    WeeklyTimeInterval weekly_interval;
    if (!DecodeWeeklyTimeFromValue(*start, &weekly_interval.start_day_of_week,
                                   &weekly_interval.start_time) ||
        !DecodeWeeklyTimeFromValue(*end, &weekly_interval.end_day_of_week,
                                   &weekly_interval.end_time)) {
      return false;
    }

    intervals_out->push_back(weekly_interval);
  }
  return true;
}

bool DevicePolicyImpl::GetDeviceQuickFixBuildToken(
    std::string* device_quick_fix_build_token) const {
  if (!IsEnterpriseEnrolled() || !device_policy_->has_auto_update_settings())
    return false;

  const em::AutoUpdateSettingsProto& proto =
      device_policy_->auto_update_settings();
  if (!proto.has_device_quick_fix_build_token())
    return false;

  *device_quick_fix_build_token = proto.device_quick_fix_build_token();
  return true;
}

bool DevicePolicyImpl::GetDeviceDirectoryApiId(
    std::string* directory_api_id_out) const {
  if (!policy_data_->has_directory_api_id())
    return false;

  *directory_api_id_out = policy_data_->directory_api_id();
  return true;
}

bool DevicePolicyImpl::GetCustomerId(std::string* customer_id_out) const {
  if (!policy_data_->has_obfuscated_customer_id())
    return false;

  *customer_id_out = policy_data_->obfuscated_customer_id();
  return true;
}

bool DevicePolicyImpl::GetChannelDowngradeBehavior(
    int* channel_downgrade_behavior_out) const {
  if (!device_policy_->has_auto_update_settings())
    return false;

  const em::AutoUpdateSettingsProto& proto =
      device_policy_->auto_update_settings();
  if (!proto.has_channel_downgrade_behavior())
    return false;

  *channel_downgrade_behavior_out = proto.channel_downgrade_behavior();
  return true;
}

bool DevicePolicyImpl::GetHighestDeviceMinimumVersion(
    base::Version* version_out) const {
  if (!IsEnterpriseEnrolled())
    return false;

  if (!device_policy_->has_device_minimum_version())
    return false;

  const em::StringPolicyProto& policy_string(
      device_policy_->device_minimum_version());
  if (!policy_string.has_value())
    return false;

  const std::optional<base::Value> decoded_policy =
      DecodeDictValueFromJSON(policy_string.value(), "device_minimum_version");
  if (!decoded_policy)
    return false;

  const base::Value::List* requirements_entries =
      decoded_policy->GetDict().FindList("requirements");
  if (!requirements_entries || requirements_entries->empty())
    return false;

  base::Version highest_version("0");
  bool valid_version_found = false;
  for (const auto& version_value : *requirements_entries) {
    if (!version_value.is_dict()) {
      LOG(WARNING) << "Invalid JSON string given. Version is not a dictionary.";
      continue;
    }
    const auto& version_value_dict = version_value.GetDict();

    const std::string* version_str =
        version_value_dict.FindString("chromeos_version");
    if (!version_str) {
      LOG(WARNING) << " Invalid JSON string given. Version is missing.";
      continue;
    }

    base::Version version(*version_str);
    if (!version.IsValid()) {
      LOG(WARNING) << "Invalid JSON string given. String is not a version.";
      continue;
    }

    if (version > highest_version) {
      valid_version_found = true;
      highest_version = version;
    }
  }

  if (!valid_version_found) {
    LOG(ERROR) << "No valid entry found in device_minimum_version";
    return false;
  }

  *version_out = highest_version;
  return true;
}

bool DevicePolicyImpl::GetDeviceMarketSegment(
    DeviceMarketSegment* device_market_segment) const {
  if (!policy_data_->has_market_segment()) {
    return false;
  }

  em::PolicyData::MarketSegment market_segment = policy_data_->market_segment();
  switch (market_segment) {
    case em::PolicyData::MARKET_SEGMENT_UNSPECIFIED:
      *device_market_segment = DeviceMarketSegment::kUnknown;
      break;
    case em::PolicyData::ENROLLED_EDUCATION:
      *device_market_segment = DeviceMarketSegment::kEducation;
      break;
    case em::PolicyData::ENROLLED_ENTERPRISE:
      *device_market_segment = DeviceMarketSegment::kEnterprise;
      break;
    default:
      LOG(ERROR) << "MarketSegment enum value has changed!";
      *device_market_segment = DeviceMarketSegment::kUnknown;
  }

  return true;
}

bool DevicePolicyImpl::GetDeviceDebugPacketCaptureAllowed(bool* allowed) const {
  if (!device_policy_->has_device_debug_packet_capture_allowed())
    return false;

  const em::DeviceDebugPacketCaptureAllowedProto& proto =
      device_policy_->device_debug_packet_capture_allowed();

  if (!proto.has_allowed())
    return false;

  *allowed = proto.allowed();
  return true;
}

bool DevicePolicyImpl::GetDeviceKeylockerForStorageEncryptionEnabled(
    bool* keylocker_enabled) const {
  if (!device_policy_->has_keylocker_for_storage_encryption_enabled())
    return false;

  *keylocker_enabled =
      device_policy_->keylocker_for_storage_encryption_enabled()
          .has_enabled() &&
      device_policy_->keylocker_for_storage_encryption_enabled().enabled();
  return true;
}

std::optional<bool> DevicePolicyImpl::GetDeviceReportXDREvents() const {
  if (!device_policy_->has_device_report_xdr_events())
    return {};

  const em::DeviceReportXDREventsProto& proto =
      device_policy_->device_report_xdr_events();

  if (!proto.has_enabled()) {
    return {};
  }
  return proto.enabled();
}

bool DevicePolicyImpl::VerifyPolicyFile(const base::FilePath& policy_path) {
  if (!verify_root_ownership_) {
    return true;
  }

  // Both the policy and its signature have to exist.
  if (!base::PathExists(policy_path) || !base::PathExists(keyfile_path_)) {
    return false;
  }

  // Check if the policy and signature file is owned by root.
  struct stat file_stat;
  stat(policy_path.value().c_str(), &file_stat);
  if (file_stat.st_uid != 0) {
    LOG(ERROR) << "Policy file is not owned by root!";
    return false;
  }
  stat(keyfile_path_.value().c_str(), &file_stat);
  if (file_stat.st_uid != 0) {
    LOG(ERROR) << "Policy signature file is not owned by root!";
    return false;
  }
  return true;
}

bool DevicePolicyImpl::VerifyPolicySignature() {
  if (policy_->has_policy_data_signature()) {
    std::string policy_data = policy_->policy_data();
    std::string policy_data_signature = policy_->policy_data_signature();
    std::string public_key;
    if (!ReadPublicKeyFromFile(base::FilePath(keyfile_path_), &public_key)) {
      LOG(ERROR) << "Could not read owner key off disk";
      return false;
    }

    em::PolicyFetchRequest::SignatureType signature_type =
        em::PolicyFetchRequest::SHA1_RSA;
    // Use `policy_data_signature_type` field to determine which algorithm
    // to use.
    // In some cases the field is missing, but the blob is still signed
    // with SHA1_RSA (e.g. device owner settings). That's why we default to
    // SHA1_RSA.
    if (policy_->has_policy_data_signature_type()) {
      signature_type = policy_->policy_data_signature_type();
    }
    if (!VerifySignature(policy_data, policy_data_signature, public_key,
                         signature_type)) {
      LOG(ERROR) << "Failed to verify against signature_type: "
                 << signature_type << ". "
                 << "Signature does not match the data or can not be verified!";

      return false;
    }
    return true;
  }
  LOG(ERROR) << "The policy blob is not signed!";
  return false;
}

bool DevicePolicyImpl::LoadPolicyFromFile(const base::FilePath& policy_path) {
  std::string policy_data_str;
  if (policy::LoadPolicyFromPath(policy_path, &policy_data_str,
                                 policy_.get()) != LoadPolicyResult::kSuccess) {
    return false;
  }
  if (!policy_->has_policy_data()) {
    LOG(ERROR) << "Policy on disk could not be parsed!";
    return false;
  }
  if (!policy_data_->ParseFromString(policy_->policy_data()) ||
      !policy_data_->has_policy_value()) {
    LOG(ERROR) << "Policy data could not be parsed!";
    return false;
  }

  if (!install_attributes_reader_) {
    install_attributes_reader_ = std::make_unique<InstallAttributesReader>();
  }

  if (verify_policy_ && !VerifyPolicyFile(policy_path)) {
    return false;
  }

  // Make sure the signature is still valid.
  if (verify_policy_ && !VerifyPolicySignature()) {
    LOG(ERROR) << "Policy signature verification failed!";
    return false;
  }

  // The policy data must have a DMToken if the device is managed.
  if (!policy_data_->has_request_token() && IsEnterpriseEnrolled()) {
    LOG(ERROR) << "Enrolled policy has no DMToken!";
    return false;
  }
  if (!device_policy_->ParseFromString(policy_data_->policy_value())) {
    LOG(ERROR) << "Policy on disk could not be parsed!";
    return false;
  }

  return true;
}

// Methods that can be used only for testing.
void DevicePolicyImpl::set_policy_data_for_testing(
    const enterprise_management::PolicyData& policy_data) {
  policy_data_ =
      std::make_unique<enterprise_management::PolicyData>(policy_data);
}
void DevicePolicyImpl::set_verify_root_ownership_for_testing(
    bool verify_root_ownership) {
  verify_root_ownership_ = verify_root_ownership;
}
void DevicePolicyImpl::set_install_attributes_for_testing(
    std::unique_ptr<InstallAttributesReader> install_attributes_reader) {
  install_attributes_reader_ = std::move(install_attributes_reader);
}
void DevicePolicyImpl::set_policy_for_testing(
    const enterprise_management::ChromeDeviceSettingsProto& device_policy) {
  device_policy_ =
      std::make_unique<enterprise_management::ChromeDeviceSettingsProto>(
          device_policy);
}
void DevicePolicyImpl::set_policy_path_for_testing(
    const base::FilePath& policy_path) {
  policy_path_ = policy_path;
}
void DevicePolicyImpl::set_key_file_path_for_testing(
    const base::FilePath& keyfile_path) {
  keyfile_path_ = keyfile_path;
}
void DevicePolicyImpl::set_verify_policy_for_testing(bool value) {
  verify_policy_ = value;
}

}  // namespace policy
