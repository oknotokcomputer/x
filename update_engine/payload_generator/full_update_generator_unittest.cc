// Copyright 2010 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "update_engine/payload_generator/full_update_generator.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "update_engine/common/test_utils.h"
#include "update_engine/payload_consumer/payload_constants.h"
#include "update_engine/payload_generator/extent_utils.h"

using chromeos_update_engine::test_utils::FillWithData;
using std::string;
using std::vector;

namespace chromeos_update_engine {

class FullUpdateGeneratorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.is_delta = false;
    config_.version.minor = kFullPayloadMinorVersion;
    config_.hard_chunk_size = 128 * 1024;
    config_.block_size = 4096;

    new_part_conf.path = part_file_.path();

    blob_file_writer_.reset(
        new BlobFileWriter(blob_file_.fd(), &out_blobs_length_));
  }

  PayloadGenerationConfig config_;
  PartitionConfig new_part_conf{"part"};

  vector<AnnotatedOperation> aops;

  // Output file holding the payload blobs.
  off_t out_blobs_length_{0};
  ScopedTempFile part_file_{"FullUpdateTest_partition.XXXXXX"};

  ScopedTempFile blob_file_{"FullUpdateTest_blobs.XXXXXX", true};
  std::unique_ptr<BlobFileWriter> blob_file_writer_;

  // FullUpdateGenerator under test.
  FullUpdateGenerator generator_;
};

TEST_F(FullUpdateGeneratorTest, RunTest) {
  brillo::Blob new_part(9 * 1024 * 1024);
  FillWithData(&new_part);
  new_part_conf.size = new_part.size();

  EXPECT_TRUE(test_utils::WriteFileVector(new_part_conf.path, new_part));

  EXPECT_TRUE(generator_.GenerateOperations(config_,
                                            new_part_conf,  // this is ignored
                                            new_part_conf,
                                            blob_file_writer_.get(), &aops));
  int64_t new_part_chunks = new_part_conf.size / config_.hard_chunk_size;
  EXPECT_EQ(new_part_chunks, static_cast<int64_t>(aops.size()));
  for (off_t i = 0; i < new_part_chunks; ++i) {
    EXPECT_EQ(1, aops[i].op.dst_extents_size());
    EXPECT_EQ(
        static_cast<uint64_t>(i * config_.hard_chunk_size / config_.block_size),
        aops[i].op.dst_extents(0).start_block())
        << "i = " << i;
    EXPECT_EQ(config_.hard_chunk_size / config_.block_size,
              aops[i].op.dst_extents(0).num_blocks());
    if (aops[i].op.type() != InstallOperation::REPLACE) {
      EXPECT_EQ(InstallOperation::REPLACE_XZ, aops[i].op.type());
    }
  }
}

// Test that if the chunk size is not a divisor of the image size, it handles
// correctly the last chunk of the partition.
TEST_F(FullUpdateGeneratorTest, ChunkSizeTooBig) {
  config_.hard_chunk_size = 1024 * 1024;
  config_.soft_chunk_size = config_.hard_chunk_size;
  brillo::Blob new_part(1536 * 1024);  // 1.5 MiB
  new_part_conf.size = new_part.size();

  EXPECT_TRUE(test_utils::WriteFileVector(new_part_conf.path, new_part));

  EXPECT_TRUE(generator_.GenerateOperations(config_,
                                            new_part_conf,  // this is ignored
                                            new_part_conf,
                                            blob_file_writer_.get(), &aops));
  // new_part has one chunk and a half.
  EXPECT_EQ(2U, aops.size());
  EXPECT_EQ(config_.hard_chunk_size / config_.block_size,
            utils::BlocksInExtents(aops[0].op.dst_extents()));
  EXPECT_EQ((new_part.size() - config_.hard_chunk_size) / config_.block_size,
            utils::BlocksInExtents(aops[1].op.dst_extents()));
}

// Test that if the image size is much smaller than the chunk size, it handles
// correctly the only chunk of the partition.
TEST_F(FullUpdateGeneratorTest, ImageSizeTooSmall) {
  brillo::Blob new_part(16 * 1024);
  new_part_conf.size = new_part.size();

  EXPECT_TRUE(test_utils::WriteFileVector(new_part_conf.path, new_part));

  EXPECT_TRUE(generator_.GenerateOperations(config_,
                                            new_part_conf,  // this is ignored
                                            new_part_conf,
                                            blob_file_writer_.get(), &aops));

  // new_part has less than one chunk.
  EXPECT_EQ(1U, aops.size());
  EXPECT_EQ(new_part.size() / config_.block_size,
            utils::BlocksInExtents(aops[0].op.dst_extents()));
}

}  // namespace chromeos_update_engine
