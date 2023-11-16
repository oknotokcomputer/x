// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "thinpool_migrator/lvm_metadata.h"

#include <cinttypes>
#include <string>

#include <base/rand_util.h>
#include <base/strings/stringprintf.h>

namespace thinpool_migrator {
namespace {

// Metadata for a volume group. This mimics the format reported by vgcfgbackup.
// The generated metadata can be setup onto a device via vgcfgrestore.
constexpr char kVolumeGroupMetadataTemplate[] = R"(
contents = "Text Format Volume Group"
version = 1
description = "Generated by thinpool_migrator"
creation_host = "localhost"
creation_time = %ld
%s {
  id = "%s"
  seqno = %)" PRIu64 R"(
  format = "%s"
  status = %s
  flags = %s
  extent_size = %)" PRIu64 R"(
  max_lv = %)" PRIu64 R"(
  max_pv = %)" PRIu64 R"(
  metadata_copies = %)" PRIu64 R"(
  physical_volumes {
%s
  }
  logical_volumes {
%s
  }
})";

// Each volume group's metadata contains a list of physical volumes associated
// with it.
constexpr char kPhysicalVolumeMetadataTemplate[] = R"(
  pv%u {
    id = "%s"
    device = %s
    status = %s
    flags = %s
    dev_size = %)" PRIu64 R"(
    pe_start = %)" PRIu64 R"(
    pe_count = %)" PRIu64 R"(
  })";

// Volume groups also contain a description of all associated logical volumes.
constexpr char kLogicalVolumeMetadataTemplate[] = R"(
    %s {
    id = "%s"
    status = %s
    flags = %s
    creation_time = %ld
    creation_host = "localhost"
    segment_count = %zu
%s
    })";

// Each logical volume comprises of segments, which may be of the following
// types:
//
// 1. Striped: 1:1 mapping to underlying device(s) via dm-linear
//             A 'striped' logical volume can comprise of multiple
//             stripes spanning different physical volumes.
// 2. Thin-pool: Thin provisioning layer via dm-thin-pool.
//               Each thin-pool, in turn, references two logical volumes for
//               data and metadata storage.
// 3. Thin: Thinly provisioned logical volume backed by a thinpool.
constexpr char kLvSegmentMetadataTemplate[] = R"(
      segment%u {
        start_extent = %)" PRIu64 R"(
        extent_count = %)" PRIu64 R"(
        type = "%s"
%s
      })";

constexpr char kLvCompThinpoolMetadataTemplate[] = R"(
        metadata = "%s"
        pool = "%s"
        transaction_id = %)" PRIu64 R"(
        chunk_size = %)" PRIu64 R"(
        discards = "%s"
        zero_new_blocks = %)" PRIu64;

constexpr char kLvCompThinMetadataTemplate[] = R"(
        thin_pool = "%s"
        transaction_id = %)" PRIu64 R"(
        device_id = %)" PRIu64;

// Thinpool mapping metadata templates describe each thinly provisioned device's
// logical-to-physical mapping. thin-provisioning-tools provides a
// human-readable XML format for modifying thinpool metadata.
//
//
// The 'thinpool metadata' metadata comprises of:
// 1. A superblock which describes the necessary thinpool metadata.
// 2. Device specific metadata, which is keyed using device ids.
// 3. Range/single block mappings per device that represent the l2p mapping.
constexpr char kThinpoolSuperblockTemplate[] =
    R"(<superblock uuid="%s" time="%ld" transaction="%)" PRIu64
    R"(" flags="%)" PRIu64 R"(" version="%)" PRIu64
    R"(" data_block_size="%)" PRIu64 R"(" nr_data_blocks="%)" PRIu64 R"(">
%s
    </superblock>)";

constexpr char kThinDeviceMappingTemplate[] =
    R"(<device dev_id="%)" PRIu64 R"(" mapped_blocks="%)" PRIu64
    R"(" transaction="%)" PRIu64
    R"(" creation_time="%ld" snap_time="%ld">
%s
</device>)";

constexpr char kThinBlockRangeMappingTemplate[] =
    R"(  <range_mapping origin_begin="%)" PRIu64 R"(" data_begin="%)" PRIu64
    R"(" length="%)" PRIu64 R"(" time="%ld"/>)";

constexpr char kThinBlockSingleMappingTemplate[] =
    R"(<single_mapping origin_block="%)" PRIu64 R"(" data_block="%)" PRIu64
    R"(" time="%ld"/>)";

std::string GenerateRandomAlphanumString(int size) {
  constexpr const char kCharset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ\0";
  std::string random_string(size, '0');

  for (int i = 0; i < size; ++i) {
    random_string[i] = kCharset[base::RandInt(0, strlen(kCharset) - 1)];
  }
  return random_string;
}

}  // namespace

