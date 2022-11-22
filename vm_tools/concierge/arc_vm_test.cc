// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vm_tools/concierge/arc_vm.h"

#include <string>

#include <base/containers/contains.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <base/test/scoped_chromeos_version_info.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libcrossystem/crossystem_fake.h>

namespace vm_tools {
namespace concierge {
namespace {
constexpr int kSeneschalServerPort = 3000;
constexpr int kLcdDensity = 160;
}  // namespace

TEST(ArcVmTest, NonDevModeKernelParams) {
  crossystem::fake::CrossystemFake cros_system;
  cros_system.VbSetSystemPropertyInt("cros_debug", 0);
  StartArcVmRequest request;
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.dev_mode=0"));
  EXPECT_TRUE(base::Contains(params, "androidboot.disable_runas=1"));
}

TEST(ArcVmTest, DevModeKernelParams) {
  crossystem::fake::CrossystemFake cros_system;
  cros_system.VbSetSystemPropertyInt("cros_debug", 1);
  StartArcVmRequest request;
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.dev_mode=1"));
  EXPECT_TRUE(base::Contains(params, "androidboot.disable_runas=0"));
}

TEST(ArcVmTest, SeneschalServerPortParam) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params, base::StringPrintf("androidboot.seneschal_server_port=%d",
                                 kSeneschalServerPort)));
}

TEST(ArcVmTest, EnableConsumerAutoUpdateToggleParamTrue) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_enable_consumer_auto_update_toggle(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params, "androidboot.enable_consumer_auto_update_toggle=1"));
}

TEST(ArcVmTest, EnableConsumerAutoUpdateToggleParamFalse) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_enable_consumer_auto_update_toggle(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params, "androidboot.enable_consumer_auto_update_toggle=0"));
}

TEST(ArcVmTest, ArcFilePickerParamTrue) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_arc_file_picker_experiment(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.arc_file_picker=1"));
}

TEST(ArcVmTest, ArcFilePickerParamFalse) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_arc_file_picker_experiment(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.arc_file_picker=0"));
}

TEST(ArcVmTest, CustomTabsParamTrue) {
  base::test::ScopedChromeOSVersionInfo info(
      "CHROMEOS_RELEASE_TRACK=canary-channel", base::Time::Now());
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_arc_custom_tabs_experiment(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.arc_custom_tabs=1"));
}

TEST(ArcVmTest, CustomTabsParamFalse) {
  base::test::ScopedChromeOSVersionInfo info(
      "CHROMEOS_RELEASE_TRACK=canary-channel", base::Time::Now());
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_arc_custom_tabs_experiment(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.arc_custom_tabs=0"));
}

TEST(ArcVmTest, CustomTabsParamStableChannel) {
  base::test::ScopedChromeOSVersionInfo info(
      "CHROMEOS_RELEASE_TRACK=stable-channel", base::Time::Now());
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_arc_custom_tabs_experiment(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.arc_custom_tabs=1"));
}

TEST(ArcVmTest, KeyboardShortcutHelperParamTrue) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_enable_keyboard_shortcut_helper_integration(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params, "androidboot.keyboard_shortcut_helper_integration=1"));
}

TEST(ArcVmTest, KeyboardShortcutHelperParamFalse) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_enable_keyboard_shortcut_helper_integration(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params, "androidboot.keyboard_shortcut_helper_integration=0"));
}

TEST(ArcVmTest, EnableNotificationsRefreshParamTrue) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_enable_notifications_refresh(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.enable_notifications_refresh=1"));
}

TEST(ArcVmTest, EnableNotificationsRefreshParamFalse) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_enable_notifications_refresh(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.enable_notifications_refresh=0"));
}

TEST(ArcVmTest, EnableTtsCachingParamTrue) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_enable_tts_caching(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.arc.tts.caching=1"));
}

TEST(ArcVmTest, EnableTtsCachingParamFalse) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_enable_tts_caching(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_FALSE(base::Contains(params, "androidboot.arc.tts.caching=1"));
}

TEST(ArcVmTest, EnableGmscoreLmkProtectionParamTrue) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_enable_gmscore_lmk_protection(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params, "androidboot.arc_enable_gmscore_lmk_protection=1"));
}

