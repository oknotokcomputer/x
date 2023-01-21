// Copyright 2021 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <utility>

#include <base/check.h>
#include <base/check_op.h>
#include <base/files/scoped_temp_dir.h>
#include <base/files/file_util.h>
#include <chromeos-config/libcros_config/fake_cros_config.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "runtime_probe/probe_config_loader_impl.h"
#include "runtime_probe/system/context_mock_impl.h"

namespace runtime_probe {

namespace {

constexpr char kUsrLocal[] = "usr/local";

base::FilePath GetTestDataPath() {
  char* src_env = std::getenv("SRC");
  CHECK_NE(src_env, nullptr)
      << "Expect to have the envvar |SRC| set when testing.";
  return base::FilePath(src_env).Append("testdata");
}

class ProbeConfigLoaderImplTest : public ::testing::Test {
 protected:
  void SetUp() {
    PCHECK(scoped_temp_dir_.CreateUniqueTempDir());

    testdata_root_ = GetTestDataPath();

    probe_config_loader_ = std::make_unique<ProbeConfigLoaderImpl>();
    probe_config_loader_->SetRootForTest(GetRootDir());
  }

  // Sets model names to the given value.
  void SetModel(const std::string& val) {
    mock_context_.fake_cros_config()->SetString(kCrosConfigModelNamePath,
                                                kCrosConfigModelNameKey, val);
  }

  // Sets cros_debug flag to the given value.
  void SetCrosDebugFlag(int value) {
    mock_context_.fake_crossystem()->VbSetSystemPropertyInt("cros_debug",
                                                            value);
  }

  // Gets the root directory path used for unit test.
  const base::FilePath& GetRootDir() const {
    return scoped_temp_dir_.GetPath();
  }

  // Creates parent directories as needed before copying the file.
  bool CreateDirectoryAndCopyFile(const base::FilePath& from_path,
                                  const base::FilePath& to_path) const {
    PCHECK(base::CreateDirectoryAndGetError(to_path.DirName(), nullptr));
    PCHECK(base::CopyFile(from_path, to_path));
    return true;
  }

  std::unique_ptr<ProbeConfigLoaderImpl> probe_config_loader_;
  base::FilePath testdata_root_;

