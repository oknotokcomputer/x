// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "patchpanel/conntrack_monitor.h"

#include <fcntl.h>
#include <linux/netfilter/nf_conntrack_tcp.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <utility>

#include <base/files/file_util.h>
#include <base/files/scoped_file.h>
#include <base/test/task_environment.h>
#include <gtest/gtest.h>
#include <net-base/mock_socket.h>

using testing::_;
using testing::Return;

namespace patchpanel {
namespace {
constexpr uint8_t kDefaultEventBitMask = 0;
constexpr uint8_t kNewEventBitMask = (1 << 0);
constexpr ConntrackMonitor::EventType kEventType[] = {
    ConntrackMonitor::EventType::kNew};

// This buffer is taken from real data that was passed into socket.
constexpr uint8_t kEventBuf1[] = {
    252, 0,   0,   0,   0,   1,   0,   6,   0,   0,   0,   0,   0,   0,   0,
    0,   10,  0,   0,   0,   76,  0,   1,   128, 44,  0,   1,   128, 20,  0,
    3,   0,   36,  1,   250, 0,   4,   128, 238, 8,   244, 233, 24,  174, 140,
    174, 23,  33,  20,  0,   4,   0,   36,  4,   104, 0,   64,  4,   8,   34,
    0,   0,   0,   0,   0,   0,   32,  3,   28,  0,   2,   128, 5,   0,   1,
    0,   6,   0,   0,   0,   6,   0,   2,   0,   167, 64,  0,   0,   6,   0,
    3,   0,   1,   187, 0,   0,   76,  0,   2,   128, 44,  0,   1,   128, 20,
    0,   3,   0,   36,  4,   104, 0,   64,  4,   8,   34,  0,   0,   0,   0,
    0,   0,   32,  3,   20,  0,   4,   0,   36,  1,   250, 0,   4,   128, 238,
    8,   244, 233, 24,  174, 140, 174, 23,  33,  28,  0,   2,   128, 5,   0,
    1,   0,   6,   0,   0,   0,   6,   0,   2,   0,   1,   187, 0,   0,   6,
    0,   3,   0,   167, 64,  0,   0,   8,   0,   12,  0,   209, 33,  223, 24,
    8,   0,   3,   0,   0,   0,   1,   136, 8,   0,   7,   0,   0,   0,   0,
    120, 48,  0,   4,   128, 44,  0,   1,   128, 5,   0,   1,   0,   1,   0,
    0,   0,   5,   0,   2,   0,   7,   0,   0,   0,   5,   0,   3,   0,   0,
    0,   0,   0,   6,   0,   4,   0,   3,   0,   0,   0,   6,   0,   5,   0,
    0,   0,   0,   0,   8,   0,   8,   0,   3,   234, 1,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   64,  236, 61,  241, 125, 86,  0,
    0,   112, 97,  116, 99,  104, 112, 97,  110, 101, 108, 100, 0,   32,  0,
    0,   0,   4,   0,   0,   0,   21,  0,   1,   0,   62,  0,   0,   192, 6,
    0,   0,   0,   0,   0,   0,   0,   32,  0,   0,   0,   0,   0,   0,   0,
    21,  0,   0,   1,   3,   0,   0,   0,   6,   0,   0,   0,   0,   0,   255,
    127, 21,  0,   0,   1,   41,  0,   0,   0,   5,   0,   0,   0,   75,  0,
    0,   0,   21,  0,   0,   1,   55,  0,   0,   0,   6,   0,   0,   0,   0,
    0,   255, 127, 21,  0,   0,   1,   72,  0,   0,   0,   6,   0,   0,   0,
    0,   0,   255, 127, 21,  0,   0,   1,   9,   0,   0,   0,   5,   0,   0,
    0,   113, 0,   0,   0,   21,  0,   0,   1,   157, 0,   0,   0,   5,   0,
    0,   0,   135, 0,   0,   0,   21,  0,   0,   1,   6,   1,   0,   0,   6,
    0,   0,   0,   0,   0,   255, 127, 21,  0,   0,   1,   10,  0,   0,   0,
    5,   0,   0,   0,   175, 0,   0,   0,   21,  0,   0,   1,   1,   1,   0,
    0,   6,   0,   0,   0,   0,   0,   255, 127, 21,  0,   0,   1,   138, 0,
    0,   0,   6,   0,   0,   0,   0,   0,   255, 127, 21,  0,   0,   1,   0,
    0,   0,   0,   6,   0,   0,   0,   0,   0,   255, 127, 21,  0,   0,   1,
    11,  0,   0,   0,   6,   0,   0,   0,   0,   0,   255, 127, 21,  0,   0,
    1,   1,   0,   0,   0,   6,   0,   0,   0,   0,   0,   255, 127, 21,  0,
    0,   1,   125, 0,   0,   0,   6,   0,   0,   0,   0,   0,   255, 127, 21,
    0,   0,   1,   126, 0,   0,   0,   6,   0,   0,   0,   0,   0,   255, 127,
    21,  0,   0,   1,   116, 0,   0,   0,   6,   0,   0,   0,   0,   0,   255,
    127, 21,  0,   0,   1,   119, 0,   0,   0,   6,   0,   0,   0,   0,   0,
    255, 127, 21,  0,   0,   1,   117, 0,   0,   0,   6,   0,   0,   0,   0,
    0,   255, 127, 21,  0,   0,   1,   73,  0,   0,   0,   6,   0,   0,   0,
    0,   0,   255, 127, 21,  0,   0,   1,   137, 0,   0,   0,   6,   0,   0,
    0,   0,   0,   255, 127, 21,  0,   0,   1,   231, 0,   0,   0,   6,   0,
    0,   0,   0,   0,   255, 127, 21,  0,   0,   1,   219, 0,   0,   0,   6,
    0,   0,   0,   0,   0,   255, 127, 21,  0,   0,   1,   60,  0,   0};

constexpr uint8_t kEventBuf2[] = {
    156, 0,   0,   0,   0,   1,   0,   6,   0,   0,   0,   0,   0,   0,   0,
    0,   2,   0,   0,   0,   52,  0,   1,   128, 20,  0,   1,   128, 8,   0,
    1,   0,   100, 115, 92,  133, 8,   0,   2,   0,   100, 115, 92,  134, 28,
    0,   2,   128, 5,   0,   1,   0,   17,  0,   0,   0,   6,   0,   2,   0,
    83,  250, 0,   0,   6,   0,   3,   0,   0,   53,  0,   0,   52,  0,   2,
    128, 20,  0,   1,   128, 8,   0,   1,   0,   8,   8,   4,   4,   8,   0,
    2,   0,   100, 87,  84,  250, 28,  0,   2,   128, 5,   0,   1,   0,   17,
    0,   0,   0,   6,   0,   2,   0,   0,   53,  0,   0,   6,   0,   3,   0,
    83,  250, 0,   0,   8,   0,   12,  0,   238, 205, 93,  7,   8,   0,   3,
    0,   0,   0,   1,   184, 8,   0,   7,   0,   0,   0,   0,   30,  8,   0,
    8,   0,   3,   234, 1,   0,   6,   0,   2,   0,   1,   187, 0,   0,   6,
    0,   3,   0,   149, 230, 0,   0,   8,   0,   12,  0,   228, 140, 201, 89,
    8,   0,   3,   0,   0,   0,   1,   136, 8,   0,   7,   0,   0,   0,   0,
    30,  8,   0,   8,   0,   3,   234, 1,   0,   5,   0,   1,   0,   3,   0,
    0,   0,   5,   0,   2,   0,   0,   0,   0,   0,   5,   0,   3,   0,   0,
    0,   0,   0,   6,   0,   4,   0,   10,  0,   0,   0,   6,   0,   5,   0,
    10,  0,   0,   0,   8,   0,   8,   0,   3,   235, 4,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   64,  236, 61,  241, 125, 86,  0,
    0,   112, 97,  116, 99,  104, 112, 97,  110, 101, 108, 100, 0,   32,  0,
    0,   0,   4,   0,   0,   0,   21,  0,   1,   0,   62,  0,   0,   192, 6,
    0,   0,   0,   0,   0,   0,   0,   32,  0,   0,   0,   0,   0,   0,   0,
    21,  0,   0,   1,   3,   0,   0,   0,   6,   0,   0,   0,   0,   0,   255,
    127, 21,  0,   0,   1,   41,  0,   0,   0,   5,   0,   0,   0,   75,  0,
    0,   0,   21,  0,   0,   1,   55,  0,   0,   0,   6,   0,   0,   0,   0,
    0,   255, 127, 21,  0,   0,   1,   72,  0,   0,   0,   6,   0,   0,   0,
    0,   0,   255, 127, 21,  0,   0,   1,   9,   0,   0,   0,   5,   0,   0,
    0,   113, 0,   0,   0,   21,  0,   0,   1,   157, 0,   0,   0,   5,   0,
    0,   0,   135, 0,   0,   0,   21,  0,   0,   1,   6,   1,   0,   0,   6,
    0,   0,   0,   0,   0,   255, 127, 21,  0,   0,   1,   10,  0,   0,   0,
    5,   0,   0,   0,   175, 0,   0,   0,   21,  0,   0,   1,   1,   1,   0,
    0,   6,   0,   0,   0,   0,   0,   255, 127, 21,  0,   0,   1,   138, 0,
    0,   0,   6,   0,   0,   0,   0,   0,   255, 127, 21,  0,   0,   1,   0,
    0,   0,   0,   6,   0,   0,   0,   0,   0,   255, 127, 21,  0,   0,   1,
    11,  0,   0,   0,   6,   0,   0,   0,   0,   0,   255, 127, 21,  0,   0,
    1,   1,   0,   0,   0,   6,   0,   0,   0,   0,   0,   255, 127, 21,  0,
    0,   1,   125, 0,   0,   0,   6,   0,   0,   0,   0,   0,   255, 127, 21,
    0,   0,   1,   126, 0,   0,   0,   6,   0,   0,   0,   0,   0,   255, 127,
    21,  0,   0,   1,   116, 0,   0,   0,   6,   0,   0,   0,   0,   0,   255,
    127, 21,  0,   0,   1,   119, 0,   0,   0,   6,   0,   0,   0,   0,   0,
    255, 127, 21,  0,   0,   1,   117, 0,   0,   0,   6,   0,   0,   0,   0,
    0,   255, 127, 21,  0,   0,   1,   73,  0,   0,   0,   6,   0,   0,   0,
    0,   0,   255, 127, 21,  0,   0,   1,   137, 0,   0,   0,   6,   0,   0,
    0,   0,   0,   255, 127, 21,  0,   0,   1,   231, 0,   0,   0,   6,   0,
    0,   0,   0,   0,   255, 127, 21,  0,   0,   1,   219, 0,   0,   0,   6,
    0,   0,   0,   0,   0,   255, 127, 21,  0,   0,   1,   60,  0,   0,   0,
    6,   0,   0,   0,   0,   0,
};

const ConntrackMonitor::Event kEvent1 = ConntrackMonitor::Event{
    .src = *net_base::IPAddress::CreateFromString(
        "2401:fa00:480:ee08:f4e9:18ae:8cae:1721"),
    .dst = (*net_base::IPAddress::CreateFromString("2404:6800:4004:822::2003")),
    .sport = 16551,
    .dport = 47873,
    .proto = IPPROTO_TCP,
    .type = ConntrackMonitor::EventType::kNew,
    .state = TCP_CONNTRACK_SYN_SENT};

const ConntrackMonitor::Event kEvent2 = ConntrackMonitor::Event{
    .src = *net_base::IPAddress::CreateFromString("100.115.92.133"),
    .dst = (*net_base::IPAddress::CreateFromString("100.115.92.134")),
    .sport = 64083,
    .dport = 13568,
    .proto = IPPROTO_UDP,
    .type = ConntrackMonitor::EventType::kNew,
};

class MockCallback {
 public:
  MOCK_METHOD(void,
              OnConntrackEventReceived,
              (const ConntrackMonitor::Event& event));
};
}  // namespace

class ConntrackMonitorTest : public testing::Test {
 protected:
  ConntrackMonitorTest() {
    CHECK_EQ(pipe(pipe_fds_), 0);
    write_fd_ = base::ScopedFD(pipe_fds_[1]);
    read_fd_ = base::ScopedFD(pipe_fds_[0]);

    auto socket_factory = std::make_unique<net_base::MockSocketFactory>();
    socket_factory_ = socket_factory.get();
    ConntrackMonitor::GetInstance()->SetSocketFactoryForTesting(
        std::move(socket_factory));
  }

