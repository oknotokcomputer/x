// Copyright 2018 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef VM_TOOLS_CONCIERGE_PLUGIN_VM_H_
#define VM_TOOLS_CONCIERGE_PLUGIN_VM_H_

#include <stdint.h>

#include <deque>
#include <list>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <base/files/file_descriptor_watcher_posix.h>
#include <base/files/file_path.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/notreached.h>
#include <brillo/process/process.h>
#include <dbus/exported_object.h>
#include <chromeos/patchpanel/dbus/client.h>
#include <vm_concierge/concierge_service.pb.h>

#include "vm_tools/common/vm_id.h"
#include "vm_tools/concierge/plugin_vm_usb.h"
#include "vm_tools/concierge/seneschal_server_proxy.h"
#include "vm_tools/concierge/vm_base_impl.h"
#include "vm_tools/concierge/vm_builder.h"
#include "vm_tools/concierge/vmplugin_dispatcher_interface.h"

namespace vm_tools::concierge {

// The CPU cgroup where all the PluginVm crosvm processes (other than vcpu)
// should belong to.
constexpr char kPluginVmCpuCgroup[] = "/sys/fs/cgroup/cpu/plugin";
// The CPU cgroup where all the PluginVm crosvm vcpu processes should belong to.
constexpr char kPluginVmVcpuCpuCgroup[] = "/sys/fs/cgroup/cpu/plugin-vcpus";

class PluginVm final : public VmBaseImpl {
 public:
  struct Config {
    const VmId id;
    base::FilePath stateful_dir;
    base::FilePath iso_dir;
    base::FilePath root_dir;
    base::FilePath runtime_dir;
    std::unique_ptr<patchpanel::Client> network_client;
    int subnet_index;
    bool enable_vnet_hdr;
    scoped_refptr<dbus::Bus> bus;
    std::unique_ptr<SeneschalServerProxy> seneschal_server_proxy;
    dbus::ObjectProxy* vm_permission_service_proxy;
    dbus::ObjectProxy* vmplugin_service_proxy;
    VmBuilder vm_builder;
  };
  static std::unique_ptr<PluginVm> Create(Config config);
  ~PluginVm() override;

  // VmBaseImpl overrides.
  VmBaseImpl::Info GetInfo() const override;
  const std::unique_ptr<BalloonPolicyInterface>& GetBalloonPolicy(
      const MemoryMargins& margins, const std::string& vm) override {
    // Never initialized, so a balloon policy will not run.
    return balloon_policy_;
  }
  // Currently only implemented for termina, returns "Not implemented".
  bool GetVmEnterpriseReportingInfo(
      GetVmEnterpriseReportingInfoResponse* response) override;
  bool AttachUsbDevice(uint8_t bus,
                       uint8_t addr,
                       uint16_t vid,
                       uint16_t pid,
                       int fd,
                       uint8_t* out_port) override;
  bool DetachUsbDevice(uint8_t port) override;
  bool ListUsbDevice(std::vector<UsbDeviceEntry>* devices) override;
  bool SetResolvConfig(const std::vector<std::string>& nameservers,
                       const std::vector<std::string>& search_domains) override;
  bool SetTime(std::string* failure_reason) override { return true; }
  // This VM does not use maitred to set timezone.
  bool SetTimezone(const std::string& timezone,
                   std::string* out_error) override {
    *out_error = "";
    return true;
  };
  void SetTremplinStarted() override { NOTREACHED(); }
  void VmToolsStateChanged(bool running) override;
  vm_tools::concierge::DiskImageStatus ResizeDisk(
      uint64_t new_size, std::string* failure_reason) override;
  vm_tools::concierge::DiskImageStatus GetDiskResizeStatus(
      std::string* failure_reason) override;

  // Do nothing on HandleStatefulUpdate.
  void HandleStatefulUpdate(
      const spaced::StatefulDiskSpaceUpdate update) override {}

  static bool WriteResolvConf(const base::FilePath& parent_dir,
                              const std::vector<std::string>& nameservers,
                              const std::vector<std::string>& search_domains);

  static base::ScopedFD CreateUnixSocket(const base::FilePath& path, int type);

  // Adjusts the amount of CPU the Plugin VM processes are allowed to use.
  static bool SetVmCpuRestriction(CpuRestrictionState cpu_restriction_state);

