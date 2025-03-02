/*
 * Copyright 2021 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common/sensor_hal_client_impl.h"

#include <iterator>
#include <optional>
#include <utility>

#include <base/containers/contains.h>
#include <base/functional/bind.h>
#include <base/logging.h>
#include <base/posix/safe_strerror.h>
#include <base/strings/string_number_conversions.h>
#include <base/time/time.h>
#include <chromeos/mojo/service_constants.h>

#include "cros-camera/common.h"

namespace cros {

namespace {

// The time to wait before HasDevice query times out.
constexpr base::TimeDelta kDeviceQueryTimeout = base::Milliseconds(1000);

const std::pair<std::string, cros::SensorHalClient::Location>
    kLocationMapping[] = {
        {"", cros::SensorHalClient::Location::kNone},
        {mojom::kLocationBase, cros::SensorHalClient::Location::kBase},
        {mojom::kLocationLid, cros::SensorHalClient::Location::kLid},
        {mojom::kLocationCamera, cros::SensorHalClient::Location::kCamera},
};

bool IsSupported(cros::mojom::DeviceType type) {
  switch (type) {
    case cros::mojom::DeviceType::ACCEL:
      return true;

    case cros::mojom::DeviceType::ANGLVEL:
      return true;

    case cros::mojom::DeviceType::GRAVITY:
      return true;

    default:
      return false;
  }
}

std::optional<mojom::DeviceType> ConvertDeviceType(
    SensorHalClient::DeviceType type) {
  switch (type) {
    case SensorHalClient::DeviceType::kAccel:
      return mojom::DeviceType::ACCEL;

    case SensorHalClient::DeviceType::kAnglVel:
      return mojom::DeviceType::ANGLVEL;

    case SensorHalClient::DeviceType::kGravity:
      return mojom::DeviceType::GRAVITY;

    default:
      return std::nullopt;
  }
}

std::optional<cros::SensorHalClient::Location> ParseLocation(
    const std::optional<std::string>& raw_location) {
  if (!raw_location) {
    LOGF(WARNING) << "No location attribute";
    return cros::SensorHalClient::Location::kNone;
  }

  for (const auto& mapping : kLocationMapping) {
    if (*raw_location == mapping.first)
      return mapping.second;
  }

  return cros::SensorHalClient::Location::kNone;
}

}  // namespace

// static
SensorHalClient* SensorHalClient::GetInstance(
    CameraMojoChannelManagerToken* token) {
  return CameraMojoChannelManager::FromToken(token)->GetSensorHalClient();
}

SensorHalClientImpl::SensorHalClientImpl(CameraMojoChannelManager* mojo_manager)
    : cancellation_relay_(new CancellationRelay),
      ipc_bridge_(mojo_manager->GetIpcTaskRunner(), mojo_manager) {}

SensorHalClientImpl::~SensorHalClientImpl() {
  ipc_bridge_.Reset();
  cancellation_relay_.reset();
}

bool SensorHalClientImpl::HasDevice(DeviceType type, Location location) {
  std::optional<mojom::DeviceType> device_type = ConvertDeviceType(type);
  if (!device_type) {
    return false;
  }

  auto future = cros::Future<bool>::Create(cancellation_relay_.get());

  ipc_bridge_.AsyncCall(&SensorHalClientImpl::IPCBridge::HasDevice)
      .WithArgs(*device_type, location, GetFutureCallback(future));

  if (!future->Wait())
    return false;

  return future->Get();
}

bool SensorHalClientImpl::RegisterSamplesObserver(
    DeviceType type,
    Location location,
    double frequency,
    SamplesObserver* samples_observer) {
  std::optional<mojom::DeviceType> device_type = ConvertDeviceType(type);
  if (!device_type) {
    return false;
  }

  if (frequency <= 0.0) {
    LOGF(ERROR) << "Invalid frequency: " << frequency;
    return false;
  }

  if (!samples_observer) {
    LOGF(ERROR) << "Invalid SamplesObserver";
    return false;
  }

  auto future = cros::Future<bool>::Create(cancellation_relay_.get());

  ipc_bridge_
      .AsyncCall(&SensorHalClientImpl::IPCBridge::RegisterSamplesObserver)
      .WithArgs(*device_type, location, frequency, samples_observer,
                GetFutureCallback(future));

  if (!future->Wait())
    return false;

  return future->Get();
}

void SensorHalClientImpl::UnregisterSamplesObserver(
    SamplesObserver* samples_observer) {
  if (!samples_observer) {
    return;
  }

  ipc_bridge_
      .AsyncCall(&SensorHalClientImpl::IPCBridge::UnregisterSamplesObserver)
      .WithArgs(samples_observer);
}

SensorHalClientImpl::IPCBridge::IPCBridge(
    CameraMojoChannelManager* mojo_manager)
    : mojo_manager_(mojo_manager) {
  mojo_service_manager_observer_ =
      mojo_manager_->CreateMojoServiceManagerObserver(
          /*service_name=*/chromeos::mojo_services::kIioSensor,
          /*on_register_callback=*/
          base::BindRepeating(&SensorHalClientImpl::IPCBridge::RequestService,
                              GetWeakPtr()),
          /*on_unregister_callback=*/
          base::BindRepeating(
              &SensorHalClientImpl::IPCBridge::OnUnregisterCallback,
              GetWeakPtr()));
}

