// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ARC_VM_VSOCK_PROXY_PIPE_STREAM_H_
#define ARC_VM_VSOCK_PROXY_PIPE_STREAM_H_

#include "arc/vm/vsock_proxy/stream_base.h"

#include <base/files/scoped_file.h>
#include <base/macros.h>

namespace arc {

// Wrapper of pipe file descriptor to support reading and writing
// Message protocol buffer.
class PipeStream : public StreamBase {
 public:
  explicit PipeStream(base::ScopedFD pipe_fd);
  ~PipeStream() override;

  // StreamBase overrides:
  base::Optional<arc_proxy::Message> Read() override;
  bool Write(arc_proxy::Message message) override;

 private:
  base::ScopedFD pipe_fd_;

  DISALLOW_COPY_AND_ASSIGN(PipeStream);
};

}  // namespace arc

#endif  // ARC_VM_VSOCK_PROXY_PIPE_STREAM_H_