  ~ConntrackMonitorTest() {
    // Need to clear the state for the singleton manually after each test,
    // otherwise its state will affect following tests.
    ConntrackMonitor::GetInstance()->StopForTesting();
  }
  int read_fd() const { return pipe_fds_[0]; }
  int write_fd() const { return pipe_fds_[1]; }

  // The environment instances which are required for using
  // base::FileDescriptorWatcher::WatchReadable. Declared them first to ensure
  // they are the last things to be cleaned up.
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO};
  net_base::MockSocketFactory* socket_factory_;  // Owned by ConntrackMonitor
  int pipe_fds_[2];
  base::ScopedFD write_fd_, read_fd_;
};

MATCHER_P(IsNetlinkAddr, groups, "") {
  const struct sockaddr_nl* socket_address =
      reinterpret_cast<const struct sockaddr_nl*>(arg);
  return socket_address->nl_family == AF_NETLINK &&
         socket_address->nl_groups == groups;
}

MATCHER(IsNetlinkAddrLength, "") {
  const unsigned int* addrlen = reinterpret_cast<const unsigned int*>(arg);
  return *addrlen == sizeof(sockaddr_nl);
}

MATCHER_P(IsConntrackEvent, event, "") {
  return arg == event;
}

TEST_F(ConntrackMonitorTest, Start) {
  auto socket = std::make_unique<net_base::MockSocket>(
      base::ScopedFD(std::move(read_fd_)), SOCK_RAW);
  bool read_once = 0;
  EXPECT_CALL(*socket_factory_, Create(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER))
      .WillOnce([&socket, this, &read_once]() {
        EXPECT_CALL(*socket, GetSockName(IsNetlinkAddr(kDefaultEventBitMask),
                                         IsNetlinkAddrLength()))
            .WillOnce(Return(true));
        EXPECT_CALL(*socket,
                    Bind(IsNetlinkAddr(kNewEventBitMask), sizeof(sockaddr_nl)))
            .WillRepeatedly(Return(true));

        EXPECT_CALL(*socket, RecvFrom(_, 0, IsNetlinkAddr(kDefaultEventBitMask),
                                      IsNetlinkAddrLength()))
            .WillRepeatedly(testing::WithArg<0>([this, &read_once](
                                                    base::span<uint8_t> buf) {
              // We need to set right size to receive from fd based on which
              // event we are trying to receive.
              size_t read_size =
                  read_once ? sizeof(kEventBuf2) : sizeof(kEventBuf1);
              read_once = true;
              EXPECT_TRUE(base::ReadFromFD(
                  read_fd(), reinterpret_cast<char*>(buf.data()), read_size));
              return read_size;
            }));
        return std::move(socket);
      });
  auto* monitor = ConntrackMonitor::GetInstance();
  ASSERT_NE(monitor, nullptr);
  monitor->Start(kEventType);
  MockCallback event_cb;
  auto listener = monitor->AddListener(
      kEventType, base::BindRepeating(&MockCallback::OnConntrackEventReceived,
                                      base::Unretained(&event_cb)));
  EXPECT_CALL(event_cb, OnConntrackEventReceived(IsConntrackEvent(kEvent1)));
  EXPECT_CALL(event_cb, OnConntrackEventReceived(IsConntrackEvent(kEvent2)));
  // Write a message to fill buffer and trigger file descriptor watcher.
  ASSERT_TRUE(base::WriteFileDescriptor(write_fd_.get(), kEventBuf1));
  ASSERT_TRUE(base::WriteFileDescriptor(write_fd_.get(), kEventBuf2));
  task_environment_.RunUntilIdle();
}

