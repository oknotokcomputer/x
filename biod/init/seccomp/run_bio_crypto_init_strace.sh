#!/bin/bash

# Copyright 2019 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -x

# Use this script to generate an initial list of syscalls to whitelist with
# seccomp. Note that it will generate two files, each of which ends with the
# PID of the process that ran. There are two files because the main
# bio_crypto_init process forks a child process. The higher PIDs correspond to
# the child process that actually does the seeding.
#
# To generate the policy file, copy the above strace files to the host chroot
# and run the following command:
#
# (chroot) $ generate_seccomp_policy strace.log.* --policy \
#              bio-crypto-init-seccomp-<arch>.policy

OUTPUT_DIR="$(date --iso-8601=seconds)"
mkdir "${OUTPUT_DIR}"

# Use a random seed (instead of real TPM seed)
SEED="/run/bio_crypto_init/seed"
dd if=/dev/urandom of="${SEED}" bs=32 count=1
chown biod:biod "${SEED}"

strace -ff -o "${OUTPUT_DIR}/strace.log" -u biod \
    /usr/bin/bio_crypto_init --log_dir=/var/log/bio_crypto_init
