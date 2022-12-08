// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIBHWSEC_FRONTEND_BOOTLOCKBOX_MOCK_FRONTEND_H_
#define LIBHWSEC_FRONTEND_BOOTLOCKBOX_MOCK_FRONTEND_H_

#include <optional>
#include <vector>

#include <brillo/secure_blob.h>
#include <gmock/gmock.h>

#include "libhwsec/frontend/bootlockbox/frontend.h"
#include "libhwsec/frontend/mock_frontend.h"

namespace hwsec {

class MockBootLockboxFrontend : public MockFrontend,
                                public BootLockboxFrontend {
 public:
  MockBootLockboxFrontend() = default;
  ~MockBootLockboxFrontend() override = default;

  MOCK_METHOD(StatusOr<StorageState>, GetSpaceState, (), (override));
  MOCK_METHOD(Status, PrepareSpace, (uint32_t size), (override));
  MOCK_METHOD(StatusOr<brillo::Blob>, LoadSpace, (), (override));
  MOCK_METHOD(Status, StoreSpace, (const brillo::Blob& blob), (override));
  MOCK_METHOD(Status, LockSpace, (), (override));
  MOCK_METHOD(StatusOr<bool>, IsSpaceWriteLocked, (), (override));
  MOCK_METHOD(void,
              WaitUntilReady,
              (base::OnceCallback<void(Status)> callback),
              (override));
};

}  // namespace hwsec

#endif  // LIBHWSEC_FRONTEND_BOOTLOCKBOX_MOCK_FRONTEND_H_
