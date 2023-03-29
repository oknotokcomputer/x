// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unistd.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <sys/utsname.h>

#if __has_include(<asm/bootparam.h>)
#include <asm/bootparam.h>
#define HAVE_BOOTPARAM
#endif
#include "absl/status/status.h"
#include "attestation/proto_bindings/interface.pb.h"
#include "attestation-client/attestation/dbus-proxies.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_forward.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/strcat.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "missive/proto/record_constants.pb.h"
#include "secagentd/device_user.h"
#include "secagentd/message_sender.h"
#include "secagentd/metrics_sender.h"
#include "secagentd/plugins.h"
#include "secagentd/proto/security_xdr_events.pb.h"
#include "tpm_manager/proto_bindings/tpm_manager.pb.h"
#include "vboot/crossystem.h"

namespace {

constexpr int kWaitForServicesTimeoutMs = 2000;
constexpr char kBootDataFilepath[] = "/sys/kernel/boot_params/data";

std::string TpmPropertyToStr(uint32_t value) {
  std::string str;

  for (int i = 0, shift = 24; i < 4; i++, shift -= 8) {
    auto c = static_cast<char>((value >> shift) & 0xFF);
    if (c == 0)
      break;
    str.push_back((c >= 32 && c < 127) ? c : ' ');
  }

  return str;
}
}  // namespace

namespace secagentd {

namespace pb = cros_xdr::reporting;

AgentPlugin::AgentPlugin(
    scoped_refptr<MessageSenderInterface> message_sender,
    scoped_refptr<DeviceUserInterface> device_user,
    std::unique_ptr<org::chromium::AttestationProxyInterface> attestation_proxy,
    std::unique_ptr<org::chromium::TpmManagerProxyInterface> tpm_manager_proxy,
    base::OnceCallback<void()> cb,
    uint32_t heartbeat_timer)
    : weak_ptr_factory_(this),
      message_sender_(message_sender),
      device_user_(device_user),
      heartbeat_timer_(base::Seconds(std::max(heartbeat_timer, uint32_t(1)))) {
  CHECK(message_sender != nullptr);
  attestation_proxy_ = std::move(attestation_proxy);
  tpm_manager_proxy_ = std::move(tpm_manager_proxy);
  daemon_cb_ = std::move(cb);
}

std::string AgentPlugin::GetName() const {
  return "AgentPlugin";
}

absl::Status AgentPlugin::Activate() {
  StartInitializingAgentProto();

  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AgentPlugin::SendAgentStartEvent,
                     weak_ptr_factory_.GetWeakPtr()),
      // Add delay for tpm_manager and attestation to initialize.
      base::Seconds(1));

  return absl::OkStatus();
}

void AgentPlugin::StartInitializingAgentProto() {
  attestation_proxy_->GetObjectProxy()->WaitForServiceToBeAvailable(
      base::BindOnce(&AgentPlugin::GetCrosSecureBootInformation,
                     weak_ptr_factory_.GetWeakPtr()));
  tpm_manager_proxy_->GetObjectProxy()->WaitForServiceToBeAvailable(
      base::BindOnce(&AgentPlugin::GetTpmInformation,
                     weak_ptr_factory_.GetWeakPtr()));

  char buffer[VB_MAX_STRING_PROPERTY];
  auto get_fwid_rv =
      VbGetSystemPropertyString("fwid", buffer, std::size(buffer));

  // Get linux version.
  struct utsname buf;
  int get_uname_rv = uname(&buf);

  GetUefiSecureBootInformation(base::FilePath(kBootDataFilepath));

  base::AutoLock lock(tcb_attributes_lock_);
  if (get_fwid_rv) {
    tcb_attributes_.set_system_firmware_version(get_fwid_rv);
  } else {
    LOG(ERROR) << "Failed to retrieve fwid";
  }
  if (!get_uname_rv) {
    tcb_attributes_.set_linux_kernel_version(buf.release);
  } else {
    LOG(ERROR) << "Failed to retrieve uname";
  }
}

