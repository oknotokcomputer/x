// Copyright 2022 The ChromiumOS Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/cros_healthd/system/fake_mojo_service.h"

#include <memory>

namespace diagnostics {

FakeMojoService::FakeMojoService() = default;

FakeMojoService::~FakeMojoService() = default;

void FakeMojoService::InitializeFakeMojoService() {
  chromium_data_collector_relay().InitNewPipeAndWaitForIncomingRemote();
  chromium_data_collector_relay().Bind(
      fake_chromium_data_collector_.receiver().BindNewPipeAndPassRemote());
}

}  // namespace diagnostics
