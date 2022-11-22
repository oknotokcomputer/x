// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ARC_VM_DATA_MIGRATOR_ARCVM_DATA_MIGRATION_HELPER_DELEGATE_H_
#define ARC_VM_DATA_MIGRATOR_ARCVM_DATA_MIGRATION_HELPER_DELEGATE_H_

#include <memory>
#include <string>

#include <base/files/file.h>
#include <base/files/file_path.h>
#include <cryptohome/data_migrator/migration_helper_delegate.h>

#include "arc/vm/data_migrator/metrics.h"

namespace arc::data_migrator {

// Delegate class for cryptohome::data_migrator::MigrationHelper that implements
// logic specific to ARCVM /data migration.
class ArcVmDataMigrationHelperDelegate
    : public cryptohome::data_migrator::MigrationHelperDelegate {
 public:
  explicit ArcVmDataMigrationHelperDelegate(ArcVmDataMigratorMetrics* metrics);
  ~ArcVmDataMigrationHelperDelegate() override;

  ArcVmDataMigrationHelperDelegate(const ArcVmDataMigrationHelperDelegate&) =
      delete;
  ArcVmDataMigrationHelperDelegate& operator=(
      const ArcVmDataMigrationHelperDelegate&) = delete;

  // cryptohome::data_migrator::MigrationHelperDelegate overrides:
  bool ShouldCopyQuotaProjectId() override;
  std::string GetMtimeXattrName() override;
  std::string GetAtimeXattrName() override;
  bool ConvertFileMetadata(base::stat_wrapper_t* stat) override;
  std::string ConvertXattrName(const std::string& name) override;
  void ReportStartTime() override;
  void ReportEndTime() override;
  void ReportStartStatus(
      cryptohome::data_migrator::MigrationStartStatus status) override;
  void ReportEndStatus(
      cryptohome::data_migrator::MigrationEndStatus status) override;
  void ReportTotalSize(int total_byte_count_mb, int total_file_count) override;
  void ReportFailure(
      base::File::Error error_code,
      cryptohome::data_migrator::MigrationFailedOperationType type,
      const base::FilePath& path,
      cryptohome::data_migrator::FailureLocationType location_type) override;
  void ReportFailedNoSpace(int initial_free_space_mb,
                           int failure_free_space_mb) override;
  void ReportFailedNoSpaceXattrSizeInBytes(int total_xattr_size_bytes) override;

 private:
  // Owned by arc::data_migrator::DBusAdaptor.
  ArcVmDataMigratorMetrics* const metrics_;

  // Records the time ReportStartTime() was called.
  base::TimeTicks migration_start_time_;
};

}  // namespace arc::data_migrator

#endif  // ARC_VM_DATA_MIGRATOR_ARCVM_DATA_MIGRATION_HELPER_DELEGATE_H_
