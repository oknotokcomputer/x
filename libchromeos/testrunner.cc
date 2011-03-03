// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// based on pam_google_testrunner.cc

#include <chromeos/test_helpers.h>
#include <gtest/gtest.h>
#include <glib-object.h>

int main(int argc, char** argv) {
  ::g_type_init();
  SetUpTests(&argc, argv, true);
  return RUN_ALL_TESTS();
}