TEST(ArcVmTest, EnableGmscoreLmkProtectionParamFalse) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_enable_gmscore_lmk_protection(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_FALSE(base::Contains(
      params, "androidboot.arc_enable_gmscore_lmk_protection=1"));
}

TEST(ArcVmTest, EnableVirtioBlockDataParamTrue) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_enable_virtio_blk_data(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.arcvm_virtio_blk_data=1"));
}

TEST(ArcVmTest, EnableVirtioBlockDataParamFalse) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_enable_virtio_blk_data(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.arcvm_virtio_blk_data=0"));
}

TEST(ArcVmTest, EnableBroadcastAnrPrenotifyTrue) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_enable_broadcast_anr_prenotify(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.arc.broadcast_anr_prenotify=1"));
}

TEST(ArcVmTest, EnableBroadcastAnrPrenotifyFalse) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_enable_broadcast_anr_prenotify(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_FALSE(
      base::Contains(params, "androidboot.arc.broadcast_anr_prenotify=1"));
}

TEST(ArcVmTest, VmMemoryPSIReports) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_vm_memory_psi_period(300);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.arcvm_metrics_mem_psi_period=300"));
}

TEST(ArcVmTest, VmMemoryPSIReportsDefault) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_vm_memory_psi_period(-1);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  for (const auto& param : params) {
    EXPECT_FALSE(
        base::StartsWith(param, "androidboot.arcvm_metrics_mem_psi_period="));
  }
}

TEST(ArcVmTest, DisableMediaStoreMaintenanceTrue) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_disable_media_store_maintenance(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.disable_media_store_maintenance=1"));
}

TEST(ArcVmTest, DisableMediaStoreMaintenanceFalse) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_disable_media_store_maintenance(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_FALSE(
      base::Contains(params, "androidboot.disable_media_store_maintenance=1"));
}

TEST(ArcVmTest, ArcGeneratePlayAutoInstallTrue) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_arc_generate_pai(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.arc_generate_pai=1"));
}

TEST(ArcVmTest, ArcGeneratePlayAutoInstallFalse) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_arc_generate_pai(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_FALSE(base::Contains(params, "androidboot.arc_generate_pai=1"));
}

TEST(ArcVmTest, DisableDownloadProviderTrue) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_disable_download_provider(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.disable_download_provider=1"));
}

TEST(ArcVmTest, DisableDownloadProviderFalse) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_disable_download_provider(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_FALSE(
      base::Contains(params, "androidboot.disable_download_provider=1"));
}

TEST(ArcVmTest, GuestZramSize0) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_guest_zram_size(0);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.zram_size=0"));
}

TEST(ArcVmTest, GuestZramSize100) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_guest_zram_size(100);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.zram_size=100"));
}

TEST(ArcVmTest, LogdConfigSizeSmall) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_logd_config_size(256);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.arcvm.logd.size=256K"));
}

TEST(ArcVmTest, LogdConfigSizeMed) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_logd_config_size(512);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.arcvm.logd.size=512K"));
}

TEST(ArcVmTest, LogdConfigSizeLarge) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_logd_config_size(1024);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.arcvm.logd.size=1M"));
}

TEST(ArcVmTest, LogdConfigSizeInvalid) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_logd_config_size(0);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  for (const auto& param : params) {
    EXPECT_FALSE(base::StartsWith(param, "androidboot.arcvm.logd.size="));
  }
}

TEST(ArcVmTest, ChromeOsChannelStable) {
  base::test::ScopedChromeOSVersionInfo info(
      "CHROMEOS_RELEASE_TRACK=stable-channel", base::Time::Now());
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.chromeos_channel=stable"));
}

TEST(ArcVmTest, ChromeOsChannelTestImage) {
  base::test::ScopedChromeOSVersionInfo info(
      "CHROMEOS_RELEASE_TRACK=testimage-channel", base::Time::Now());
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params, "androidboot.vshd_service_override=vshd_for_test"));
}

TEST(ArcVmTest, ChromeOsChannelUnknown) {
  base::test::ScopedChromeOSVersionInfo info("CHROMEOS_RELEASE_TRACK=invalid",
                                             base::Time::Now());
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.chromeos_channel=unknown"));
}

TEST(ArcVmTest, PanelOrientation) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_panel_orientation(StartArcVmRequest::ORIENTATION_180);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params, "androidboot.arc.primary_display_rotation=ORIENTATION_180"));
}

