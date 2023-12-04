// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_AUTH_FACTOR_TYPES_PIN_H_
#define CRYPTOHOME_AUTH_FACTOR_TYPES_PIN_H_

#include <memory>
#include <optional>
#include <set>
#include <string>

#include <base/time/time.h>
#include <cryptohome/proto_bindings/recoverable_key_store.pb.h>

#include "cryptohome/auth_blocks/auth_block_type.h"
#include "cryptohome/auth_factor/label_arity.h"
#include "cryptohome/auth_factor/metadata.h"
#include "cryptohome/auth_factor/storage_type.h"
#include "cryptohome/auth_factor/type.h"
#include "cryptohome/auth_factor/types/common.h"
#include "cryptohome/auth_factor/types/interface.h"
#include "cryptohome/auth_intent.h"
#include "cryptohome/credential_verifier.h"
#include "cryptohome/crypto.h"
#include "cryptohome/flatbuffer_schemas/auth_factor.h"
#include "cryptohome/key_objects.h"

namespace cryptohome {

class PinAuthFactorDriver final
    : public AfDriverWithType<AuthFactorType::kPin>,
      public AfDriverWithBlockTypes<AuthBlockType::kPinWeaver>,
      public AfDriverSupportedByStorage<AfDriverStorageConfig::kNoChecks,
                                        AfDriverKioskConfig::kNoKiosk>,
      public AfDriverWithMetadata<PinMetadata>,
      public AfDriverNoPrepare,
      public AfDriverFullAuthDecrypt,
      public AfDriverFullAuthIsRepeatable<true>,
      public AfDriverResetCapability<
          AuthFactorDriver::ResetCapability::kResetWrongAttemptsAndExpiration>,
      public AfDriverWithConfigurableIntents<AuthIntentSequence<>,
                                             AuthIntentSequence<>>,
      public AfDriverNoCredentialVerifier,
      public AfDriverNoExpiration,
      public AfDriverNoRateLimiter,
      public AfDriverWithLockScreenKnowledgeFactorType<
          LockScreenKnowledgeFactorType::
              LOCK_SCREEN_KNOWLEDGE_FACTOR_TYPE_PIN> {
 public:
  explicit PinAuthFactorDriver(Crypto* crypto) : crypto_(crypto) {}

 private:
  bool IsSupportedByHardware() const override;
  bool NeedsResetSecret() const override;
  bool IsDelaySupported() const override;
  CryptohomeStatusOr<base::TimeDelta> GetFactorDelay(
      const ObfuscatedUsername& username,
      const AuthFactor& factor) const override;
  AuthFactorLabelArity GetAuthFactorLabelArity() const override;

  std::optional<user_data_auth::AuthFactor> TypedConvertToProto(
      const CommonMetadata& common,
      const PinMetadata& typed_metadata) const override;

  Crypto* crypto_;
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_AUTH_FACTOR_TYPES_PIN_H_
