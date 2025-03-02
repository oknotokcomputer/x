// Copyright 2018 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/base/file_test_utils.h"

#include <initializer_list>
#include <optional>
#include <string>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/files/scoped_temp_dir.h>
#include <brillo/files/file_util.h>
#include <gtest/gtest.h>

#include "diagnostics/base/paths.h"

namespace diagnostics {

bool WriteFileAndCreateParentDirs(const base::FilePath& file_path,
                                  const std::string& file_contents) {
  if (!base::CreateDirectory(file_path.DirName())) {
    return false;
  }
  return base::WriteFile(file_path, file_contents.c_str(),
                         file_contents.size()) == file_contents.size();
}

bool WriteFileAndCreateSymbolicLink(const base::FilePath& file_path,
                                    const std::string& file_contents,
                                    const base::FilePath& symlink_path) {
  if (!WriteFileAndCreateParentDirs(file_path, file_contents))
    return false;
  if (!base::CreateDirectory(symlink_path.DirName()))
    return false;
  return base::CreateSymbolicLink(file_path, symlink_path);
}

BaseFileTest::PathType::PathType(std::initializer_list<std::string> paths) {
  auto it = paths.begin();
  file_path_ = base::FilePath(*it);
  for (++it; it != paths.end(); ++it) {
    file_path_ = file_path_.Append(*it);
  }
}

void BaseFileTest::UnsetPath(const PathType& path) const {
  ASSERT_FALSE(GetRootDir().empty());
  ASSERT_TRUE(brillo::DeletePathRecursively(GetPathUnderRoot(path)));
}

void BaseFileTest::SetSymbolicLink(const PathType& target,
                                   const PathType& path) {
  UnsetPath(path);
  auto file = GetPathUnderRoot(path);
  ASSERT_TRUE(base::CreateDirectory(file.DirName()));
  auto real_target = target.file_path().IsAbsolute() ? GetPathUnderRoot(target)
                                                     : target.file_path();
  ASSERT_TRUE(base::CreateSymbolicLink(real_target, file));
}

base::FilePath BaseFileTest::GetPathUnderRoot(const PathType& path) const {
  if (!path.file_path().IsAbsolute())
    return GetRootedPath(base::FilePath{"/"}.Append(path.file_path()));
  return GetRootedPath(path.file_path());
}

void BaseFileTest::SetFakeCrosConfig(const PathType& path,
                                     const std::optional<std::string>& data) {
  base::FilePath full_path =
      paths::cros_config::kRoot.ToPath().Append(path.file_path());
  if (data) {
    SetFile(full_path, data.value());
  } else {
    UnsetPath(full_path);
  }
}

}  // namespace diagnostics
