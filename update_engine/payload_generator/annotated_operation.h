// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UPDATE_ENGINE_PAYLOAD_GENERATOR_ANNOTATED_OPERATION_H_
#define UPDATE_ENGINE_PAYLOAD_GENERATOR_ANNOTATED_OPERATION_H_

#include <ostream>  // NOLINT(readability/streams)
#include <string>

#include "update_engine/update_metadata.pb.h"

namespace chromeos_update_engine {

struct AnnotatedOperation {
  // The name given to the operation, for logging and debugging purposes only.
  // This normally includes the path to the file and the chunk used, if any.
  std::string name;

  // The InstallOperation, as defined by the protobuf.
  DeltaArchiveManifest_InstallOperation op;

  // Sets |name| to a human readable representation of a chunk in a file.
  void SetNameFromFileAndChunk(const std::string& filename,
                               off_t chunk_offset, off_t chunk_size);
};

// For logging purposes.
std::ostream& operator<<(std::ostream& os, const AnnotatedOperation& aop);

std::string InstallOperationTypeName(
    DeltaArchiveManifest_InstallOperation_Type op_type);

}  // namespace chromeos_update_engine

#endif  // UPDATE_ENGINE_PAYLOAD_GENERATOR_ANNOTATED_OPERATION_H_
