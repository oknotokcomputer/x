// Copyright 2019 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vm_tools/concierge/untrusted_vm_utils.h"

#include <memory>
#include <string>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/files/scoped_temp_dir.h>
#include <gtest/gtest.h>

#include "vm_tools/concierge/vm_util.h"

namespace vm_tools {
namespace concierge {

namespace {

// Test fixture for actually testing the VirtualMachine functionality.
class UntrustedVMUtilsTest : public ::testing::Test {
 public:
  UntrustedVMUtilsTest() = default;
  ~UntrustedVMUtilsTest() override = default;

  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    l1tf_status_path_ = temp_dir_.GetPath().Append("l1tf");
    mds_status_path_ = temp_dir_.GetPath().Append("mds");

    // By default make MDS and l1tf passing, individual tests can set them to
    // fail.
    SetMDSStatus("Mitigation: Clear CPU buffers; SMT disabled");
    SetL1TFStatus(
        "Mitigation: PTE Inversion; VMX: cache flushes, SMT "
        "disabled");
  }

 protected:
  // Checks if |l1tf_status| yields |expected_status| when
  // |CheckUntrustedVMMitigationStatus| is called.
  void SetL1TFStatus(const std::string& l1tf_status) {
    ASSERT_EQ(base::WriteFile(l1tf_status_path_, l1tf_status.c_str(),
                              l1tf_status.size()),
              l1tf_status.size());
  }

  // Checks if |mds_status| yields |expected_status| when
  // |CheckUntrustedVMMitigationStatus| is called.
  void SetMDSStatus(const std::string& mds_status) {
    ASSERT_EQ(base::WriteFile(mds_status_path_, mds_status.c_str(),
                              mds_status.size()),
              mds_status.size());
  }

  class FakeUntrustedVMUtils : public UntrustedVMUtils {
   public:
    FakeUntrustedVMUtils(base::FilePath l1tf_status_path,
                         base::FilePath mds_status_path)
        : UntrustedVMUtils(std::move(l1tf_status_path),
                           std::move(mds_status_path)) {}
    ~FakeUntrustedVMUtils() override = default;
  };

  // Directory and file path used for reading test vulnerability statuses.
  base::ScopedTempDir temp_dir_;
  base::FilePath l1tf_status_path_;
  base::FilePath mds_status_path_;
};

}  // anonymous namespace

// Checks mitigation status for all L1TF statuses.
TEST_F(UntrustedVMUtilsTest, CheckL1TFStatus) {
  LOG(ERROR) << l1tf_status_path_;
  FakeUntrustedVMUtils utils(l1tf_status_path_, mds_status_path_);

  SetL1TFStatus("Not affected");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::NOT_VULNERABLE);

  SetL1TFStatus("Mitigation: PTE Inversion");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::NOT_VULNERABLE);

  SetL1TFStatus("Some gibberish; some more gibberish");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::VULNERABLE);

  SetL1TFStatus(
      "Mitigation: PTE Inversion; VMX: conditional cache flushes, SMT "
      "vulnerable");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::VULNERABLE);

  SetL1TFStatus(
      "Mitigation: PTE Inversion; VMX: cache flushes, SMT vulnerable");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::VULNERABLE_DUE_TO_SMT_ENABLED);

  SetL1TFStatus("Mitigation: PTE Inversion; VMX: cache flushes, SMT disabled");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::NOT_VULNERABLE);

  SetL1TFStatus(
      "Mitigation: PTE Inversion; VMX: flush not necessary, SMT disabled");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::NOT_VULNERABLE);
}

// Checks mitigation status for all MDS statuses.
TEST_F(UntrustedVMUtilsTest, CheckMDSStatus) {
  FakeUntrustedVMUtils utils(l1tf_status_path_, mds_status_path_);

  SetMDSStatus("Not affected");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::NOT_VULNERABLE);

  SetMDSStatus("Some gibberish; some more gibberish");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::VULNERABLE);

  SetMDSStatus("Vulnerable: Clear CPU buffers attempted, no microcode");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::VULNERABLE);

  SetMDSStatus(
      "Vulnerable: Clear CPU buffers attempted, no microcode; SMT enabled");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::VULNERABLE);

  SetMDSStatus("Vulnerable; SMT disabled");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::VULNERABLE);

  SetMDSStatus("Mitigation: Clear CPU buffers; SMT disabled");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::NOT_VULNERABLE);

  SetMDSStatus("Mitigation: Clear CPU buffers; SMT mitigated");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::VULNERABLE_DUE_TO_SMT_ENABLED);

  SetMDSStatus("Mitigation: Clear CPU buffers; SMT vulnerable");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::VULNERABLE_DUE_TO_SMT_ENABLED);

  SetMDSStatus("Mitigation: Clear CPU buffers; SMT Host state unknown");
  EXPECT_EQ(utils.CheckUntrustedVMMitigationStatus(),
            UntrustedVMUtils::MitigationStatus::VULNERABLE_DUE_TO_SMT_ENABLED);
}

TEST_F(UntrustedVMUtilsTest, SafeToRunVirtualMachines) {
  std::string reason;

  FakeUntrustedVMUtils new_kernel_utils(l1tf_status_path_, mds_status_path_);
  EXPECT_TRUE(new_kernel_utils.SafeToRunVirtualMachines(&reason))
      << "CPU has appropriate security mitigations";

  // Set the status to unmitigated.
  SetMDSStatus("foo");
  SetL1TFStatus("bar");

  EXPECT_FALSE(new_kernel_utils.SafeToRunVirtualMachines(&reason))
      << "CPU vulnerabilities are not mitigated";
}

}  // namespace concierge
}  // namespace vm_tools