 private:
  base::ScopedTempDir scoped_temp_dir_;
  ContextMockImpl mock_context_;
};

}  // namespace

TEST(ProbeConfigLoaderImplTestConstructor, DefaultConstructor) {
  auto probe_config_loader_ = std::make_unique<ProbeConfigLoaderImpl>();
  EXPECT_NE(probe_config_loader_, nullptr);
}

TEST_F(ProbeConfigLoaderImplTest, LoadFromFile_WithoutCrosDebug) {
  for (const auto cros_config_flag : {0, 2 /* invalid flags */}) {
    SetCrosDebugFlag(cros_config_flag);
    const base::FilePath rel_path{kRuntimeProbeConfigName};
    const auto rel_file_path = testdata_root_.Append(kRuntimeProbeConfigName);

    const auto probe_config = probe_config_loader_->LoadFromFile(rel_file_path);
    EXPECT_FALSE(probe_config);
  }
}

TEST_F(ProbeConfigLoaderImplTest, LoadFromFile_RelativePath) {
  SetCrosDebugFlag(1);
  const base::FilePath rel_path{kRuntimeProbeConfigName};
  const auto rel_file_path = testdata_root_.Append(kRuntimeProbeConfigName);
  const auto abs_file_path = base::MakeAbsoluteFilePath(rel_file_path);

  const auto probe_config = probe_config_loader_->LoadFromFile(rel_file_path);
  EXPECT_TRUE(probe_config);
  EXPECT_EQ(probe_config->path, abs_file_path);
  EXPECT_FALSE(probe_config->config.DictEmpty());
  EXPECT_EQ(probe_config->sha1_hash,
            "0B6621DE5CDB0F805E614F19CAA6C38104F1F178");
}

TEST_F(ProbeConfigLoaderImplTest, LoadFromFile_AbsolutePath) {
  SetCrosDebugFlag(1);
  const base::FilePath rel_path{kRuntimeProbeConfigName};
  const auto rel_file_path = testdata_root_.Append(kRuntimeProbeConfigName);
  const auto abs_file_path = base::MakeAbsoluteFilePath(rel_file_path);

  const auto probe_config = probe_config_loader_->LoadFromFile(abs_file_path);
  EXPECT_TRUE(probe_config);
  EXPECT_EQ(probe_config->path, abs_file_path);
  EXPECT_FALSE(probe_config->config.DictEmpty());
  EXPECT_EQ(probe_config->sha1_hash,
            "0B6621DE5CDB0F805E614F19CAA6C38104F1F178");
}

TEST_F(ProbeConfigLoaderImplTest, LoadFromFile_MissingFile) {
  SetCrosDebugFlag(1);
  const base::FilePath rel_path{"missing_file.json"};

  const auto probe_config = probe_config_loader_->LoadFromFile(rel_path);
  EXPECT_FALSE(probe_config);
}

TEST_F(ProbeConfigLoaderImplTest, LoadFromFile_InvalidFile) {
  SetCrosDebugFlag(1);
  const base::FilePath rel_path{"invalid_config.json"};
  const char invalid_probe_config[] = "foo\nbar";
  PCHECK(WriteFile(GetRootDir().Append(rel_path), invalid_probe_config));

  const auto probe_config = probe_config_loader_->LoadFromFile(rel_path);
  EXPECT_FALSE(probe_config);
}

TEST_F(ProbeConfigLoaderImplTest, LoadFromFile_SymbolicLink) {
  SetCrosDebugFlag(1);
  const base::FilePath rel_path{kRuntimeProbeConfigName};
  const auto rel_file_path = testdata_root_.Append(kRuntimeProbeConfigName);
  const auto abs_file_path = base::MakeAbsoluteFilePath(rel_file_path);
  const auto symlink_config_path = GetRootDir().Append("config.json");

  PCHECK(base::CreateSymbolicLink(abs_file_path, symlink_config_path));
  auto probe_config = probe_config_loader_->LoadFromFile(symlink_config_path);
  EXPECT_TRUE(probe_config);
  EXPECT_EQ(probe_config->path, abs_file_path);
  EXPECT_FALSE(probe_config->config.DictEmpty());
  EXPECT_EQ(probe_config->sha1_hash,
            "0B6621DE5CDB0F805E614F19CAA6C38104F1F178");
}

TEST_F(ProbeConfigLoaderImplTest, GetDefaultPaths_WithoutCrosDebug) {
  const char model_name[] = "ModelFoo";
  SetCrosDebugFlag(0);
  SetModel(model_name);
  const auto default_paths = probe_config_loader_->GetDefaultPaths();
  EXPECT_THAT(default_paths, ::testing::ElementsAreArray({
                                 GetRootDir()
                                     .Append(kRuntimeProbeConfigDir)
                                     .Append(model_name)
                                     .Append(kRuntimeProbeConfigName),
                                 GetRootDir()
                                     .Append(kRuntimeProbeConfigDir)
                                     .Append(kRuntimeProbeConfigName),
                             }));
}

TEST_F(ProbeConfigLoaderImplTest, GetDefaultPaths_WithCrosDebug) {
  const char model_name[] = "ModelFoo";
  SetCrosDebugFlag(1);
  SetModel(model_name);
  const auto default_paths = probe_config_loader_->GetDefaultPaths();
  EXPECT_THAT(default_paths, ::testing::ElementsAreArray({
                                 GetRootDir()
                                     .Append(kUsrLocal)
                                     .Append(kRuntimeProbeConfigDir)
                                     .Append(model_name)
                                     .Append(kRuntimeProbeConfigName),
                                 GetRootDir()
                                     .Append(kUsrLocal)
                                     .Append(kRuntimeProbeConfigDir)
                                     .Append(kRuntimeProbeConfigName),
                                 GetRootDir()
                                     .Append(kRuntimeProbeConfigDir)
                                     .Append(model_name)
                                     .Append(kRuntimeProbeConfigName),
                                 GetRootDir()
                                     .Append(kRuntimeProbeConfigDir)
                                     .Append(kRuntimeProbeConfigName),
                             }));
}

TEST_F(ProbeConfigLoaderImplTest, LoadDefault_WithoutCrosDebug) {
  const char model_name[] = "ModelFoo";
  SetCrosDebugFlag(0);
  SetModel(model_name);
  const base::FilePath config_a_path{"probe_config.json"};
  const base::FilePath config_b_path{"probe_config_b.json"};
  const base::FilePath rootfs_config_path =
      GetRootDir().Append(kRuntimeProbeConfigDir);
  const base::FilePath stateful_partition_config_path =
      GetRootDir().Append(kUsrLocal).Append(kRuntimeProbeConfigDir);

  // Copy config_a to rootfs.
  CreateDirectoryAndCopyFile(
      testdata_root_.Append(config_a_path),
      rootfs_config_path.Append(model_name).Append(kRuntimeProbeConfigName));
  // Copy config_b to stateful partition.
  CreateDirectoryAndCopyFile(testdata_root_.Append(config_b_path),
                             stateful_partition_config_path.Append(model_name)
                                 .Append(kRuntimeProbeConfigName));

  const auto probe_config = probe_config_loader_->LoadDefault();
  EXPECT_TRUE(probe_config);
  EXPECT_EQ(
      probe_config->path,
      rootfs_config_path.Append(model_name).Append(kRuntimeProbeConfigName));
  EXPECT_FALSE(probe_config->config.DictEmpty());
  EXPECT_EQ(probe_config->sha1_hash,
            "0B6621DE5CDB0F805E614F19CAA6C38104F1F178");
}

TEST_F(ProbeConfigLoaderImplTest, LoadDefault_WithCrosDebug) {
  const char model_name[] = "ModelFoo";
  SetCrosDebugFlag(1);
  SetModel(model_name);
  const base::FilePath config_a_path{"probe_config.json"};
  const base::FilePath config_b_path{"probe_config_b.json"};
  const base::FilePath rootfs_config_path =
      GetRootDir().Append(kRuntimeProbeConfigDir);
  const base::FilePath stateful_partition_config_path =
      GetRootDir().Append(kUsrLocal).Append(kRuntimeProbeConfigDir);

  // Copy config_a to rootfs.
  CreateDirectoryAndCopyFile(
      testdata_root_.Append(config_a_path),
      rootfs_config_path.Append(model_name).Append(kRuntimeProbeConfigName));
  // Copy config_b to stateful partition.
  CreateDirectoryAndCopyFile(testdata_root_.Append(config_b_path),
                             stateful_partition_config_path.Append(model_name)
                                 .Append(kRuntimeProbeConfigName));

  const auto probe_config = probe_config_loader_->LoadDefault();
  EXPECT_TRUE(probe_config);
  EXPECT_EQ(probe_config->path,
            stateful_partition_config_path.Append(model_name)
                .Append(kRuntimeProbeConfigName));
  EXPECT_FALSE(probe_config->config.DictEmpty());
  EXPECT_EQ(probe_config->sha1_hash,
            "BC65881109108FB248B76554378AC493CD4D5C6D");
}

TEST_F(ProbeConfigLoaderImplTest, LoadDefault_MissingFile) {
  const char model_name[] = "ModelFoo";
  SetCrosDebugFlag(0);
  SetModel(model_name);

  const auto probe_config = probe_config_loader_->LoadDefault();
  EXPECT_FALSE(probe_config);
}

}  // namespace runtime_probe