void AgentPlugin::GetUefiSecureBootInformation(
    const base::FilePath& boot_params_filepath) {
#ifdef HAVE_BOOTPARAM
  std::string content;

  if (!base::ReadFileToStringWithMaxSize(boot_params_filepath, &content,
                                         sizeof(boot_params))) {
    LOG(ERROR) << "Failed to read file: " << boot_params_filepath.value();
    uefi_bootmode_metric_ = metrics::UefiBootmode::kFailedToReadBootParams;
    return;
  }
  if (content.size() != sizeof(boot_params)) {
    LOG(ERROR) << boot_params_filepath.value()
               << " boot params invalid file size";
    uefi_bootmode_metric_ = metrics::UefiBootmode::kBootParamInvalidSize;
    return;
  }
  const boot_params* boot =
      reinterpret_cast<const boot_params*>(content.c_str());

  // defined in kernel's include/linux/efi.h
  static constexpr int kEfiSecurebootModeEnabled = 3;
  if (boot->secure_boot == kEfiSecurebootModeEnabled) {
    base::AutoLock lock(tcb_attributes_lock_);
    tcb_attributes_.set_firmware_secure_boot(
        pb::TcbAttributes_FirmwareSecureBoot_CROS_FLEX_UEFI_SECURE_BOOT);
  }
#else
  LOG(WARNING)
      << "Header bootparam.h is not present. Assuming not uefi secure boot.";
  uefi_bootmode_metric_ = metrics::UefiBootmode::kFileNotFound;
#endif
}

void AgentPlugin::GetCrosSecureBootInformation(bool available) {
  if (!available) {
    LOG(ERROR) << "Failed waiting for attestation to become available";
    cros_bootmode_metric_ = metrics::CrosBootmode::kUnavailable;
    return;
  }

  // Get boot information.
  attestation::GetStatusRequest request;
  attestation::GetStatusReply out_reply;
  brillo::ErrorPtr error;

  if (!attestation_proxy_->GetStatus(request, &out_reply, &error,
                                     kWaitForServicesTimeoutMs) ||
      error.get()) {
    cros_bootmode_metric_ = metrics::CrosBootmode::kFailedRetrieval;
    LOG(ERROR) << "Failed to get boot information " << error->GetMessage();
    return;
  }

  cros_bootmode_metric_ = metrics::CrosBootmode::kSuccess;
  base::AutoLock lock(tcb_attributes_lock_);
  if (out_reply.verified_boot()) {
    tcb_attributes_.set_firmware_secure_boot(
        pb::TcbAttributes_FirmwareSecureBoot_CROS_VERIFIED_BOOT);
  } else {
    if (!tcb_attributes_.has_firmware_secure_boot()) {
      tcb_attributes_.set_firmware_secure_boot(
          pb::TcbAttributes_FirmwareSecureBoot_NONE);
    }
  }
}

