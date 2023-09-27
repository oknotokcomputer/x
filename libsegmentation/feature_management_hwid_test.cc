// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <utility>

#include <base/files/scoped_temp_dir.h>
#include <base/logging.h>
#include <brillo/data_encoding.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <google/protobuf/message.h>
#include <google/protobuf/text_format.h>

#include <libcrossystem/crossystem_fake.h>

#include "libsegmentation/device_info.pb.h"
#include "libsegmentation/feature_management_hwid.h"

#include "proto/feature_management.pb.h"

namespace segmentation {

using chromiumos::feature_management::api::software::DeviceSelection;
using chromiumos::feature_management::api::software::SelectionBundle;

using ::testing::Return;

using ::google::protobuf::TextFormat;

// Test fixture for testing feature management.
class FeatureManagementHwidTest : public ::testing::Test {
 public:
  FeatureManagementHwidTest() = default;
  ~FeatureManagementHwidTest() override = default;
};

TEST_F(FeatureManagementHwidTest, GetBasicSelection) {
  // Test with a simple file,no hwid_profiles.
  const char* selection_device_proto = R""""(
selections {
  feature_level: 1
  scope: SCOPE_DEVICES_1
  hwid_profiles {
    prefixes: "marasov-AA"
    prefixes: "marasov-AB"
  }
}
selections {
  feature_level: 2
  scope: SCOPE_DEVICES_1
  hwid_profiles {
    prefixes: "marasov-AC"
  }
  hwid_profiles {
    prefixes: "marasov-AD"
  }
}
)"""";
  SelectionBundle selection_bundle;
  bool status =
      TextFormat::ParseFromString(selection_device_proto, &selection_bundle);
  EXPECT_TRUE(status);
  EXPECT_EQ(selection_bundle.selections().size(), 2);

  const char* marasov_aa_hwid = "marasov-AA E2A";
  std::optional<DeviceSelection> marasov_aa =
      FeatureManagementHwid::GetSelectionFromHWID(selection_bundle,
                                                  marasov_aa_hwid, true);
  EXPECT_TRUE(marasov_aa);

  DeviceSelection marasov_aa_exp;
  marasov_aa_exp.set_feature_level(1);
  EXPECT_TRUE(marasov_aa->feature_level() == marasov_aa_exp.feature_level());

  const char* marasov_ad_hwid = "marasov-AD E2A";
  std::optional<DeviceSelection> marasov_ad =
      FeatureManagementHwid::GetSelectionFromHWID(selection_bundle,
                                                  marasov_ad_hwid, false);
  EXPECT_TRUE(marasov_ad);

  DeviceSelection marasov_ad_exp;
  marasov_ad_exp.set_feature_level(2);
  EXPECT_TRUE(marasov_ad->feature_level() == marasov_ad_exp.feature_level());

  const char* marasov_ae_hwid = "marasov-AE E2A";
  std::optional<DeviceSelection> marasov_ae =
      FeatureManagementHwid::GetSelectionFromHWID(selection_bundle,
                                                  marasov_ae_hwid, false);
  EXPECT_FALSE(marasov_ae);
}

TEST_F(FeatureManagementHwidTest, GetHwidProfileSelection) {
  // Test with hwid_profiles: D3B is 00011 001 00001 10000 101 01010"
  // Look for bit 3: 1, 4: 1, 11: 0, 12: 0 and bit 13: 1
  // Then bit 14 is the End Of String and 10101010 the dummy CRC.
  // There are not bit 13 in D3A, but the code should append a 0,
  // so D3A should match.
  const char* selection_device_proto = R""""(
selections {
  feature_level: 1
  scope: SCOPE_DEVICES_1
  hwid_profiles {
    prefixes: "marasov-AA"
    encoding_requirements {
      bit_locations: 3
      bit_locations: 4
      bit_locations: 11
      bit_locations: 12
      bit_locations: 13
      required_values: "11000"
      required_values: "10000"
    }
  }
}
)"""";
  SelectionBundle selection_bundle;
  bool status =
      TextFormat::ParseFromString(selection_device_proto, &selection_bundle);
  EXPECT_TRUE(status);
  EXPECT_EQ(selection_bundle.selections().size(), 1);

  const char* marasov_aa5_hwid = "marasov-AA D3A-Q7K";
  std::optional<DeviceSelection> marasov_aa5 =
      FeatureManagementHwid::GetSelectionFromHWID(selection_bundle,
                                                  marasov_aa5_hwid, false);
  EXPECT_TRUE(marasov_aa5);

  DeviceSelection marasov_aa5_exp;
  marasov_aa5_exp.set_feature_level(1);
  EXPECT_TRUE(marasov_aa5->feature_level() == marasov_aa5_exp.feature_level());

  const char* marasov_aa7_hwid = "marasov-AA D3B-Q7K";
  std::optional<DeviceSelection> marasov_aa7 =
      FeatureManagementHwid::GetSelectionFromHWID(selection_bundle,
                                                  marasov_aa7_hwid, false);
  EXPECT_FALSE(marasov_aa7);
  std::optional<DeviceSelection> marasov_aa7_prefix_only =
      FeatureManagementHwid::GetSelectionFromHWID(selection_bundle,
                                                  marasov_aa7_hwid, true);
  EXPECT_TRUE(marasov_aa7_prefix_only);
}

}  // namespace segmentation
