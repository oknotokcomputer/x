// Copyright 2019 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "dlcservice/dbus_adaptors/dbus_adaptor.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/files/file_path.h>
#include <base/strings/stringprintf.h>
#include <brillo/errors/error.h>
#include <chromeos/dbus/service_constants.h>
#include <constants/imageloader.h>
#include <dbus/dlcservice/dbus-constants.h>
#include <dlcservice/proto_bindings/dlcservice.pb.h>

#include "dlcservice/dlc_base.h"
#include "dlcservice/error.h"
#include "dlcservice/proto_utils.h"
#include "dlcservice/types.h"
#include "dlcservice/utils.h"

using std::string;
using std::unique_ptr;
using std::vector;

namespace dlcservice {

DBusService::DBusService(DlcServiceInterface* dlc_service)
    : dlc_service_(dlc_service) {}

void DBusService::Install(
    std::unique_ptr<brillo::dbus_utils::DBusMethodResponse<>> response,
    const dlcservice::InstallRequest& in_install_request) {
  dlc_service_->Install(in_install_request, std::move(response));
}

bool DBusService::Uninstall(brillo::ErrorPtr* err, const string& id_in) {
  return dlc_service_->Uninstall(id_in, err);
}

// Purge is the same as Uninstall.
bool DBusService::Purge(brillo::ErrorPtr* err, const string& id_in) {
  return dlc_service_->Uninstall(id_in, err);
}

bool DBusService::Deploy(brillo::ErrorPtr* err, const string& id_in) {
  return dlc_service_->Deploy(id_in, err);
}

bool DBusService::GetInstalled(brillo::ErrorPtr* err, vector<string>* ids_out) {
  ListRequest request;
  request.set_check_mount(false);
  *ids_out = dlc_service_->GetInstalled(request);
  return true;
}

bool DBusService::GetInstalled2(brillo::ErrorPtr* err,
                                const dlcservice::ListRequest& in_list_request,
                                dlcservice::DlcStateList* out_state_list) {
  const auto& ids = dlc_service_->GetInstalled(in_list_request);
  for (const auto& id : ids) {
    DlcState state;
    brillo::ErrorPtr tmp_err;
    if (!GetDlcState(&tmp_err, id, &state)) {
      LOG(WARNING) << "Unable to GetDlcState for DLC=" << id;
      continue;
    }
    out_state_list->add_states()->Swap(&state);
  }
  return true;
}

bool DBusService::GetExistingDlcs(brillo::ErrorPtr* err,
                                  DlcsWithContent* dlc_list_out) {
  DlcIdList ids = dlc_service_->GetExistingDlcs();
  for (const auto& id : ids) {
    const auto* dlc = dlc_service_->GetDlc(id, err);
    if (dlc == nullptr)
      continue;
    auto* dlc_info = dlc_list_out->add_dlc_infos();
    dlc_info->set_id(id);
    dlc_info->set_name(dlc->GetName());
    dlc_info->set_description(dlc->GetDescription());
    dlc_info->set_used_bytes_on_disk(dlc->GetUsedBytesOnDisk());

    // TODO(crbug.com/1092770): This is a very temporarily measure so UI can
    // handle is_removable logic with exceptions for pita. Once the bug is
    // resolved, this logic should change.
    dlc_info->set_is_removable(id != "pita");
  }
  return true;
}

bool DBusService::Unload(brillo::ErrorPtr* err,
                         const dlcservice::UnloadRequest& in_unload_request) {
  switch (in_unload_request.DlcInfo_case()) {
    case UnloadRequest::kId:
      return dlc_service_->Unload(in_unload_request.id(), err);
    case UnloadRequest::kSelect:
      return dlc_service_->Unload(
          in_unload_request.select(),
          base::FilePath(imageloader::kImageloaderMountBase), err);
    default:
      *err =
          Error::Create(FROM_HERE, kErrorInvalidDlc, "Invalid DLC specifier.");
      return false;
  }
}

bool DBusService::GetDlcsToUpdate(brillo::ErrorPtr* err,
                                  std::vector<std::string>* ids_out) {
  *ids_out = dlc_service_->GetDlcsToUpdate();
  return true;
}

bool DBusService::GetDlcState(brillo::ErrorPtr* err,
                              const string& id_in,
                              DlcState* dlc_state_out) {
  auto* dlc = dlc_service_->GetDlc(id_in, err);
  if (dlc == nullptr) {
    return false;
  }
  dlc->UpdateState();
  *dlc_state_out = dlc->GetState();
  return true;
}

bool DBusService::InstallCompleted(brillo::ErrorPtr* err,
                                   const vector<string>& ids_in) {
  return dlc_service_->InstallCompleted(ids_in, err);
}

bool DBusService::UpdateCompleted(brillo::ErrorPtr* err,
                                  const vector<string>& ids_in) {
  return dlc_service_->UpdateCompleted(ids_in, err);
}

DBusAdaptor::DBusAdaptor(unique_ptr<DBusService> dbus_service)
    : org::chromium::DlcServiceInterfaceAdaptor(dbus_service.get()),
      dbus_service_(std::move(dbus_service)) {}

void DBusAdaptor::DlcStateChanged(const DlcState& dlc_state) {
  brillo::MessageLoop::current()->PostTask(
      FROM_HERE, base::BindOnce(&DBusAdaptor::SendDlcStateChangedSignal,
                                base::Unretained(this), dlc_state));
}

}  // namespace dlcservice
