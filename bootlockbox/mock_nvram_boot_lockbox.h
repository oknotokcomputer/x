// Copyright 2018 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BOOTLOCKBOX_MOCK_NVRAM_BOOT_LOCKBOX_H_
#define BOOTLOCKBOX_MOCK_NVRAM_BOOT_LOCKBOX_H_

#include <string>

#include "bootlockbox/proto_bindings/boot_lockbox_rpc.pb.h"
#include "bootlockbox/nvram_boot_lockbox.h"
#include "bootlockbox/tpm_nvspace.h"

#include <gmock/gmock.h>

namespace cryptohome {

class MockNVRamBootLockbox : public NVRamBootLockbox {
 public:
  MockNVRamBootLockbox() : NVRamBootLockbox(NULL) {}
  virtual ~MockNVRamBootLockbox() {}

  MOCK_METHOD(bool,
              Store,
              (const std::string&, const std::string&, BootLockboxErrorCode*),
              (override));
  MOCK_METHOD(bool,
              Read,
              (const std::string&, std::string*, BootLockboxErrorCode*),
              (override));
  MOCK_METHOD(bool, Finalize, (), (override));
  MOCK_METHOD(NVSpaceState, GetState, (), (override));
  MOCK_METHOD(bool, DefineSpace, (), (override));
  MOCK_METHOD(bool, RegisterOwnershipCallback, (), (override));
  MOCK_METHOD(bool, Load, (), (override));
};

}  // namespace cryptohome

#endif  // BOOTLOCKBOX_MOCK_NVRAM_BOOT_LOCKBOX_H_
