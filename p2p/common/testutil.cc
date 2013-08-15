// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/testutil.h"

#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <glib-object.h>

#include <cctype>
#include <cinttypes>
#include <cstdlib>

#include <base/logging.h>
#include <gtest/gtest.h>

using std::string;

using base::FilePath;

namespace p2p {

namespace testutil {

// This is the message to be displayed if the TimeBombAbort timeout is
// reached.
static const char* time_bomb_abort_message_ = NULL;

TimeBombAbort::TimeBombAbort(int timeout_seconds, const char* message) {
  CHECK(time_bomb_abort_message_ == NULL);
  time_bomb_abort_message_ = message;
  // Install the signal handler keeping the previous one.
  struct sigaction time_bomb_action;
  time_bomb_action.sa_flags = 0;
  time_bomb_action.sa_handler = TimeoutHandler;
  sigaction(SIGALRM, &time_bomb_action, &previous_);
  alarm(timeout_seconds);
}

TimeBombAbort::~TimeBombAbort() {
  // Restore the previous sigaction.
  alarm(0);
  sigaction(SIGALRM, &previous_, NULL);
  time_bomb_abort_message_ = NULL;
}

void TimeBombAbort::TimeoutHandler(int signal) {
  // Does a "best-effort" write but fails silently.
  const char* msg = "\n\nTimeBombAbort::TimeoutHandler reached.\n";
  if (write(STDERR_FILENO, msg, strlen(msg)));
  if (write(STDERR_FILENO, time_bomb_abort_message_,
            strlen(time_bomb_abort_message_)));
  exit(1);
}

void ExpectCommand(int expected_exit_status,
                   const char* format,
                   ...) {
  va_list args;
  char buf[8192];

  va_start(args, format);
  vsnprintf(buf, sizeof buf, format, args);
  va_end(args);

  int rc = system(buf);

  EXPECT_TRUE(WIFEXITED(rc));
  EXPECT_EQ(WEXITSTATUS(rc), expected_exit_status);
}

FilePath SetupTestDir(const string& test_name) {
  // Create testing directory
  FilePath ret;
  string dir_name = string("/tmp/p2p-testing-") + test_name + ".XXXXXX";
  char* buf = strdup(dir_name.c_str());
  EXPECT_TRUE(mkdtemp(buf) != NULL);
  ret = FilePath(buf);
  free(buf);
  return ret;
}

void TeardownTestDir(const FilePath& dir_path) {
  // Sanity check
  EXPECT_TRUE(dir_path.value().find("/tmp/p2p-testing-") == 0);
  ExpectCommand(0, "rm -rf %s", dir_path.value().c_str());
}

static gboolean RunGMainLoopOnTimeout(gpointer user_data) {
  bool* timeout = static_cast<bool*>(user_data);
  *timeout = true;
  return FALSE;  // Remove timeout source
}

void RunGMainLoopUntil(int timeout_msec,
                                         base::Callback<bool()> terminate) {
  GMainLoop* loop = g_main_loop_new(NULL, FALSE);
  GMainContext* context = g_main_context_default();

  bool timeout = false;
  guint source_id = g_timeout_add(
      timeout_msec, p2p::testutil::RunGMainLoopOnTimeout, &timeout);

  while (!timeout && (terminate.is_null() || !terminate.Run()))
    g_main_context_iteration(context, TRUE);

  g_source_remove(source_id);
  g_main_loop_unref(loop);
}

int RunGMainLoopMaxIterations(int iterations) {
  int result;
  GMainContext* context = g_main_context_default();
  for (result = 0;
      result < iterations && g_main_context_iteration(context, FALSE);
      result++);
  return result;
}

size_t FileSize(const FilePath& dir,
                const string& file_name) {
  struct stat stat_buf;
  FilePath path = dir.Append(file_name);
  if (stat(path.value().c_str(), &stat_buf) != 0) {
    return 0;
  }
  return stat_buf.st_size;
}

void ExpectFileSize(const FilePath& dir,
                    const string& file_name,
                    size_t expected_size) {
  EXPECT_EQ(FileSize(dir, file_name), expected_size);
}

}  // namespace testutil

}  // namespace p2p