  // The 9p server managed by seneschal that provides access to shared files for
  // this VM.  Returns 0 if there is no seneschal server associated with this
  // VM.
  uint32_t seneschal_server_handle() const {
    if (seneschal_server_proxy_) {
      return seneschal_server_proxy_->handle();
    }

    return 0;
  }

 protected:
  // VmBaseImpl overrides.
  std::vector<StopStep> GetStopSteps(StopType type) override;

 private:
  explicit PluginVm(Config config);
  PluginVm(const PluginVm&) = delete;
  PluginVm& operator=(const PluginVm&) = delete;

  void HandleSuspendImminent() override {}
  void HandleSuspendDone() override {}
  bool Start(base::FilePath stateful_dir,
             int subnet_index,
             bool enable_vnet_hdr,
             VmBuilder vm_builder);
  bool CreateUsbListeningSocket();
  void HandleUsbControlResponse();

  void OnListenFileCanReadWithoutBlocking();
  void OnVmFileCanReadWithoutBlocking();
  void OnVmFileCanWriteWithoutBlocking();

  // Gets the sequence to cleanly shutdown the VM.
  std::vector<StopStep> GetShutdownSteps();

  // Gets the sequence to suspend the VM.
  std::vector<StopStep> GetSuspendSteps();

  // Cleans up resources used by the VM. Called before shutdown or if the VM
  // exited unexpectedly.
  void ResourceCleanup(base::OnceClosure callback);

  // Initiates shutdown via the pvm dispatcher. Runs callback when the shutdown
  // response is received from the dispatcher.
  void InitiateShutdownViaPvmDispatcher(base::OnceClosure callback);

  // Run upon receiving a shutdown response from the pvm dispatcher.
  void OnDispatcherShutdownResponse(base::OnceClosure callback,
                                    pvm::dispatcher::VmOpResult result);

  // Initiates suspend via the pvm dispatcher. Runss callback when the suspend
  // response is received from the dispatcher or the timeout is reached.
  void InitiateSuspendViaPvmDispatcher(std::optional<base::TimeTicks> deadline,
                                       base::OnceClosure callback);

  // Run upon receiving a suspend response from the pvm dispatcher
  void OnDispatcherSuspendResponse(base::TimeTicks deadline,
                                   base::OnceClosure callback,
                                   pvm::dispatcher::VmOpResult result);

  // This VM ID. It is used to communicate with the dispatcher to request
  // VM state changes.
  const VmId id_;
  std::size_t id_hash_;

  // Specifies directory holding ISO images that can be attached to the VM.
  base::FilePath iso_dir_;

  // Allows to build skeleton of root file system for the plugin.
  // Individual directories, such as /etc, are mounted plugin jail.
  base::ScopedTempDir root_dir_;

  // Network IPv4 subnet and tap device allocation from patchpanel.
  patchpanel::Client::ParallelsAllocation network_alloc_;

  // Connection to the system bus.
  scoped_refptr<dbus::Bus> bus_;

  // Proxy to the dispatcher service.  Not owned.
  dbus::ObjectProxy* vm_permission_service_proxy_;

  // Token assigned to the VM by the permission service.
  std::string permission_token_;

  // Proxy to the dispatcher service.  Not owned.
  dbus::ObjectProxy* vmplugin_service_proxy_;

  // List of USB devices attached to VM (vid, pid, handle)
  using UsbDeviceInfo = std::tuple<uint16_t, uint16_t, uint32_t>;
  std::list<UsbDeviceInfo> usb_devices_;

  // Monotonically increasing handle (port) number for USB devices passed
  // to the Plugin VM.
  uint32_t usb_last_handle_ = 0;

  // Outstanding control requests waiting to be transmitted to plugin.
  std::deque<std::pair<UsbCtrlRequest, base::ScopedFD>> usb_req_waiting_xmit_;

  // Outstanding control requests waiting response from plugin.
  std::list<UsbCtrlRequest> usb_req_waiting_response_;

  // File descriptors to pass USB devices over to plugin.
  base::ScopedFD usb_listen_fd_;
  base::ScopedFD usb_vm_fd_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> usb_listen_watcher_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> usb_vm_read_watcher_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller>
      usb_vm_write_watcher_;

  // This should be the last member of the class.
  base::WeakPtrFactory<PluginVm> weak_ptr_factory_;
};

}  // namespace vm_tools::concierge

#endif  // VM_TOOLS_CONCIERGE_PLUGIN_VM_H_
