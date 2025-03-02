// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_AUTH_BLOCKS_BIOMETRICS_AUTH_BLOCK_SERVICE_H_
#define CRYPTOHOME_AUTH_BLOCKS_BIOMETRICS_AUTH_BLOCK_SERVICE_H_

#include <memory>
#include <optional>
#include <string>

#include <base/functional/callback.h>
#include <base/memory/weak_ptr.h>
#include <brillo/secure_blob.h>
#include <cryptohome/proto_bindings/UserDataAuth.pb.h>

#include "cryptohome/auth_blocks/biometrics_command_processor.h"
#include "cryptohome/auth_blocks/prepare_token.h"
#include "cryptohome/error/cryptohome_error.h"
#include "cryptohome/username.h"

namespace cryptohome {

constexpr char CrosFpAuthStackManagerRelativePath[] = "/CrosFpAuthStackManager";

// BiometricsAuthBlockService is in charge of managing biometrics sessions
// and handling biometrics commands.
class BiometricsAuthBlockService {
 public:
  using OperationInput = BiometricsCommandProcessor::OperationInput;
  using OperationOutput = BiometricsCommandProcessor::OperationOutput;
  using OperationCallback = BiometricsCommandProcessor::OperationCallback;
  using DeleteResult = BiometricsCommandProcessor::DeleteResult;
  using LegacyRecordsCallback =
      BiometricsCommandProcessor::LegacyRecordsCallback;

  BiometricsAuthBlockService(
      std::unique_ptr<BiometricsCommandProcessor> processor,
      base::RepeatingCallback<void(user_data_auth::AuthEnrollmentProgress)>
          enroll_signal_sender,
      base::RepeatingCallback<void(user_data_auth::AuthScanDone)>
          auth_signal_sender);
  BiometricsAuthBlockService(const BiometricsAuthBlockService&) = delete;
  BiometricsAuthBlockService& operator=(const BiometricsAuthBlockService&) =
      delete;
  ~BiometricsAuthBlockService() = default;

  // IsReady returns whether the biometrics auth block service is ready. Once
  // this returns true in a boot cycle, the caller can assume it's always ready.
  // This can be used to determine biometrics auth factor's availability.
  bool IsReady();

  // GetNonce fetches the nonce from the biometrics auth stack that will be used
  // for initiating the encrypted session between PinWeaver and it.
  void GetNonce(base::OnceCallback<void(std::optional<brillo::Blob>)> callback);

  // StartEnrollSession initiates a biometrics enrollment session. If
  // successful, enroll_signal_sender will be triggered with upcoming enrollment
  // progress signals.
  void StartEnrollSession(AuthFactorType auth_factor_type,
                          OperationInput payload,
                          PreparedAuthFactorToken::Consumer on_done);

  // CreateCredential returns the necessary data for cryptohome to
  // create an AuthFactor for the newly created biometrics credential.
  void CreateCredential(OperationCallback on_done);

  // EndEnrollSession ends the biometrics enrollment session.
  void EndEnrollSession();

  // StartAuthenticateSession initiates a biometrics authentication session. If
  // successful, auth_signal_sender_ will be triggered with upcoming
  // authentication scan signals.
  void StartAuthenticateSession(AuthFactorType auth_factor_type,
                                ObfuscatedUsername obfuscated_username,
                                OperationInput payload,
                                PreparedAuthFactorToken::Consumer on_done);

  // MatchCredential returns the necessary data for cryptohome to
  // authenticate the AuthFactor for the matched biometrics credential.
  void MatchCredential(OperationCallback on_done);

  // EndAuthenticateSession ends the biometrics authentication session.
  void EndAuthenticateSession();

  // Calls BiometricsCommandProcessor::DeleteCredential.
  void DeleteCredential(ObfuscatedUsername obfuscated_username,
                        const std::string& record_id,
                        base::OnceCallback<void(DeleteResult)> on_done);

  // EnrollLegacyTemplate initiates a biometrics enrollment session and loads
  // the legacy template as the enrollment result. If successful, the session
  // is ready to accept a subsequent CreateCredential call.
  void EnrollLegacyTemplate(AuthFactorType auth_factor_type,
                            const std::string& template_id,
                            OperationInput payload,
                            StatusCallback on_done);

  // Calls BiometricsCommandProcessor::ListLegacyRecords. It returns a list of
  // legacy fingerprint records from biod's daemon store.
  void ListLegacyRecords(LegacyRecordsCallback on_done);

 private:
  class Token : public PreparedAuthFactorToken {
   public:
    enum class TokenType {
      kEnroll,
      kAuthenticate,
    };

    Token(AuthFactorType auth_factor_type, TokenType token_type);

    // Attaches the token to the underlying service. Ideally we'd do this in the
    // constructor but the token is constructed when we initiate the request to
    // start the session, not after the session is (successfully) started. We
    // don't want the token to be able to terminate the session until it
    // starts, so we wait until that point to attach it.
    void AttachToService(BiometricsAuthBlockService* service);

    // Detaches the token from the underlying service. Usually the token should
    // be in charge of closing the service's session, but when the session is
    // terminated because of other reasons, we need to detach the token from the
    // service so it doesn't terminate it again.
    void DetachFromService();

    TokenType type() const { return token_type_; }

   private:
    CryptohomeStatus TerminateAuthFactor() override;

    TokenType token_type_;
    BiometricsAuthBlockService* service_ = nullptr;
    TerminateOnDestruction terminate_;
  };

  // Depending on the result of success, this will pass either the given auth
  // factor token, or a not-OK status to the given callback. This function is
  // designed to be used as a callback with BiometricsCommandProcessor.
  void CheckSessionStartResult(PreparedAuthFactorToken::Consumer on_done,
                               bool success);

  // Depending on the result of success, this will pass either ok or error
  // status. This function is intended as a callback with
  // BiometricsCommandProcessor::EnrollLegacyTemplate.
  void CheckEnrollLegacyResult(StatusCallback on_done, bool success);

  void OnEnrollScanDone(user_data_auth::AuthEnrollmentProgress signal);

  void OnAuthScanDone(user_data_auth::AuthScanDone signal);

  // This is triggered when biometrics command processor reports a session
  // failure. The way we indicate session failures (and hence there will be no
  // upcoming signals) to the user is to use the designated
  // FINGERPRINT_SCAN_RESULT_FATAL_ERROR error code. We can detach the active
  // token without an extra EndSession call because the biod implementation
  // always ends the session when there is a session failure.
  void OnSessionFailed();

  std::unique_ptr<BiometricsCommandProcessor> processor_;
  // The token created when starting a session. This is cleared and returned to
  // the caller when the session is started successfully.
  std::unique_ptr<Token> pending_token_;
  // The token for the currently active session, if there is one. This will
  // be set to null otherwise.
  Token* active_token_ = nullptr;
  // A callback to send cryptohome AuthEnrollmentProgress signal.
  base::RepeatingCallback<void(user_data_auth::AuthEnrollmentProgress)>
      enroll_signal_sender_;
  // A callback to send cryptohome AuthScanDone signal.
  base::RepeatingCallback<void(user_data_auth::AuthScanDone)>
      auth_signal_sender_;

  // The last member, to invalidate weak references first on destruction.
  base::WeakPtrFactory<BiometricsAuthBlockService> weak_factory_{this};
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_AUTH_BLOCKS_BIOMETRICS_AUTH_BLOCK_SERVICE_H_
