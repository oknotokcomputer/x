// Copyright 2020 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vm_tools/common/naming.h"
#include "vm_tools/common/pstore.h"
#include "vm_tools/pstore_dump/persistent_ram_buffer.h"

#include <string>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <brillo/flag_helper.h>
#include <brillo/cryptohome.h>
#include <brillo/dbus/async_event_sequencer.h>
#include <chromeos/dbus/service_constants.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_proxy.h>

namespace {

// Return 2 as the exit status when the .pstore file doesn't exist. This value
// is used to distinguish the reason of failure from other critial errors.
constexpr int EXIT_NO_PSTORE_FILE = 2;
static_assert(EXIT_NO_PSTORE_FILE != EXIT_FAILURE);

bool GetPrimaryUsername(std::string* out_username) {
  DCHECK(out_username);

  dbus::Bus::Options options;
  options.bus_type = dbus::Bus::SYSTEM;
  scoped_refptr<dbus::Bus> bus = new dbus::Bus(options);
  CHECK(bus->Connect()) << "Failed to connect to system D-Bus";

  dbus::ObjectProxy* session_manager_proxy = bus->GetObjectProxy(
      login_manager::kSessionManagerServiceName,
      dbus::ObjectPath(login_manager::kSessionManagerServicePath));
  dbus::MethodCall method_call(
      login_manager::kSessionManagerInterface,
      login_manager::kSessionManagerRetrievePrimarySession);
  std::unique_ptr<dbus::Response> response =
      session_manager_proxy->CallMethodAndBlock(
          &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT);
  if (!response.get()) {
    LOG(ERROR) << "Cannot retrieve username for primary session.";
    bus->ShutdownAndBlock();
    return false;
  }

  dbus::MessageReader response_reader(response.get());
  if (!response_reader.PopString(out_username)) {
    LOG(ERROR) << "Primary session username bad format.";
    bus->ShutdownAndBlock();
    return false;
  }
  bus->ShutdownAndBlock();
  return true;
}

bool FindARCVMPstorePath(base::FilePath* out_path) {
  DCHECK(out_path);

  // Before users logged in to Chrome OS, mini-ARCVM uses
  // /run/arcvm/arcvm.pstore for the path.
  base::FilePath nonuser_pstore_path(vm_tools::kArcVmPstorePath);
  if (base::PathExists(nonuser_pstore_path)) {
    *out_path = nonuser_pstore_path;
    return true;
  }

  // /run/arcvm/arcvm.pstore is moved to /home/root/<hash>/crosvm/*.pstore by
  // arcvm-forward-pstore service after users logged in and mini-ARCVM is
  // upgraded.
  std::string primary_username;
  if (!GetPrimaryUsername(&primary_username)) {
    LOG(ERROR) << "Failed to get primary username";
    return false;
  }
  base::FilePath root_path =
      brillo::cryptohome::home::GetRootPath(primary_username);
  if (root_path.empty()) {
    LOG(ERROR) << "Failed to get the cryptohome root path of user of ARCVM";
    return false;
  }
  base::FilePath cryptohome_pstore_path = root_path.Append("crosvm").Append(
      vm_tools::GetEncodedName("arcvm") + ".pstore");
  if (base::PathExists(cryptohome_pstore_path)) {
    *out_path = cryptohome_pstore_path;
    return true;
  }

  LOG(ERROR) << "The .pstore file doesn't exist at both "
             << vm_tools::kArcVmPstorePath << " and " << cryptohome_pstore_path;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  DEFINE_string(file, "", "path to a .pstore file  (default: ARCVM's .pstore)");
  brillo::FlagHelper::Init(
      argc, argv,
      "A helper to read .pstore files generated by the ARCVM's guest kernel.");

  base::FilePath path;
  if (!FLAGS_file.empty()) {
    path = base::FilePath(FLAGS_file);
  } else if (!FindARCVMPstorePath(&path)) {
    LOG(ERROR)
        << "Failed to detect the .pstore file. Please use --file option.";
    exit(EXIT_NO_PSTORE_FILE);
  }

  if (!vm_tools::pstore_dump::HandlePstore(path)) {
    exit(EXIT_FAILURE);
  }
  exit(EXIT_SUCCESS);
}
