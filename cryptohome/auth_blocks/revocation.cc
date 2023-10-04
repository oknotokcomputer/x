// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/auth_blocks/revocation.h"

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include <brillo/secure_blob.h>
#include <libhwsec/backend/pinweaver_manager/pinweaver_manager.h>
#include <libhwsec-foundation/crypto/secure_blob_util.h>
#include <libhwsec-foundation/crypto/scrypt.h>
#include <libhwsec-foundation/crypto/aes.h>
#include <libhwsec-foundation/crypto/hkdf.h>

#include "cryptohome/crypto_error.h"
#include "cryptohome/cryptohome_common.h"
#include "cryptohome/cryptohome_metrics.h"
#include "cryptohome/error/cryptohome_crypto_error.h"
#include "cryptohome/error/cryptohome_tpm_error.h"
#include "cryptohome/error/location_utils.h"
#include "cryptohome/error/locations.h"
#include "cryptohome/flatbuffer_schemas/auth_block_state.h"
#include "cryptohome/key_objects.h"

using ::cryptohome::error::CryptohomeCryptoError;
using ::cryptohome::error::ErrorActionSet;
using ::cryptohome::error::PossibleAction;
using ::cryptohome::error::PrimaryAction;
using ::hwsec_foundation::CreateSecureRandomBlob;
using ::hwsec_foundation::DeriveSecretsScrypt;
using ::hwsec_foundation::Hkdf;
using ::hwsec_foundation::HkdfHash;
using ::hwsec_foundation::kDefaultAesKeySize;
using ::hwsec_foundation::status::MakeStatus;
using ::hwsec_foundation::status::OkStatus;

namespace cryptohome {
namespace revocation {

namespace {
constexpr int kDefaultSecretSize = 32;
// String used as vector in HKDF operation to derive vkk_key from he_secret.
const char kHESecretHkdfData[] = "hkdf_data";
// String used as info in HKDF operation to derive le_secret from
// per_credential_secret.
const char kLeSecretInfo[] = "le_secret_info";
// String used as info in HKDF operation to derive kdf_skey from
// per_credential_secret.
const char kKdfSkeyInfo[] = "kdf_skey_info";

// The format for a delay schedule entry is as follows:
// (number_of_incorrect_attempts, delay before_next_attempt)
// The delay is not needed for revocation, so we set
// number_of_incorrect_attempts to UINT32_MAX.
hwsec::PinWeaverManagerFrontend::DelaySchedule GetDelaySchedule() {
  return std::map<uint32_t, uint32_t>{{UINT32_MAX, 1}};
}

CryptoError RevokeTPMRetryActionToCryptoError(
    hwsec::TPMRetryAction retry_action) {
  switch (retry_action) {
    case hwsec::TPMRetryAction::kNoRetry:
    case hwsec::TPMRetryAction::kSpaceNotFound:
      // Do not return an error here. RemoveCredential returns:
      // - LE_CRED_ERROR_INVALID_LABEL for invalid label.
      // - LE_CRED_ERROR_HASH_TREE for hash tree error (implies that all state
      // in PinWeaver is lost). Both of these cases are considered as "success"
      // for revocation.
      return CryptoError::CE_NONE;
    default:
      return CryptoError::CE_OTHER_CRYPTO;
  }
}

bool DeriveSecret(const brillo::SecureBlob& key,
                  const brillo::SecureBlob& hkdf_info,
                  brillo::SecureBlob* gen_secret) {
  // Note: the key is high entropy, so the salt can be empty.
  if (!Hkdf(HkdfHash::kSha256, /*key=*/key,
            /*info=*/hkdf_info,
            /*salt=*/brillo::SecureBlob(),
            /*result_len=*/gen_secret->size(), gen_secret)) {
    LOG(ERROR) << "HKDF failed for revocation during secret derivation.";
    return false;
  }
  return true;
}

}  // namespace

bool IsRevocationSupported(const hwsec::CryptohomeFrontend* hwsec) {
  hwsec::StatusOr<bool> enabled = hwsec->IsPinWeaverEnabled();
  return enabled.ok() && *enabled;
}

CryptoStatus Create(const hwsec::PinWeaverManagerFrontend* hwsec_pw_manager,
                    RevocationState* revocation_state,
                    KeyBlobs* key_blobs) {
  if (!key_blobs->vkk_key.has_value() || key_blobs->vkk_key.value().empty()) {
    LOG(ERROR) << "Failed to create secret for revocation: vkk_key is not set";
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(kLocRevocationNoVkkKeyInCreate),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        CryptoError::CE_OTHER_CRYPTO);
  }