TEST_F(ConntrackMonitorTest, CreateGetSockNameFailed) {
  EXPECT_CALL(*socket_factory_, Create(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER))
      .WillOnce([this]() {
        auto socket = std::make_unique<net_base::MockSocket>(
            base::ScopedFD(std::move(read_fd_)), SOCK_RAW);
        EXPECT_CALL(*socket, GetSockName(IsNetlinkAddr(kDefaultEventBitMask),
                                         IsNetlinkAddrLength()))
            .WillOnce(Return(false));
        return socket;
      });

  auto monitor = ConntrackMonitor::GetInstance();
  ASSERT_NE(monitor, nullptr);
  monitor->Start(kEventType);
  EXPECT_TRUE(monitor->IsSocketNullForTesting());
}

TEST_F(ConntrackMonitorTest, CreateBindFailed) {
  EXPECT_CALL(*socket_factory_, Create(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER))
      .WillOnce([this]() {
        auto socket = std::make_unique<net_base::MockSocket>(
            std::move(read_fd_), SOCK_RAW);

        EXPECT_CALL(*socket, GetSockName(IsNetlinkAddr(kDefaultEventBitMask),
                                         IsNetlinkAddrLength()))
            .WillOnce(Return(true));
        EXPECT_CALL(*socket,
                    Bind(IsNetlinkAddr(kNewEventBitMask), sizeof(sockaddr_nl)))
            .WillOnce(Return(false));
        return socket;
      });

  auto monitor = ConntrackMonitor::GetInstance();
  ASSERT_NE(monitor, nullptr);
  monitor->Start(kEventType);
  EXPECT_TRUE(monitor->IsSocketNullForTesting());
}

TEST_F(ConntrackMonitorTest, CreateSocketIsNull) {
  EXPECT_CALL(*socket_factory_, Create(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER))
      .WillOnce(Return(nullptr));

  auto monitor = ConntrackMonitor::GetInstance();
  ASSERT_NE(monitor, nullptr);
  monitor->Start(kEventType);
  EXPECT_TRUE(monitor->IsSocketNullForTesting());
}
}  // namespace patchpanel
