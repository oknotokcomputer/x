/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CAMERA_HAL_ADAPTER_CAMERA_HAL_SERVER_IMPL_H_
#define CAMERA_HAL_ADAPTER_CAMERA_HAL_SERVER_IMPL_H_

#include <memory>

#include <base/files/file_path.h>
#include <base/files/file_path_watcher.h>
#include <base/single_thread_task_runner.h>
#include <base/threading/thread_checker.h>
#include <mojo/public/cpp/bindings/binding.h>

#include "hal_adapter/camera_hal_adapter.h"
#include "mojo/cros_camera_service.mojom.h"

namespace cros {

class CameraMojoChannelManager;

// CameraHalServerImpl is the implementation of the CameraHalServer Mojo
// interface.  It hosts the camera HAL v3 adapter and registers itself to the
// CameraHalDispatcher Mojo proxy in started by Chrome.  Camera clients such
// as Chrome VideoCaptureDeviceFactory and Android cameraserver process connect
// to the CameraHalDispatcher to ask for camera service; CameraHalDispatcher
// proxies the service requests to CameraHalServerImpl.
class CameraHalServerImpl final {
 public:
  CameraHalServerImpl();
  ~CameraHalServerImpl();

  // Initializes the threads and start monitoring the unix domain socket file
  // created by Chrome.
  bool Start();

 private:
  // IPCBridge wraps all the IPC-related calls. Most of its methods should/will
  // be run on IPC thread.
  class IPCBridge : public mojom::CameraHalServer {
   public:
    explicit IPCBridge(CameraHalServerImpl* camera_hal_server,
                       CameraMojoChannelManager* camera_mojo_channel_manager);

    ~IPCBridge();

    void RegisterCameraHal(CameraHalAdapter* camera_hal_adapter);

    // CameraHalServer Mojo interface implementation.

    void CreateChannel(mojom::CameraModuleRequest camera_module_request) final;

    void SetTracingEnabled(bool enabled) final;

    // Connection error handler for the Mojo connection to CameraHalDispatcher.
    void OnServiceMojoChannelError();

    // Gets a weak pointer of the IPCBridge. This method can be called on
    // non-IPC thread.
    base::WeakPtr<IPCBridge> GetWeakPtr();

   private:
    CameraHalServerImpl* camera_hal_server_;

    CameraMojoChannelManager* mojo_manager_;

    // The Mojo IPC task runner.
    const scoped_refptr<base::SingleThreadTaskRunner> ipc_task_runner_;

    const scoped_refptr<base::SingleThreadTaskRunner> main_task_runner_;

    CameraHalAdapter* camera_hal_adapter_;

    // The CameraHalServer implementation binding.  All the function calls to
    // |binding_| runs on |ipc_task_runner_|.
    mojo::Binding<mojom::CameraHalServer> binding_;

    base::WeakPtrFactory<IPCBridge> weak_ptr_factory_{this};
  };

  // Callback method for the unix domain socket file change events.  The method
  // will try to establish the Mojo connection to the CameraHalDispatcher
  // started by Chrome.
  void OnSocketFileStatusChange(const base::FilePath& socket_path, bool error);

  // Loads all the camera HAL implementations.
  void LoadCameraHal();

  void ExitOnMainThread(int exit_status);

  // Watches for change events on the unix domain socket file created by Chrome.
  // Upon file change OnScoketFileStatusChange will be called to initiate
  // connection to CameraHalDispatcher.
  base::FilePathWatcher watcher_;

  std::unique_ptr<CameraMojoChannelManager> mojo_manager_;

  // The instance which deals with the IPC-related calls. It should always run
  // and be deleted on IPC thread.
  std::unique_ptr<IPCBridge> ipc_bridge_;

  // The camera HAL adapter instance.  Each call to CreateChannel creates a
  // new Mojo binding in the camera HAL adapter.  Currently the camera HAL
  // adapter serves two clients: Chrome VideoCaptureDeviceFactory and Android
  // cameraserver process.
  std::unique_ptr<CameraHalAdapter> camera_hal_adapter_;

  THREAD_CHECKER(thread_checker_);

  DISALLOW_COPY_AND_ASSIGN(CameraHalServerImpl);
};

}  // namespace cros

#endif  // CAMERA_HAL_ADAPTER_CAMERA_HAL_SERVER_IMPL_H_
