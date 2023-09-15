// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/user_secret_stash/decrypted.h"

#include <map>
#include <string>
#include <utility>

#include <base/system/sys_info.h>
#include <brillo/secure_blob.h>
#include <cryptohome/proto_bindings/UserDataAuth.pb.h>
#include <libhwsec-foundation/crypto/aes.h>
#include <libhwsec-foundation/crypto/secure_blob_util.h>
#include <libhwsec-foundation/status/status_chain.h>
#include <libhwsec-foundation/status/status_chain_macros.h>

#include "cryptohome/error/action.h"
#include "cryptohome/error/cryptohome_error.h"
#include "cryptohome/error/location_utils.h"
#include "cryptohome/error/locations.h"
#include "cryptohome/flatbuffer_schemas/user_secret_stash_payload.h"
#include "cryptohome/storage/file_system_keyset.h"
#include "cryptohome/user_secret_stash/encrypted.h"
#include "cryptohome/user_secret_stash/storage.h"
#include "cryptohome/username.h"

namespace cryptohome {
namespace {

using ::cryptohome::error::CryptohomeError;
using ::cryptohome::error::ErrorActionSet;
using ::cryptohome::error::PossibleAction;
using ::hwsec_foundation::AesGcmEncrypt;
using ::hwsec_foundation::CreateSecureRandomBlob;
using ::hwsec_foundation::kAesGcm256KeySize;
using ::hwsec_foundation::status::MakeStatus;
using ::hwsec_foundation::status::OkStatus;

// Construct a FileSystemKeyset from a given USS payload. Returns an error if
// any of the components of the keyset appear to be missing.
CryptohomeStatusOr<FileSystemKeyset> GetFileSystemKeysetFromPayload(
    const UserSecretStashPayload& payload) {
  if (payload.fek.empty()) {
    LOG(ERROR) << "UserSecretStashPayload has no FEK.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSNoFEKInGetFSKeyFromPayload),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_BACKING_STORE_FAILURE);
  }
  if (payload.fnek.empty()) {
    LOG(ERROR) << "UserSecretStashPayload has no FNEK.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSNoFNEKInGetFSKeyFromPayload),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_BACKING_STORE_FAILURE);
  }
  if (payload.fek_salt.empty()) {
    LOG(ERROR) << "UserSecretStashPayload has no FEK salt.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSNoFEKSaltInGetFSKeyFromPayload),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_BACKING_STORE_FAILURE);
  }
  if (payload.fnek_salt.empty()) {
    LOG(ERROR) << "UserSecretStashPayload has no FNEK salt.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSNoFNEKSaltInGetFSKeyFromPayload),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_BACKING_STORE_FAILURE);
  }
  if (payload.fek_sig.empty()) {
    LOG(ERROR) << "UserSecretStashPayload has no FEK signature.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSNoFEKSigInGetFSKeyFromPayload),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_BACKING_STORE_FAILURE);
  }
  if (payload.fnek_sig.empty()) {
    LOG(ERROR) << "UserSecretStashPayload has no FNEK signature.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSNoFNEKSigInGetFSKeyFromPayload),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_BACKING_STORE_FAILURE);
  }
  if (payload.chaps_key.empty()) {
    LOG(ERROR) << "UserSecretStashPayload has no Chaps key.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSNoChapsKeyInGetFSKeyFromPayload),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_BACKING_STORE_FAILURE);
  }
  FileSystemKey file_system_key = {
      .fek = payload.fek,
      .fnek = payload.fnek,
      .fek_salt = payload.fek_salt,
      .fnek_salt = payload.fnek_salt,
  };
  FileSystemKeyReference file_system_key_reference = {
      .fek_sig = payload.fek_sig,
      .fnek_sig = payload.fnek_sig,
  };
  return FileSystemKeyset(std::move(file_system_key),
                          std::move(file_system_key_reference),
                          payload.chaps_key);
}

// Loads the current OS version from the CHROMEOS_RELEASE_VERSION field in
// /etc/lsb-release. Returns an empty string if that is not available.
std::string GetCurrentOsVersion() {
  std::string version;
  if (!base::SysInfo::GetLsbReleaseValue("CHROMEOS_RELEASE_VERSION",
                                         &version)) {
    return std::string();
  }
  return version;
}

// Use the main key to encrypt all the given data into the USS container. This
// will replace the ciphertext, IV and GCM tag in the container.
CryptohomeStatus EncryptIntoContainer(
    const brillo::SecureBlob& main_key,
    const FileSystemKeyset& file_system_keyset,
    const std::map<std::string, brillo::SecureBlob>& reset_secrets,
    const std::map<AuthFactorType, brillo::SecureBlob>&
        rate_limiter_reset_secrets,
    EncryptedUss::Container& container) {
  // Create a basic payload with the filesystem keys.
  UserSecretStashPayload payload = {
      .fek = file_system_keyset.Key().fek,
      .fnek = file_system_keyset.Key().fnek,
      .fek_salt = file_system_keyset.Key().fek_salt,
      .fnek_salt = file_system_keyset.Key().fnek_salt,
      .fek_sig = file_system_keyset.KeyReference().fek_sig,
      .fnek_sig = file_system_keyset.KeyReference().fnek_sig,
      .chaps_key = file_system_keyset.chaps_key(),
  };

  // Copy all of the reset secrets into the payload.
  for (const auto& [auth_factor_label, reset_secret] : reset_secrets) {
    payload.reset_secrets.push_back(ResetSecretMapping{
        .auth_factor_label = auth_factor_label,
        .reset_secret = reset_secret,
    });
  }
  for (const auto& [auth_factor_type, reset_secret] :
       rate_limiter_reset_secrets) {
    payload.rate_limiter_reset_secrets.push_back(TypeToResetSecretMapping{
        .auth_factor_type = static_cast<uint32_t>(auth_factor_type),
        .reset_secret = reset_secret,
    });
  }

  // Serialize and then encrypt the payload.
  auto serialized_payload = payload.Serialize();
  if (!serialized_payload) {
    LOG(ERROR) << "Failed to serialize UserSecretStashPayload.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSPayloadSerializeFailedInGetEncContainer),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState,
                        PossibleAction::kAuth, PossibleAction::kDeleteVault}),
        user_data_auth::CRYPTOHOME_ERROR_BACKING_STORE_FAILURE);
  }
  brillo::Blob iv, tag, ciphertext;
  if (!AesGcmEncrypt(serialized_payload.value(), /*ad=*/std::nullopt, main_key,
                     &iv, &tag, &ciphertext)) {
    LOG(ERROR) << "Failed to encrypt UserSecretStash.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSPayloadEncryptFailedInGetEncContainer),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState,
                        PossibleAction::kAuth, PossibleAction::kDeleteVault}),
        user_data_auth::CRYPTOHOME_ERROR_BACKING_STORE_FAILURE);
  }

  // Copy the resulting encrypted output into the container.
  container.ciphertext = std::move(ciphertext);
  container.iv = std::move(iv);
  container.gcm_tag = std::move(tag);

  return OkStatus<CryptohomeError>();
}

}  // namespace

