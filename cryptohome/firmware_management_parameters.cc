// Copyright 2016 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/firmware_management_parameters.h"

#include <arpa/inet.h>
#include <stdint.h>

#include <string>
#include <vector>

#include <base/check.h>
#include <base/logging.h>
#include <base/strings/string_split.h>
#include <brillo/secure_blob.h>
#include <openssl/sha.h>

#include "cryptohome/crc.h"

namespace cryptohome {
namespace {
const uint32_t kNvramVersionV1_0 = 0x10;
}

// Defines the raw NVRAM contents.
struct FirmwareManagementParametersRawV1_0 {
  uint8_t crc;
  uint8_t struct_size;
  // Data after this is covered by the crc
  uint8_t struct_version;  // Set to kNvramVersionV1_0
  uint8_t reserved0;
  uint32_t flags;
  uint8_t developer_key_hash[SHA256_DIGEST_LENGTH];
} __attribute__((packed));

static_assert(sizeof(FirmwareManagementParametersRawV1_0) == 40,
              "Unexpected size of FWMP");

// Index must match firmware; see README.firmware_management_parameters
const uint32_t FirmwareManagementParameters::kNvramIndex = 0x100a;
const uint32_t FirmwareManagementParameters::kNvramBytes =
    sizeof(struct FirmwareManagementParametersRawV1_0);
const uint32_t FirmwareManagementParameters::kCrcDataOffset = 2;

FirmwareManagementParameters::FirmwareManagementParameters(
    const hwsec::CryptohomeFrontend* hwsec)
    : hwsec_(hwsec), raw_(new FirmwareManagementParametersRawV1_0()) {
  CHECK(hwsec_);

  if (PLATFORM_FWMP_INDEX) {
    fwmp_type_ = hwsec::Space::kPlatformFirmwareManagementParameters;
  } else if (hwsec::StatusOr<hwsec::CryptohomeFrontend::StorageState> state =
                 hwsec->GetSpaceState(
                     hwsec::Space::kPlatformFirmwareManagementParameters);
             !state.ok()) {
    fwmp_type_ = hwsec::Space::kFirmwareManagementParameters;
  } else {
    fwmp_type_ = hwsec::Space::kPlatformFirmwareManagementParameters;
  }
}

FirmwareManagementParameters::FirmwareManagementParameters(
    hwsec::Space fwmp_type, const hwsec::CryptohomeFrontend* hwsec)
    : fwmp_type_(fwmp_type),
      hwsec_(hwsec),
      raw_(new FirmwareManagementParametersRawV1_0()) {
  CHECK(hwsec_);
}

// constructor for mock testing purpose.
FirmwareManagementParameters::FirmwareManagementParameters()
    : fwmp_type_(hwsec::Space::kFirmwareManagementParameters),
      hwsec_(nullptr) {}

FirmwareManagementParameters::~FirmwareManagementParameters() = default;

bool FirmwareManagementParameters::GetFWMP(
    user_data_auth::FirmwareManagementParameters* fwmp) {
  if (!Load()) {
    return false;
  }

  uint32_t flags;
  if (GetFlags(&flags)) {
    fwmp->set_flags(flags);
  } else {
    LOG(WARNING) << "Failed to GetFlags() for GetFWMP().";
    return false;
  }

  std::vector<uint8_t> hash;
  if (GetDeveloperKeyHash(&hash)) {
    *fwmp->mutable_developer_key_hash() = {hash.begin(), hash.end()};
  } else {
    LOG(WARNING) << "Failed to GetDeveloperKeyHash() for GetFWMP().";
    return false;
  }

  return true;
}

bool FirmwareManagementParameters::SetFWMP(
    const user_data_auth::FirmwareManagementParameters& fwmp) {
  if (!Create()) {
    return false;
  }

  uint32_t flags = fwmp.flags();
  std::unique_ptr<std::vector<uint8_t>> hash;

  if (!fwmp.developer_key_hash().empty()) {
    hash = std::make_unique<std::vector<uint8_t>>(
        fwmp.developer_key_hash().begin(), fwmp.developer_key_hash().end());
  }

  if (!Store(flags, hash.get())) {
    return false;
  }

  return true;
}

bool FirmwareManagementParameters::Destroy() {
  if (fwmp_type_ == hwsec::Space::kPlatformFirmwareManagementParameters) {
    return Store(/*flags=*/0, /*developer_key_hash=*/nullptr);
  }

  if (hwsec::Status status = hwsec_->DestroySpace(fwmp_type_); !status.ok()) {
    LOG(ERROR) << "Failed to destroy FWMP: " << status;
    return false;
  }

  loaded_ = false;
  return true;
}

bool FirmwareManagementParameters::Create() {
  if (fwmp_type_ == hwsec::Space::kPlatformFirmwareManagementParameters) {
    return Store(/*flags=*/0, /*developer_key_hash=*/nullptr);
  }

  if (hwsec::Status status = hwsec_->PrepareSpace(fwmp_type_, kNvramBytes);
      !status.ok()) {
    LOG(ERROR) << "Failed to prepare FWMP: " << status;
    return false;
  }

  LOG(INFO) << "Firmware Management Parameters created.";
  return true;
}

bool FirmwareManagementParameters::Load() {
  if (loaded_) {
    return true;
  }

  auto state = hwsec_->GetSpaceState(fwmp_type_);
  if (!state.ok()) {
    LOG(ERROR) << "Failed to get FWMP state: " << state.status();
    return false;
  }

  if (!state->readable) {
    LOG(INFO) << "Load() called with unreadable FWMP.";
    return false;
  }

  auto data = hwsec_->LoadSpace(fwmp_type_);
  if (!data.ok()) {
    LOG(ERROR) << "Failed to load FWMP: " << data.status();
    return false;
  }

  brillo::SecureBlob nvram_data(data->begin(), data->end());

  // Make sure we've read enough data for a 1.0 struct
  unsigned int nvram_size = nvram_data.size();
  if (nvram_size < kNvramBytes) {
    LOG(ERROR) << "Load() found unexpected NVRAM size: " << nvram_size;
    return false;
  }

  // Copy the raw data
  memcpy(raw_.get(), nvram_data.data(), kNvramBytes);

  // Verify the size
  if (raw_->struct_size != nvram_size) {
    LOG(ERROR) << "Load() found unexpected NVRAM size: " << nvram_size;
    return false;
  }

  // Verify the CRC
  uint8_t crc =
      Crc8(nvram_data.data() + kCrcDataOffset, nvram_size - kCrcDataOffset);
  if (crc != raw_->crc) {
    LOG(ERROR) << "Load() got bad CRC";
    return false;
  }

  // We are a 1.0 reader, so we can read 1.x structs
  if ((raw_->struct_version >> 4) != (kNvramVersionV1_0 >> 4)) {
    LOG(ERROR) << "Load() got incompatible NVRAM version: "
               << (unsigned int)raw_->struct_version;
    return false;
  }
  // We don't need to check minor version, because all 1.x structs are
  // compatible with us

  DLOG(INFO) << "Load() successfully loaded NVRAM data.";
  loaded_ = true;
  return true;
}

bool FirmwareManagementParameters::Store(
    uint32_t flags, const brillo::Blob* developer_key_hash) {
  // Check the FWMP state.
  auto state = hwsec_->GetSpaceState(fwmp_type_);
  if (!state.ok()) {
    LOG(ERROR) << "Failed to get FWMP state: " << state.status();
    return false;
  }

  if (!state->writable) {
    LOG(INFO) << "Store() called with unwritable FWMP state.";
    return false;
  }

  // Reset the NVRAM contents
  loaded_ = false;
  memset(raw_.get(), 0, kNvramBytes);
  raw_->struct_size = kNvramBytes;
  raw_->struct_version = kNvramVersionV1_0;
  raw_->flags = flags;

  // Store the hash, if any
  if (developer_key_hash) {
    // Make sure hash is the right size
    if ((developer_key_hash->size() != sizeof(raw_->developer_key_hash))) {
      LOG(ERROR) << "Store() called with bad hash size "
                 << developer_key_hash->size() << ".";
      return false;
    }

    memcpy(raw_->developer_key_hash, developer_key_hash->data(),
           sizeof(raw_->developer_key_hash));
  }

  // Recalculate the CRC
  const uint8_t* raw8 = reinterpret_cast<uint8_t*>(raw_.get());
  raw_->crc = Crc8(raw8 + kCrcDataOffset, raw_->struct_size - kCrcDataOffset);

  // Write the data to nvram
  brillo::Blob nvram_data(raw_->struct_size);
  memcpy(nvram_data.data(), raw_.get(), raw_->struct_size);

  if (hwsec::Status status = hwsec_->StoreSpace(fwmp_type_, nvram_data);
      !status.ok()) {
    LOG(ERROR) << "Failed to store FWMP: " << status;
    return false;
  }

  loaded_ = true;
  return true;
}

bool FirmwareManagementParameters::GetFlags(uint32_t* flags) {
  CHECK(flags);

  // Load if needed
  if (!Load()) {
    return false;
  }

  *flags = raw_->flags;
  return true;
}

bool FirmwareManagementParameters::GetDeveloperKeyHash(brillo::Blob* hash) {
  CHECK(hash);

  // Load if needed
  if (!Load()) {
    return false;
  }

  hash->resize(sizeof(raw_->developer_key_hash));
  memcpy(hash->data(), raw_->developer_key_hash, hash->size());
  return true;
}

}  // namespace cryptohome
