#!/bin/bash
# Copyright 2023 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -eux -o pipefail

main() {
  install_deps

  # shellcheck disable=SC2154
  local src_root="${KOKORO_ARTIFACTS_DIR}/git/platform2/vm_tools/reference_vm"

  timestamp=$(date +%s)
  prefix="$(date -d @"${timestamp}" --utc +%Y-%m)"
  suffix="$(date -d @"${timestamp}" --utc +%Y%m%d_%H%M%S)"

  local result_dir="${src_root}/out/${prefix}"
  mkdir -p "${result_dir}"
  cd "${result_dir}"

  image_path="refvm-${suffix}.img"
  sudo "${src_root}/build.py" \
    --debian-release=bookworm \
    --vg-name="refvm_${suffix}" \
    -o "${image_path}"

  sudo virt-sparsify --in-place --machine-readable "${image_path}"

  zstd --no-progress -16 "${image_path}"
  sudo rm -f "${image_path}"
}

install_deps() {
  # If additional dependencies are required, please also note them in README.md.
  # Googlers: libguestfs-tools depends on qemu-system-x86 which may cause HT/SMT
  # to be disabled.
  sudo apt-get update
  sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y install \
    eatmydata fai-setup-storage lvm2 python3-jinja2 python3-requests \
    python3-yaml libguestfs-tools

  # TODO(b/280695675): Remove this once the VM image has a newer OS.
  # shellcheck disable=SC2154
  sudo dpkg -i "${KOKORO_GFILE_DIR}/fai-setup-storage_5.10.3ubuntu1_all.deb"
}

main "$@"