SensorHalClientImpl::IPCBridge::~IPCBridge() {
  ResetSensorService();
}

void SensorHalClientImpl::IPCBridge::RequestService() {
  mojo::PendingRemote<cros::mojom::SensorService> sensor_service_remote;
  mojo_manager_->RequestServiceFromMojoServiceManager(
      /*service_name=*/chromeos::mojo_services::kIioSensor,
      sensor_service_remote.InitWithNewPipeAndPassReceiver().PassPipe());

  SetUpChannel(std::move(sensor_service_remote));
}

void SensorHalClientImpl::IPCBridge::OnUnregisterCallback() {
  LOGF(WARNING)
      << "IioSensor service is no longer registered in mojo service manager.";
}

void SensorHalClientImpl::IPCBridge::HasDevice(
    mojom::DeviceType type,
    Location location,
    base::OnceCallback<void(bool)> callback) {
  if (HasDeviceInternal(type, location)) {
    std::move(callback).Run(true);
    return;
  }

  if (AreAllDevicesOfTypeInitialized(type)) {
    std::move(callback).Run(false);
    return;
  }

  uint32_t info_id = device_query_info_counter_++;
  DeviceQueryInfo info = {
      .type = type, .location = location, .callback = std::move(callback)};
  device_queries_info_.emplace(info_id, std::move(info));

  // As there are devices uninitialized, wait for iioservice to report device
  // info to us before the query times out.
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&SensorHalClientImpl::IPCBridge::OnDeviceQueryTimedOut,
                     GetWeakPtr(), info_id),
      kDeviceQueryTimeout);
}

void SensorHalClientImpl::IPCBridge::RegisterSamplesObserver(
    mojom::DeviceType type,
    Location location,
    double frequency,
    SamplesObserver* samples_observer,
    base::OnceCallback<void(bool)> callback) {
  DCHECK_GT(frequency, 0.0);
  DCHECK(samples_observer);

  if (base::Contains(readers_, samples_observer)) {
    LOGF(ERROR) << "This SamplesObserver is already registered to a device";
    std::move(callback).Run(false);
    return;
  }

  if (!HasDeviceInternal(type, location)) {
    if (AreAllDevicesOfTypeInitialized(type)) {
      LOGF(ERROR) << "Invalid DeviceType: " << type
                  << " and Location: " << static_cast<int>(location) << " pair";
    } else {
      LOGF(ERROR) << "Not all devices with type: " << type
                  << " have been initialized";
    }

    samples_observer->OnErrorOccurred(
        SamplesObserver::ErrorType::DEVICE_REMOVED);

    std::move(callback).Run(false);
    return;
  }

  int32_t iio_device_id = device_maps_[type][location];
  DCHECK(devices_[iio_device_id].scale.has_value());

  // If iioservice is not connected, delay constructing SensorReader.
  ReaderData reader_data = {
      .iio_device_id = iio_device_id,
      .type = type,
      .frequency = frequency,
      .sensor_reader =
          sensor_service_remote_.is_bound()
              ? std::make_unique<SensorReader>(
                    iio_device_id, type, frequency,
                    devices_[iio_device_id].scale.value(), samples_observer,
                    GetSensorDeviceRemote(iio_device_id))
              : nullptr};
  readers_.emplace(samples_observer, std::move(reader_data));

  std::move(callback).Run(true);
}

void SensorHalClientImpl::IPCBridge::UnregisterSamplesObserver(
    SamplesObserver* samples_observer) {
  DCHECK(samples_observer);

  readers_.erase(samples_observer);
}

