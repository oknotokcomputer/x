// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/auth_block.h"

#include <string>
#include <utility>
#include <vector>

#include <base/files/file_path.h>
#include <gtest/gtest.h>

#include "cryptohome/crypto.h"
#include "cryptohome/crypto_error.h"
#include "cryptohome/libscrypt_compat_auth_block.h"
#include "cryptohome/mock_le_credential_backend.h"
#include "cryptohome/mock_le_credential_manager.h"
#include "cryptohome/mock_tpm.h"
#include "cryptohome/mock_tpm_init.h"
#include "cryptohome/pin_weaver_auth_block.h"
#include "cryptohome/tpm_auth_block.h"
#include "cryptohome/vault_keyset.h"

using ::testing::_;
using ::testing::Exactly;
using ::testing::NiceMock;
using ::testing::Return;

namespace cryptohome {

TEST(PinWeaverAuthBlockTest, DeriveTest) {
  brillo::SecureBlob vault_key(20, 'C');
  brillo::SecureBlob tpm_key(20, 'B');
  brillo::SecureBlob salt(PKCS5_SALT_LEN, 'A');
  brillo::SecureBlob chaps_iv(kAesBlockSize, 'F');
  brillo::SecureBlob fek_iv(kAesBlockSize, 'X');

  brillo::SecureBlob le_secret(kDefaultAesKeySize);
  ASSERT_TRUE(CryptoLib::DeriveSecretsScrypt(vault_key, salt, {&le_secret}));

  NiceMock<MockLECredentialManager> le_cred_manager;

  ON_CALL(le_cred_manager, CheckCredential(_, _, _, _))
      .WillByDefault(Return(LE_CRED_SUCCESS));
  EXPECT_CALL(le_cred_manager, CheckCredential(_, le_secret, _, _))
      .Times(Exactly(1));

  PinWeaverAuthBlock auth_block(&le_cred_manager);

  // Construct the vault keyset.
  SerializedVaultKeyset serialized;
  serialized.set_flags(SerializedVaultKeyset::LE_CREDENTIAL);
  serialized.set_salt(salt.data(), salt.size());
  serialized.set_le_chaps_iv(chaps_iv.data(), chaps_iv.size());
  serialized.set_le_label(0);
  serialized.set_le_fek_iv(fek_iv.data(), fek_iv.size());

  CryptoError error;
  KeyBlobs key_blobs;
  AuthInput auth_input = {vault_key};
  AuthBlockState auth_state = {
      base::make_optional<SerializedVaultKeyset>(std::move(serialized))};
  EXPECT_TRUE(auth_block.Derive(auth_input, auth_state, &key_blobs, &error));

  // Set expectations of the key blobs.
  EXPECT_NE(key_blobs.reset_secret, base::nullopt);
  EXPECT_NE(key_blobs.authorization_data_iv, base::nullopt);
  EXPECT_NE(key_blobs.chaps_iv, base::nullopt);
  EXPECT_NE(key_blobs.vkk_iv, base::nullopt);

  // PinWeaver should always use unique IVs.
  EXPECT_NE(key_blobs.chaps_iv.value(), key_blobs.vkk_iv.value());
  EXPECT_NE(key_blobs.authorization_data_iv.value(), key_blobs.vkk_iv.value());
}

TEST(PinWeaverAuthBlockTest, CheckCredentialFailureTest) {
  brillo::SecureBlob vault_key(20, 'C');
  brillo::SecureBlob tpm_key(20, 'B');
  brillo::SecureBlob salt(PKCS5_SALT_LEN, 'A');
  brillo::SecureBlob chaps_iv(kAesBlockSize, 'F');
  brillo::SecureBlob fek_iv(kAesBlockSize, 'X');

  brillo::SecureBlob le_secret(kDefaultAesKeySize);
  ASSERT_TRUE(CryptoLib::DeriveSecretsScrypt(vault_key, salt, {&le_secret}));

  NiceMock<MockLECredentialManager> le_cred_manager;

  ON_CALL(le_cred_manager, CheckCredential(_, _, _, _))
      .WillByDefault(Return(LE_CRED_ERROR_INVALID_LE_SECRET));
  EXPECT_CALL(le_cred_manager, CheckCredential(_, le_secret, _, _))
      .Times(Exactly(1));

  PinWeaverAuthBlock auth_block(&le_cred_manager);

  // Construct the vault keyset.
  SerializedVaultKeyset serialized;
  serialized.set_flags(SerializedVaultKeyset::LE_CREDENTIAL);
  serialized.set_salt(salt.data(), salt.size());
  serialized.set_le_chaps_iv(chaps_iv.data(), chaps_iv.size());
  serialized.set_le_label(0);
  serialized.set_le_fek_iv(fek_iv.data(), fek_iv.size());

  CryptoError error;
  KeyBlobs key_blobs;
  AuthInput auth_input = {vault_key};
  AuthBlockState auth_state = {
      base::make_optional<SerializedVaultKeyset>(std::move(serialized))};
  EXPECT_FALSE(auth_block.Derive(auth_input, auth_state, &key_blobs, &error));
  EXPECT_EQ(CryptoError::CE_LE_INVALID_SECRET, error);
}

TEST(TPMAuthBlockTest, DecryptBoundToPcrTest) {
  brillo::SecureBlob vault_key(20, 'C');
  brillo::SecureBlob tpm_key(20, 'B');
  brillo::SecureBlob salt(PKCS5_SALT_LEN, 'A');

  brillo::SecureBlob vkk_iv(kDefaultAesKeySize);
  brillo::SecureBlob vkk_key;

  brillo::SecureBlob pass_blob(kDefaultPassBlobSize);
  ASSERT_TRUE(CryptoLib::DeriveSecretsScrypt(vault_key, salt, {&pass_blob}));

  NiceMock<MockTpm> tpm;
  NiceMock<MockTpmInit> tpm_init;
  EXPECT_CALL(tpm, UnsealWithAuthorization(_, _, pass_blob, _, _))
      .Times(Exactly(1));

  CryptoError error = CryptoError::CE_NONE;
  TpmAuthBlock tpm_auth_block(&tpm, &tpm_init);
  EXPECT_TRUE(tpm_auth_block.DecryptTpmBoundToPcr(vault_key, tpm_key, salt,
                                                  &error, &vkk_iv, &vkk_key));
  EXPECT_EQ(CryptoError::CE_NONE, error);
}

TEST(TPMAuthBlockTest, DecryptNotBoundToPcrTest) {
  // Set up a SerializedVaultKeyset. In this case, it is only used to check the
  // flags and password_rounds.
  SerializedVaultKeyset serialized;
  serialized.set_flags(SerializedVaultKeyset::TPM_WRAPPED |
                       SerializedVaultKeyset::SCRYPT_DERIVED);

  brillo::SecureBlob vault_key(20, 'C');
  brillo::SecureBlob tpm_key(20, 'B');
  brillo::SecureBlob salt(PKCS5_SALT_LEN, 'A');

  brillo::SecureBlob vkk_key;
  brillo::SecureBlob vkk_iv(kDefaultAesKeySize);
  brillo::SecureBlob aes_key(kDefaultAesKeySize);

  ASSERT_TRUE(CryptoLib::DeriveSecretsScrypt(vault_key, salt, {&aes_key}));

  NiceMock<MockTpm> tpm;
  NiceMock<MockTpmInit> tpm_init;
  EXPECT_CALL(tpm, DecryptBlob(_, tpm_key, aes_key, _, _)).Times(Exactly(1));

  CryptoError error = CryptoError::CE_NONE;
  TpmAuthBlock tpm_auth_block(&tpm, &tpm_init);
  EXPECT_TRUE(tpm_auth_block.DecryptTpmNotBoundToPcr(
      serialized, vault_key, tpm_key, salt, &error, &vkk_iv, &vkk_key));
  EXPECT_EQ(CryptoError::CE_NONE, error);
}

TEST(TpmAuthBlockTest, DeriveTest) {
  SerializedVaultKeyset serialized;
  serialized.set_flags(SerializedVaultKeyset::TPM_WRAPPED |
                       SerializedVaultKeyset::PCR_BOUND |
                       SerializedVaultKeyset::SCRYPT_DERIVED);

  brillo::SecureBlob key(20, 'B');
  brillo::SecureBlob tpm_key(20, 'C');
  std::string salt(PKCS5_SALT_LEN, 'A');

  serialized.set_salt(salt);
  serialized.set_tpm_key(tpm_key.data(), tpm_key.size());

  // Make sure TpmAuthBlock calls DecryptTpmBoundToPcr in this case.
  NiceMock<MockTpm> tpm;
  NiceMock<MockTpmInit> tpm_init;
  EXPECT_CALL(tpm, UnsealWithAuthorization(_, _, _, _, _)).Times(Exactly(1));

  TpmAuthBlock auth_block(&tpm, &tpm_init);

  KeyBlobs key_out_data;
  AuthInput auth_input;
  auth_input.user_input = key;
  auth_input.locked_to_single_user = false;

  AuthBlockState auth_state = {
      base::make_optional<SerializedVaultKeyset>(std::move(serialized))};
  CryptoError error;
  EXPECT_TRUE(auth_block.Derive(auth_input, auth_state, &key_out_data, &error));

  // Assert that the returned key blobs isn't uninitialized.
  EXPECT_NE(key_out_data.vkk_iv, base::nullopt);
  EXPECT_NE(key_out_data.vkk_key, base::nullopt);
  EXPECT_EQ(key_out_data.vkk_iv.value(), key_out_data.chaps_iv.value());
  EXPECT_EQ(key_out_data.vkk_iv.value(),
            key_out_data.authorization_data_iv.value());
}

TEST(LibScryptCompatAuthBlockTest, DeriveTest) {
  SerializedVaultKeyset serialized;
  serialized.set_flags(SerializedVaultKeyset::SCRYPT_DERIVED);

  std::vector<uint8_t> wrapped_keyset = {
      0x73, 0x63, 0x72, 0x79, 0x70, 0x74, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x08,
      0x00, 0x00, 0x00, 0x01, 0x4D, 0xEE, 0xFC, 0x79, 0x0D, 0x79, 0x08, 0x79,
      0xD5, 0xF6, 0x07, 0x65, 0xDF, 0x76, 0x5A, 0xAE, 0xD1, 0xBD, 0x1D, 0xCF,
      0x29, 0xF6, 0xFF, 0x5C, 0x31, 0x30, 0x23, 0xD1, 0x22, 0x17, 0xDF, 0x74,
      0x26, 0xD5, 0x11, 0x88, 0x8D, 0x40, 0xA6, 0x9C, 0xB9, 0x72, 0xCE, 0x37,
      0x71, 0xB7, 0x39, 0x0E, 0x3E, 0x34, 0x0F, 0x73, 0x29, 0xF4, 0x0F, 0x89,
      0x15, 0xF7, 0x6E, 0xA1, 0x5A, 0x29, 0x78, 0x21, 0xB7, 0xC0, 0x76, 0x50,
      0x14, 0x5C, 0xAD, 0x77, 0x53, 0xC9, 0xD0, 0xFE, 0xD1, 0xB9, 0x81, 0x32,
      0x75, 0x0E, 0x1E, 0x45, 0x34, 0xBD, 0x0B, 0xF7, 0xFA, 0xED, 0x9A, 0xD7,
      0x6B, 0xE4, 0x2F, 0xC0, 0x2F, 0x58, 0xBE, 0x3A, 0x26, 0xD1, 0x82, 0x41,
      0x09, 0x82, 0x7F, 0x17, 0xA8, 0x5C, 0x66, 0x0E, 0x24, 0x8B, 0x7B, 0xF5,
      0xEB, 0x0C, 0x6D, 0xAE, 0x19, 0x5C, 0x7D, 0xC4, 0x0D, 0x8D, 0xB2, 0x18,
      0x13, 0xD4, 0xC0, 0x32, 0x34, 0x15, 0xAE, 0x1D, 0xA1, 0x44, 0x2E, 0x80,
      0xD8, 0x00, 0x8A, 0xB9, 0xDD, 0xA4, 0xC0, 0x33, 0xAE, 0x26, 0xD3, 0xE6,
      0x53, 0xD6, 0x31, 0x5C, 0x4C, 0x10, 0xBB, 0xA9, 0xD5, 0x53, 0xD7, 0xAD,
      0xCD, 0x97, 0x20, 0x83, 0xFC, 0x18, 0x4B, 0x7F, 0xC1, 0xBD, 0x85, 0x43,
      0x12, 0x85, 0x4F, 0x6F, 0xAA, 0xDB, 0x58, 0xA0, 0x0F, 0x2C, 0xAB, 0xEA,
      0x74, 0x8E, 0x2C, 0x28, 0x01, 0x88, 0x48, 0xA5, 0x0A, 0xFC, 0x2F, 0xB4,
      0x59, 0x4B, 0xF6, 0xD9, 0xE5, 0x47, 0x94, 0x42, 0xA5, 0x61, 0x06, 0x8C,
      0x5A, 0x9C, 0xD3, 0xA6, 0x30, 0x2C, 0x13, 0xCA, 0xF1, 0xFF, 0xFE, 0x5C,
      0xE8, 0x21, 0x25, 0x9A, 0xE0, 0x50, 0xC3, 0x2F, 0x14, 0x71, 0x38, 0xD0,
      0xE7, 0x79, 0x5D, 0xF0, 0x71, 0x80, 0xF0, 0x3D, 0x05, 0xB6, 0xF7, 0x67,
      0x3F, 0x22, 0x21, 0x7A, 0xED, 0x48, 0xC4, 0x2D, 0xEA, 0x2E, 0xAE, 0xE9,
      0xA8, 0xFF, 0xA0, 0xB6, 0xB4, 0x0A, 0x94, 0x34, 0x40, 0xD1, 0x6C, 0x6C,
      0xC7, 0x90, 0x9C, 0xF7, 0xED, 0x0B, 0xED, 0x90, 0xB1, 0x4D, 0x6D, 0xB4,
      0x3D, 0x04, 0x7E, 0x7B, 0x16, 0x59, 0xFF, 0xFE};

  std::vector<uint8_t> wrapped_chaps_key = {
      0x73, 0x63, 0x72, 0x79, 0x70, 0x74, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x08,
      0x00, 0x00, 0x00, 0x01, 0xC9, 0x80, 0xA1, 0x30, 0x82, 0x40, 0xE6, 0xCF,
      0xC8, 0x59, 0xE9, 0xB6, 0xB0, 0xE8, 0xBF, 0x95, 0x82, 0x79, 0x71, 0xF9,
      0x86, 0x8A, 0xCA, 0x53, 0x23, 0xCF, 0x31, 0xFE, 0x4B, 0xD2, 0xA5, 0x26,
      0xA4, 0x46, 0x3D, 0x35, 0xEF, 0x69, 0x02, 0xC4, 0xBF, 0x72, 0xDC, 0xF8,
      0x90, 0x77, 0xFB, 0x59, 0x0D, 0x41, 0xCB, 0x5B, 0x58, 0xC6, 0x08, 0x0F,
      0x19, 0x4E, 0xC8, 0x4A, 0x57, 0xE7, 0x63, 0x43, 0x39, 0x79, 0xD7, 0x6E,
      0x0D, 0xD0, 0xE4, 0x4F, 0xFA, 0x55, 0x32, 0xE1, 0x6B, 0xE4, 0xFF, 0x12,
      0xB1, 0xA3, 0x75, 0x9C, 0x44, 0x3A, 0x16, 0x68, 0x5C, 0x11, 0xD0, 0xA5,
      0x4C, 0x65, 0xB0, 0xBF, 0x04, 0x41, 0x94, 0xFE, 0xC5, 0xDD, 0x5C, 0x78,
      0x5B, 0x14, 0xA1, 0x3F, 0x0B, 0x17, 0x9C, 0x75, 0xA5, 0x9E, 0x36, 0x14,
      0x5B, 0xC4, 0xAC, 0x77, 0x28, 0xDE, 0xEB, 0xB4, 0x51, 0x5F, 0x33, 0x36};

  std::vector<uint8_t> wrapped_reset_seed = {
      0x73, 0x63, 0x72, 0x79, 0x70, 0x74, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x08,
      0x00, 0x00, 0x00, 0x01, 0x7F, 0x40, 0x30, 0x51, 0x2F, 0x15, 0x62, 0x15,
      0xB1, 0x2E, 0x58, 0x27, 0x52, 0xE4, 0xFF, 0xC5, 0x3C, 0x1E, 0x19, 0x05,
      0x84, 0xD8, 0xE8, 0xD4, 0xFD, 0x8C, 0x33, 0xE8, 0x06, 0x1A, 0x38, 0x28,
      0x2D, 0xD7, 0x01, 0xD2, 0xB3, 0xE1, 0x95, 0xC3, 0x49, 0x63, 0x39, 0xA2,
      0xB2, 0xE3, 0xDA, 0xE2, 0x76, 0x40, 0x40, 0x11, 0xD1, 0x98, 0xD2, 0x03,
      0xFB, 0x60, 0xD0, 0xA1, 0xA5, 0xB5, 0x51, 0xAA, 0xEF, 0x6C, 0xB3, 0xAB,
      0x23, 0x65, 0xCA, 0x44, 0x84, 0x7A, 0x71, 0xCA, 0x0C, 0x36, 0x33, 0x7F,
      0x53, 0x06, 0x0E, 0x03, 0xBB, 0xC1, 0x9A, 0x9D, 0x40, 0x1C, 0x2F, 0x46,
      0xB7, 0x84, 0x00, 0x59, 0x5B, 0xD6, 0x53, 0xE4, 0x51, 0x82, 0xC2, 0x3D,
      0xF4, 0x46, 0xD2, 0xDD, 0xE5, 0x7A, 0x0A, 0xEB, 0xC8, 0x45, 0x7C, 0x37,
      0x01, 0xD5, 0x37, 0x4E, 0xE3, 0xC7, 0xBC, 0xC6, 0x5E, 0x25, 0xFE, 0xE2,
      0x05, 0x14, 0x60, 0x33, 0xB8, 0x1A, 0xF1, 0x17, 0xE1, 0x0C, 0x25, 0x00,
      0xA5, 0x0A, 0xD5, 0x03};

  serialized.set_wrapped_keyset(wrapped_keyset.data(), wrapped_keyset.size());
  serialized.set_wrapped_chaps_key(wrapped_chaps_key.data(),
                                   wrapped_chaps_key.size());
  serialized.set_wrapped_reset_seed(wrapped_reset_seed.data(),
                                    wrapped_reset_seed.size());

  brillo::SecureBlob key = {0x31, 0x35, 0x64, 0x64, 0x38, 0x38, 0x66, 0x36,
                            0x35, 0x31, 0x30, 0x65, 0x30, 0x64, 0x35, 0x64,
                            0x35, 0x35, 0x36, 0x35, 0x35, 0x35, 0x38, 0x36,
                            0x31, 0x32, 0x62, 0x37, 0x39, 0x36, 0x30, 0x65};

  KeyBlobs key_out_data;
  AuthInput auth_input;
  auth_input.user_input = key;
  auth_input.locked_to_single_user = false;

  AuthBlockState auth_state = {
      base::make_optional<SerializedVaultKeyset>(std::move(serialized))};
  CryptoError error;
  LibScryptCompatAuthBlock auth_block;
  EXPECT_TRUE(auth_block.Derive(auth_input, auth_state, &key_out_data, &error));

  brillo::SecureBlob derived_key = {
      0x58, 0x2A, 0x41, 0x1F, 0xC0, 0x27, 0x2D, 0xC7, 0xF8, 0xEC, 0xA3,
      0x4E, 0xC0, 0x3F, 0x6C, 0x56, 0x6D, 0x88, 0x69, 0x3F, 0x50, 0x20,
      0x37, 0xE3, 0x77, 0x5F, 0xDD, 0xC3, 0x61, 0x2D, 0x27, 0xAD, 0xD3,
      0x55, 0x4D, 0x66, 0xE5, 0x83, 0xD2, 0x5E, 0x02, 0x0C, 0x22, 0x59,
      0x6C, 0x39, 0x35, 0x86, 0xEC, 0x46, 0xB0, 0x85, 0x89, 0xE3, 0x4C,
      0xB9, 0xE2, 0x0C, 0xA1, 0x27, 0x60, 0x85, 0x5A, 0x37};

  brillo::SecureBlob derived_chaps_key = {
      0x16, 0x53, 0xEE, 0x4D, 0x76, 0x47, 0x68, 0x09, 0xB3, 0x39, 0x1D,
      0xD3, 0x6F, 0xA2, 0x8F, 0x8A, 0x3E, 0xB3, 0x64, 0xDD, 0x4D, 0xC4,
      0x64, 0x6F, 0xE1, 0xB8, 0x82, 0x28, 0x68, 0x72, 0x68, 0x84, 0x93,
      0xE2, 0xDB, 0x2F, 0x27, 0x91, 0x08, 0x2C, 0xA0, 0xD9, 0xA1, 0x6E,
      0x6F, 0x0E, 0x13, 0x66, 0x1D, 0x94, 0x12, 0x6F, 0xF4, 0x98, 0x7B,
      0x44, 0x62, 0x57, 0x47, 0x33, 0x46, 0xD2, 0x30, 0x42};

  brillo::SecureBlob derived_reset_seed_key = {
      0xFA, 0x93, 0x57, 0xCE, 0x21, 0xBB, 0x82, 0x4D, 0x3A, 0x3B, 0x26,
      0x88, 0x8C, 0x7E, 0x61, 0x52, 0x52, 0xF0, 0x12, 0x25, 0xA3, 0x59,
      0xCA, 0x71, 0xD2, 0x0C, 0x52, 0x8A, 0x5B, 0x7A, 0x7D, 0xBF, 0x8E,
      0xC7, 0x4D, 0x1D, 0xB5, 0xF9, 0x01, 0xA6, 0xE5, 0x5D, 0x47, 0x2E,
      0xFD, 0x7C, 0x78, 0x1D, 0x9B, 0xAD, 0xE6, 0x71, 0x35, 0x2B, 0x32,
      0x1E, 0x59, 0x19, 0x47, 0x88, 0x92, 0x50, 0x28, 0x09};

  EXPECT_EQ(derived_key, key_out_data.scrypt_key);
  EXPECT_EQ(derived_chaps_key, key_out_data.chaps_scrypt_key);
  EXPECT_EQ(derived_reset_seed_key, key_out_data.scrypt_wrapped_reset_seed_key);
}

}  // namespace cryptohome
