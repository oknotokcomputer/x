// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_USER_SESSION_USER_SESSION_H_
#define CRYPTOHOME_USER_SESSION_USER_SESSION_H_

#include <memory>
#include <string>
#include <vector>

#include <base/timer/timer.h>
#include <brillo/secure_blob.h>

#include "cryptohome/credential_verifier.h"
#include "cryptohome/credentials.h"
#include "cryptohome/dircrypto_data_migrator/migration_helper.h"
#include "cryptohome/error/cryptohome_mount_error.h"
#include "cryptohome/key_objects.h"
#include "cryptohome/migration_type.h"
#include "cryptohome/pkcs11/pkcs11_token.h"
#include "cryptohome/storage/cryptohome_vault.h"

namespace cryptohome {

class UserSession {
 public:
  UserSession() = default;
  virtual ~UserSession() = default;

  // Disallow Copy/Move/Assign
  UserSession(const UserSession&) = delete;
  UserSession(const UserSession&&) = delete;
  void operator=(const UserSession&) = delete;
  void operator=(const UserSession&&) = delete;

  // Returns whether the user session represents an active login session.
  virtual bool IsActive() const = 0;

  // Returns whether the session is for an ephemeral user.
  virtual bool IsEphemeral() const = 0;

  // Returns whether the path belong to the session.
  // TODO(dlunev): remove it once recovery logic is embedded into storage code.
  virtual bool OwnsMountPoint(const base::FilePath& path) const = 0;

  // Perform migration of the vault to a different encryption type.
  virtual bool MigrateVault(
      const dircrypto_data_migrator::MigrationHelper::ProgressCallback&
          callback,
      MigrationType migration_type) = 0;

  // Mounts disk backed vault for the given username with the supplied file
  // system keyset.
  virtual MountStatus MountVault(
      const std::string& username,
      const FileSystemKeyset& fs_keyset,
      const CryptohomeVault::Options& vault_options) = 0;

  // Creates and mounts a ramdisk backed ephemeral session for the given user.
  virtual MountStatus MountEphemeral(const std::string& username) = 0;

  // Creates and mounts a ramdisk backed ephemeral session for an anonymous
  // user.
  virtual MountStatus MountGuest() = 0;

  // Unmounts the session.
  virtual bool Unmount() = 0;

  // Returns status string of the proxied Mount objest.
  //
  // The returned object is a dictionary whose keys describe the mount. Current
  // keys are: "keysets", "mounted", "owner", "enterprise", and "type".
  virtual base::Value GetStatus() const = 0;

  // Returns the WebAuthn secret and clears it from memory.
  virtual std::unique_ptr<brillo::SecureBlob> GetWebAuthnSecret() = 0;

  // Returns the WebAuthn secret hash.
  virtual const brillo::SecureBlob& GetWebAuthnSecretHash() const = 0;

  // Returns the hibernate secret.
  virtual std::unique_ptr<brillo::SecureBlob> GetHibernateSecret() = 0;

  // Adds credentials the current session can be re-authenticated with.
  // Logs warning in case anything went wrong in setting up new re-auth state.
  virtual void AddCredentials(const Credentials& credentials) = 0;

  // Adds a new credential verifier to this session. Note that verifiers are
  // stored by label with new verifiers replacing old ones with the same label.
  virtual void AddCredentialVerifier(
      std::unique_ptr<CredentialVerifier> verifier) = 0;

  // Returns a bool indicating if this session has any credential verifiers
  // (0-arg) or if it has a verifier with a specific label (1-arg).
  virtual bool HasCredentialVerifier() const = 0;
  virtual bool HasCredentialVerifier(const std::string& label) const = 0;

  // Returns all the credential verifiers for this session.
  virtual std::vector<const CredentialVerifier*> GetCredentialVerifiers()
      const = 0;

  // Checks that the session belongs to the obfuscated_user.
  virtual bool VerifyUser(const std::string& obfuscated_username) const = 0;

  // Verifies credentials against store re-auth state. Returns true if the
  // credentials were successfully re-authenticated against the saved re-auth
  // state.
  virtual bool VerifyCredentials(const Credentials& credentials) const = 0;

  // Verifies input against stored re-auth state for the given label. Returns
  // true if the credentials were successfully re-authenticated against the
  // saved state.
  virtual bool VerifyInput(const std::string& label,
                           const AuthInput& input) const = 0;

  // Returns or sets key_data of the current session credentials.
  virtual const KeyData& key_data() const = 0;
  virtual void set_key_data(KeyData key_data) = 0;

  // Returns PKCS11 token associated with the session.
  virtual Pkcs11Token* GetPkcs11Token() = 0;

  // Returns the name of the user associated with the session.
  virtual std::string GetUsername() const = 0;

  // Computes a public derivative from |fek| and |fnek| for u2fd to fetch.
  virtual void PrepareWebAuthnSecret(const brillo::SecureBlob& fek,
                                     const brillo::SecureBlob& fnek) = 0;

  // Removes the credential_verifier if key_label matches the current verifier
  // label (stored in RealUserSession::key_data_).
  virtual void RemoveCredentialVerifierForKeyLabel(
      const std::string& key_label) = 0;

  // Resets the application container for a given session.
  virtual bool ResetApplicationContainer(const std::string& application) = 0;
};

}  // namespace cryptohome

#endif  // CRYPTOHOME_USER_SESSION_USER_SESSION_H_
