// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIBHWSEC_ERROR_TPM_ERROR_H_
#define LIBHWSEC_ERROR_TPM_ERROR_H_

#include <memory>
#include <string>
#include <utility>

#include <libhwsec-foundation/error/error.h>

#include "libhwsec/error/tpm_retry_action.h"

namespace hwsec {
namespace error {

// A base class of all kinds of TPM ErrorBase.
class TPMErrorBaseObj : public hwsec_foundation::error::ErrorBaseObj {
 public:
  TPMErrorBaseObj() = default;
  virtual ~TPMErrorBaseObj() = default;

  // Returns what the action should do after this error happen.
  virtual TPMRetryAction ToTPMRetryAction() const = 0;

 protected:
  TPMErrorBaseObj(TPMErrorBaseObj&&) = default;
};
using TPMErrorBase = std::unique_ptr<TPMErrorBaseObj>;

// A TPM error which contains an error message and retry action instead of an
// error code.
class TPMErrorObj : public TPMErrorBaseObj {
 public:
  TPMErrorObj(const std::string& error_message, TPMRetryAction action)
      : error_message_(error_message), retry_action_(action) {}
  TPMErrorObj(std::string&& error_message, TPMRetryAction action)
      : error_message_(std::move(error_message)), retry_action_(action) {}
  virtual ~TPMErrorObj() = default;

  hwsec_foundation::error::ErrorBase SelfCopy() const {
    return std::make_unique<TPMErrorObj>(error_message_, retry_action_);
  }

  TPMRetryAction ToTPMRetryAction() const { return retry_action_; }

  std::string ToReadableString() const { return error_message_; }

 protected:
  TPMErrorObj(TPMErrorObj&&) = default;

 private:
  const std::string error_message_;
  const TPMRetryAction retry_action_;
};
using TPMError = std::unique_ptr<TPMErrorObj>;

}  // namespace error
}  // namespace hwsec

namespace hwsec_foundation {
namespace error {

// Overloads the helper to wrap a TPMErrorBase into a TPMError without
// specifying the retry action.
template <typename ErrorType,
          typename InnerErrorType,
          typename StringType,
          typename std::enable_if<
              std::is_same<ErrorType, hwsec::error::TPMError>::value>::type* =
              nullptr,
          typename std::enable_if<std::is_base_of<
              hwsec::error::TPMErrorBaseObj,
              typename UnwarpErrorType<InnerErrorType>::type>::value>::type* =
              nullptr,
          decltype(hwsec::error::TPMErrorObj(
              std::forward<StringType>(std::declval<StringType&&>()),
              std::declval<hwsec::error::TPMRetryAction>()))* = nullptr>
hwsec::error::TPMError CreateErrorWrap(InnerErrorType err,
                                       StringType&& error_message) {
  hwsec::error::TPMRetryAction action = err->ToTPMRetryAction();
  return CreateErrorWrap<hwsec::error::TPMError>(
      std::move(err), std::forward<StringType>(error_message), action);
}

}  // namespace error
}  // namespace hwsec_foundation

#endif  // LIBHWSEC_ERROR_TPM_ERROR_H_
