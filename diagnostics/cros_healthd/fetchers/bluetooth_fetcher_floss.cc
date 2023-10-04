// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/cros_healthd/fetchers/bluetooth_fetcher_floss.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/functional/bind.h>
#include <base/functional/callback.h>
#include <base/strings/string_number_conversions.h>
#include <brillo/variant_dictionary.h>
#include <dbus/object_path.h>

#include "diagnostics/cros_healthd/system/context.h"
#include "diagnostics/cros_healthd/system/floss_controller.h"
#include "diagnostics/cros_healthd/utils/callback_barrier.h"
#include "diagnostics/cros_healthd/utils/dbus_utils.h"
#include "diagnostics/cros_healthd/utils/error_utils.h"
#include "diagnostics/dbus_bindings/bluetooth_manager/dbus-proxies.h"
#include "diagnostics/dbus_bindings/floss/dbus-proxies.h"
#include "diagnostics/mojom/public/cros_healthd_probe.mojom.h"

namespace diagnostics {

namespace {

namespace mojom = ::ash::cros_healthd::mojom;

class State {
 public:
  explicit State(FlossController* floss_controller);
  State(const State&) = delete;
  State& operator=(const State&) = delete;
  ~State();

  void AddAdapterInfo(mojom::BluetoothAdapterInfoPtr adapter_info);

  void FetchEnabledAdapterInfo(
      mojom::BluetoothAdapterInfo* const adapter_info_ptr,
      CallbackBarrier& barrier,
      int32_t hci_interface);

  void HandleAdapterAddressResponse(
      mojom::BluetoothAdapterInfo* const adapter_info_ptr,
      brillo::Error* err,
      const std::string& address);
  void HandleAdapterNameResponse(
      mojom::BluetoothAdapterInfo* const adapter_info_ptr,
      brillo::Error* err,
      const std::string& name);
  void HandleAdapterDiscoveringResponse(
      mojom::BluetoothAdapterInfo* const adapter_info_ptr,
      brillo::Error* err,
      bool discovering);
  void HandleAdapterDiscoverableResponse(
      mojom::BluetoothAdapterInfo* const adapter_info_ptr,
      brillo::Error* err,
      bool discoverable);
  void FetchConnectedDevicesInfo(
      mojom::BluetoothAdapterInfo* const adapter_info_ptr,
      brillo::Error* err,
      const std::vector<brillo::VariantDictionary>& devices);

  void HandleResult(FetchBluetoothInfoFromFlossCallback callback, bool success);

