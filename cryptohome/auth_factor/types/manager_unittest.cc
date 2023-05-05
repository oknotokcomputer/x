// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/auth_factor/types/manager.h"

#include <base/functional/callback.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <libhwsec/frontend/cryptohome/mock_frontend.h>
#include <libhwsec/frontend/pinweaver/mock_frontend.h>

#include "cryptohome/auth_factor/auth_factor_label_arity.h"
#include "cryptohome/auth_factor/auth_factor_type.h"
#include "cryptohome/auth_factor/types/interface.h"
#include "cryptohome/auth_intent.h"
#include "cryptohome/crypto.h"
#include "cryptohome/mock_cryptohome_keys_manager.h"
#include "cryptohome/mock_fingerprint_manager.h"

namespace cryptohome {
namespace {

using ::testing::Eq;
using ::testing::IsFalse;
using ::testing::IsTrue;
using ::testing::Ref;

class AuthFactorDriverManagerTest : public ::testing::Test {
 protected:
  // Mocks for all of the manager dependencies.
  hwsec::MockCryptohomeFrontend hwsec_;
  hwsec::MockPinWeaverFrontend pinweaver_;
  MockCryptohomeKeysManager cryptohome_keys_manager_;
  Crypto crypto_{&hwsec_, &pinweaver_, &cryptohome_keys_manager_,
                 /*recovery_hwsec=*/nullptr};
  MockFingerprintManager fp_manager_;
  FingerprintAuthBlockService fp_service_{
      AsyncInitPtr<FingerprintManager>(&fp_manager_), base::DoNothing()};