TEST(ArcVmTest, IioservicePresentParam) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params,
      base::StringPrintf("androidboot.iioservice_present=%d", USE_IIOSERVICE)));
}

TEST(ArcVmTest, SwappinessNotPresentByDefault) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  for (const auto& oneParam : params) {
    EXPECT_FALSE(base::StartsWith(oneParam, "sysctl.vm.swappiness="));
  }
}

TEST(ArcVmTest, SwappinessPresentParam) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_guest_swappiness(55);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params, base::StringPrintf("sysctl.vm.swappiness=%d", 55)));
}

TEST(ArcVmTest, MglruReclaimIntervalDisabled) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_mglru_reclaim_interval(0);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  for (const auto& param : params) {
    EXPECT_FALSE(
        base::StartsWith(param, "androidboot.arcvm_mglru_reclaim_interval="));
  }
}

TEST(ArcVmTest, MglruReclaimWithoutSwappiness) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_mglru_reclaim_interval(30000);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params,
      base::StringPrintf("androidboot.arcvm_mglru_reclaim_interval=30000")));
  EXPECT_TRUE(base::Contains(
      params,
      base::StringPrintf("androidboot.arcvm_mglru_reclaim_swappiness=0")));
}

TEST(ArcVmTest, MglruReclaimWithSwappiness) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_mglru_reclaim_interval(30000);
  request.set_mglru_reclaim_swappiness(100);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params,
      base::StringPrintf("androidboot.arcvm_mglru_reclaim_interval=30000")));
  EXPECT_TRUE(base::Contains(
      params,
      base::StringPrintf("androidboot.arcvm_mglru_reclaim_swappiness=100")));
}

TEST(ArcVmTest, UpdateO4CListViaA2C2Param) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  {
    request.set_update_o4c_list_via_a2c2(true);
    std::vector<std::string> params =
        ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
    EXPECT_TRUE(
        base::Contains(params, "androidboot.update_o4c_list_via_a2c2=1"));
  }
  {
    request.set_update_o4c_list_via_a2c2(false);
    std::vector<std::string> params =
        ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
    EXPECT_TRUE(
        base::Contains(params, "androidboot.update_o4c_list_via_a2c2=0"));
  }
}

TEST(ArcVmTest, NativeBridgeExperimentNone) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_native_bridge_experiment(
      vm_tools::concierge::StartArcVmRequest::BINARY_TRANSLATION_TYPE_NONE);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.native_bridge=0"));
}

TEST(ArcVmTest, NativeBridgeExperimentHoudini) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_native_bridge_experiment(
      vm_tools::concierge::StartArcVmRequest::BINARY_TRANSLATION_TYPE_HOUDINI);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.native_bridge=libhoudini.so"));
}

TEST(ArcVmTest, NativeBridgeExperimentNdkTranslation) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_native_bridge_experiment(
      vm_tools::concierge::StartArcVmRequest::
          BINARY_TRANSLATION_TYPE_NDK_TRANSLATION);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params, "androidboot.native_bridge=libndk_translation.so"));
}

TEST(ArcVmTest, UsapProfileDefault) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_usap_profile(
      vm_tools::concierge::StartArcVmRequest::USAP_PROFILE_DEFAULT);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  for (const auto& oneParam : params) {
    EXPECT_FALSE(base::StartsWith(oneParam, "androidboot.usap_profile="));
  }
}

TEST(ArcVmTest, UsapProfile4G) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_usap_profile(
      vm_tools::concierge::StartArcVmRequest::USAP_PROFILE_4G);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.usap_profile=4G"));
}

TEST(ArcVmTest, UsapProfile8G) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_usap_profile(
      vm_tools::concierge::StartArcVmRequest::USAP_PROFILE_8G);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.usap_profile=8G"));
}

TEST(ArcVmTest, UsapProfile16G) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_usap_profile(
      vm_tools::concierge::StartArcVmRequest::USAP_PROFILE_16G);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.usap_profile=16G"));
}

TEST(ArcVmTest, PlayStoreAutoUpdateDefault) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_play_store_auto_update(
      arc::StartArcMiniInstanceRequest::AUTO_UPDATE_DEFAULT);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  for (const auto& oneParam : params) {
    EXPECT_FALSE(
        base::StartsWith(oneParam, "androidboot.play_store_auto_update="));
  }
}

