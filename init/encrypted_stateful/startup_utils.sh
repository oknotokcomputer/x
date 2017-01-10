# Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Create, possibly migrate from, the unencrypted stateful partition, and bind
# mount the /var and /home/chronos mounts from the encrypted filesystem
# /mnt/stateful_partition/encrypted, all managed by the "mount-encrypted"
# helper. Takes the same arguments as mount-encrypted. Since /var is managed by
# mount-encrypted, it should not be created in the unencrypted stateful
# partition. Its mount point in the root filesystem exists already from the
# rootfs image.  Since /home is still mounted from the unencrypted stateful
# partition, having /home/chronos already doesn't matter. It will be created by
# mount-encrypted if it is missing. These mounts inherit nodev,noexec,nosuid
# from the encrypted filesystem /mnt/stateful_partition/encrypted.

# This should be same as unencrypted/startup_utils.sh
mount_var_and_home_chronos_unencrypted() {
  mkdir -p /mnt/stateful_partition/var || return 1
  mount -n --bind /mnt/stateful_partition/var /var || return 1
  mount -n --bind /mnt/stateful_partition/home/chronos /home/chronos
}

mount_var_and_home_chronos() {
  if [ "$*" = "factory" ]; then
    # chromium:679676 Factory tests may need to run in unencrypted environment.
    mount_var_and_home_chronos_unencrypted
  else
    mount-encrypted "$@" >/tmp/mount-encrypted.log 2>&1
  fi
}