  // A real version of the manager, using mock inputs.
  AuthFactorDriverManager manager_{
      &crypto_, AsyncInitPtr<ChallengeCredentialsHelper>(nullptr), nullptr,
      &fp_service_, AsyncInitPtr<BiometricsAuthBlockService>(nullptr)};
};

TEST_F(AuthFactorDriverManagerTest, GetDriverIsSameForConstAndNonconst) {
  const auto& const_manager = manager_;

  EXPECT_THAT(manager_.GetDriver(AuthFactorType::kPassword),
              Ref(const_manager.GetDriver(AuthFactorType::kPassword)));
  EXPECT_THAT(manager_.GetDriver(AuthFactorType::kPin),
              Ref(const_manager.GetDriver(AuthFactorType::kPin)));
  EXPECT_THAT(
      manager_.GetDriver(AuthFactorType::kCryptohomeRecovery),
      Ref(const_manager.GetDriver(AuthFactorType::kCryptohomeRecovery)));
  EXPECT_THAT(manager_.GetDriver(AuthFactorType::kKiosk),
              Ref(const_manager.GetDriver(AuthFactorType::kKiosk)));
  EXPECT_THAT(manager_.GetDriver(AuthFactorType::kSmartCard),
              Ref(const_manager.GetDriver(AuthFactorType::kSmartCard)));
  EXPECT_THAT(manager_.GetDriver(AuthFactorType::kLegacyFingerprint),
              Ref(const_manager.GetDriver(AuthFactorType::kLegacyFingerprint)));
  EXPECT_THAT(manager_.GetDriver(AuthFactorType::kFingerprint),
              Ref(const_manager.GetDriver(AuthFactorType::kFingerprint)));

  static_assert(static_cast<int>(AuthFactorType::kUnspecified) == 7,
                "All types of AuthFactorType are not all included here");
}

// Test AuthFactorDriver::IsPrepareRequired. We do this here instead of in a
// per-driver test because the check is trivial enough that one test is simpler
// to validate than N separate tests.
TEST_F(AuthFactorDriverManagerTest, IsPrepareRequired) {
  auto prepare_req = [this](AuthFactorType type) {
    return manager_.GetDriver(type).IsPrepareRequired();
  };

  EXPECT_THAT(prepare_req(AuthFactorType::kPassword), IsFalse());
  EXPECT_THAT(prepare_req(AuthFactorType::kPin), IsFalse());
  EXPECT_THAT(prepare_req(AuthFactorType::kCryptohomeRecovery), IsFalse());
  EXPECT_THAT(prepare_req(AuthFactorType::kKiosk), IsFalse());
  EXPECT_THAT(prepare_req(AuthFactorType::kSmartCard), IsFalse());
  EXPECT_THAT(prepare_req(AuthFactorType::kLegacyFingerprint), IsTrue());
  EXPECT_THAT(prepare_req(AuthFactorType::kFingerprint), IsTrue());

  EXPECT_THAT(prepare_req(AuthFactorType::kUnspecified), IsFalse());
  static_assert(static_cast<int>(AuthFactorType::kUnspecified) == 7,
                "All types of AuthFactorType are not all included here");
}

// Test AuthFactorDriver::IsVerifySupported. We do this here instead of in a
// per-driver test because the check is trivial enough that one test is simpler
// to validate than N separate tests.
TEST_F(AuthFactorDriverManagerTest, IsVerifySupported) {
  auto decrypt_verify = [this](AuthFactorType type) {
    return manager_.GetDriver(type).IsVerifySupported(AuthIntent::kDecrypt);
  };
  auto vonly_verify = [this](AuthFactorType type) {
    return manager_.GetDriver(type).IsVerifySupported(AuthIntent::kVerifyOnly);
  };
  auto webauthn_verify = [this](AuthFactorType type) {
    return manager_.GetDriver(type).IsVerifySupported(AuthIntent::kWebAuthn);
  };

  EXPECT_THAT(decrypt_verify(AuthFactorType::kPassword), IsFalse());
  EXPECT_THAT(decrypt_verify(AuthFactorType::kPin), IsFalse());
  EXPECT_THAT(decrypt_verify(AuthFactorType::kCryptohomeRecovery), IsFalse());
  EXPECT_THAT(decrypt_verify(AuthFactorType::kKiosk), IsFalse());
  EXPECT_THAT(decrypt_verify(AuthFactorType::kSmartCard), IsFalse());
  EXPECT_THAT(decrypt_verify(AuthFactorType::kLegacyFingerprint), IsFalse());
  EXPECT_THAT(decrypt_verify(AuthFactorType::kFingerprint), IsFalse());

  EXPECT_THAT(vonly_verify(AuthFactorType::kPassword), IsTrue());
  EXPECT_THAT(vonly_verify(AuthFactorType::kPin), IsFalse());
  EXPECT_THAT(vonly_verify(AuthFactorType::kCryptohomeRecovery), IsFalse());
  EXPECT_THAT(vonly_verify(AuthFactorType::kKiosk), IsFalse());
  EXPECT_THAT(vonly_verify(AuthFactorType::kSmartCard), IsTrue());
  EXPECT_THAT(vonly_verify(AuthFactorType::kLegacyFingerprint), IsTrue());
  EXPECT_THAT(vonly_verify(AuthFactorType::kFingerprint), IsFalse());

  EXPECT_THAT(webauthn_verify(AuthFactorType::kPassword), IsFalse());
  EXPECT_THAT(webauthn_verify(AuthFactorType::kPin), IsFalse());
  EXPECT_THAT(webauthn_verify(AuthFactorType::kCryptohomeRecovery), IsFalse());
  EXPECT_THAT(webauthn_verify(AuthFactorType::kKiosk), IsFalse());
  EXPECT_THAT(webauthn_verify(AuthFactorType::kSmartCard), IsFalse());
  EXPECT_THAT(webauthn_verify(AuthFactorType::kLegacyFingerprint), IsTrue());
  EXPECT_THAT(webauthn_verify(AuthFactorType::kFingerprint), IsFalse());

  EXPECT_THAT(decrypt_verify(AuthFactorType::kUnspecified), IsFalse());
  EXPECT_THAT(vonly_verify(AuthFactorType::kUnspecified), IsFalse());
  EXPECT_THAT(webauthn_verify(AuthFactorType::kUnspecified), IsFalse());
  static_assert(static_cast<int>(AuthFactorType::kUnspecified) == 7,
                "All types of AuthFactorType are not all included here");
}

// Test AuthFactorDriver::NeedsResetSecret. We do this here instead of in a
// per-driver test because the check is trivial enough that one test is simpler
// to validate than N separate tests.
TEST_F(AuthFactorDriverManagerTest, NeedsResetSecret) {
  auto needs_secret = [this](AuthFactorType type) {
    return manager_.GetDriver(type).NeedsResetSecret();
  };

  EXPECT_THAT(needs_secret(AuthFactorType::kPassword), IsFalse());
  EXPECT_THAT(needs_secret(AuthFactorType::kPin), IsTrue());
  EXPECT_THAT(needs_secret(AuthFactorType::kCryptohomeRecovery), IsFalse());
  EXPECT_THAT(needs_secret(AuthFactorType::kKiosk), IsFalse());
  EXPECT_THAT(needs_secret(AuthFactorType::kSmartCard), IsFalse());
  EXPECT_THAT(needs_secret(AuthFactorType::kLegacyFingerprint), IsFalse());
  EXPECT_THAT(needs_secret(AuthFactorType::kFingerprint), IsFalse());

  EXPECT_THAT(needs_secret(AuthFactorType::kUnspecified), IsFalse());
  static_assert(static_cast<int>(AuthFactorType::kUnspecified) == 7,
                "All types of AuthFactorType are not all included here");
}

// Test AuthFactorDriver::NeedsRateLimiter. We do this here instead of in a
// per-driver test because the check is trivial enough that one test is simpler
// to validate than N separate tests.
TEST_F(AuthFactorDriverManagerTest, NeedsRateLimiter) {
  auto needs_limiter = [this](AuthFactorType type) {
    return manager_.GetDriver(type).NeedsRateLimiter();
  };

  EXPECT_THAT(needs_limiter(AuthFactorType::kPassword), IsFalse());
  EXPECT_THAT(needs_limiter(AuthFactorType::kPin), IsFalse());
  EXPECT_THAT(needs_limiter(AuthFactorType::kCryptohomeRecovery), IsFalse());
  EXPECT_THAT(needs_limiter(AuthFactorType::kKiosk), IsFalse());
  EXPECT_THAT(needs_limiter(AuthFactorType::kSmartCard), IsFalse());
  EXPECT_THAT(needs_limiter(AuthFactorType::kLegacyFingerprint), IsFalse());
  EXPECT_THAT(needs_limiter(AuthFactorType::kFingerprint), IsTrue());

  EXPECT_THAT(needs_limiter(AuthFactorType::kUnspecified), IsFalse());

  static_assert(static_cast<int>(AuthFactorType::kUnspecified) == 7,
                "All types of AuthFactorType are not all included here");
}

// Test AuthFactorDriver::IsDelaySupported. We do this here instead of in a
// per-driver test because the check is trivial enough that one test is simpler
// to validate than N separate tests.
TEST_F(AuthFactorDriverManagerTest, IsDelaySupported) {
  auto is_delayable = [this](AuthFactorType type) {
    return manager_.GetDriver(type).IsDelaySupported();
  };

  EXPECT_THAT(is_delayable(AuthFactorType::kPassword), IsFalse());
  EXPECT_THAT(is_delayable(AuthFactorType::kPin), IsTrue());
  EXPECT_THAT(is_delayable(AuthFactorType::kCryptohomeRecovery), IsFalse());
  EXPECT_THAT(is_delayable(AuthFactorType::kKiosk), IsFalse());
  EXPECT_THAT(is_delayable(AuthFactorType::kSmartCard), IsFalse());
  EXPECT_THAT(is_delayable(AuthFactorType::kLegacyFingerprint), IsFalse());
  EXPECT_THAT(is_delayable(AuthFactorType::kFingerprint), IsFalse());

  EXPECT_THAT(is_delayable(AuthFactorType::kUnspecified), IsFalse());

  static_assert(static_cast<int>(AuthFactorType::kUnspecified) == 7,
                "All types of AuthFactorType are not all included here");
}

// Test AuthFactorDriver::GetAuthFactorLabelArity. We do this here instead of in
// a per-driver test because the check is trivial enough that one test is
// simpler to validate than N separate tests.
TEST_F(AuthFactorDriverManagerTest, GetAuthFactorLabelArity) {
  auto get_arity = [this](AuthFactorType type) {
    return manager_.GetDriver(type).GetAuthFactorLabelArity();
  };

  EXPECT_THAT(get_arity(AuthFactorType::kPassword),
              Eq(AuthFactorLabelArity::kSingle));
  EXPECT_THAT(get_arity(AuthFactorType::kPin),
              Eq(AuthFactorLabelArity::kSingle));
  EXPECT_THAT(get_arity(AuthFactorType::kCryptohomeRecovery),
              Eq(AuthFactorLabelArity::kSingle));
  EXPECT_THAT(get_arity(AuthFactorType::kKiosk),
              Eq(AuthFactorLabelArity::kSingle));
  EXPECT_THAT(get_arity(AuthFactorType::kSmartCard),
              Eq(AuthFactorLabelArity::kSingle));
  EXPECT_THAT(get_arity(AuthFactorType::kLegacyFingerprint),
              Eq(AuthFactorLabelArity::kNone));
  EXPECT_THAT(get_arity(AuthFactorType::kFingerprint),
              Eq(AuthFactorLabelArity::kMultiple));

  EXPECT_THAT(get_arity(AuthFactorType::kUnspecified),
              Eq(AuthFactorLabelArity::kNone));
  static_assert(static_cast<int>(AuthFactorType::kUnspecified) == 7,
                "All types of AuthFactorType are not all included here");
}

}  // namespace
}  // namespace cryptohome