 private:
  FlossController* const floss_controller_;
  std::vector<mojom::BluetoothAdapterInfoPtr> adapter_infos_;
  mojom::ProbeErrorPtr error_ = nullptr;
};

State::State(FlossController* floss_controller)
    : floss_controller_(floss_controller) {
  CHECK(floss_controller_);
}

State::~State() = default;

void State::AddAdapterInfo(mojom::BluetoothAdapterInfoPtr adapter_info) {
  adapter_infos_.push_back(std::move(adapter_info));
}

void State::FetchEnabledAdapterInfo(
    mojom::BluetoothAdapterInfo* const adapter_info_ptr,
    CallbackBarrier& barrier,
    int32_t hci_interface) {
  auto target_adapter_path =
      dbus::ObjectPath("/org/chromium/bluetooth/hci" +
                       base::NumberToString(hci_interface) + "/adapter");
  org::chromium::bluetooth::BluetoothProxyInterface* target_adapter = nullptr;
  for (const auto& adapter : floss_controller_->GetAdapters()) {
    if (!adapter || adapter->GetObjectPath() != target_adapter_path)
      continue;
    target_adapter = adapter;
  }
  if (!target_adapter) {
    error_ = CreateAndLogProbeError(mojom::ErrorType::kServiceUnavailable,
                                    "Failed to get target adapter");
    return;
  }

  // Address.
  auto address_cb = SplitDbusCallback(
      barrier.Depend(base::BindOnce(&State::HandleAdapterAddressResponse,
                                    base::Unretained(this), adapter_info_ptr)));
  target_adapter->GetAddressAsync(std::move(address_cb.first),
                                  std::move(address_cb.second));
  // Name.
  auto name_cb = SplitDbusCallback(
      barrier.Depend(base::BindOnce(&State::HandleAdapterNameResponse,
                                    base::Unretained(this), adapter_info_ptr)));
  target_adapter->GetNameAsync(std::move(name_cb.first),
                               std::move(name_cb.second));
  // Discovering.
  auto discovering_cb = SplitDbusCallback(
      barrier.Depend(base::BindOnce(&State::HandleAdapterDiscoveringResponse,
                                    base::Unretained(this), adapter_info_ptr)));
  target_adapter->IsDiscoveringAsync(std::move(discovering_cb.first),
                                     std::move(discovering_cb.second));
  // Discoverable.
  auto discoverable_cb = SplitDbusCallback(
      barrier.Depend(base::BindOnce(&State::HandleAdapterDiscoverableResponse,
                                    base::Unretained(this), adapter_info_ptr)));
  target_adapter->GetDiscoverableAsync(std::move(discoverable_cb.first),
                                       std::move(discoverable_cb.second));
  // Connected devices.
  auto devices_cb = SplitDbusCallback(
      barrier.Depend(base::BindOnce(&State::FetchConnectedDevicesInfo,
                                    base::Unretained(this), adapter_info_ptr)));
  target_adapter->GetConnectedDevicesAsync(std::move(devices_cb.first),
                                           std::move(devices_cb.second));
  // TODO(b/300239084): Fetch more adapter info via Floss.
}

void State::HandleAdapterAddressResponse(
    mojom::BluetoothAdapterInfo* const adapter_info_ptr,
    brillo::Error* err,
    const std::string& address) {
  if (err) {
    error_ = CreateAndLogProbeError(mojom::ErrorType::kSystemUtilityError,
                                    "Failed to get adapter address");
    return;
  }
  adapter_info_ptr->address = address;
}
void State::HandleAdapterNameResponse(
    mojom::BluetoothAdapterInfo* const adapter_info_ptr,
    brillo::Error* err,
    const std::string& name) {
  if (err) {
    error_ = CreateAndLogProbeError(mojom::ErrorType::kSystemUtilityError,
                                    "Failed to get adapter name");
    return;
  }
  adapter_info_ptr->name = name;
}

void State::HandleAdapterDiscoveringResponse(
    mojom::BluetoothAdapterInfo* const adapter_info_ptr,
    brillo::Error* err,
    bool discovering) {
  if (err) {
    error_ = CreateAndLogProbeError(mojom::ErrorType::kSystemUtilityError,
                                    "Failed to get adapter discovering");
    return;
  }
  adapter_info_ptr->discovering = discovering;
}

void State::HandleAdapterDiscoverableResponse(
    mojom::BluetoothAdapterInfo* const adapter_info_ptr,
    brillo::Error* err,
    bool discoverable) {
  if (err) {
    error_ = CreateAndLogProbeError(mojom::ErrorType::kSystemUtilityError,
                                    "Failed to get adapter discoverable");
    return;
  }
  adapter_info_ptr->discoverable = discoverable;
}

void State::FetchConnectedDevicesInfo(
    mojom::BluetoothAdapterInfo* const adapter_info_ptr,
    brillo::Error* err,
    const std::vector<brillo::VariantDictionary>& devices) {
  if (err) {
    error_ = CreateAndLogProbeError(mojom::ErrorType::kSystemUtilityError,
                                    "Failed to get connected devices");
    return;
  }

  for (const auto& device : devices) {
    if (!device.contains("address") || !device.contains("name")) {
      error_ = CreateAndLogProbeError(mojom::ErrorType::kSystemUtilityError,
                                      "Failed to parse connected devices");
      return;
    }
  }

  adapter_info_ptr->num_connected_devices = devices.size();
  for (const auto& device : devices) {
    auto address =
        brillo::GetVariantValueOrDefault<std::string>(device, "address");
    auto name = brillo::GetVariantValueOrDefault<std::string>(device, "name");
    auto device_info = mojom::BluetoothDeviceInfo::New();
    device_info->address = address;
    device_info->name = name;
    adapter_info_ptr->connected_devices->push_back(std::move(device_info));
  }

  // TODO(b/300239084): Fetch Bluetooth device info via Floss.
}

void State::HandleResult(FetchBluetoothInfoFromFlossCallback callback,
                         bool success) {
  if (!success) {
    std::move(callback).Run(mojom::BluetoothResult::NewError(
        CreateAndLogProbeError(mojom::ErrorType::kServiceUnavailable,
                               "Failed to finish all callbacks.")));
    return;
  }

  if (!error_.is_null()) {
    std::move(callback).Run(
        mojom::BluetoothResult::NewError(std::move(error_)));
    return;
  }

  std::move(callback).Run(mojom::BluetoothResult::NewBluetoothAdapterInfo(
      std::move(adapter_infos_)));
}

void FetchAvailableAdaptersInfo(
    FlossController* floss_controller,
    FetchBluetoothInfoFromFlossCallback callback,
    brillo::Error* err,
    const std::vector<brillo::VariantDictionary>& adapters) {
  if (err) {
    std::move(callback).Run(mojom::BluetoothResult::NewError(
        CreateAndLogProbeError(mojom::ErrorType::kSystemUtilityError,
                               "Failed to get available adapters")));
    return;
  }

  for (const auto& adapter : adapters) {
    if (!adapter.contains("enabled") || !adapter.contains("hci_interface")) {
      std::move(callback).Run(mojom::BluetoothResult::NewError(
          CreateAndLogProbeError(mojom::ErrorType::kSystemUtilityError,
                                 "Failed to parse available adapters")));
      return;
    }
  }

  auto state = std::make_unique<State>(floss_controller);
  State* state_ptr = state.get();
  CallbackBarrier barrier{base::BindOnce(&State::HandleResult, std::move(state),
                                         std::move(callback))};

  for (const auto& adapter : adapters) {
    bool enabled = brillo::GetVariantValueOrDefault<bool>(adapter, "enabled");
    int32_t hci_interface =
        brillo::GetVariantValueOrDefault<int32_t>(adapter, "hci_interface");
    auto info = mojom::BluetoothAdapterInfo::New();
    info->powered = enabled;
    info->connected_devices = std::vector<mojom::BluetoothDeviceInfoPtr>{};
    if (enabled) {
      state_ptr->FetchEnabledAdapterInfo(info.get(), barrier, hci_interface);
    } else {
      // Report the default value since we can't access the adapter instance
      // when the powered is off.
      info->address = "";
      info->name = "hci" + base::NumberToString(hci_interface) + " (disabled)";
      info->discovering = false;
      info->discoverable = false;
      info->num_connected_devices = 0;
    }
    state_ptr->AddAdapterInfo(std::move(info));
  }
}

}  // namespace

void FetchBluetoothInfoFromFloss(Context* context,
                                 FetchBluetoothInfoFromFlossCallback callback) {
  CHECK(context);
  const auto floss_controller = context->floss_controller();
  CHECK(floss_controller);

  const auto manager = floss_controller->GetManager();
  if (!manager) {
    std::move(callback).Run(mojom::BluetoothResult::NewError(
        CreateAndLogProbeError(mojom::ErrorType::kServiceUnavailable,
                               "Floss proxy is not ready")));
    return;
  }

  auto [on_success, on_error] = SplitDbusCallback(base::BindOnce(
      &FetchAvailableAdaptersInfo, floss_controller, std::move(callback)));
  manager->GetAvailableAdaptersAsync(std::move(on_success),
                                     std::move(on_error));
}

}  // namespace diagnostics
