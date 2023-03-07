// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <utility>

#include <gtest/gtest.h>
#include <libhwsec-foundation/crypto/rsa.h>
#include <libhwsec-foundation/error/testing_helper.h>

#include "libhwsec/backend/tpm1/backend_test_base.h"

using hwsec_foundation::kWellKnownExponent;
using hwsec_foundation::error::testing::IsOk;
using hwsec_foundation::error::testing::IsOkAndHolds;
using hwsec_foundation::error::testing::NotOk;
using hwsec_foundation::error::testing::ReturnError;
using hwsec_foundation::error::testing::ReturnValue;
using testing::_;
using testing::DoAll;
using testing::NiceMock;
using testing::Return;
using testing::SaveArg;
using testing::SetArgPointee;
using tpm_manager::TpmManagerStatus;
namespace hwsec {

class BackendVendorTpm1Test : public BackendTpm1TestBase {};

TEST_F(BackendVendorTpm1Test, GetVersionInfo) {
  const brillo::Blob kFakeVendorSpecific = {0x06, 0x2B, 0x00, 0xF3, 0x00,
                                            0x74, 0x70, 0x6D, 0x73, 0x31,
                                            0x35, 0xFF, 0xFF};
  tpm_manager::GetVersionInfoReply reply;
  reply.set_status(TpmManagerStatus::STATUS_SUCCESS);
  reply.set_family(0x312E3200);
  reply.set_spec_level(0x200000003);
  reply.set_manufacturer(0x49465800);
  reply.set_tpm_model(0xFFFFFFFF);
  reply.set_firmware_version(0x62B);
  reply.set_vendor_specific(brillo::BlobToString(kFakeVendorSpecific));
  reply.set_gsc_version(tpm_manager::GSC_VERSION_NOT_GSC);
  EXPECT_CALL(proxy_->GetMock().tpm_manager, GetVersionInfo(_, _, _, _))
      .WillOnce(DoAll(SetArgPointee<1>(reply), Return(true)));

  EXPECT_THAT(backend_->GetVendorTpm1().GetFamily(), IsOkAndHolds(0x312E3200));

  EXPECT_THAT(backend_->GetVendorTpm1().GetSpecLevel(),
              IsOkAndHolds(0x200000003));

  EXPECT_THAT(backend_->GetVendorTpm1().GetManufacturer(),
              IsOkAndHolds(0x49465800));

  EXPECT_THAT(backend_->GetVendorTpm1().GetTpmModel(),
              IsOkAndHolds(0xFFFFFFFF));

  EXPECT_THAT(backend_->GetVendorTpm1().GetFirmwareVersion(),
              IsOkAndHolds(0x62B));

  EXPECT_THAT(backend_->GetVendorTpm1().GetVendorSpecific(),
              IsOkAndHolds(kFakeVendorSpecific));

  EXPECT_THAT(backend_->GetVendorTpm1().GetFingerprint(),
              IsOkAndHolds(0x2081EE27));
}

TEST_F(BackendVendorTpm1Test, IsSrkRocaVulnerable) {
  constexpr uint8_t kFakeParms[] = {0xde, 0xad, 0xbe, 0xef, 0x12,
                                    0x34, 0x56, 0x78, 0x90};

  // This is a modulus from key generated by a TPM running vulnerable firmware.
  constexpr uint8_t kVulnerableModulus[] = {
      0x00, 0x9e, 0x31, 0xea, 0x73, 0xed, 0x06, 0x22, 0x52, 0x30, 0x85, 0x22,
      0x75, 0xa8, 0x60, 0x6e, 0x08, 0x56, 0xbc, 0xee, 0xb1, 0xba, 0xd5, 0x62,
      0xe0, 0x3b, 0x03, 0xc4, 0x68, 0x2a, 0x20, 0x72, 0xa2, 0x5c, 0x7a, 0xd8,
      0x9d, 0x00, 0xf8, 0xb3, 0xf8, 0x83, 0xc3, 0x97, 0xaa, 0x5d, 0x55, 0xfe,
      0x75, 0x1f, 0x0a, 0x25, 0xbf, 0xe0, 0x89, 0x0c, 0x02, 0x30, 0x6b, 0x5f,
      0xfa, 0x0f, 0x6c, 0xc6, 0x20, 0x79, 0xc9, 0x6a, 0x32, 0x4a, 0x15, 0xf3,
      0x87, 0xf8, 0x24, 0x0b, 0x1b, 0x62, 0x9d, 0xcc, 0xe5, 0xc5, 0x14, 0x5d,
      0x69, 0xcc, 0x2f, 0x97, 0x3f, 0x40, 0x51, 0xe3, 0x35, 0x38, 0x99, 0x14,
      0xcc, 0x45, 0x91, 0x93, 0x65, 0x31, 0x98, 0x03, 0x80, 0x2a, 0x13, 0x37,
      0x89, 0x0b, 0xfb, 0x87, 0xae, 0x99, 0xa1, 0x75, 0x72, 0xdc, 0x53, 0x64,
      0x71, 0x6f, 0xdc, 0x13, 0x91, 0xf8, 0x16, 0x5c, 0xdc, 0xb9, 0x07, 0x9c,
      0xc2, 0x0e, 0x5b, 0x71, 0xf7, 0x6d, 0x70, 0xba, 0x05, 0x1a, 0x47, 0x06,
      0xb2, 0x7e, 0x65, 0xdf, 0xae, 0x8f, 0x49, 0xb5, 0x4e, 0x5e, 0x7a, 0x8d,
      0x1e, 0x81, 0x6f, 0x2e, 0x31, 0x35, 0x88, 0x03, 0x1d, 0xe7, 0xe0, 0x87,
      0x7a, 0x87, 0xc0, 0x8b, 0xe0, 0xbb, 0x9c, 0x05, 0x68, 0x89, 0xe8, 0x04,
      0x69, 0xc1, 0x33, 0xec, 0x14, 0xe0, 0x11, 0xd1, 0xae, 0x4a, 0xd0, 0xd9,
      0x3a, 0x5b, 0x79, 0xc7, 0x12, 0x78, 0x2d, 0x8a, 0x8f, 0x2d, 0x00, 0xf7,
      0x0d, 0x5e, 0x00, 0xa0, 0x35, 0x9a, 0x02, 0xb0, 0x73, 0xad, 0xbc, 0x44,
      0xd2, 0x67, 0x73, 0x64, 0x08, 0xc8, 0x60, 0x58, 0x04, 0xf1, 0xa5, 0xd2,
      0xd5, 0x18, 0x4e, 0x39, 0x3e, 0x68, 0xe6, 0xfa, 0xa7, 0x55, 0xd9, 0xeb,
      0xd8, 0x5f, 0xe7, 0xde, 0xab, 0x2e, 0x8b, 0x17, 0x5d, 0x08, 0x79, 0x6b,
      0x7a, 0x7e, 0xf0, 0x06, 0x61,
  };

  constexpr uint8_t kFakeExponent[] = {0xfa, 0x42, 0x24, 0x55, 0x66};

  SetupSrk();

  // These ptrs would be owned by the fake_pub_key.
  uint8_t* parms_ptr = static_cast<uint8_t*>(malloc(sizeof(kFakeParms)));
  memcpy(parms_ptr, kFakeParms, sizeof(kFakeParms));
  uint8_t* key_ptr = static_cast<uint8_t*>(malloc(sizeof(kVulnerableModulus)));
  memcpy(key_ptr, kVulnerableModulus, sizeof(kVulnerableModulus));

  TPM_PUBKEY fake_pub_key{
      .algorithmParms =
          TPM_KEY_PARMS{
              .algorithmID = TPM_ALG_RSA,
              .encScheme = TPM_ES_NONE,
              .sigScheme = TPM_SS_NONE,
              .parmSize = sizeof(kFakeParms),
              .parms = parms_ptr,
          },
      .pubKey =
          TPM_STORE_PUBKEY{
              .keyLength = sizeof(kVulnerableModulus),
              .key = key_ptr,
          },
  };

  EXPECT_CALL(proxy_->GetMock().overalls,
              Orspi_UnloadBlob_PUBKEY_s(_, _, kDefaultSrkPubkey.size(), _))
      .WillOnce(DoAll(SetArgPointee<0>(kDefaultSrkPubkey.size()),
                      SetArgPointee<3>(fake_pub_key), Return(TPM_SUCCESS)));

  // This ptr would be owned by the key_parms.
  uint8_t* exp_ptr = static_cast<uint8_t*>(malloc(sizeof(kFakeExponent)));
  memcpy(exp_ptr, kFakeExponent, sizeof(kFakeExponent));

  TPM_RSA_KEY_PARMS key_parms{
      .keyLength = 0,
      .numPrimes = 1,
      .exponentSize = sizeof(kFakeExponent),
      .exponent = exp_ptr,
  };

  EXPECT_CALL(
      proxy_->GetMock().overalls,
      Orspi_UnloadBlob_RSA_KEY_PARMS_s(_, parms_ptr, sizeof(kFakeParms), _))
      .WillOnce(DoAll(SetArgPointee<0>(sizeof(kFakeParms)),
                      SetArgPointee<3>(key_parms), Return(TPM_SUCCESS)));

  EXPECT_THAT(backend_->GetVendorTpm1().IsSrkRocaVulnerable(),
              IsOkAndHolds(true));
}

TEST_F(BackendVendorTpm1Test, IsSrkRocaVulnerableFalse) {
  constexpr uint8_t kFakeParms[] = {0xde, 0xad, 0xbe, 0xef, 0x12,
                                    0x34, 0x56, 0x78, 0x90};

  // A key generated by a non-vulnerable TPM.
  constexpr uint8_t kVulnerableModulus[] = {
      0x00, 0xcc, 0xe8, 0xcf, 0xb5, 0x6e, 0x36, 0x99, 0x21, 0x7b, 0x95, 0xb9,
      0x75, 0xa6, 0x80, 0x12, 0xb0, 0x54, 0x1c, 0x62, 0x10, 0x77, 0x06, 0xbf,
      0x2c, 0xad, 0xa6, 0x5a, 0x79, 0x6a, 0x23, 0x06, 0x87, 0x2a, 0xf8, 0x37,
      0x4c, 0x47, 0xa7, 0xcf, 0x82, 0x7e, 0xa1, 0xd5, 0x73, 0x56, 0x04, 0xc4,
      0x60, 0xd7, 0x43, 0x5d, 0xa6, 0x6b, 0x44, 0x83, 0x77, 0xf9, 0x72, 0xff,
      0x7d, 0xc4, 0x5c, 0x74, 0x3a, 0x43, 0x97, 0x68, 0xa1, 0x01, 0x57, 0x94,
      0x22, 0xd8, 0xea, 0x19, 0x50, 0xf0, 0x4d, 0x29, 0x59, 0x04, 0xca, 0x92,
      0x64, 0xb1, 0x3e, 0x13, 0x9e, 0x38, 0x82, 0xbf, 0xaa, 0xb5, 0x25, 0x57,
      0xa1, 0xe0, 0x46, 0x89, 0x7f, 0x5d, 0x22, 0x03, 0x82, 0x89, 0x93, 0xa7,
      0x6f, 0xb9, 0xb5, 0x2f, 0x51, 0x98, 0xa1, 0x8a, 0xae, 0xca, 0x97, 0x6b,
      0x1d, 0x33, 0xbf, 0xc0, 0x04, 0x63, 0x47, 0x04, 0x5c, 0xfc, 0x98, 0x88,
      0x6c, 0xb1, 0x05, 0x9b, 0xab, 0x69, 0x91, 0xca, 0xab, 0xa0, 0x39, 0x62,
      0xcd, 0x0e, 0xa2, 0xb0, 0x04, 0x36, 0xa3, 0x1f, 0x08, 0x82, 0xf0, 0x16,
      0xd9, 0xf8, 0xdf, 0x08, 0xaa, 0xa6, 0xac, 0x2e, 0x60, 0x77, 0xb3, 0xbb,
      0x17, 0x71, 0x60, 0x7e, 0xb1, 0x46, 0x0d, 0x7b, 0xf2, 0x81, 0xef, 0x45,
      0xb0, 0xa5, 0xbd, 0x3f, 0x8a, 0xe4, 0x3d, 0x81, 0x51, 0x3b, 0xbe, 0xc4,
      0x84, 0x5d, 0x82, 0xba, 0xff, 0xca, 0x6c, 0x21, 0x90, 0x9c, 0x94, 0x3f,
      0x1e, 0x34, 0x41, 0x02, 0x87, 0xcb, 0xa9, 0xd8, 0x01, 0x48, 0xe5, 0x8b,
      0x7f, 0x38, 0xd4, 0x6e, 0xf3, 0xf8, 0x7b, 0xd8, 0xa3, 0x8e, 0x3d, 0xb9,
      0x58, 0x8c, 0xab, 0x57, 0x03, 0x3b, 0xff, 0x94, 0x0b, 0x8b, 0x94, 0xf4,
      0x36, 0xd7, 0x7f, 0x4f, 0xf6, 0x56, 0x3f, 0x80, 0x2a, 0x4a, 0xea, 0xfd,
      0x74, 0x20, 0x5f, 0x90, 0xa3,
  };

  SetupSrk();

  // These ptrs would be owned by the fake_pub_key.
  uint8_t* parms_ptr = static_cast<uint8_t*>(malloc(sizeof(kFakeParms)));
  memcpy(parms_ptr, kFakeParms, sizeof(kFakeParms));
  uint8_t* key_ptr = static_cast<uint8_t*>(malloc(sizeof(kVulnerableModulus)));
  memcpy(key_ptr, kVulnerableModulus, sizeof(kVulnerableModulus));

  TPM_PUBKEY fake_pub_key{
      .algorithmParms =
          TPM_KEY_PARMS{
              .algorithmID = TPM_ALG_RSA,
              .encScheme = TPM_ES_NONE,
              .sigScheme = TPM_SS_NONE,
              .parmSize = sizeof(kFakeParms),
              .parms = parms_ptr,
          },
      .pubKey =
          TPM_STORE_PUBKEY{
              .keyLength = sizeof(kVulnerableModulus),
              .key = key_ptr,
          },
  };

  EXPECT_CALL(proxy_->GetMock().overalls,
              Orspi_UnloadBlob_PUBKEY_s(_, _, kDefaultSrkPubkey.size(), _))
      .WillOnce(DoAll(SetArgPointee<0>(kDefaultSrkPubkey.size()),
                      SetArgPointee<3>(fake_pub_key), Return(TPM_SUCCESS)));

  TPM_RSA_KEY_PARMS key_parms{
      .keyLength = 0,
      .numPrimes = 0,
      .exponentSize = 0,
      .exponent = nullptr,
  };

  EXPECT_CALL(
      proxy_->GetMock().overalls,
      Orspi_UnloadBlob_RSA_KEY_PARMS_s(_, parms_ptr, sizeof(kFakeParms), _))
      .WillOnce(DoAll(SetArgPointee<0>(sizeof(kFakeParms)),
                      SetArgPointee<3>(key_parms), Return(TPM_SUCCESS)));

  EXPECT_THAT(backend_->GetVendorTpm1().IsSrkRocaVulnerable(),
              IsOkAndHolds(false));
}

TEST_F(BackendVendorTpm1Test, IsSrkRocaVulnerableLengthFailed) {
  SetupSrk();

  TPM_PUBKEY fake_pub_key{};

  EXPECT_CALL(proxy_->GetMock().overalls,
              Orspi_UnloadBlob_PUBKEY_s(_, _, kDefaultSrkPubkey.size(), _))
      .WillOnce(DoAll(SetArgPointee<0>(kDefaultSrkPubkey.size() - 1),
                      SetArgPointee<3>(fake_pub_key), Return(TPM_SUCCESS)));

  auto result = backend_->GetVendorTpm1().IsSrkRocaVulnerable();
  ASSERT_NOT_OK(result);
}

TEST_F(BackendVendorTpm1Test, IsSrkRocaVulnerableLengthFailed2) {
  SetupSrk();

  TPM_PUBKEY fake_pub_key{};

  EXPECT_CALL(proxy_->GetMock().overalls,
              Orspi_UnloadBlob_PUBKEY_s(_, _, kDefaultSrkPubkey.size(), _))
      .WillOnce(DoAll(SetArgPointee<0>(kDefaultSrkPubkey.size()),
                      SetArgPointee<3>(fake_pub_key), Return(TPM_SUCCESS)));

  TPM_RSA_KEY_PARMS key_parms{};

  EXPECT_CALL(proxy_->GetMock().overalls,
              Orspi_UnloadBlob_RSA_KEY_PARMS_s(_, _, 0, _))
      .WillOnce(DoAll(SetArgPointee<0>(1), SetArgPointee<3>(key_parms),
                      Return(TPM_SUCCESS)));

  auto result = backend_->GetVendorTpm1().IsSrkRocaVulnerable();
  ASSERT_NOT_OK(result);
}

TEST_F(BackendVendorTpm1Test, GetIFXFieldUpgradeInfo) {
  brillo::Blob fake_result(108, 'Z');
  fake_result[0] = 0;
  fake_result[1] = 106;

  EXPECT_CALL(proxy_->GetMock().overalls,
              Ospi_TPM_FieldUpgrade(kDefaultTpm, _, _, _, _))
      .WillOnce(DoAll(SetArgPointee<3>(fake_result.size()),
                      SetArgPointee<4>(fake_result.data()),
                      Return(TPM_SUCCESS)));

  EXPECT_CALL(proxy_->GetMock().overalls, Orspi_UnloadBlob_UINT16_s(_, _, _, _))
      .WillRepeatedly(Trspi_UnloadBlob_UINT16_s);

  EXPECT_CALL(proxy_->GetMock().overalls, Orspi_UnloadBlob_UINT32_s(_, _, _, _))
      .WillRepeatedly(Trspi_UnloadBlob_UINT32_s);

  EXPECT_THAT(backend_->GetVendorTpm1().GetIFXFieldUpgradeInfo(), IsOk());
}

TEST_F(BackendVendorTpm1Test, GetIFXFieldUpgradeInfoLengthMismatch) {
  brillo::Blob fake_result{42, 42, 42, 42, 42};

  EXPECT_CALL(proxy_->GetMock().overalls,
              Ospi_TPM_FieldUpgrade(kDefaultTpm, _, _, _, _))
      .WillOnce(DoAll(SetArgPointee<3>(fake_result.size()),
                      SetArgPointee<4>(fake_result.data()),
                      Return(TPM_SUCCESS)));

  EXPECT_CALL(proxy_->GetMock().overalls, Orspi_UnloadBlob_UINT16_s(_, _, _, _))
      .WillRepeatedly(Trspi_UnloadBlob_UINT16_s);

  EXPECT_CALL(proxy_->GetMock().overalls, Orspi_UnloadBlob_UINT32_s(_, _, _, _))
      .WillRepeatedly(Trspi_UnloadBlob_UINT32_s);

  EXPECT_THAT(backend_->GetVendorTpm1().GetIFXFieldUpgradeInfo(), NotOk());
}

TEST_F(BackendVendorTpm1Test, GetIFXFieldUpgradeInfoUnknownLength) {
  brillo::Blob fake_result{0, 3, 1, 2, 3};

  EXPECT_CALL(proxy_->GetMock().overalls,
              Ospi_TPM_FieldUpgrade(kDefaultTpm, _, _, _, _))
      .WillOnce(DoAll(SetArgPointee<3>(fake_result.size()),
                      SetArgPointee<4>(fake_result.data()),
                      Return(TPM_SUCCESS)));

  EXPECT_CALL(proxy_->GetMock().overalls, Orspi_UnloadBlob_UINT16_s(_, _, _, _))
      .WillRepeatedly(Trspi_UnloadBlob_UINT16_s);

  EXPECT_CALL(proxy_->GetMock().overalls, Orspi_UnloadBlob_UINT32_s(_, _, _, _))
      .WillRepeatedly(Trspi_UnloadBlob_UINT32_s);

  EXPECT_THAT(backend_->GetVendorTpm1().GetIFXFieldUpgradeInfo(), NotOk());
}

}  // namespace hwsec
