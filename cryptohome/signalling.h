// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_SIGNALLING_H_
#define CRYPTOHOME_SIGNALLING_H_

#include <cryptohome/proto_bindings/UserDataAuth.pb.h>

namespace cryptohome {

// Defines a standard interface for sending D-Bus signals.
class SignallingInterface {
 public:
  SignallingInterface() = default;
  virtual ~SignallingInterface() = default;

  // Send the given signal. All of these functions work the same way: calling
  // "SendXxx" will send the UserDataAuth D-Bus signal named "Xxx".
  virtual void SendAuthFactorStatusUpdate(
      const user_data_auth::AuthFactorStatusUpdate& signal) = 0;
  virtual void SendLowDiskSpace(const user_data_auth::LowDiskSpace& signal) = 0;
  virtual void SendAuthScanResult(
      const user_data_auth::AuthScanResult& signal) = 0;
  virtual void SendPrepareAuthFactorProgress(
      const user_data_auth::PrepareAuthFactorProgress& signal) = 0;
  virtual void SendAuthenticateStarted(
      const user_data_auth::AuthenticateStarted& signal) = 0;
  virtual void SendAuthenticateAuthFactorCompleted(
      const user_data_auth::AuthenticateAuthFactorCompleted& signal) = 0;
  virtual void SendMountStarted(const user_data_auth::MountStarted& signal) = 0;
  virtual void SendMountCompleted(
      const user_data_auth::MountCompleted& signal) = 0;
  virtual void SendAuthFactorAdded(
      const user_data_auth::AuthFactorAdded& signal) = 0;
  virtual void SendAuthFactorRemoved(
      const user_data_auth::AuthFactorRemoved& signal) = 0;
  virtual void SendAuthFactorUpdated(
      const user_data_auth::AuthFactorUpdated& signal) = 0;
  virtual void SendAuthSessionExpiring(
      const user_data_auth::AuthSessionExpiring& signal) = 0;
  virtual void SendEvictedKeyRestored(
      const user_data_auth::EvictedKeyRestored& signal) = 0;
  virtual void SendRemoveCompleted(
      const user_data_auth::RemoveCompleted& signal) = 0;
};

// Null implementation of the signalling interface that considers every signal
// to be a no-op. Useful as a default handler in cases where the real D-Bus
// version is not yet available.
class NullSignalling : public SignallingInterface {
 public:
  NullSignalling() = default;

  NullSignalling(const NullSignalling&) = delete;
  NullSignalling& operator=(const NullSignalling&) = delete;

 private:
  void SendAuthFactorStatusUpdate(
      const user_data_auth::AuthFactorStatusUpdate& signal) override {}
  void SendLowDiskSpace(const user_data_auth::LowDiskSpace& signal) override {}
  void SendAuthScanResult(
      const user_data_auth::AuthScanResult& signal) override {}
  void SendPrepareAuthFactorProgress(
      const user_data_auth::PrepareAuthFactorProgress& signal) override {}
  void SendAuthenticateStarted(
      const user_data_auth::AuthenticateStarted& signal) override {}
  void SendAuthenticateAuthFactorCompleted(
      const user_data_auth::AuthenticateAuthFactorCompleted& signal) override {}
  void SendMountStarted(const user_data_auth::MountStarted& signal) override {}
  void SendMountCompleted(
      const user_data_auth::MountCompleted& signal) override {}
  void SendAuthFactorAdded(
      const user_data_auth::AuthFactorAdded& signal) override {}
  void SendAuthFactorRemoved(
      const user_data_auth::AuthFactorRemoved& signal) override {}
  void SendAuthFactorUpdated(
      const user_data_auth::AuthFactorUpdated& signal) override {}
  void SendAuthSessionExpiring(
      const user_data_auth::AuthSessionExpiring& signal) override {}
  void SendEvictedKeyRestored(
      const user_data_auth::EvictedKeyRestored& signal) override {}
  void SendRemoveCompleted(
      const user_data_auth::RemoveCompleted& signal) override {}
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_SIGNALLING_H_
