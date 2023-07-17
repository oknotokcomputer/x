// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "diagnostics/cros_healthd/executor/utils/dlc_manager.h"

#include <cstdlib>
#include <utility>

#include <base/files/file_path.h>
#include <base/functional/bind.h>
#include <base/functional/callback_helpers.h>
#include <dlcservice/proto_bindings/dlcservice.pb.h>
// NOLINTNEXTLINE(build/include_alpha) dbus-proxies.h needs dlcservice.pb.h
#include <dlcservice/dbus-proxies.h>
#include <mojo/public/cpp/bindings/callback_helpers.h>

namespace diagnostics {

DlcManager::DlcManager(
    org::chromium::DlcServiceInterfaceProxyInterface* dlcservice_proxy)
    : dlcservice_proxy_(dlcservice_proxy) {}

void DlcManager::Initialize() {
  if (initialize_state_ != InitializeState::kNotInitialized) {
    LOG(ERROR) << "DLC service is initializing or initialized";
    return;
  }

  initialize_state_ = InitializeState::kInitializing;
  dlcservice_proxy_->GetObjectProxy()->WaitForServiceToBeAvailable(
      base::BindOnce(&DlcManager::RegisterDlcStateChangedEvents,
                     weak_factory_.GetWeakPtr()));
}

void DlcManager::RegisterDlcStateChangedEvents(bool service_is_available) {
  if (!service_is_available) {
    LOG(ERROR) << "DLC service is not available";
    initialize_state_ = InitializeState::kNotInitialized;
    pending_initialized_callbacks_.clear();
    return;
  }

  dlcservice_proxy_->RegisterDlcStateChangedSignalHandler(
      base::BindRepeating(&DlcManager::OnDlcStateChanged,
                          weak_factory_.GetWeakPtr()),
      base::BindOnce(&DlcManager::HandleRegisterDlcStateChangedResponse,
                     weak_factory_.GetWeakPtr()));
}

void DlcManager::HandleRegisterDlcStateChangedResponse(
    const std::string& interface,
    const std::string& signal,
    const bool success) {
  if (!success) {
    LOG(ERROR) << "Failed to register DLC state changed signal ("
               << interface << ":" << signal << ")";
    initialize_state_ = InitializeState::kNotInitialized;
    pending_initialized_callbacks_.clear();
    return;
  }

  initialize_state_ = InitializeState::kInitialized;
  for (auto& cb : pending_initialized_callbacks_) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(cb)));
  }
  pending_initialized_callbacks_.clear();
}

void DlcManager::GetBinaryRootPath(const std::string& dlc_id,
                                   DlcRootPathCallback root_path_cb) {
  pending_root_path_callbacks_[dlc_id].push_back(std::move(root_path_cb));
  WaitForInitialized(mojo::WrapCallbackWithDefaultInvokeIfNotRun(base::BindOnce(
      &DlcManager::InstallDlc, weak_factory_.GetWeakPtr(), dlc_id)));
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&DlcManager::HandleDlcRootPathCallbackTimeout,
                     weak_factory_.GetWeakPtr(), dlc_id),
      kGetDlcRootPathTimeout);
}

void DlcManager::WaitForInitialized(base::OnceClosure on_initialized) {
  switch (initialize_state_) {
    case InitializeState::kNotInitialized:
      pending_initialized_callbacks_.push_back(std::move(on_initialized));
      Initialize();
      break;
    case InitializeState::kInitializing:
      pending_initialized_callbacks_.push_back(std::move(on_initialized));
      break;
    case InitializeState::kInitialized:
      std::move(on_initialized).Run();
      break;
  }
}

void DlcManager::InstallDlc(const std::string& dlc_id) {
  if (initialize_state_ != InitializeState::kInitialized) {
    InvokeRootPathCallbacks(dlc_id, std::nullopt);
    return;
  }

  // Even if the DLC is installed, we can receive a state change event after the
  // installation is complete.
  dlcservice::InstallRequest install_request;
  install_request.set_id(dlc_id);
  dlcservice_proxy_->InstallAsync(
      install_request, /*on_success=*/base::DoNothing(),
      base::BindOnce(&DlcManager::HandleDlcInstallError,
                     weak_factory_.GetWeakPtr(), dlc_id));
}

void DlcManager::HandleDlcInstallError(const std::string& dlc_id,
                                       brillo::Error* err) {
  if (err) {
    LOG(ERROR) << "DLC installation error (" << dlc_id
               << "): " << err->GetCode() + ", message: " << err->GetMessage();
  }
  InvokeRootPathCallbacks(dlc_id, std::nullopt);
}

void DlcManager::OnDlcStateChanged(const dlcservice::DlcState& state) {
  // Skipped state changed if there are no pending callbacks.
  if (!pending_root_path_callbacks_.count(state.id())) {
    return;
  }

  switch (state.state()) {
    case dlcservice::DlcState::INSTALLED:
      InvokeRootPathCallbacks(state.id(), base::FilePath(state.root_path()));
      break;
    case dlcservice::DlcState::INSTALLING:
      break;
    case dlcservice::DlcState::NOT_INSTALLED:
    default:
      LOG(ERROR) << "DLC installation error (" << state.id()
                 << "), error: " << state.last_error_code();
      InvokeRootPathCallbacks(state.id(), std::nullopt);
      break;
  }
}

void DlcManager::InvokeRootPathCallbacks(
    const std::string& dlc_id, std::optional<base::FilePath> root_path) {
  const auto iter = pending_root_path_callbacks_.find(dlc_id);
  if (iter == pending_root_path_callbacks_.end()) {
    return;
  }

  // Invokes all callbacks when the installation completes.
  for (auto& root_path_cb : iter->second) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(root_path_cb), root_path));
  }
  pending_root_path_callbacks_.erase(iter);
}

void DlcManager::HandleDlcRootPathCallbackTimeout(const std::string& dlc_id) {
  const auto iter = pending_root_path_callbacks_.find(dlc_id);
  if (iter == pending_root_path_callbacks_.end()) {
    return;
  }

  auto& root_path_callbacks = iter->second;
  LOG(ERROR) << "DLC timeout error (" << dlc_id << ")";
  // Invokes the earliest callback with null result, which is the first one in
  // |root_path_callbacks|.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(root_path_callbacks[0]), std::nullopt));
  root_path_callbacks.erase(root_path_callbacks.begin());

  if (root_path_callbacks.empty()) {
    pending_root_path_callbacks_.erase(iter);
  }
}

}  // namespace diagnostics
