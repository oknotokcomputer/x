// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RUNTIME_PROBE_AVL_PROBE_CONFIG_LOADER_H_
#define RUNTIME_PROBE_AVL_PROBE_CONFIG_LOADER_H_

#include <vector>

#include <base/files/file_path.h>

#include "runtime_probe/probe_config_loader.h"

namespace runtime_probe {

inline constexpr char kRuntimeProbeConfigDir[] = "etc/runtime_probe";
inline constexpr char kAvlProbeConfigName[] = "probe_config.json";
inline constexpr char kUsrLocal[] = "usr/local";

// AvlProbeConfigLoader loads probe configs for AVL verification.
class AvlProbeConfigLoader : public ProbeConfigLoader {
 public:
  AvlProbeConfigLoader() = default;

  // Load probe config from AVL config paths. The function will return
  // std::nullopt when loading fails.
  std::optional<ProbeConfigData> Load() const override;

 private:
  // Return default paths for AVL probe configs.  When cros_debug is disabled,
  // the default paths will be:
  //     * `/etc/runtime_probe/<model_name>/probe_config.json`
  //     * `/etc/runtime_probe/probe_config.json`
  // When cros_debug is enabled, the config paths under the stateful partition
  // will also be included:
  //     * `/usr/local/etc/runtime_probe/<model_name>/probe_config.json`
  //     * `/usr/local/etc/runtime_probe/probe_config.json`
  //     * `/etc/runtime_probe/<model_name>/probe_config.json`
  //     * `/etc/runtime_probe/probe_config.json`
  std::vector<base::FilePath> GetPaths() const;
};

}  // namespace runtime_probe

#endif  // RUNTIME_PROBE_AVL_PROBE_CONFIG_LOADER_H_