void SensorHalClientImpl::IPCBridge::SetUpChannel(
    mojo::PendingRemote<mojom::SensorService> pending_remote) {
  if (IsReady()) {
    LOGF(ERROR) << "Ignoring the second Remote<SensorService>";
    return;
  }

  sensor_service_remote_.Bind(std::move(pending_remote));
  sensor_service_remote_.set_disconnect_handler(
      base::BindOnce(&SensorHalClientImpl::IPCBridge::OnSensorServiceDisconnect,
                     GetWeakPtr()));

  sensor_service_remote_->RegisterNewDevicesObserver(
      new_devices_observer_.BindNewPipeAndPassRemote());
  new_devices_observer_.set_disconnect_handler(base::BindOnce(
      &SensorHalClientImpl::IPCBridge::OnNewDevicesObserverDisconnect,
      GetWeakPtr()));

  sensor_service_remote_->GetAllDeviceIds(base::BindOnce(
      &SensorHalClientImpl::IPCBridge::GetAllDeviceIdsCallback, GetWeakPtr()));

  // Re-establish mojo channels for the existing observers with SensorReaders.
  for (auto& [samples_observer, reader_data] : readers_) {
    reader_data.sensor_reader = std::make_unique<SensorReader>(
        reader_data.iio_device_id, reader_data.type, reader_data.frequency,
        devices_[reader_data.iio_device_id].scale.value(), samples_observer,
        GetSensorDeviceRemote(reader_data.iio_device_id));
  }
}

void SensorHalClientImpl::IPCBridge::OnNewDeviceAdded(
    int32_t iio_device_id, const std::vector<mojom::DeviceType>& types) {
  if (base::Contains(devices_, iio_device_id))
    return;

  RegisterDevice(iio_device_id, types);
}

void SensorHalClientImpl::IPCBridge::OnDeviceRemoved(int32_t iio_device_id) {
  LOGF(INFO) << "OnDeviceRemoved: " << iio_device_id;
  for (auto it = readers_.begin(); it != readers_.end();) {
    if (it->second.iio_device_id == iio_device_id) {
      it->first->OnErrorOccurred(SamplesObserver::ErrorType::DEVICE_REMOVED);
      it = readers_.erase(it);
    } else {
      ++it;
    }
  }

  // Look for replacement sensors for the same types & location.
  std::vector<mojom::DeviceType> types = devices_[iio_device_id].types;
  std::optional<Location> location_opt = devices_[iio_device_id].location;
  devices_.erase(iio_device_id);
  if (!location_opt.has_value())
    return;

  auto location = location_opt.value();
  for (const mojom::DeviceType& type : types) {
    auto& map_type = device_maps_[type];
    if (map_type[location] == iio_device_id) {
      map_type.erase(location);

      // Currently we couldn't differentiate devices with the same type and
      // location.
      for (auto& device : devices_) {
        if (base::Contains(device.second.types, type) &&
            device.second.location == location) {
          map_type[location] = device.first;
          RunDeviceQueriesForType(type);
          break;
        }
      }
    }
  }
}

base::WeakPtr<SensorHalClientImpl::IPCBridge>
SensorHalClientImpl::IPCBridge::GetWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
}

void SensorHalClientImpl::IPCBridge::GetAllDeviceIdsCallback(
    const base::flat_map<int32_t, std::vector<mojom::DeviceType>>&
        iio_device_ids_types) {
  devices_retrieved_ = true;

  for (const auto& [iio_device_id, types] : iio_device_ids_types) {
    RegisterDevice(iio_device_id, types);
  }
}

void SensorHalClientImpl::IPCBridge::OnDeviceQueryTimedOut(uint32_t info_id) {
  auto it = device_queries_info_.find(info_id);
  if (it == device_queries_info_.end()) {
    // Task was already processed.
    return;
  }

  LOGF(ERROR) << "HasDevice query timed out with type: " << it->second.type
              << ", and location: " << static_cast<int>(it->second.location);

  std::move(it->second.callback).Run(false);
  device_queries_info_.erase(it);
}

void SensorHalClientImpl::IPCBridge::RegisterDevice(
    int32_t iio_device_id, const std::vector<mojom::DeviceType>& types) {
  DeviceData& device = devices_[iio_device_id];

  if (device.ignored)
    return;

  // This should only be processed once.
  if (device.types.empty()) {
    for (const auto& type : types) {
      if (IsSupported(type)) {
        device.types.push_back(type);
      }
    }
  }

  // Not needed.
  if (device.types.empty()) {
    device.ignored = true;
    return;
  }

  std::vector<std::string> attr_names;
  if (!device.location) {
    attr_names.push_back(mojom::kLocation);
  }
  if (!device.scale) {
    attr_names.push_back(mojom::kScale);
  }

  if (attr_names.empty()) {
    return;
  }

  device.remote = GetSensorDeviceRemote(iio_device_id);

  device.remote->GetAttributes(
      attr_names,
      base::BindOnce(&SensorHalClientImpl::IPCBridge::GetAttributesCallback,
                     GetWeakPtr(), iio_device_id, attr_names));
}

