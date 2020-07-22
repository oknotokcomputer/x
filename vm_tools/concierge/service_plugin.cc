// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/socket.h>
#include <sys/types.h>

#include <string>

#include <base/base64url.h>
#include <base/files/file_util.h>
#include <base/guid.h>

#include "vm_tools/concierge/plugin_vm.h"
#include "vm_tools/concierge/plugin_vm_helper.h"
#include "vm_tools/concierge/service.h"
#include "vm_tools/concierge/shared_data.h"

namespace vm_tools {
namespace concierge {

namespace {

bool GetPluginStatefulDirectory(const std::string& vm_id,
                                const std::string& cryptohome_id,
                                base::FilePath* path_out) {
  return GetPluginDirectory(base::FilePath(kCryptohomeRoot)
                                .Append(kPluginVmDir)
                                .Append(cryptohome_id),
                            "pvm", vm_id, true /* create */, path_out);
}

bool GetPluginRuntimeDirectory(const std::string& vm_id,
                               base::ScopedTempDir* runtime_dir_out) {
  base::FilePath path;
  if (GetPluginDirectory(base::FilePath("/run/pvm"), "", vm_id,
                         true /*create */, &path)) {
    // Take ownership of directory
    CHECK(runtime_dir_out->Set(path));
    return true;
  }

  return false;
}

bool GetPluginRootDirectory(const std::string& vm_id,
                            base::ScopedTempDir* root_dir_out) {
  base::FilePath path;
  if (!base::CreateTemporaryDirInDir(base::FilePath(kRuntimeDir), "vm.",
                                     &path)) {
    PLOG(ERROR) << "Unable to create root directory for VM";
    return false;
  }

  // Take ownership of directory
  CHECK(root_dir_out->Set(path));
  return true;
}

bool CreatePluginRootHierarchy(const base::FilePath& root_path) {
  base::FilePath etc_dir(root_path.Append("etc"));
  base::File::Error dir_error;
  if (!CreateDirectoryAndGetError(etc_dir, &dir_error)) {
    LOG(ERROR) << "Unable to create /etc in root directory for VM "
               << base::File::ErrorToString(dir_error);
    return false;
  }

  // Note that this will be dangling (or rather point to concierge's timezone
  // instance) until crosvm bind mounts /var/lib/timezone and
  // /usr/share/zoneinfo into plugin jail.
  if (!base::CreateSymbolicLink(base::FilePath("/var/lib/timezone/localtime"),
                                etc_dir.Append("localtime"))) {
    PLOG(ERROR) << "Unable to create /etc/localtime symlink";
    return false;
  }

  return true;
}

bool GetPlugin9PSocketPath(const std::string& vm_id, base::FilePath* path_out) {
  base::FilePath runtime_dir;
  if (!GetPluginDirectory(base::FilePath("/run/pvm"), "", vm_id,
                          true /* create */, &runtime_dir)) {
    LOG(ERROR) << "Unable to get runtime directory for 9P socket";
    return false;
  }

  *path_out = runtime_dir.Append("9p.sock");
  return true;
}

}  // namespace

std::unique_ptr<dbus::Response> Service::StartPluginVm(
    dbus::MethodCall* method_call) {
  LOG(INFO) << "Received StartPluginVm request";

  std::unique_ptr<dbus::Response> dbus_response(
      dbus::Response::FromMethodCall(method_call));
  dbus::MessageReader reader(method_call);
  dbus::MessageWriter writer(dbus_response.get());
  StartPluginVmRequest request;
  StartVmResponse response;
  auto helper_result = StartVmHelper<StartPluginVmRequest>(
      method_call, &reader, &writer, true /* allow_zero_cpus */);
  if (!helper_result) {
    return dbus_response;
  }
  std::tie(request, response) = *helper_result;

  // Get the stateful directory.
  base::FilePath stateful_dir;
  if (!GetPluginStatefulDirectory(request.name(), request.owner_id(),
                                  &stateful_dir)) {
    LOG(ERROR) << "Unable to create stateful directory for VM";

    response.set_failure_reason("Unable to create stateful directory");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  // Get the directory for ISO images.
  base::FilePath iso_dir;
  if (!GetPluginIsoDirectory(request.name(), request.owner_id(),
                             true /* create */, &iso_dir)) {
    LOG(ERROR) << "Unable to create directory holding ISOs for VM";

    response.set_failure_reason("Unable to create ISO directory");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  // Create the runtime directory.
  base::ScopedTempDir runtime_dir;
  if (!GetPluginRuntimeDirectory(request.name(), &runtime_dir)) {
    LOG(ERROR) << "Unable to create runtime directory for VM";

    response.set_failure_reason("Unable to create runtime directory");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  // Create the root directory.
  base::ScopedTempDir root_dir;
  if (!GetPluginRootDirectory(request.name(), &root_dir)) {
    LOG(ERROR) << "Unable to create runtime directory for VM";

    response.set_failure_reason("Unable to create runtime directory");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  if (!CreatePluginRootHierarchy(root_dir.GetPath())) {
    response.set_failure_reason("Unable to create plugin root hierarchy");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  if (!PluginVm::WriteResolvConf(root_dir.GetPath().Append("etc"), nameservers_,
                                 search_domains_)) {
    LOG(ERROR) << "Unable to seed resolv.conf for the Plugin VM";

    response.set_failure_reason("Unable to seed resolv.conf");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  // Generate the token used by cicerone to identify the VM and write it to
  // a VM specific directory that gets mounted into the VM.
  std::string vm_token = base::GenerateGUID();
  if (base::WriteFile(runtime_dir.GetPath().Append("cicerone.token"),
                      vm_token.c_str(),
                      vm_token.length()) != vm_token.length()) {
    PLOG(ERROR) << "Failure writing out cicerone token to file";

    response.set_failure_reason("Unable to set cicerone token");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  base::FilePath p9_socket_path;
  if (!GetPlugin9PSocketPath(request.name(), &p9_socket_path)) {
    response.set_failure_reason("Internal error: unable to get 9P directory");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  base::ScopedFD p9_socket =
      PluginVm::CreateUnixSocket(p9_socket_path, SOCK_STREAM);
  if (!p9_socket.is_valid()) {
    LOG(ERROR) << "Failed creating 9P socket for file sharing";

    response.set_failure_reason("Internal error: unable to create 9P socket");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  std::unique_ptr<patchpanel::Client> network_client =
      patchpanel::Client::New();
  if (!network_client) {
    LOG(ERROR) << "Unable to open networking service client";

    response.set_failure_reason("Unable to open network service client");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  std::unique_ptr<SeneschalServerProxy> seneschal_server_proxy =
      SeneschalServerProxy::CreateFdProxy(seneschal_service_proxy_, p9_socket);
  if (!seneschal_server_proxy) {
    LOG(ERROR) << "Unable to start shared directory server";

    response.set_failure_reason("Unable to start shared directory server");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  // Build the plugin params.
  std::vector<std::string> params(
      std::make_move_iterator(request.mutable_params()->begin()),
      std::make_move_iterator(request.mutable_params()->end()));

  // Now start the VM.
  VmId vm_id(request.owner_id(), request.name());
  std::unique_ptr<PluginVm> vm = PluginVm::Create(
      vm_id, request.cpus(), std::move(params), std::move(stateful_dir),
      std::move(iso_dir), root_dir.Take(), runtime_dir.Take(),
      std::move(network_client), request.subnet_index(),
      request.net_options().enable_vnet_hdr(),
      std::move(seneschal_server_proxy), vm_permission_service_proxy_,
      vmplugin_service_proxy_);
  if (!vm) {
    LOG(ERROR) << "Unable to start VM";
    response.set_failure_reason("Unable to start VM");
    writer.AppendProtoAsArrayOfBytes(response);
    return dbus_response;
  }

  VmInterface::Info info = vm->GetInfo();

  VmInfo* vm_info = response.mutable_vm_info();
  vm_info->set_ipv4_address(info.ipv4_address);
  vm_info->set_pid(info.pid);
  vm_info->set_cid(info.cid);
  vm_info->set_seneschal_server_handle(info.seneschal_server_handle);
  vm_info->set_permission_token(info.permission_token);
  switch (info.status) {
    case VmInterface::Status::STARTING: {
      response.set_status(VM_STATUS_STARTING);
      break;
    }
    case VmInterface::Status::RUNNING: {
      response.set_status(VM_STATUS_RUNNING);
      break;
    }
    default: {
      response.set_status(VM_STATUS_UNKNOWN);
      break;
    }
  }
  response.set_success(true);
  writer.AppendProtoAsArrayOfBytes(response);

  NotifyCiceroneOfVmStarted(vm_id, 0 /* cid */, info.pid, std::move(vm_token));
  SendVmStartedSignal(vm_id, *vm_info, response.status());

  vms_[vm_id] = std::move(vm);
  return dbus_response;
}

}  // namespace concierge
}  // namespace vm_tools