CryptohomeStatus DecryptedUss::Transaction::InsertWrappedMainKey(
    std::string wrapping_id, const brillo::SecureBlob& wrapping_key) {
  // Check if the wrapping ID already exists and return an error if it does. If
  // it doesn't exist then the rest of the work can be delegated to assign.
  if (container_.wrapped_key_blocks.contains(wrapping_id)) {
    LOG(ERROR) << "A UserSecretStash main key with the given wrapping_id "
                  "already exists.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSDuplicateWrappingInInsertWrappedMainKey),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState,
                        PossibleAction::kAuth, PossibleAction::kDeleteVault}),
        user_data_auth::CRYPTOHOME_ERROR_AUTHORIZATION_KEY_FAILED);
  }
  return AssignWrappedMainKey(std::move(wrapping_id), wrapping_key);
}

CryptohomeStatus DecryptedUss::Transaction::AssignWrappedMainKey(
    std::string wrapping_id, const brillo::SecureBlob& wrapping_key) {
  // Verify that both the wrapping ID and wrapping key are valid.
  if (wrapping_id.empty()) {
    LOG(ERROR)
        << "Empty wrapping ID is passed for UserSecretStash main key wrapping.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSWrappingIDEmptyInAssignWrappedMainKey),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_INVALID_ARGUMENT);
  }
  if (wrapping_key.size() != kAesGcm256KeySize) {
    LOG(ERROR) << "Wrong wrapping key size is passed for UserSecretStash "
                  "main key wrapping. Received: "
               << wrapping_key.size() << ", expected " << kAesGcm256KeySize
               << ".";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSWrappingWrongSizeInAssignWrappedMainKey),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_INVALID_ARGUMENT);
  }

  // Wrap the main key with the given wrapped key.
  brillo::Blob iv, tag, encrypted_key;
  if (!AesGcmEncrypt(uss_.main_key_, /*ad=*/std::nullopt, wrapping_key, &iv,
                     &tag, &encrypted_key)) {
    LOG(ERROR) << "Failed to wrap UserSecretStash main key.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSEncryptFailedInAssignWrappedMainKey),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_AUTHORIZATION_KEY_FAILED);
  }

  // Store the results in the wrapped key map.
  container_.wrapped_key_blocks[std::move(wrapping_id)] =
      EncryptedUss::WrappedKeyBlock{
          .encryption_algorithm =
              UserSecretStashEncryptionAlgorithm::AES_GCM_256,
          .encrypted_key = std::move(encrypted_key),
          .iv = std::move(iv),
          .gcm_tag = std::move(tag),
      };

  return OkStatus<CryptohomeError>();
}

