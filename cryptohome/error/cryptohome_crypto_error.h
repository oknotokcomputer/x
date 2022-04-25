// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_ERROR_CRYPTOHOME_CRYPTO_ERROR_H_
#define CRYPTOHOME_ERROR_CRYPTOHOME_CRYPTO_ERROR_H_

#include <memory>
#include <set>
#include <string>
#include <utility>

#include <base/optional.h>
#include <chromeos/dbus/service_constants.h>
#include <libhwsec-foundation/status/status_chain_or.h>

#include "cryptohome/crypto_error.h"
#include "cryptohome/error/cryptohome_error.h"

namespace cryptohome {

namespace error {

// This class is a CryptohomeError that holds an extra CryptoError.
// It is designed for situations that needs the content of the CryptoError and
// still be compatible with CryptohomeError.
class CryptohomeCryptoError : public CryptohomeError {
 public:
  struct MakeStatusTrait {
    // |Unactioned| represents an intermediate state, when we create an error
    // without fully specifying that error. That allows to require Wrap to be
    // called, or otherwise a type mismatch error will be raised.
    class Unactioned {
     public:
      Unactioned(const ErrorLocationPair& loc,
                 const std::set<CryptohomeError::Action>& actions,
                 const std::optional<user_data_auth::CryptohomeErrorCode> ec);

      hwsec_foundation::status::StatusChain<CryptohomeCryptoError> Wrap(
          hwsec_foundation::status::StatusChain<CryptohomeCryptoError>
              status) &&;

     private:
      const ErrorLocationPair loc_;
      const std::set<CryptohomeError::Action> actions_;
      const std::optional<user_data_auth::CryptohomeErrorCode> ec_;
    };

    // Creates a stub which has to wrap another |CryptohomeCryptoError| to
    // become a valid status chain.
    Unactioned operator()(
        const ErrorLocationPair& loc,
        const std::set<CryptohomeError::Action>& actions,
        const std::optional<user_data_auth::CryptohomeErrorCode> ec =
            std::nullopt);

    // Creates a stub which has to wrap another |CryptohomeCryptoError| to
    // become a valid status chain. This variant is without ErrorAction.
    Unactioned operator()(
        const ErrorLocationPair& loc,
        const std::optional<user_data_auth::CryptohomeErrorCode> ec =
            std::nullopt);

    // Create an error directly.
    hwsec_foundation::status::StatusChain<CryptohomeCryptoError> operator()(
        const ErrorLocationPair& loc,
        const std::set<CryptohomeError::Action>& actions,
        const CryptoError crypto_err,
        const std::optional<user_data_auth::CryptohomeErrorCode> ec =
            std::nullopt);
  };

  // The copyable/movable aspect of this class depends on the base
  // hwsec_foundation::status::Error class. See that class for more info.

  // Direct construction. If ec is std::nullopt, it'll be assigned through
  // conversion from crypto_err.
  CryptohomeCryptoError(
      const ErrorLocationPair& loc,
      const std::set<Action>& actions,
      const CryptoError crypto_err,
      const std::optional<user_data_auth::CryptohomeErrorCode> ec);

  CryptoError local_crypto_error() const { return crypto_error_; }

 private:
  CryptoError crypto_error_;
};

}  // namespace error

// Define an alias in the cryptohome namespace for easier access.
using CryptoStatus =
    hwsec_foundation::status::StatusChain<error::CryptohomeCryptoError>;
template <typename _Et>
using CryptoStatusOr =
    hwsec_foundation::status::StatusChainOr<_Et, error::CryptohomeCryptoError>;

}  // namespace cryptohome

#endif  // CRYPTOHOME_ERROR_CRYPTOHOME_CRYPTO_ERROR_H_