TEST(ArcVmTest, PlayStoreAutoUpdateON) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_play_store_auto_update(
      arc::StartArcMiniInstanceRequest::AUTO_UPDATE_ON);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.play_store_auto_update=1"));
}

TEST(ArcVmTest, PlayStoreAutoUpdateOFF) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_play_store_auto_update(
      arc::StartArcMiniInstanceRequest::AUTO_UPDATE_OFF);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.play_store_auto_update=0"));
}

TEST(ArcVmTest, DalvikMemoryProfileDefault) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_dalvik_memory_profile(
      arc::StartArcMiniInstanceRequest::MEMORY_PROFILE_DEFAULT);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.arc_dalvik_memory_profile=4G"));
}

TEST(ArcVmTest, DalvikMemoryProfile4G) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_dalvik_memory_profile(
      arc::StartArcMiniInstanceRequest::MEMORY_PROFILE_4G);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.arc_dalvik_memory_profile=4G"));
}

TEST(ArcVmTest, DalvikMemoryProfile8G) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_dalvik_memory_profile(
      arc::StartArcMiniInstanceRequest::MEMORY_PROFILE_8G);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.arc_dalvik_memory_profile=8G"));
}

TEST(ArcVmTest, DalvikMemoryProfile16G) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_dalvik_memory_profile(
      arc::StartArcMiniInstanceRequest::MEMORY_PROFILE_16G);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.arc_dalvik_memory_profile=16G"));
}

TEST(ArcVmTest, LcdDensity) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  auto* mini_instance_request = request.mutable_mini_instance_request();
  mini_instance_request->set_lcd_density(kLcdDensity);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(
      params, base::StringPrintf("androidboot.lcd_density=%d", kLcdDensity)));
}

TEST(ArcVmTest, HostOnVmTrue) {
  crossystem::fake::CrossystemFake cros_system;
  cros_system.VbSetSystemPropertyInt("inside_vm", 1);
  StartArcVmRequest request;
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.host_is_in_vm=1"));
}

TEST(ArcVmTest, HostOnVmFalse) {
  crossystem::fake::CrossystemFake cros_system;
  cros_system.VbSetSystemPropertyInt("inside_vm", 0);
  StartArcVmRequest request;
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "androidboot.host_is_in_vm=0"));
}

TEST(ArcVmTest, UreadaheadModeReadahead) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_ureadahead_mode(
      vm_tools::concierge::StartArcVmRequest::UREADAHEAD_MODE_READAHEAD);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.arcvm_ureadahead_mode=readahead"));
}

TEST(ArcVmTest, UreadaheadModeGenerate) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_ureadahead_mode(
      vm_tools::concierge::StartArcVmRequest::UREADAHEAD_MODE_GENERATE);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.arcvm_ureadahead_mode=generate"));
}

TEST(ArcVmTest, UreadaheadModeDisabled) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_ureadahead_mode(
      vm_tools::concierge::StartArcVmRequest::UREADAHEAD_MODE_DISABLED);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  for (const auto& oneParam : params) {
    EXPECT_FALSE(
        base::StartsWith(oneParam, "androidboot.arcvm_ureadahead_mode="));
  }
}

TEST(ArcVmTest, ReadWriteEnabled) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_enable_rw(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(base::Contains(params, "rw"));
}

TEST(ArcVmTest, ReadWriteDisabled) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_enable_rw(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_FALSE(base::Contains(params, "rw"));
}

TEST(ArcVmTest, WebViewZygoteLazyInitEnabled) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_enable_web_view_zygote_lazy_init(true);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_TRUE(
      base::Contains(params, "androidboot.arc.web_view_zygote.lazy_init=1"));
}

TEST(ArcVmTest, WebViewZygoteLazyInitDisabled) {
  crossystem::fake::CrossystemFake cros_system;
  StartArcVmRequest request;
  request.set_enable_web_view_zygote_lazy_init(false);
  std::vector<std::string> params =
      ArcVm::GetKernelParams(cros_system, request, kSeneschalServerPort);
  EXPECT_FALSE(
      base::Contains(params, "androidboot.arc.web_view_zygote.lazy_init=1"));
}
}  // namespace concierge
}  // namespace vm_tools