mojo::Remote<mojom::SensorDevice>
SensorHalClientImpl::IPCBridge::GetSensorDeviceRemote(int32_t iio_device_id) {
  DCHECK(sensor_service_remote_.is_bound());

  mojo::Remote<mojom::SensorDevice> sensor_device_remote;
  auto& device = devices_[iio_device_id];
  if (device.remote.is_bound()) {
    // Reuse the previous remote.
    sensor_device_remote = std::move(device.remote);
  } else {
    sensor_service_remote_->GetDevice(
        iio_device_id, sensor_device_remote.BindNewPipeAndPassReceiver());
  }

  return sensor_device_remote;
}

void SensorHalClientImpl::IPCBridge::GetAttributesCallback(
    int32_t iio_device_id,
    const std::vector<std::string> attr_names,
    const std::vector<std::optional<std::string>>& values) {
  auto it = devices_.find(iio_device_id);
  DCHECK(it != devices_.end());
  auto& device = it->second;
  DCHECK(device.remote.is_bound());

  if (attr_names.size() != values.size()) {
    LOGF(ERROR) << "Size of attribute names: " << attr_names.size()
                << " doesn't match size of attribute values: " << values.size();
    IgnoreDevice(iio_device_id);
  }

  for (size_t i = 0; i < attr_names.size(); ++i) {
    if (attr_names[i] == mojom::kLocation && !device.location) {
      device.location = ParseLocation(values[i]);
      if (!device.location) {
        LOGF(ERROR) << "Failed to parse location: " << values[i].value_or("")
                    << ", with sensor id: " << iio_device_id;
        IgnoreDevice(iio_device_id);
        return;
      }
    } else if (attr_names[i] == mojom::kScale && !device.scale) {
      double scale = 0.0;
      if (!values[i] || !base::StringToDouble(*values[i], &scale)) {
        LOGF(ERROR) << "Invalid scale: " << values[i].value_or("")
                    << ", for device with id: " << iio_device_id;
        // Assume the scale to be 1.
        scale = 1.0;
      }

      device.scale = scale;
    }
  }

  DCHECK(device.location && device.scale);

  for (const auto& type : device.types) {
    // Currently we couldn't differentiate devices with the same type and
    // location.
    if (!HasDeviceInternal(type, *device.location)) {
      device_maps_[type][*device.location] = iio_device_id;
    }

    RunDeviceQueriesForType(type);
  }
}

void SensorHalClientImpl::IPCBridge::IgnoreDevice(int32_t iio_device_id) {
  auto& device = devices_[iio_device_id];
  device.ignored = true;
  device.remote.reset();

  for (const auto& type : device.types) {
    RunDeviceQueriesForType(type);
  }
}

bool SensorHalClientImpl::IPCBridge::AreAllDevicesOfTypeInitialized(
    mojom::DeviceType type) {
  if (!devices_retrieved_) {
    return false;
  }

  for (auto& [iio_device_id, device] : devices_) {
    if (device.ignored || !base::Contains(device.types, type))
      continue;

    if (!device.location)
      return false;
  }

  return true;
}

void SensorHalClientImpl::IPCBridge::RunDeviceQueriesForType(
    mojom::DeviceType type) {
  bool all_initialized = AreAllDevicesOfTypeInitialized(type);

  for (auto it = device_queries_info_.begin();
       it != device_queries_info_.end();) {
    bool processed = false;
    if (type == it->second.type) {
      if (HasDeviceInternal(it->second.type, it->second.location)) {
        std::move(it->second.callback).Run(true);
        processed = true;
      } else if (all_initialized) {
        std::move(it->second.callback).Run(false);
        processed = true;
      }
    }

    if (processed) {
      it = device_queries_info_.erase(it);
    } else {
      ++it;
    }
  }
}

bool SensorHalClientImpl::IPCBridge::HasDeviceInternal(mojom::DeviceType type,
                                                       Location location) {
  return base::Contains(device_maps_, type) &&
         base::Contains(device_maps_[type], location);
}

void SensorHalClientImpl::IPCBridge::ResetSensorService() {
  for (auto& [id, device] : devices_) {
    // Only reset the mojo pipe and keep all the other initialized types and
    // attributes, so that it won't need to be initialized twice when iioservice
    // restarts and the mojo connection is re-established.
    device.remote.reset();
  }

  new_devices_observer_.reset();
  sensor_service_remote_.reset();

  for (auto& [samples_observer, reader_data] : readers_) {
    reader_data.sensor_reader.reset();
  }
}

void SensorHalClientImpl::IPCBridge::OnSensorServiceDisconnect() {
  LOGF(ERROR) << "Wait for IIO Service's reconnection.";

  ResetSensorService();
}

void SensorHalClientImpl::IPCBridge::OnNewDevicesObserverDisconnect() {
  LOGF(ERROR) << "Wait for IIO Service's reconnection.";

  // Assumes IIO Service has crashed and waits for its relaunch.
  ResetSensorService();
}

}  // namespace cros
