// Copyright 2020 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lorgnette/sane_client_fake.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

#include <chromeos/dbus/service_constants.h>

#include "lorgnette/constants.h"
#include "lorgnette/dbus_adaptors/org.chromium.lorgnette.Manager.h"
#include "lorgnette/scanner_match.h"

namespace lorgnette {

std::unique_ptr<SaneDevice> SaneClientFake::ConnectToDeviceInternal(
    brillo::ErrorPtr* error,
    SANE_Status* sane_status,
    const std::string& device_name) {
  if (devices_.count(device_name) > 0) {
    auto ptr = std::move(devices_[device_name]);
    devices_.erase(device_name);
    // Put back a fresh copy so the device can be opened again later.
    SetDeviceForName(device_name, ptr->CloneForTesting());
    return ptr;
  }

  brillo::Error::AddTo(error, FROM_HERE, kDbusDomain, kManagerServiceError,
                       "No device");
  if (sane_status) {
    *sane_status = SANE_STATUS_INVAL;
  }
  return nullptr;
}

void SaneClientFake::SetListDevicesResult(bool value) {
  list_devices_result_ = value;
}

void SaneClientFake::AddDeviceListing(const std::string& name,
                                      const std::string& manufacturer,
                                      const std::string& model,
                                      const std::string& type) {
  ScannerInfo info;
  info.set_name(name);
  info.set_manufacturer(manufacturer);
  info.set_model(model);
  info.set_type(type);
  info.set_connection_type(ConnectionTypeForScanner(info));
  info.set_secure(info.connection_type() == lorgnette::CONNECTION_USB);
  info.set_protocol_type(ProtocolTypeForScanner(info));
  info.set_display_name(DisplayNameForScanner(info));
  scanners_.push_back(info);
}

void SaneClientFake::RemoveDeviceListing(const std::string& name) {
  auto it = scanners_.begin();
  while (it != scanners_.end()) {
    if (it->name() == name) {
      it = scanners_.erase(it);
    } else {
      ++it;
    }
  }
}

void SaneClientFake::SetDeviceForName(const std::string& device_name,
                                      std::unique_ptr<SaneDeviceFake> device) {
  if (device) {
    devices_.emplace(device_name, std::move(device));
  } else {
    devices_.erase(device_name);
  }
}

void SaneClientFake::SetIppUsbSocketDir(base::FilePath path) {
  ippusb_socket_dir_ = std::move(path);
}

base::FilePath SaneClientFake::IppUsbSocketDir() const {
  return ippusb_socket_dir_ ? *ippusb_socket_dir_
                            : SaneClient::IppUsbSocketDir();
}

}  // namespace lorgnette
