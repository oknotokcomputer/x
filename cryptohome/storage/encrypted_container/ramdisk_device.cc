// Copyright 2021 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/storage/encrypted_container/ramdisk_device.h"

#include <sys/stat.h>

#include <memory>
#include <string>

#include <base/files/file_path.h>

#include "cryptohome/platform.h"
#include "cryptohome/storage/encrypted_container/loopback_device.h"

namespace cryptohome {
RamdiskDevice::RamdiskDevice(const BackingDeviceConfig& config,

                             Platform* platform)
    : LoopbackDevice(config, platform), platform_(platform) {}

bool RamdiskDevice::Create() {
  if (!platform_->CreateDirectory(backing_file_path_.DirName())) {
    LOG(ERROR) << "Can't create directory for ephemeral backing file";
    return false;
  }
  // TODO(dlunev): do stat VFS on the created backing file to make sure it is
  // on tmpfs.
  return LoopbackDevice::Create();
}

bool RamdiskDevice::Teardown() {
  bool ok = LoopbackDevice::Teardown();
  if (!platform_->DeleteFileDurable(backing_file_path_)) {
    LOG(ERROR) << "Can't delete ephemeral file";
    return false;
  }
  return ok;
}

bool RamdiskDevice::Purge() {
  bool ok = LoopbackDevice::Purge();
  if (!platform_->DeleteFileDurable(backing_file_path_)) {
    LOG(ERROR) << "Can't delete ephemeral file";
    return false;
  }
  return ok;
}

std::unique_ptr<RamdiskDevice> RamdiskDevice::Generate(
    const base::FilePath& backing_file_path, Platform* platform) {
  // Determine ephemeral cryptohome size.
  struct statvfs vfs;
  if (!platform->StatVFS(base::FilePath(backing_file_path.DirName().DirName()),
                         &vfs)) {
    PLOG(ERROR) << "Can't determine size for ephemeral device";
    return nullptr;
  }

  const int64_t sparse_size = static_cast<int64_t>(vfs.f_blocks * vfs.f_frsize);

  BackingDeviceConfig config{
      .type = BackingDeviceType::kLoopbackDevice,
      .name = "ephemeral",
      .size = sparse_size,
      .loopback =
          {
              .backing_file_path = backing_file_path,
          },
  };

  return std::unique_ptr<RamdiskDevice>(new RamdiskDevice(config, platform));
}

}  // namespace cryptohome