  // The secret generated by AuthBlock.
  brillo::SecureBlob per_credential_secret = key_blobs->vkk_key.value();

  brillo::SecureBlob salt =
      CreateSecureRandomBlob(kCryptohomeDefaultKeySaltSize);

  // Derive two secrets from per_credential_secret:
  // le_secret to be stored in LECredentialManager,
  // kdf_skey to be combined with he_secret for vkk_key generation.
  brillo::SecureBlob le_secret(kDefaultSecretSize);
  brillo::SecureBlob kdf_skey(kDefaultSecretSize);
  if (!DeriveSecret(per_credential_secret, brillo::SecureBlob(kLeSecretInfo),
                    &le_secret) ||
      !DeriveSecret(per_credential_secret, brillo::SecureBlob(kKdfSkeyInfo),
                    &kdf_skey)) {
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(kLocRevocationDeriveSecretsFailedInCreate),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        CryptoError::CE_OTHER_CRYPTO);
  }

  // Generate a random high entropy secret to be stored in LECredentialManager.
  brillo::SecureBlob he_secret = CreateSecureRandomBlob(kDefaultSecretSize);

  // Note:
  // - We send an empty blob as reset_secret because resetting the delay counter
  // will not compromise security (we send MAX_UINT32 attempts for the delay
  // schedule). The size should still be kDefaultSecretSize.
  // - We don't set policies because PCR binding is expected to be
  // already done by the AuthBlock.
  hwsec::StatusOr<uint64_t> result = hwsec_pw_manager->InsertCredential(
      /*policies=*/std::vector<hwsec::OperationPolicySetting>(),
      /*le_secret=*/le_secret,
      /*he_secret=*/he_secret,
      /*reset_secret=*/brillo::SecureBlob(kDefaultSecretSize),
      /*delay_schedule=*/GetDelaySchedule(),
      /*expiration_delay=*/std::nullopt);

  if (!result.ok()) {
    LOG(ERROR) << "InsertCredential failed for revocation with error "
               << result.err_status();
    return MakeStatus<CryptohomeCryptoError>(
               CRYPTOHOME_ERR_LOC(kLocRevocationInsertCredentialFailedInCreate))
        .Wrap(MakeStatus<error::CryptohomeTPMError>(
            std::move(result).err_status()));
  }
  revocation_state->le_label = result.value();

  // Combine he_secret with kdf_skey:
  brillo::SecureBlob vkk_key;
  if (!Hkdf(HkdfHash::kSha256,
            /*key=*/brillo::SecureBlob::Combine(he_secret, kdf_skey),
            /*info=*/brillo::SecureBlob(),
            /*salt=*/brillo::SecureBlob(kHESecretHkdfData),
            /*result_len=*/0, &vkk_key)) {
    LOG(ERROR) << "vkk_key HKDF derivation failed for revocation";
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(kLocRevocationHkdfFailedInCreate),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        CryptoError::CE_OTHER_CRYPTO);
  }

  key_blobs->vkk_key = std::move(vkk_key);

  return OkStatus<CryptohomeCryptoError>();
}

