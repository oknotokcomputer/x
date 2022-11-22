// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/mount.h>

#include <arcvm_data_migrator/proto_bindings/arcvm_data_migrator.pb.h>
#include <base/command_line.h>
#include <base/files/file_enumerator.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/functional/bind.h>
#include <base/logging.h>
#include <base/strings/stringprintf.h>
#include <base/synchronization/lock.h>
#include <base/threading/thread.h>
#include <brillo/blkdev_utils/loop_device.h>
#include <brillo/cryptohome.h>
#include <brillo/daemons/dbus_daemon.h>
#include <brillo/syslog_logging.h>
#include <chromeos/dbus/service_constants.h>
#include <cryptohome/data_migrator/migration_helper.h>
#include <cryptohome/platform.h>
#include <dbus/bus.h>

#include "arc/vm/data_migrator/arcvm_data_migration_helper_delegate.h"
#include "arc/vm/data_migrator/dbus_adaptors/org.chromium.ArcVmDataMigrator.h"
#include "arc/vm/data_migrator/metrics.h"

// This is provided as macro because providing it as a function would cause the
// line numbers emitted from FROM_HERE and logger(ERROR) to be the location of
// the utility function and not the caller.
#define LOG_AND_ADD_ERROR(logger, error, message)                         \
  logger(ERROR) << message;                                               \
  brillo::Error::AddTo((error), FROM_HERE, brillo::errors::dbus::kDomain, \
                       DBUS_ERROR_FAILED, (message))

