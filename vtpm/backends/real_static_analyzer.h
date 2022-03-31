// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef VTPM_BACKENDS_REAL_STATIC_ANALYZER_H_
#define VTPM_BACKENDS_REAL_STATIC_ANALYZER_H_

#include "vtpm/backends/static_analyzer.h"

#include <trunks/tpm_generated.h>

namespace vtpm {

// This implements `StaticAnalyzer` with knowledge on how real TPM2.0 works.
class RealStaticAnalyzer : public StaticAnalyzer {
 public:
  ~RealStaticAnalyzer() override = default;
  int GetCommandHandleCount(trunks::TPM_CC cc) override;
};

}  // namespace vtpm

#endif  // VTPM_BACKENDS_REAL_STATIC_ANALYZER_H_