CryptohomeStatus DecryptedUss::Transaction::RenameWrappedMainKey(
    const std::string& old_wrapping_id, std::string new_wrapping_id) {
  if (container_.wrapped_key_blocks.contains(new_wrapping_id)) {
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSNewIdAlreadyExistsInRenameWrappedMainKey),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_INVALID_ARGUMENT);
  }
  auto node = container_.wrapped_key_blocks.extract(old_wrapping_id);
  if (!node) {
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSOldIdDoesntExistInRenameWrappedMainKey),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_INVALID_ARGUMENT);
  }
  node.key() = std::move(new_wrapping_id);
  container_.wrapped_key_blocks.insert(std::move(node));
  return OkStatus<CryptohomeError>();
}

CryptohomeStatus DecryptedUss::Transaction::RemoveWrappedMainKey(
    const std::string& wrapping_id) {
  auto iter = container_.wrapped_key_blocks.find(wrapping_id);
  if (iter == container_.wrapped_key_blocks.end()) {
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSIdDoesntExistInRemoveWrappedMainKey),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_INVALID_ARGUMENT);
  }
  container_.wrapped_key_blocks.erase(iter);
  return OkStatus<CryptohomeError>();
}

CryptohomeStatus DecryptedUss::Transaction::InsertResetSecret(
    std::string label, brillo::SecureBlob secret) {
  auto [iter, was_inserted] =
      reset_secrets_.emplace(std::move(label), std::move(secret));
  if (!was_inserted) {
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSSecretAlreadyExistsInInsertResetSecret),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_INVALID_ARGUMENT);
  }
  return OkStatus<CryptohomeError>();
}

CryptohomeStatus DecryptedUss::Transaction::RemoveResetSecret(
    const std::string& label) {
  auto iter = reset_secrets_.find(label);
  if (iter == reset_secrets_.end()) {
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSResetSecretDoesntExistInRemove),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_INVALID_ARGUMENT);
  }
  reset_secrets_.erase(iter);
  return OkStatus<CryptohomeError>();
}

CryptohomeStatus DecryptedUss::Transaction::InsertRateLimiterResetSecret(
    AuthFactorType auth_factor_type, brillo::SecureBlob secret) {
  auto [iter, was_inserted] =
      rate_limiter_reset_secrets_.emplace(auth_factor_type, std::move(secret));
  if (!was_inserted) {
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(
            kLocUSSSecretAlreadyExistsInInsertRateLimiterResetSecret),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_INVALID_ARGUMENT);
  }
  return OkStatus<CryptohomeError>();
}

CryptohomeStatus DecryptedUss::Transaction::InitializeFingerprintRateLimiterId(
    uint64_t id) {
  if (container_.user_metadata.fingerprint_rate_limiter_id) {
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUssInitializeAlreadySetFpRateLimiterId),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_INVALID_ARGUMENT);
  }
  container_.user_metadata.fingerprint_rate_limiter_id = id;
  return OkStatus<CryptohomeError>();
}

CryptohomeStatus DecryptedUss::Transaction::Commit() && {
  RETURN_IF_ERROR(EncryptIntoContainer(
      uss_.main_key_, uss_.file_system_keyset_, reset_secrets_,
      rate_limiter_reset_secrets_, container_));
  uss_.encrypted_ = EncryptedUss(std::move(container_));
  uss_.reset_secrets_ = std::move(reset_secrets_);
  uss_.rate_limiter_reset_secrets_ = std::move(rate_limiter_reset_secrets_);
  return OkStatus<CryptohomeError>();
}