std::string GenerateLvmDeviceId() {
  return GenerateRandomAlphanumString(6) + '-' +
         GenerateRandomAlphanumString(4) + '-' +
         GenerateRandomAlphanumString(4) + '-' +
         GenerateRandomAlphanumString(4) + '-' +
         GenerateRandomAlphanumString(4) + '-' +
         GenerateRandomAlphanumString(4) + '-' +
         GenerateRandomAlphanumString(6);
}

std::string GenerateVolumeGroupName() {
  return GenerateRandomAlphanumString(16);
}

// Physical volumes are unnamed in metadata and referred to as pv0, pv1...
std::string PhysicalVolumeMetadata::ToString(int num) const {
  return base::StringPrintf(kPhysicalVolumeMetadataTemplate, num, id.c_str(),
                            device.c_str(), status.c_str(), flags.c_str(),
                            dev_size, pe_start, pe_count);
}

std::string LogicalVolumeSegment::ToString(int num) const {
  std::string segment_specific_data;

  if (type == "striped") {
    segment_specific_data.append(
        base::StringPrintf("stripe_count = %zu\n", stripe.stripes.size()));
    segment_specific_data.append("stripes = [\n");
    for (const auto& i : stripe.stripes) {
      segment_specific_data.append(base::StringPrintf(
          "\"%s\", %" PRIu64 "\n", i.first.c_str(), i.second));
    }
    segment_specific_data.append("]\n");
  } else if (type == "thin-pool") {
    segment_specific_data.append(base::StringPrintf(
        kLvCompThinpoolMetadataTemplate, thinpool.metadata.c_str(),
        thinpool.pool.c_str(), thinpool.transaction_id, thinpool.chunk_size,
        thinpool.discards.c_str(), thinpool.zero_new_blocks));
  } else if (type == "thin") {
    segment_specific_data.append(
        base::StringPrintf(kLvCompThinMetadataTemplate, thin.thin_pool.c_str(),
                           thin.transaction_id, thin.device_id));
  }

  return base::StringPrintf(kLvSegmentMetadataTemplate, num, start_extent,
                            extent_count, type.c_str(),
                            segment_specific_data.c_str());
}

std::string LogicalVolumeMetadata::GetCollatedSegments() const {
  std::string lv_segment_metadata;
  int lv_idx = 1;
  for (const auto& segment : segments) {
    lv_segment_metadata.append(segment.ToString(lv_idx++));
  }
  return lv_segment_metadata;
}

std::string LogicalVolumeMetadata::ToString() const {
  return base::StringPrintf(kLogicalVolumeMetadataTemplate, name.c_str(),
                            id.c_str(), status.c_str(), flags.c_str(),
                            creation_time, segments.size(),
                            GetCollatedSegments().c_str());
}

std::string VolumeGroupMetadata::GetCollatedPvMetadata() const {
  std::string physical_volume_metadata;
  int pv_count = 0;
  for (const auto& pv : pv_metadata) {
    physical_volume_metadata.append(pv.ToString(pv_count++));
  }
  return physical_volume_metadata;
}

std::string VolumeGroupMetadata::GetCollatedLvMetadata() const {
  std::string logical_volume_metadata;
  for (const auto& lv : lv_metadata) {
    logical_volume_metadata.append(lv.ToString());
  }
  return logical_volume_metadata;
}

std::string VolumeGroupMetadata::ToString() const {
  return base::StringPrintf(
      kVolumeGroupMetadataTemplate, creation_time, name.c_str(), id.c_str(),
      seqno, format.c_str(), status.c_str(), flags.c_str(), extent_size, max_lv,
      max_pv, metadata_copies, GetCollatedPvMetadata().c_str(),
      GetCollatedLvMetadata().c_str());
}

std::string ThinBlockMapping::ToString() const {
  if (type == "range") {
    return base::StringPrintf(
        kThinBlockRangeMappingTemplate, mapping.range.origin_begin,
        mapping.range.data_begin, mapping.range.length, time);
  } else {
    return base::StringPrintf(kThinBlockSingleMappingTemplate,
                              mapping.single.origin_block,
                              mapping.single.data_block, time);
  }
}

std::string ThinDeviceMapping::ToString() const {
  std::string device_block_mappings;
  for (auto& block : mappings) {
    device_block_mappings.append(block.ToString());
  }

  return base::StringPrintf(kThinDeviceMappingTemplate, device_id,
                            mapped_blocks, transaction, creation_time,
                            snap_time, device_block_mappings.c_str());
}

std::string ThinpoolSuperblockMetadata::ToString() const {
  std::string thin_device_mappings;

  for (const auto& device : device_mappings) {
    thin_device_mappings.append(device.ToString());
  }

  return base::StringPrintf(kThinpoolSuperblockTemplate, uuid.c_str(), time,
                            transaction, flags, version, data_block_size,
                            nr_data_blocks, thin_device_mappings.c_str());
}

}  // namespace thinpool_migrator
