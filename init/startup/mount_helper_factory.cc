// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>

#include <base/files/file_path.h>
#include <base/values.h>
#include <libcrossystem/crossystem.h>

#include "init/startup/factory_mode_mount_helper.h"
#include "init/startup/flags.h"
#include "init/startup/mount_helper.h"
#include "init/startup/mount_helper_factory.h"
#include "init/startup/startup_dep_impl.h"
#include "init/startup/standard_mount_helper.h"
#include "init/startup/test_mode_mount_helper.h"

namespace startup {

MountHelperFactory::MountHelperFactory(StartupDep* startup_dep,
                                       const Flags& flags,
                                       const base::FilePath& root,
                                       const base::FilePath& stateful,
                                       const base::FilePath& lsb_file)
    : startup_dep_(startup_dep),
      flags_(flags),
      root_(root),
      stateful_(stateful),
      lsb_file_(lsb_file) {}

// Generate the mount helper class to use by determining whether a device
// is in dev mode, running a test image, and in factory mode. These different
// possible device configurations need different implementations of the
// functions DoMountVarAndHomeChronos and DoUmountVarAndHomeChronos.
// In the previous bash version of chromeos_startup, these different function
// implementations came from loading dev_utils.sh, test_utils.sh,
// factory_utils.sh and factory_utils.sh.
std::unique_ptr<MountHelper> MountHelperFactory::Generate(
    crossystem::Crossystem* crossystem) {
  bool dev_mode = InDevMode(crossystem);
  bool is_test_image = IsTestImage(lsb_file_);
  bool is_factory_mode = IsFactoryMode(crossystem, root_);

  // Use factory mount helper.
  if (dev_mode && is_test_image && is_factory_mode) {
    return std::make_unique<FactoryModeMountHelper>(
        std::move(startup_dep_), flags_, root_, stateful_, dev_mode);
  }

  if (dev_mode && is_test_image) {
    return std::make_unique<TestModeMountHelper>(startup_dep_, flags_, root_,
                                                 stateful_, dev_mode);
  }

  return std::make_unique<StandardMountHelper>(startup_dep_, flags_, root_,
                                               stateful_, dev_mode);
}

}  // namespace startup