CryptohomeStatus DecryptedUss::Transaction::Commit(
    const ObfuscatedUsername& username, UssStorage& storage) && {
  // Build a new EncryptedUss with new ciphertext that reflects all of the
  // changes in the transaction.
  RETURN_IF_ERROR(EncryptIntoContainer(
      uss_.main_key_, uss_.file_system_keyset_, reset_secrets_,
      rate_limiter_reset_secrets_, container_));
  EncryptedUss encrypted_uss(std::move(container_));
  // Persist the new encrypted data out to storage.
  RETURN_IF_ERROR(encrypted_uss.ToStorage(username, storage));
  // The stored USS is updated so push the updates in-memory as well.
  uss_.encrypted_ = std::move(encrypted_uss);
  uss_.reset_secrets_ = std::move(reset_secrets_);
  uss_.rate_limiter_reset_secrets_ = std::move(rate_limiter_reset_secrets_);
  return OkStatus<CryptohomeError>();
}

DecryptedUss::Transaction::Transaction(
    DecryptedUss& uss,
    EncryptedUss::Container container,
    std::map<std::string, brillo::SecureBlob> reset_secrets,
    std::map<AuthFactorType, brillo::SecureBlob> rate_limiter_reset_secrets)
    : uss_(uss),
      container_(std::move(container)),
      reset_secrets_(std::move(reset_secrets)),
      rate_limiter_reset_secrets_(std::move(rate_limiter_reset_secrets)) {}

CryptohomeStatusOr<DecryptedUss> DecryptedUss::CreateWithMainKey(
    FileSystemKeyset file_system_keyset, brillo::SecureBlob main_key) {
  // Check that the given key has the correct size.
  if (main_key.size() != kAesGcm256KeySize) {
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSMainKeyWrongSizeInCreateUss),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_INVALID_ARGUMENT);
  }

  // Construct a new encrypted container with minimal data.
  EncryptedUss::Container container;
  container.created_on_os_version = GetCurrentOsVersion();
  RETURN_IF_ERROR(
      EncryptIntoContainer(main_key, file_system_keyset, {}, {}, container));

  return DecryptedUss(EncryptedUss(std::move(container)), std::move(main_key),
                      std::move(file_system_keyset), {}, {});
}

CryptohomeStatusOr<DecryptedUss> DecryptedUss::CreateWithRandomMainKey(
    FileSystemKeyset file_system_keyset) {
  // Generate a new main key and delegate to the WithMainKey factor.
  return CreateWithMainKey(std::move(file_system_keyset),
                           CreateSecureRandomBlob(kAesGcm256KeySize));
}

CryptohomeStatusOr<DecryptedUss> DecryptedUss::FromBlobUsingMainKey(
    const brillo::Blob& flatbuffer, brillo::SecureBlob main_key) {
  ASSIGN_OR_RETURN(EncryptedUss encrypted, EncryptedUss::FromBlob(flatbuffer));
  return FromEncryptedUss(std::move(encrypted), std::move(main_key));
}

CryptohomeStatusOr<DecryptedUss> DecryptedUss::FromBlobUsingWrappedKey(
    const brillo::Blob& flatbuffer,
    const std::string& wrapping_id,
    const brillo::SecureBlob& wrapping_key) {
  ASSIGN_OR_RETURN(EncryptedUss encrypted, EncryptedUss::FromBlob(flatbuffer));
  ASSIGN_OR_RETURN(brillo::SecureBlob main_key,
                   encrypted.UnwrapMainKey(wrapping_id, wrapping_key));
  return FromEncryptedUss(std::move(encrypted), std::move(main_key));
}

DecryptedUss DecryptedUss::CreateCopyForSnapshot() const {
  return DecryptedUss(EncryptedUss(encrypted_.container({})), main_key_,
                      file_system_keyset_, reset_secrets_,
                      rate_limiter_reset_secrets_);
}

