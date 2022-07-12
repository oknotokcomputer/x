// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/auth_factor/auth_factor.h"

#include <memory>
#include <utility>

#include <base/logging.h>
#include <cryptohome/proto_bindings/UserDataAuth.pb.h>

#include "cryptohome/auth_blocks/auth_block_utility.h"
#include "cryptohome/crypto_error.h"
#include "cryptohome/error/converter.h"
#include "cryptohome/error/location_utils.h"
#include "cryptohome/key_objects.h"

using cryptohome::error::CryptohomeCryptoError;
using hwsec_foundation::status::MakeStatus;
using hwsec_foundation::status::OkStatus;

namespace cryptohome {

AuthFactor::AuthFactor(AuthFactorType type,
                       const std::string& label,
                       const AuthFactorMetadata& metadata,
                       const AuthBlockState& auth_block_state)
    : type_(type),
      label_(label),
      metadata_(metadata),
      auth_block_state_(auth_block_state) {}

CryptoStatus AuthFactor::Authenticate(const AuthInput& auth_input,
                                      AuthBlockUtility* auth_block_utility,
                                      KeyBlobs& out_key_blobs,
                                      AuthBlockType& out_auth_block_type) {
  CryptoStatus crypto_error = auth_block_utility->DeriveKeyBlobs(
      auth_input, auth_block_state_, out_key_blobs, out_auth_block_type);
  if (!crypto_error.ok()) {
    LOG(ERROR) << "Auth factor authentication failed: error " << crypto_error;
    return MakeStatus<CryptohomeCryptoError>(
               CRYPTOHOME_ERR_LOC(kLocAuthFactorDeriveFailedInAuth))
        .Wrap(std::move(crypto_error));
  }
  return OkStatus<CryptohomeCryptoError>();
}

CryptoStatus AuthFactor::PrepareForRemoval(
    AuthBlockUtility* auth_block_utility) {
  CryptoStatus crypto_error =
      auth_block_utility->PrepareAuthBlockForRemoval(auth_block_state_);
  if (!crypto_error.ok()) {
    LOG(ERROR) << "Prepare auth factor for removal failed: error "
               << crypto_error;
    return MakeStatus<CryptohomeCryptoError>(
               CRYPTOHOME_ERR_LOC(kLocAuthFactorPrepareForRemovalFailed))
        .Wrap(std::move(crypto_error));
  }
  return OkStatus<CryptohomeCryptoError>();
}

}  // namespace cryptohome
