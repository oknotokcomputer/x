/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CAMERA_INCLUDE_CROS_CAMERA_IPC_UTIL_H_
#define CAMERA_INCLUDE_CROS_CAMERA_IPC_UTIL_H_

#include <string>

#include <base/files/scoped_file.h>
#include <mojo/edk/embedder/embedder.h>

#include "cros-camera/export.h"

namespace base {
class FilePath;
}  // namespace base

namespace cros {

CROS_CAMERA_EXPORT bool CreateServerUnixDomainSocket(
    const base::FilePath& socket_path, int* server_listen_fd);

CROS_CAMERA_EXPORT bool ServerAcceptConnection(int server_listen_fd,
                                               int* server_socket);

CROS_CAMERA_EXPORT base::ScopedFD CreateClientUnixDomainSocket(
    const base::FilePath& socket_path);

CROS_CAMERA_EXPORT MojoResult CreateMojoChannelToParentByUnixDomainSocket(
    const base::FilePath& socket_path,
    mojo::ScopedMessagePipeHandle* child_pipe);

CROS_CAMERA_EXPORT MojoResult CreateMojoChannelToChildByUnixDomainSocket(
    const base::FilePath& socket_path,
    mojo::ScopedMessagePipeHandle* parent_pipe);

CROS_CAMERA_EXPORT mojo::ScopedHandle WrapPlatformHandle(int handle);
CROS_CAMERA_EXPORT int UnwrapPlatformHandle(mojo::ScopedHandle handle);

}  // namespace cros

#endif  // CAMERA_INCLUDE_CROS_CAMERA_IPC_UTIL_H_
