// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <midis/client.h>

#include <sys/socket.h>

#include <utility>

#include <base/bind.h>
#include <base/memory/ptr_util.h>
#include <base/posix/eintr_wrapper.h>
#include <mojo/edk/embedder/embedder.h>
#include <mojo/public/cpp/system/handle.h>

#include "midis/constants.h"

namespace midis {

Client::Client(DeviceTracker* device_tracker,
               uint32_t client_id,
               ClientDeletionCallback del_cb,
               arc::mojom::MidisServerRequest request,
               arc::mojom::MidisClientPtr client_ptr)
    : device_tracker_(device_tracker),
      client_id_(client_id),
      del_cb_(del_cb),
      client_ptr_(std::move(client_ptr)),
      binding_(this, std::move(request)),
      weak_factory_(this) {
  device_tracker_->AddDeviceObserver(this);
}

Client::~Client() {
  LOG(INFO) << "Deleting client: " << client_id_;
  device_tracker_->RemoveDeviceObserver(this);
}

void Client::TriggerClientDeletion() {
  brillo::MessageLoop::TaskId ret_id = brillo::MessageLoop::current()->PostTask(
      FROM_HERE, base::Bind(del_cb_, client_id_));
  if (ret_id == brillo::MessageLoop::kTaskIdNull) {
    LOG(ERROR) << "Couldn't schedule the client deletion callback!";
  }
}

void Client::OnDeviceAddedOrRemoved(const Device& dev, bool added) {
  arc::mojom::MidisDeviceInfoPtr dev_info = arc::mojom::MidisDeviceInfo::New();
  dev_info->card = dev.GetCard();
  dev_info->device_num = dev.GetDeviceNum();
  dev_info->num_subdevices = dev.GetNumSubdevices();
  dev_info->name = dev.GetName();
  dev_info->manufacturer = dev.GetManufacturer();
  if (added) {
    client_ptr_->OnDeviceAdded(std::move(dev_info));
  } else {
    client_ptr_->OnDeviceRemoved(std::move(dev_info));
  }
}

void Client::ListDevices(const ListDevicesCallback& callback) {
  // Get all the device information from device_tracker.
  mojo::Array<arc::mojom::MidisDeviceInfoPtr> device_list;
  device_tracker_->ListDevices(&device_list);
  callback.Run(std::move(device_list));
}

void Client::RequestPort(arc::mojom::MidisRequestPtr request,
                         const RequestPortCallback& callback) {
  base::ScopedFD clientfd = device_tracker_->AddClientToReadSubdevice(
      request->card, request->device_num, request->subdevice_num, client_id_);
  if (!clientfd.is_valid()) {
    LOG(ERROR) << "AddClientToReadSubdevice failed.";
    // We don't delete the client here, because this could mean an issue with
    // the device h/w.
    callback.Run(mojo::ScopedHandle());
    return;
  }

  mojo::edk::ScopedPlatformHandle platform_handle{
      mojo::edk::PlatformHandle(clientfd.release())};
  MojoHandle wrapped_handle;
  MojoResult wrap_result = mojo::edk::CreatePlatformHandleWrapper(
      std::move(platform_handle), &wrapped_handle);
  if (wrap_result != MOJO_RESULT_OK) {
    LOG(ERROR) << "Failed to wrap port FD in a Mojo Handle.";
    callback.Run(mojo::ScopedHandle());
    return;
  }

  mojo::ScopedHandle scoped_handle{mojo::Handle(wrapped_handle)};

  callback.Run(std::move(scoped_handle));
  DVLOG(1) << "Converted port into Mojo scoped handle successfully.";
}

void Client::CloseDevice(arc::mojom::MidisRequestPtr request) {
  device_tracker_->RemoveClientFromDevice(client_id_, request->card,
                                          request->device_num);
}

}  // namespace midis
