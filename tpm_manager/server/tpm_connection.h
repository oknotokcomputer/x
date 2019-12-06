// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TPM_MANAGER_SERVER_TPM_CONNECTION_H_
#define TPM_MANAGER_SERVER_TPM_CONNECTION_H_

#include <string>

#include <base/macros.h>
#include <tpm_manager/proto_bindings/tpm_manager.pb.h>
#include <trousers/scoped_tss_type.h>

namespace tpm_manager {

class TpmConnection {
 public:
  enum ConnectionType {
    kConnectWithoutAuth,
    kConnectWithPassword,
    kConnectWithDelegate,
  };

  // Creates a TPM connection as a normal user w/o any auth.
  TpmConnection();

  // Creates a TPM connection on behalf of the owner with |owner_password|.
  explicit TpmConnection(const std::string& owner_password);

  // Creates a TPM connection on behalf of the owner with the owner delegate.
  explicit TpmConnection(const AuthDelegate& owner_delegate);

  ~TpmConnection() = default;

  // This method returns a handle to the current Tpm context.
  // Note: this method still retains ownership of the context. If this class
  // is deleted, the context handle will be invalidated. Returns 0 on failure.
  TSS_HCONTEXT GetContext();

  // This method tries to get a handle to the TPM. Returns 0 on failure.
  TSS_HTPM GetTpm();

 private:
  // This method connects to the Tpm. Returns true on success.
  bool ConnectContextIfNeeded();

  trousers::ScopedTssContext context_;

  const std::string owner_password_;
  const AuthDelegate owner_delegate_;
  const ConnectionType connection_type_;

  DISALLOW_COPY_AND_ASSIGN(TpmConnection);
};

}  // namespace tpm_manager

#endif  // TPM_MANAGER_SERVER_TPM_CONNECTION_H_
