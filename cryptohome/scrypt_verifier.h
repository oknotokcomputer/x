// Copyright 2021 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_SCRYPT_VERIFIER_H_
#define CRYPTOHOME_SCRYPT_VERIFIER_H_

#include <memory>
#include <string>

#include <brillo/secure_blob.h>

#include "cryptohome/credential_verifier.h"

namespace cryptohome {

class ScryptVerifier final : public CredentialVerifier {
 public:
  // Attempt to construct a credential verifier with the given passkey. Will
  // return null on failure.
  static std::unique_ptr<ScryptVerifier> Create(
      std::string auth_factor_label, const brillo::SecureBlob& passkey);

  ScryptVerifier(const ScryptVerifier&) = delete;
  ScryptVerifier& operator=(const ScryptVerifier&) = delete;

  bool Verify(const brillo::SecureBlob& secret) const override;

 private:
  ScryptVerifier(std::string auth_factor_label,
                 brillo::SecureBlob scrypt_salt,
                 brillo::SecureBlob verifier);

  brillo::SecureBlob scrypt_salt_;
  brillo::SecureBlob verifier_;
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_SCRYPT_VERIFIER_H_