namespace arc::data_migrator {

namespace {

// The mount point for the migration destinaiton.
constexpr char kDestinationMountPoint[] = "/tmp/arcvm-data-migration-mount";

class DBusAdaptor : public org::chromium::ArcVmDataMigratorAdaptor,
                    public org::chromium::ArcVmDataMigratorInterface {
 public:
  explicit DBusAdaptor(scoped_refptr<dbus::Bus> bus)
      : org::chromium::ArcVmDataMigratorAdaptor(this),
        dbus_object_(nullptr, bus, GetObjectPath()) {
    exported_object_ =
        bus->GetExportedObject(dbus::ObjectPath(kArcVmDataMigratorServicePath));
  }

  ~DBusAdaptor() override {
    {
      // Cancel migration so that it doesn't block the destruction.
      base::AutoLock lock(migration_helper_lock_);
      if (migration_helper_) {
        migration_helper_->Cancel();
      }
    }
    CleanupMount();
  }

  DBusAdaptor(const DBusAdaptor&) = delete;
  DBusAdaptor& operator=(const DBusAdaptor&) = delete;

  // Registers the D-Bus object and interfaces.
  void RegisterAsync(
      brillo::dbus_utils::AsyncEventSequencer::CompletionAction cb) {
    RegisterWithDBusObject(&dbus_object_);
    dbus_object_.RegisterAsync(std::move(cb));
  }

  // org::chromium::ArcVmDataMigratorInterface overrides:
  bool HasDataToMigrate(brillo::ErrorPtr* error,
                        const HasDataToMigrateRequest& request,
                        bool* response) override {
    // We use /home/root/<hash>/android-data/data/data/ because host-side
    // services like arc-setup creates .../android-data/data/media/0/ even when
    // the device is already running with virtio-blk /data. The existence of
    // .../android-data/data/data would imply that there is data to migrate.
    const base::FilePath android_data_data_dir =
        brillo::cryptohome::home::GetRootPath(
            brillo::cryptohome::home::Username(request.username()))
            .Append("android-data/data/data");
    *response = base::DirectoryExists(android_data_data_dir);
    return true;
  }

  bool GetAndroidDataSize(brillo::ErrorPtr* error,
                          const GetAndroidDataSizeRequest& request,
                          int64_t* size) override {
    const base::FilePath android_data_dir =
        brillo::cryptohome::home::GetRootPath(
            brillo::cryptohome::home::Username(request.username()))
            .Append("android-data/data");

    base::FileEnumerator enumerator(
        android_data_dir, /*recursive=*/true,
        // Use the same set of file types as
        // cryptohome::data_migrator::MigrationHelper::CalculateDataToMigrate.
        base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES |
            base::FileEnumerator::SHOW_SYM_LINKS);

    *size = 0;
    for (base::FilePath entry = enumerator.Next(); !entry.empty();
         entry = enumerator.Next()) {
      *size += enumerator.GetInfo().GetSize();
    }
    return true;
  }

  bool StartMigration(brillo::ErrorPtr* error,
                      const StartMigrationRequest& request) override {
    const brillo::cryptohome::home::Username username(request.username());
    const base::FilePath user_root_dir =
        brillo::cryptohome::home::GetRootPath(username);
    const base::FilePath android_data_dir =
        user_root_dir.Append("android-data");
    const base::FilePath source_dir = android_data_dir.Append("data");

    base::FilePath destination_disk;
    switch (request.destination_type()) {
      case CROSVM_DISK: {
        // Disk path /home/root/<hash>/crosvm/YXJjdm0=.img is constructed in
        // concierge's CreateDiskImage method. Image name YXJjdm0=.img is static
        // because it is generated by vm_tools::GetEncodedName("arcvm").
        destination_disk = user_root_dir.Append("crosvm/YXJjdm0=.img");
        break;
      }
      case LVM_DEVICE: {
        const std::string user_hash =
            *brillo::cryptohome::home::SanitizeUserName(username);
        // The volume path is constructed using
        // cryptohome::DmcryptVolumePrefix().
        destination_disk = base::FilePath(base::StringPrintf(
            "/dev/mapper/vm/dmcrypt-%s-arcvm", user_hash.substr(0, 8).c_str()));
        break;
      }
      default:
        NOTREACHED();
    }

    // The mount point will be automatically removed when the upstart job stops
    // since it is created under /tmp where tmpfs is mounted.
    if (!base::CreateDirectory(base::FilePath(kDestinationMountPoint))) {
      LOG_AND_ADD_ERROR(PLOG, error,
                        "Failed to create destination mount point");
      metrics_.ReportSetupResult(SetupResult::kMountPointCreationFailure);
      return false;
    }

    loop_device_manager_ = std::make_unique<brillo::LoopDeviceManager>();
    loop_device_ = loop_device_manager_->AttachDeviceToFile(destination_disk);
    if (!loop_device_->IsValid()) {
      LOG_AND_ADD_ERROR(PLOG, error, "Failed to attach a loop device");
      metrics_.ReportSetupResult(SetupResult::kLoopDeviceAttachmentFailure);
      CleanupMount();
      return false;
    }

    if (mount(loop_device_->GetDevicePath().value().c_str(),
              kDestinationMountPoint, "ext4", 0, "")) {
      LOG_AND_ADD_ERROR(PLOG, error, "Failed to mount the loop device");
      metrics_.ReportSetupResult(SetupResult::kMountFailure);
      CleanupMount();
      return false;
    }
    mounted_ = true;

    // Unretained is safe to use here because |migration_thread_| will be joined
    // on the destruction of |this|.
    auto migrate = base::BindOnce(&DBusAdaptor::Migrate, base::Unretained(this),
                                  source_dir, android_data_dir);
    migration_thread_ = std::make_unique<base::Thread>("migration_helper");
    if (!migration_thread_->Start()) {
      LOG_AND_ADD_ERROR(LOG, error, "Failed to start thread for migration");
      metrics_.ReportSetupResult(SetupResult::kThreadStartFailure);
      CleanupMount();
      return false;
    }
    migration_thread_->task_runner()->PostTask(FROM_HERE, std::move(migrate));

    metrics_.ReportSetupResult(SetupResult::kSuccess);
    return true;
  }

  // TODO(momohatt): Add StopMigration as a D-Bus method?

 private:
  void Migrate(const base::FilePath& source_dir,
               const base::FilePath& status_files_dir) {
    cryptohome::Platform platform;
    ArcVmDataMigrationHelperDelegate delegate(&metrics_);
    constexpr uint64_t kMaxChunkSize = 128 * 1024 * 1024;

    {
      base::AutoLock lock(migration_helper_lock_);
      migration_helper_ =
          std::make_unique<cryptohome::data_migrator::MigrationHelper>(
              &platform, &delegate, source_dir,
              base::FilePath(kDestinationMountPoint), status_files_dir,
              kMaxChunkSize);
    }

    // Unretained is safe to use here because this method (DBusAdaptor::Migrate)
    // runs on |migration_thread_| which is joined on the destruction on |this|.
    bool success = migration_helper_->Migrate(base::BindRepeating(
        &DBusAdaptor::MigrationHelperCallback, base::Unretained(this)));
    {
      base::AutoLock lock(migration_helper_lock_);
      migration_helper_.reset();
    }

    DataMigrationProgress progress;
    if (success) {
      progress.set_status(DATA_MIGRATION_SUCCESS);
    } else {
      progress.set_status(DATA_MIGRATION_FAILED);
    }
    SendMigrationProgressSignal(progress);

    CleanupMount();
  }

  void MigrationHelperCallback(uint64_t current_bytes, uint64_t total_bytes) {
    DataMigrationProgress progress_to_send;
    if (total_bytes == 0) {
      // Ignore the callback when MigrationHelper is still initializing.
      return;
    }
    progress_to_send.set_status(DATA_MIGRATION_IN_PROGRESS);
    progress_to_send.set_current_bytes(current_bytes);
    progress_to_send.set_total_bytes(total_bytes);
    SendMigrationProgressSignal(progress_to_send);
  }

  void SendMigrationProgressSignal(const DataMigrationProgress& progress) {
    dbus::Signal signal(kArcVmDataMigratorInterface, kMigrationProgressSignal);
    dbus::MessageWriter writer(&signal);
    writer.AppendProtoAsArrayOfBytes(progress);

    exported_object_->SendSignal(&signal);
  }

  void CleanupMount() {
    if (mounted_) {
      PLOG_IF(ERROR, umount(kDestinationMountPoint))
          << "Failed to unmount the loop device from "
          << kDestinationMountPoint;
      mounted_ = false;
    }
    if (loop_device_) {
      PLOG_IF(ERROR, !loop_device_->Detach()) << "Failed to detach loop device";
      loop_device_.reset();
    }
  }

  // Set to true if the migration destination has been mounted on host.
  bool mounted_ = false;

  std::unique_ptr<brillo::LoopDevice> loop_device_;
  std::unique_ptr<brillo::LoopDeviceManager> loop_device_manager_;

  std::unique_ptr<base::Thread> migration_thread_;
  std::unique_ptr<cryptohome::data_migrator::MigrationHelper> migration_helper_;
  base::Lock migration_helper_lock_;

  ArcVmDataMigratorMetrics metrics_;

  brillo::dbus_utils::DBusObject dbus_object_;
  dbus::ExportedObject* exported_object_;  // Owned by the Bus object
};

class Daemon : public brillo::DBusServiceDaemon {
 public:
  Daemon() : DBusServiceDaemon(kArcVmDataMigratorServiceName) {}
  ~Daemon() override = default;

  Daemon(const Daemon&) = delete;
  Daemon& operator=(const Daemon&) = delete;

 protected:
  void RegisterDBusObjectsAsync(
      brillo::dbus_utils::AsyncEventSequencer* sequencer) override {
    adaptor_ = std::make_unique<DBusAdaptor>(bus_);
    adaptor_->RegisterAsync(
        sequencer->GetHandler("RegisterAsync() failed.", true));
  }

 private:
  std::unique_ptr<DBusAdaptor> adaptor_;
};

}  // namespace

}  // namespace arc::data_migrator

int main(int argc, char** argv) {
  base::CommandLine::Init(argc, argv);
  brillo::InitLog(brillo::kLogToSyslog | brillo::kLogHeader |
                  brillo::kLogToStderrIfTty);

  return arc::data_migrator::Daemon().Run();
}