CryptoStatus Derive(const hwsec::PinWeaverManagerFrontend* hwsec_pw_manager,
                    const RevocationState& revocation_state,
                    KeyBlobs* key_blobs) {
  if (!key_blobs->vkk_key.has_value() || key_blobs->vkk_key.value().empty()) {
    LOG(ERROR) << "Failed to derive secret for revocation: vkk_key is not set";
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(kLocRevocationNoVkkKeyInDerive),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        CryptoError::CE_OTHER_CRYPTO);
  }

  if (!revocation_state.le_label.has_value()) {
    LOG(ERROR)
        << "Failed to derive secret: revocation_state.le_label is not set";
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(kLocRevocationNoLeLabelInDerive),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        CryptoError::CE_OTHER_CRYPTO);
  }

  // The secret generated by AuthBlock.
  brillo::SecureBlob per_credential_secret = key_blobs->vkk_key.value();
  brillo::SecureBlob le_secret(kDefaultSecretSize);
  brillo::SecureBlob kdf_skey(kDefaultSecretSize);
  if (!DeriveSecret(per_credential_secret, brillo::SecureBlob(kLeSecretInfo),
                    &le_secret) ||
      !DeriveSecret(per_credential_secret, brillo::SecureBlob(kKdfSkeyInfo),
                    &kdf_skey)) {
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(kLocRevocationDeriveSecretsFailedInDerive),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        CryptoError::CE_OTHER_CRYPTO);
  }

  hwsec::StatusOr<hwsec::PinWeaverManagerFrontend::CheckCredentialReply>
      result = hwsec_pw_manager->CheckCredential(
          /*label=*/revocation_state.le_label.value(),
          /*le_secret=*/le_secret);

  if (!result.ok()) {
    LOG(ERROR) << "CheckCredential failed for revocation with error "
               << result.err_status();
    return MakeStatus<CryptohomeCryptoError>(
               CRYPTOHOME_ERR_LOC(kLocRevocationCheckCredentialFailedInDerive))
        .Wrap(MakeStatus<error::CryptohomeTPMError>(
            std::move(result).err_status()));
  }

  // Combine he_secret with kdf_skey:
  brillo::SecureBlob vkk_key;
  if (!Hkdf(HkdfHash::kSha256,
            /*key=*/
            brillo::SecureBlob::Combine(result->he_secret, kdf_skey),
            /*info=*/brillo::SecureBlob(),
            /*salt=*/brillo::SecureBlob(kHESecretHkdfData),
            /*result_len=*/0, &vkk_key)) {
    LOG(ERROR) << "vkk_key HKDF derivation failed for revocation";
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(kLocRevocationHkdfFailedInDerive),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        CryptoError::CE_OTHER_CRYPTO);
  }

  key_blobs->vkk_key = std::move(vkk_key);
  return OkStatus<CryptohomeCryptoError>();
}

CryptoStatus Revoke(AuthBlockType auth_block_type,
                    const hwsec::PinWeaverManagerFrontend* hwsec_pw_manager,
                    const RevocationState& revocation_state) {
  if (!revocation_state.le_label.has_value()) {
    LOG(ERROR)
        << "Failed to revoke secret: revocation_state.le_label is not set";
    return MakeStatus<CryptohomeCryptoError>(
        CRYPTOHOME_ERR_LOC(kLocRevocationNoLeLabelInRevoke),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        CryptoError::CE_OTHER_CRYPTO);
  }

  hwsec::Status result = hwsec_pw_manager->RemoveCredential(
      /*label=*/revocation_state.le_label.value());

  if (!result.ok()) {
    LOG(ERROR) << "RemoveCredential failed for revocation with error: "
               << result.err_status();
    ReportRevokeCredentialResult(auth_block_type, result->ToTPMRetryAction());
    CryptoError revoke_error =
        RevokeTPMRetryActionToCryptoError(result->ToTPMRetryAction());
    if (revoke_error == CryptoError::CE_NONE) {
      // This case is considered a success - do not return an error here. See
      // the comment in `RevokeTPMRetryActionToCryptoError`.
      return OkStatus<CryptohomeCryptoError>();
    }
    // Note: the local error must be overridden with revoke_error.
    return MakeStatus<CryptohomeCryptoError>(
               CRYPTOHOME_ERR_LOC(kLocRevocationRemoveCredentialFailedInRevoke),
               ErrorActionSet({}), revoke_error)
        .Wrap(MakeStatus<error::CryptohomeTPMError>(
            std::move(result).err_status()));
  }

  ReportRevokeCredentialResult(auth_block_type, hwsec::TPMRetryAction::kNone);
  return OkStatus<CryptohomeCryptoError>();
}

}  // namespace revocation
}  // namespace cryptohome
