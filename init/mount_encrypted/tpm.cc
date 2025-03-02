// Copyright 2018 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "init/mount_encrypted/tpm.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <utility>

#include <base/check_op.h>
#include <base/logging.h>
#include <base/strings/string_number_conversions.h>
#include <crypto/libcrypto-compat.h>
#include <crypto/scoped_openssl_types.h>
#include <libhwsec-foundation/crypto/rsa.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <vboot/tlcl.h>

namespace mount_encrypted {
namespace {

#if !USE_TPM2

// A delegation family label identifying the delegation family we create as a
// flag that persists until the next TPM clear, at which point it gets cleared
// automatically. This is by the system key handling logic to determine whether
// a fresh system key has been generated after the last TPM clear.
uint8_t kSystemKeyInitializedFakeDelegationFamilyLabel = 0xff;

// Maximum TPM delegation table size.
const uint32_t kDelegationTableSize = 8;

#endif  // !USE_TPM2

// Initial auth policy buffer size that's expected to be large enough across TPM
// 1.2 and TPM 2.0 hardware. The code uses this for retrieving auth policies.
// Note that if the buffer is too small, it retries with the size indicated by
// the failing function.
const size_t kInitialAuthPolicySize = 128;

}  // namespace

NvramSpace::NvramSpace(Tpm* tpm, uint32_t index) : tpm_(tpm), index_(index) {}

void NvramSpace::Reset() {
  attributes_ = 0;
  auth_policy_.clear();
  contents_.clear();
  status_ = Status::kUnknown;
}

bool NvramSpace::GetAttributes(uint32_t* attributes) {
  bool rc = GetSpaceInfo();
  if (!rc) {
    return false;
  }

  *attributes = attributes_;
  return true;
}

bool NvramSpace::Read(uint32_t size) {
  status_ = Status::kUnknown;
  attributes_ = 0;
  contents_.clear();

  VLOG(1) << "Reading NVRAM area " << index_ << " (size " << size << ")";

  if (!tpm_->available()) {
    status_ = Status::kAbsent;
    return false;
  }

  brillo::SecureBlob buffer(size);
  uint32_t result = TlclRead(index_, buffer.data(), buffer.size());

  VLOG(1) << "NVRAM read returned: " << (result == TPM_SUCCESS ? "ok" : "FAIL");

  if (result != TPM_SUCCESS) {
    if (result == TPM_E_BADINDEX) {
      LOG(INFO) << "NVRAM space " << index_ << " doesn't exist";
    } else {
      LOG(ERROR) << "Failed to read NVRAM space " << index_ << ": " << result;
    }
    status_ = result == TPM_E_BADINDEX ? Status::kAbsent : Status::kTpmError;
    return false;
  }

  if (!USE_TPM2) {
    // Ignore defined but unwritten NVRAM area.
    uint8_t bytes_ored = 0x0;
    uint8_t bytes_anded = 0xff;
    for (uint8_t byte : buffer) {
      bytes_ored |= byte;
      bytes_anded &= byte;
    }
    if (bytes_ored == 0x0 || bytes_anded == 0xff) {
      // Still records the contents so the caller can judge if the size is
      // good  before writing.
      contents_.swap(buffer);
      status_ = Status::kWritable;
      LOG(INFO) << "NVRAM area has been defined but not written.";
      return false;
    }
  }

  contents_.swap(buffer);
  status_ = Status::kValid;
  return true;
}

bool NvramSpace::Write(const brillo::SecureBlob& contents) {
  VLOG(1) << "Writing NVRAM area " << index_ << " (size " << contents.size()
          << ")";

  if (!tpm_->available()) {
    return false;
  }

  brillo::SecureBlob buffer(contents.size());
  uint32_t result = TlclWrite(index_, contents.data(), contents.size());

  VLOG(1) << "NVRAM write returned: "
          << (result == TPM_SUCCESS ? "ok" : "FAIL");

  if (result != TPM_SUCCESS) {
    LOG(ERROR) << "Failed to write NVRAM space " << index_ << ": " << result;
    return false;
  }

  contents_ = contents;
  status_ = Status::kValid;
  return true;
}

bool NvramSpace::ReadLock() {
  if (!tpm_->available()) {
    return false;
  }

  uint32_t result = TlclReadLock(index_);
  if (result != TPM_SUCCESS) {
    LOG(ERROR) << "Failed to set read lock on NVRAM space " << index_ << ": "
               << result;
    return false;
  }

  return true;
}

bool NvramSpace::WriteLock() {
  if (!tpm_->available()) {
    return false;
  }

  uint32_t result = TlclWriteLock(index_);
  if (result != TPM_SUCCESS) {
    LOG(ERROR) << "Failed to set write lock on NVRAM space " << index_ << ": "
               << result;
    return false;
  }

  return true;
}

bool NvramSpace::Define(uint32_t attributes,
                        uint32_t size,
                        uint32_t pcr_selection) {
  if (!tpm_->available()) {
    return false;
  }

  std::vector<uint8_t> policy;
  bool rc = GetPCRBindingPolicy(pcr_selection, &policy);
  if (!rc) {
    LOG(ERROR) << "Failed to initialize PCR binding policy for " << index_;
    return false;
  }

  uint32_t result = TlclDefineSpaceEx(
      kOwnerSecret, kOwnerSecretSize, index_, attributes, size,
      policy.empty() ? nullptr : policy.data(), policy.size());
  if (result != TPM_SUCCESS) {
    LOG(ERROR) << "Failed to define NVRAM space " << index_ << ": " << result;
    return false;
  }

  // `kWritable` is not included in the state machine for TPM2.0 by design.
  // Ideally the status should always be consistent with the value of `status_`
  // and it should be TPM-independent. However, for TPM2.0 we don't have to
  // have `kWritable`; once stopping support for TPM1.2, it could be
  // over-complicated for TPM2.0 and hard to clean up. Thus, pursuing the
  // consistency doesn't seem to be a good idea.
  if (USE_TPM2) {
    status_ = Status::kValid;
  } else {
    status_ = Status::kWritable;
  }

  contents_.clear();
  contents_.resize(size);
  attributes_ = attributes;
  auth_policy_ = std::move(policy);

  return true;
}

bool NvramSpace::CheckPCRBinding(uint32_t pcr_selection, bool* match) {
  *match = false;

  std::vector<uint8_t> policy;
  bool rc = GetSpaceInfo();
  if (!rc) {
    return false;
  }

  rc = GetPCRBindingPolicy(pcr_selection, &policy);
  if (!rc) {
    return false;
  }

  *match = auth_policy_ == policy;
  return true;
}

bool NvramSpace::GetSpaceInfo() {
  if (attributes_ != 0) {
    return true;
  }

  if (!tpm_->available()) {
    return false;
  }

  uint32_t auth_policy_size = kInitialAuthPolicySize;
  auth_policy_.resize(auth_policy_size);
  uint32_t size;
  uint32_t result = TlclGetSpaceInfo(index_, &attributes_, &size,
                                     auth_policy_.data(), &auth_policy_size);
  if (result == TPM_E_BUFFER_SIZE && auth_policy_size > 0) {
    auth_policy_.resize(auth_policy_size);
    result = TlclGetSpaceInfo(index_, &attributes_, &size, auth_policy_.data(),
                              &auth_policy_size);
  }
  if (result != TPM_SUCCESS) {
    attributes_ = 0;
    auth_policy_.clear();
    LOG(ERROR) << "Failed to read NVRAM space info for index " << index_ << ": "
               << result;
    return false;
  }

  CHECK_LE(auth_policy_size, auth_policy_.size());
  auth_policy_.resize(auth_policy_size);

  return true;
}

bool NvramSpace::GetPCRBindingPolicy(uint32_t pcr_selection,
                                     std::vector<uint8_t>* policy) {
  if (!tpm_->available()) {
    return false;
  }

  if (pcr_selection == 0) {
    policy->clear();
    return true;
  }

  int value_index = 0;
  uint8_t pcr_values[32][TPM_PCR_DIGEST] = {};
  for (int index = 0; index < 32; ++index) {
    if (((1 << index) & pcr_selection) != 0) {
      std::vector<uint8_t> pcr_value;
      bool rc = tpm_->ReadPCR(index, &pcr_value);
      if (!rc) {
        return false;
      }
      CHECK_EQ(TPM_PCR_DIGEST, pcr_value.size());
      memcpy(pcr_values[value_index++], pcr_value.data(), TPM_PCR_DIGEST);
    }
  }

  uint32_t auth_policy_size = kInitialAuthPolicySize;
  policy->resize(auth_policy_size);
  uint32_t result = TlclInitNvAuthPolicy(pcr_selection, pcr_values,
                                         policy->data(), &auth_policy_size);
  if (result == TPM_E_BUFFER_SIZE && auth_policy_size > 0) {
    policy->resize(auth_policy_size);
    result = TlclInitNvAuthPolicy(pcr_selection, pcr_values, policy->data(),
                                  &auth_policy_size);
  }

  if (result != TPM_SUCCESS) {
    policy->clear();
    LOG(ERROR) << "Failed to get NV policy " << result;
    return false;
  }

  CHECK_LE(auth_policy_size, policy->size());
  policy->resize(auth_policy_size);

  return true;
}

Tpm::Tpm() {
#if USE_TPM2
  is_tpm2_ = true;
#endif

  VLOG(1) << "Opening TPM";

  setenv("TPM_NO_EXIT", "1", 1);
  available_ = (TlclLibInit() == TPM_SUCCESS);

  LOG(INFO) << "TPM " << (available_ ? "ready" : "not available");
}

Tpm::~Tpm() {
  if (available_) {
    TlclLibClose();
  }
}

bool Tpm::IsOwned(bool* owned) {
  if (ownership_checked_) {
    *owned = owned_;
    return true;
  }

  VLOG(1) << "Reading TPM Ownership Flag";
  if (!available_) {
    return false;
  }

  uint8_t tmp_owned = 0;
  uint32_t result = TlclGetOwnership(&tmp_owned);
  VLOG(1) << "TPM Ownership Flag returned: " << (result ? "FAIL" : "ok");
  if (result != TPM_SUCCESS) {
    LOG(INFO) << "Could not determine TPM ownership: error " << result;
    return false;
  }

  ownership_checked_ = true;
  owned_ = tmp_owned;
  *owned = owned_;
  return true;
}

bool Tpm::GetRandomBytes(uint8_t* buffer, int wanted) {
  if (available()) {
    // Read random bytes from TPM, which can return short reads.
    int remaining = wanted;
    while (remaining) {
      uint32_t result, size;
      result = TlclGetRandom(buffer + (wanted - remaining), remaining, &size);
      if (result != TPM_SUCCESS) {
        LOG(ERROR) << "TPM GetRandom failed: error " << result;
        return false;
      }
      CHECK_LE(size, remaining);
      remaining -= size;
    }

    if (remaining == 0) {
      return true;
    }
  }

  // Fall back to system random source.
  if (RAND_bytes(buffer, wanted)) {
    return true;
  }

  LOG(ERROR) << "Failed to obtain randomness.";
  return false;
}

bool Tpm::ReadPCR(uint32_t index, std::vector<uint8_t>* value) {
  // See whether the PCR is available in the cache. Note that we currently
  // assume PCR values remain constant during the lifetime of the process, so we
  // only ever read once.
  auto entry = pcr_values_.find(index);
  if (entry != pcr_values_.end()) {
    *value = entry->second;
    return true;
  }

  if (!available()) {
    return false;
  }

  std::vector<uint8_t> temp_value(TPM_PCR_DIGEST);
  uint32_t result = TlclPCRRead(index, temp_value.data(), temp_value.size());
  if (result != TPM_SUCCESS) {
    LOG(ERROR) << "TPM PCR " << index << " read failed: " << result;
    return false;
  }

  pcr_values_[index] = temp_value;
  *value = std::move(temp_value);
  return true;
}

bool Tpm::GetVersionInfo(uint32_t* vendor,
                         uint64_t* firmware_version,
                         std::vector<uint8_t>* vendor_specific) {
  size_t vendor_specific_size = 32;
  vendor_specific->resize(vendor_specific_size);
  uint32_t result = TlclGetVersion(
      vendor, firmware_version, vendor_specific->data(), &vendor_specific_size);
  if (result != TPM_SUCCESS) {
    LOG(ERROR) << "Failed to obtain TPM version info.";
    return false;
  }

  vendor_specific->resize(vendor_specific_size);
  return true;
}

bool Tpm::GetIFXFieldUpgradeInfo(TPM_IFX_FIELDUPGRADEINFO* field_upgrade_info) {
  uint32_t result = TlclIFXFieldUpgradeInfo(field_upgrade_info);
  if (result != TPM_SUCCESS) {
    LOG(ERROR) << "Failed to obtain IFX field upgrade info.";
    return false;
  }

  return true;
}

NvramSpace* Tpm::GetLockboxSpace() {
  if (lockbox_space_) {
    return lockbox_space_.get();
  }

  lockbox_space_ = std::make_unique<NvramSpace>(this, kLockboxIndex);

  // Reading the NVRAM takes 40ms. Instead of querying the NVRAM area for its
  // size (which takes time), just read the expected size. If it fails, then
  // fall back to the older size. This means cleared devices take 80ms (2 failed
  // reads), legacy devices take 80ms (1 failed read, 1 good read), and
  // populated devices take 40ms, which is the minimum possible time (instead of
  // 40ms + time to query NVRAM size).
  if (lockbox_space_->Read(kLockboxSizeV2)) {
    LOG(INFO) << "Version 2 Lockbox NVRAM area found.";
  } else if (lockbox_space_->Read(kLockboxSizeV1)) {
    LOG(INFO) << "Version 1 Lockbox NVRAM area found.";
  } else {
    LOG(INFO) << "No Lockbox NVRAM area defined.";
  }

  return lockbox_space_.get();
}

NvramSpace* Tpm::GetEncStatefulSpace() {
  if (encstateful_space_) {
    return encstateful_space_.get();
  }

  encstateful_space_ = std::make_unique<NvramSpace>(this, kEncStatefulIndex);

  if (encstateful_space_->Read(kEncStatefulSize)) {
    LOG(INFO) << "Found encstateful NVRAM area.";
  } else {
    LOG(INFO) << "No encstateful NVRAM area defined.";
  }

  return encstateful_space_.get();
}

#if USE_TPM2

bool Tpm::TakeOwnership() {
  return false;
}

bool Tpm::SetSystemKeyInitializedFlag() {
  return false;
}

bool Tpm::HasSystemKeyInitializedFlag(bool* flag_value) {
  return false;
}

#else

bool Tpm::TakeOwnership() {
  // Read the public half of the EK.
  uint32_t public_exponent = 0;
  uint8_t modulus[TPM_RSA_2048_LEN];
  uint32_t modulus_size = sizeof(modulus);
  uint32_t result = TlclReadPubek(&public_exponent, modulus, &modulus_size);
  if (result != TPM_SUCCESS) {
    LOG(ERROR) << "Failed to read public endorsement key: " << result;
    return false;
  }

  crypto::ScopedRSA rsa = hwsec_foundation::CreateRSAFromNumber(
      brillo::Blob(modulus, modulus + modulus_size), public_exponent);
  if (!rsa) {
    LOG(ERROR) << "Failed to create RSA.";
    return false;
  }

  // Encrypt the well-known owner secret under the EK.
  brillo::SecureBlob owner_auth(kOwnerSecret, kOwnerSecret + kOwnerSecretSize);
  brillo::SecureBlob enc_auth;
  if (!hwsec_foundation::TpmCompatibleOAEPEncrypt(rsa.get(), owner_auth,
                                                  &enc_auth)) {
    LOG(ERROR) << "Failed to encrypt owner secret.";
    return false;
  }

  // Take ownership.
  result =
      TlclTakeOwnership(enc_auth.data(), enc_auth.data(), owner_auth.data());
  if (result != TPM_SUCCESS) {
    LOG(ERROR) << "Failed to take TPM ownership: " << result;
    return false;
  }

  ownership_checked_ = true;
  owned_ = true;

  // Ownership implies the initialization flag.
  initialized_flag_checked_ = true;
  initialized_flag_ = true;

  return true;
}

bool Tpm::SetSystemKeyInitializedFlag() {
  bool flag_value = false;
  bool rc = HasSystemKeyInitializedFlag(&flag_value);
  if (!rc) {
    return false;
  }

  if (flag_value) {
    return true;
  }

  uint32_t result = TlclCreateDelegationFamily(
      kSystemKeyInitializedFakeDelegationFamilyLabel);
  if (result != TPM_SUCCESS) {
    LOG(ERROR) << "Failed to create fake delegation family: " << result;
    return false;
  }

  initialized_flag_ = true;
  initialized_flag_checked_ = true;

  return true;
}

bool Tpm::HasSystemKeyInitializedFlag(bool* flag_value) {
  if (!available()) {
    return false;
  }

  if (initialized_flag_checked_) {
    *flag_value = initialized_flag_;
    return true;
  }

  // The fake delegation family is only relevant for unowned TPMs.
  // Pretend the flag is present if the TPM is owned.
  bool owned = false;
  bool rc = IsOwned(&owned);
  if (!rc) {
    LOG(ERROR) << "Failed to determine ownership.";
    return false;
  }
  if (owned) {
    initialized_flag_checked_ = true;
    initialized_flag_ = true;
    *flag_value = initialized_flag_;
    return true;
  }

  TPM_FAMILY_TABLE_ENTRY table[kDelegationTableSize];
  uint32_t table_size = kDelegationTableSize;
  uint32_t result = TlclReadDelegationFamilyTable(table, &table_size);
  if (result != TPM_SUCCESS) {
    LOG(ERROR) << "Failed to read delegation family table: " << result;
    return false;
  }

  for (uint32_t i = 0; i < table_size; ++i) {
    if (table[i].familyLabel ==
        kSystemKeyInitializedFakeDelegationFamilyLabel) {
      initialized_flag_ = true;
      break;
    }
  }

  initialized_flag_checked_ = true;
  *flag_value = initialized_flag_;
  return true;
}

#endif
}  // namespace mount_encrypted
