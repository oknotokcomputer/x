// Copyright 2020 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_AUTH_BLOCKS_TPM_NOT_BOUND_TO_PCR_AUTH_BLOCK_H_
#define CRYPTOHOME_AUTH_BLOCKS_TPM_NOT_BOUND_TO_PCR_AUTH_BLOCK_H_

#include <memory>

#include <base/gtest_prod_util.h>
#include <libhwsec/frontend/cryptohome/frontend.h>

#include "cryptohome/auth_blocks/auth_block.h"
#include "cryptohome/auth_blocks/auth_block_type.h"
#include "cryptohome/auth_blocks/tpm_auth_block_utils.h"
#include "cryptohome/crypto.h"
#include "cryptohome/cryptohome_keys_manager.h"
#include "cryptohome/error/cryptohome_crypto_error.h"
#include "cryptohome/flatbuffer_schemas/auth_block_state.h"

namespace cryptohome {

class TpmNotBoundToPcrAuthBlock : public AuthBlock {
 public:
  // Implement the GenericAuthBlock concept.
  static constexpr auto kType = AuthBlockType::kTpmNotBoundToPcr;
  using StateType = TpmNotBoundToPcrAuthBlockState;
  static CryptoStatus IsSupported(Crypto& crypto);
  static std::unique_ptr<AuthBlock> New(
      const hwsec::CryptohomeFrontend& hwsec,
      CryptohomeKeysManager& cryptohome_keys_manager);

  TpmNotBoundToPcrAuthBlock(const hwsec::CryptohomeFrontend* hwsec,
                            CryptohomeKeysManager* cryptohome_keys_manager);

  TpmNotBoundToPcrAuthBlock(const TpmNotBoundToPcrAuthBlock&) = delete;
  TpmNotBoundToPcrAuthBlock& operator=(const TpmNotBoundToPcrAuthBlock&) =
      delete;

  void Create(const AuthInput& user_input,
              const AuthFactorMetadata& auth_factor_metadata,
              CreateCallback callback) override;

  void Derive(const AuthInput& auth_input,
              const AuthBlockState& state,
              DeriveCallback callback) override;

 private:
  const hwsec::CryptohomeFrontend* hwsec_;
  CryptohomeKeyLoader* cryptohome_key_loader_;
  TpmAuthBlockUtils utils_;

  FRIEND_TEST_ALL_PREFIXES(TPMAuthBlockTest, DecryptNotBoundToPcrTest);
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_AUTH_BLOCKS_TPM_NOT_BOUND_TO_PCR_AUTH_BLOCK_H_
