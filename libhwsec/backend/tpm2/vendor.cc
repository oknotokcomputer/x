// Copyright 2022 The ChromiumOS Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "libhwsec/backend/tpm2/backend.h"

#include <cinttypes>
#include <string>
#include <utility>
#include <base/strings/stringprintf.h>

#include <libhwsec-foundation/crypto/sha.h>
#include <libhwsec-foundation/status/status_chain_macros.h>
#include <tpm_manager/proto_bindings/tpm_manager.pb.h>
#include <tpm_manager-client/tpm_manager/dbus-proxies.h>
#include <trunks/tpm_generated.h>
#include <trunks/tpm_utility.h>

#include "libhwsec/error/tpm2_error.h"
#include "libhwsec/error/tpm_manager_error.h"

using brillo::BlobFromString;
using brillo::BlobToString;
using hwsec_foundation::Sha256;
using hwsec_foundation::status::MakeStatus;

namespace hwsec {

using VendorTpm2 = BackendTpm2::VendorTpm2;

namespace {

Status GetResponseStatus(const std::string& response) {
  std::string buffer(response);

  trunks::TPM_ST tag;
  RETURN_IF_ERROR(
      MakeStatus<TPM2Error>(trunks::Parse_TPM_ST(&buffer, &tag, nullptr)))
      .WithStatus<TPMError>("Failed to parse response tag");

  trunks::UINT32 response_size;
  RETURN_IF_ERROR(MakeStatus<TPM2Error>(
                      trunks::Parse_UINT32(&buffer, &response_size, nullptr)))
      .WithStatus<TPMError>("Failed to parse response size");

  if (response_size != response.size()) {
    return MakeStatus<TPMError>("Mismatch response size",
                                TPMRetryAction::kNoRetry);
  }

  trunks::TPM_RC rc;
  RETURN_IF_ERROR(
      MakeStatus<TPM2Error>(trunks::Parse_TPM_RC(&buffer, &rc, nullptr)))
      .WithStatus<TPMError>("Failed to parse TPM_RC");

  return MakeStatus<TPM2Error>(rc);
}

}  // namespace

Status VendorTpm2::EnsureVersionInfo() {
  if (version_info_.has_value()) {
    return OkStatus();
  }

  tpm_manager::GetVersionInfoRequest request;
  tpm_manager::GetVersionInfoReply reply;

  if (brillo::ErrorPtr err; !backend_.proxy_.GetTpmManager().GetVersionInfo(
          request, &reply, &err, Proxy::kDefaultDBusTimeoutMs)) {
    return MakeStatus<TPMError>(TPMRetryAction::kCommunication)
        .Wrap(std::move(err));
  }

  RETURN_IF_ERROR(MakeStatus<TPMManagerError>(reply.status()));

  version_info_ = std::move(reply);
  return OkStatus();
}

StatusOr<uint32_t> VendorTpm2::GetFamily() {
  RETURN_IF_ERROR(EnsureVersionInfo());

  return version_info_->family();
}

StatusOr<uint64_t> VendorTpm2::GetSpecLevel() {
  RETURN_IF_ERROR(EnsureVersionInfo());

  return version_info_->spec_level();
}

StatusOr<uint32_t> VendorTpm2::GetManufacturer() {
  RETURN_IF_ERROR(EnsureVersionInfo());

  return version_info_->manufacturer();
}

StatusOr<uint32_t> VendorTpm2::GetTpmModel() {
  RETURN_IF_ERROR(EnsureVersionInfo());

  return version_info_->tpm_model();
}

StatusOr<uint64_t> VendorTpm2::GetFirmwareVersion() {
  RETURN_IF_ERROR(EnsureVersionInfo());

  return version_info_->firmware_version();
}

StatusOr<brillo::Blob> VendorTpm2::GetVendorSpecific() {
  RETURN_IF_ERROR(EnsureVersionInfo());

  return brillo::BlobFromString(version_info_->vendor_specific());
}

StatusOr<int32_t> VendorTpm2::GetFingerprint() {
  RETURN_IF_ERROR(EnsureVersionInfo());

  // The exact encoding doesn't matter as long as its unambiguous, stable and
  // contains all information present in the version fields.
  std::string encoded_parameters = base::StringPrintf(
      "%08" PRIx32 "%016" PRIx64 "%08" PRIx32 "%08" PRIx32 "%016" PRIx64
      "%016zx",
      version_info_->family(), version_info_->spec_level(),
      version_info_->manufacturer(), version_info_->tpm_model(),
      version_info_->firmware_version(),
      version_info_->vendor_specific().size());
  encoded_parameters.append(version_info_->vendor_specific());

  brillo::Blob hash = Sha256(brillo::BlobFromString(encoded_parameters));

  // Return the first 31 bits from |hash|.
  uint32_t result = static_cast<uint32_t>(hash[0]) |
                    static_cast<uint32_t>(hash[1]) << 8 |
                    static_cast<uint32_t>(hash[2]) << 16 |
                    static_cast<uint32_t>(hash[3]) << 24;
  return result & 0x7fffffff;
}

StatusOr<bool> VendorTpm2::IsSrkRocaVulnerable() {
  return false;
}

StatusOr<brillo::Blob> VendorTpm2::GetIFXFieldUpgradeInfo() {
  return MakeStatus<TPMError>("Unsupported command", TPMRetryAction::kNoRetry);
}

Status VendorTpm2::DeclareTpmFirmwareStable() {
  if (fw_declared_stable_) {
    return OkStatus();
  }

  TrunksClientContext& context = backend_.trunks_context_;

  RETURN_IF_ERROR(
      MakeStatus<TPM2Error>(context.tpm_utility->DeclareTpmFirmwareStable()))
      .WithStatus<TPMError>("Failed to declare TPM firmware stable");

  fw_declared_stable_ = true;

  return OkStatus();
}

StatusOr<brillo::Blob> VendorTpm2::SendRawCommand(const brillo::Blob& command) {
  std::string response =
      backend_.trunks_context_.command_transceiver.SendCommandAndWait(
          BlobToString(command));

  RETURN_IF_ERROR(GetResponseStatus(response));

  return BlobFromString(response);
}

}  // namespace hwsec