void AgentPlugin::GetTpmInformation(bool available) {
  if (!available) {
    LOG(ERROR) << "Failed waiting for tpm_manager to become available";
    tpm_metric_ = metrics::Tpm::kUnavailable;
    return;
  }

  // Check if TPM is enabled.
  tpm_manager::GetTpmStatusRequest status_request;
  tpm_manager::GetTpmStatusReply status_reply;
  brillo::ErrorPtr error;

  if (!tpm_manager_proxy_->GetTpmStatus(status_request, &status_reply, &error,
                                        kWaitForServicesTimeoutMs) ||
      error.get()) {
    LOG(ERROR) << "Failed to get TPM status " << error->GetMessage();
    tpm_metric_ = metrics::Tpm::kFailedRetrieval;
    return;
  }
  if (status_reply.has_enabled() && !status_reply.enabled()) {
    LOG(INFO) << "TPM is disabled on device";
    return;
  }

  // Get TPM information.
  tpm_manager::GetVersionInfoRequest version_request;
  tpm_manager::GetVersionInfoReply version_reply;

  if (!tpm_manager_proxy_->GetVersionInfo(version_request, &version_reply,
                                          &error, kWaitForServicesTimeoutMs) ||
      error.get()) {
    tpm_metric_ = metrics::Tpm::kFailedRetrieval;
    LOG(ERROR) << "Failed to get TPM information " << error->GetMessage();
    return;
  }
  tpm_metric_ = metrics::Tpm::kSuccess;

  base::AutoLock lock(tcb_attributes_lock_);
  auto security_chip = tcb_attributes_.mutable_security_chip();
  if (version_reply.has_gsc_version()) {
    switch (version_reply.gsc_version()) {
      case tpm_manager::GSC_VERSION_NOT_GSC: {
        security_chip->set_kind(pb::TcbAttributes_SecurityChip::Kind::
                                    TcbAttributes_SecurityChip_Kind_TPM);
        break;
      }
      case tpm_manager::GSC_VERSION_CR50:
      case tpm_manager::GSC_VERSION_TI50:
        security_chip->set_kind(
            pb::TcbAttributes_SecurityChip::Kind::
                TcbAttributes_SecurityChip_Kind_GOOGLE_SECURITY_CHIP);
    }
    auto family = TpmPropertyToStr(version_reply.family());
    auto level =
        std::to_string((version_reply.spec_level() >> 32) & 0xffffffff);
    security_chip->set_chip_version(base::StringPrintf(
        "%s.%s.%s", family.c_str(), level.c_str(),
        std::to_string(version_reply.spec_level() & 0xffffffff).c_str()));
    security_chip->set_spec_family(family);
    security_chip->set_spec_level(level);
    security_chip->set_manufacturer(
        TpmPropertyToStr(version_reply.manufacturer()));
    security_chip->set_vendor_id(version_reply.vendor_specific());
    security_chip->set_tpm_model(std::to_string(version_reply.tpm_model()));
    security_chip->set_firmware_version(
        std::to_string(version_reply.firmware_version()));
  } else {
    security_chip->set_kind(pb::TcbAttributes_SecurityChip::Kind::
                                TcbAttributes_SecurityChip_Kind_NONE);
  }
}

void AgentPlugin::SendAgentStartEvent() {
  auto agent_event = std::make_unique<pb::XdrAgentEvent>();
  base::AutoLock lock(tcb_attributes_lock_);
  agent_event->mutable_agent_start()->mutable_tcb()->CopyFrom(tcb_attributes_);
  message_sender_->SendMessage(
      reporting::CROS_SECURITY_AGENT, agent_event->mutable_common(),
      std::move(agent_event),
      base::BindOnce(&AgentPlugin::StartEventStatusCallback,
                     weak_ptr_factory_.GetWeakPtr()));
}

void AgentPlugin::SendAgentHeartbeatEvent() {
  // Create agent heartbeat event.
  auto agent_event = std::make_unique<pb::XdrAgentEvent>();
  base::AutoLock lock(tcb_attributes_lock_);
  agent_event->mutable_agent_heartbeat()->mutable_tcb()->CopyFrom(
      tcb_attributes_);
  message_sender_->SendMessage(reporting::CROS_SECURITY_AGENT,
                               agent_event->mutable_common(),
                               std::move(agent_event), std::nullopt);
}

void AgentPlugin::StartEventStatusCallback(reporting::Status status) {
  if (status.ok()) {
    // Start heartbeat timer.
    agent_heartbeat_timer_.Start(
        FROM_HERE, heartbeat_timer_,
        base::BindRepeating(&AgentPlugin::SendAgentHeartbeatEvent,
                            weak_ptr_factory_.GetWeakPtr()));

    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, std::move(daemon_cb_));
  } else {
    LOG(ERROR) << "Agent Start failed to send. Will retry in 3s.";
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AgentPlugin::SendAgentStartEvent,
                       weak_ptr_factory_.GetWeakPtr()),
        base::Seconds(3));
  }

  static bool sent_metrics = false;
  // Should be sent once per daemon lifetime.
  if (!sent_metrics) {
    MetricsSender::GetInstance().SendEnumMetricToUMA(metrics::kCrosBootmode,
                                                     cros_bootmode_metric_);
    MetricsSender::GetInstance().SendEnumMetricToUMA(metrics::kUefiBootmode,
                                                     uefi_bootmode_metric_);
    MetricsSender::GetInstance().SendEnumMetricToUMA(metrics::kTpm,
                                                     tpm_metric_);
    sent_metrics = true;
  }
}

}  // namespace secagentd
