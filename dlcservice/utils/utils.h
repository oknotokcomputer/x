// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DLCSERVICE_UTILS_UTILS_H_
#define DLCSERVICE_UTILS_UTILS_H_

#include <memory>
#include <string>
#include <vector>

#include <brillo/brillo_export.h>
#include <base/files/file_path.h>
#include <libimageloader/manifest.h>

#include "dlcservice/utils/utils_interface.h"

namespace dlcservice {

// DLC powerwash safe meta file.
BRILLO_EXPORT extern const char kDlcPowerwashSafeFile[];

// Default DLC package name.
BRILLO_EXPORT extern const char kPackage[];

// Default DLC manifest name.
BRILLO_EXPORT extern const char kManifestName[];

class BRILLO_EXPORT Utils : public UtilsInterface {
 public:
  Utils() = default;
  ~Utils() = default;

  Utils(const Utils&) = delete;
  Utils& operator=(const Utils&) = delete;

  // Overrides of `UtilsInterface`.
  std::string LogicalVolumeName(const std::string& id,
                                PartitionSlot slot) override;
  bool HashFile(const base::FilePath& path,
                int64_t size,
                std::vector<uint8_t>* sha256,
                bool skip_size_check) override;
  std::shared_ptr<imageloader::Manifest> GetDlcManifest(
      const base::FilePath& dlc_manifest_path,
      const std::string& id,
      const std::string& package) override;
};

// Wrapper functions to ease transitions/usages.
BRILLO_EXPORT std::string LogicalVolumeName(
    const std::string& id,
    PartitionSlot slot,
    std::unique_ptr<UtilsInterface> utils = std::make_unique<Utils>());
BRILLO_EXPORT bool HashFile(
    const base::FilePath& path,
    int64_t size,
    std::vector<uint8_t>* sha256,
    bool skip_size_check = false,
    std::unique_ptr<UtilsInterface> utils = std::make_unique<Utils>());
BRILLO_EXPORT std::shared_ptr<imageloader::Manifest> GetDlcManifest(
    const base::FilePath& dlc_manifest_path,
    const std::string& id,
    const std::string& package,
    std::unique_ptr<UtilsInterface> utils = std::make_unique<Utils>());

}  // namespace dlcservice

#endif  // DLCSERVICE_UTILS_UTILS_H_