CryptohomeStatusOr<DecryptedUss> DecryptedUss::FromEncryptedUss(
    EncryptedUss encrypted, brillo::SecureBlob main_key) {
  // Use the main key to decrypt the USS payload.
  ASSIGN_OR_RETURN(brillo::SecureBlob serialized_payload,
                   encrypted.DecryptPayload(main_key));

  // Deserialize the decrypted payload into a flatbuffer.
  std::optional<UserSecretStashPayload> payload =
      UserSecretStashPayload::Deserialize(serialized_payload);
  if (!payload) {
    LOG(ERROR) << "Failed to deserialize UserSecretStashPayload.";
    return MakeStatus<CryptohomeError>(
        CRYPTOHOME_ERR_LOC(kLocUSSDeserializeFailedInFromEncPayload),
        ErrorActionSet({PossibleAction::kDevCheckUnexpectedState}),
        user_data_auth::CRYPTOHOME_ERROR_BACKING_STORE_FAILURE);
  }

  // Extract the filesystem keyset from the payload.
  ASSIGN_OR_RETURN(FileSystemKeyset file_system_keyset,
                   GetFileSystemKeysetFromPayload(*payload),
                   _.LogError() << "UserSecretStashPayload has invalid file "
                                   "system keyset information.");

  // Extract the reset secrets from the payload.
  std::map<std::string, brillo::SecureBlob> reset_secrets;
  for (const ResetSecretMapping& item : payload->reset_secrets) {
    auto [iter, was_inserted] = reset_secrets.emplace(
        std::move(item.auth_factor_label), std::move(item.reset_secret));
    if (!was_inserted) {
      LOG(ERROR) << "UserSecretStashPayload contains multiple reset secrets "
                    "for label: "
                 << iter->first;
    }
  }

  // Extract the rate limiter secrets from the payload.
  std::map<AuthFactorType, brillo::SecureBlob> rate_limiter_reset_secrets;
  for (const TypeToResetSecretMapping& item :
       payload->rate_limiter_reset_secrets) {
    if (!item.auth_factor_type) {
      LOG(ERROR)
          << "UserSecretStashPayload contains reset secret with missing type.";
      continue;
    }
    if (*item.auth_factor_type >=
        static_cast<unsigned int>(AuthFactorType::kUnspecified)) {
      LOG(ERROR)
          << "UserSecretStashPayload contains reset secret for invalid type: "
          << *item.auth_factor_type << ".";
      continue;
    }
    AuthFactorType auth_factor_type =
        static_cast<AuthFactorType>(*item.auth_factor_type);
    auto [iter, was_inserted] = rate_limiter_reset_secrets.emplace(
        auth_factor_type, std::move(item.reset_secret));
    if (!was_inserted) {
      LOG(ERROR) << "UserSecretStashPayload contains multiple reset secrets "
                    "for type: "
                 << AuthFactorTypeToString(iter->first) << ".";
    }
  }

  return DecryptedUss(std::move(encrypted), std::move(main_key),
                      std::move(file_system_keyset), std::move(reset_secrets),
                      std::move(rate_limiter_reset_secrets));
}

std::optional<brillo::SecureBlob> DecryptedUss::GetResetSecret(
    const std::string& label) const {
  auto iter = reset_secrets_.find(label);
  if (iter == reset_secrets_.end()) {
    return std::nullopt;
  }
  return iter->second;
}

std::optional<brillo::SecureBlob> DecryptedUss::GetRateLimiterResetSecret(
    AuthFactorType auth_factor_type) const {
  auto iter = rate_limiter_reset_secrets_.find(auth_factor_type);
  if (iter == rate_limiter_reset_secrets_.end()) {
    return std::nullopt;
  }
  return iter->second;
}

DecryptedUss::Transaction DecryptedUss::StartTransaction() {
  // Note that it's important we directly return the object this way in order to
  // get copy and move elison, since transaction supports neither of those.
  return {*this, encrypted_.container({}), reset_secrets_,
          rate_limiter_reset_secrets_};
}

DecryptedUss::DecryptedUss(
    EncryptedUss encrypted,
    brillo::SecureBlob main_key,
    FileSystemKeyset file_system_keyset,
    std::map<std::string, brillo::SecureBlob> reset_secrets,
    std::map<AuthFactorType, brillo::SecureBlob> rate_limiter_reset_secrets)
    : encrypted_(std::move(encrypted)),
      main_key_(std::move(main_key)),
      file_system_keyset_(std::move(file_system_keyset)),
      reset_secrets_(std::move(reset_secrets)),
      rate_limiter_reset_secrets_(std::move(rate_limiter_reset_secrets)) {}

}  // namespace cryptohome
