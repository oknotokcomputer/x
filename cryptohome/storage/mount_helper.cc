// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/storage/mount_helper.h"

#include <sys/stat.h>

#include <memory>
#include <unordered_set>
#include <vector>

#include <base/bind.h>
#include <base/callback_helpers.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/strings/stringprintf.h>
#include <brillo/cryptohome.h>
#include <brillo/secure_blob.h>

#include "cryptohome/cryptohome_common.h"
#include "cryptohome/cryptohome_metrics.h"
#include "cryptohome/filesystem_layout.h"
#include "cryptohome/storage/homedirs.h"
#include "cryptohome/storage/mount_constants.h"

using base::FilePath;
using base::StringPrintf;
using brillo::cryptohome::home::GetRootPath;
using brillo::cryptohome::home::GetUserPath;
using brillo::cryptohome::home::SanitizeUserName;

namespace cryptohome {
namespace {

const char kEphemeralCryptohomeRootContext[] =
    "u:object_r:cros_home_shadow_uid:s0";
const int kDefaultEcryptfsKeySize = CRYPTOHOME_AES_KEY_BYTES;

FilePath GetUserEphemeralMountDirectory(
    const std::string& obfuscated_username) {
  return FilePath(kEphemeralCryptohomeDir)
      .Append(kEphemeralMountDir)
      .Append(obfuscated_username);
}

FilePath GetMountedEphemeralRootHomePath(
    const std::string& obfuscated_username) {
  return GetUserEphemeralMountDirectory(obfuscated_username)
      .Append(kRootHomeSuffix);
}

FilePath GetMountedEphemeralUserHomePath(
    const std::string& obfuscated_username) {
  return GetUserEphemeralMountDirectory(obfuscated_username)
      .Append(kUserHomeSuffix);
}

// Sets up the SELinux context for a freshly mounted ephemeral cryptohome.
bool SetUpSELinuxContextForEphemeralCryptohome(cryptohome::Platform* platform,
                                               const FilePath& source_path) {
  // Note that this is needed because the newly mounted ephemeral cryptohome is
  // a new file system, and thus the SELinux context that applies to the
  // mountpoint will not apply to the new root directory in the filesystem.
  return platform->SetSELinuxContext(source_path,
                                     kEphemeralCryptohomeRootContext);
}

constexpr mode_t kSkeletonSubDirMode = S_IRWXU | S_IRGRP | S_IXGRP;
constexpr mode_t kUserMountPointMode = S_IRWXU | S_IRGRP | S_IXGRP;
constexpr mode_t kRootMountPointMode = S_IRWXU;
constexpr mode_t kAccessMode = S_IRWXU | S_IRGRP | S_IXGRP;
constexpr mode_t kRootDirMode = S_IRWXU | S_IRWXG | S_ISVTX;

constexpr mode_t kTrackedDirMode = S_IRWXU;
constexpr mode_t kPathComponentDirMode = S_IRWXU;
constexpr mode_t kGroupWriteAccess = S_IWGRP;

struct DirectoryACL {
  base::FilePath path;
  mode_t mode;
  uid_t uid;
  gid_t gid;
};

std::vector<DirectoryACL> GetCacheSubdirectories(const FilePath& dir) {
  return std::vector<DirectoryACL>{
      {dir.Append(kUserHomeSuffix).Append(kGCacheDir), kAccessMode, kChronosUid,
       kChronosAccessGid},
      {dir.Append(kUserHomeSuffix).Append(kCacheDir), kTrackedDirMode,
       kChronosUid, kChronosGid},
      {dir.Append(kUserHomeSuffix)
           .Append(kGCacheDir)
           .Append(kGCacheVersion2Dir),
       kAccessMode | kGroupWriteAccess, kChronosUid, kChronosAccessGid}};
}

std::vector<DirectoryACL> GetCommonSubdirectories(const FilePath& dir) {
  auto result = std::vector<DirectoryACL>{
      {dir.Append(kRootHomeSuffix), kRootDirMode, kRootUid, kDaemonStoreGid},
      {dir.Append(kUserHomeSuffix), kAccessMode, kChronosUid,
       kChronosAccessGid},
      {dir.Append(kUserHomeSuffix).Append(kDownloadsDir), kAccessMode,
       kChronosUid, kChronosAccessGid},
      {dir.Append(kUserHomeSuffix).Append(kMyFilesDir), kAccessMode,
       kChronosUid, kChronosAccessGid},
      {dir.Append(kUserHomeSuffix).Append(kMyFilesDir).Append(kDownloadsDir),
       kAccessMode, kChronosUid, kChronosAccessGid},
  };
  auto cache_subdirs = GetCacheSubdirectories(dir);
  result.insert(result.end(), cache_subdirs.begin(), cache_subdirs.end());
  return result;
}

std::vector<DirectoryACL> GetDmcryptSubdirectories(const FilePath& dir) {
  auto data_volume_subdirs = GetCommonSubdirectories(dir.Append(kMountDir));
  auto cache_volume_subdirs =
      GetCacheSubdirectories(dir.Append(kDmcryptCacheDir));

  auto result = cache_volume_subdirs;
  result.insert(result.end(), data_volume_subdirs.begin(),
                data_volume_subdirs.end());
  return result;
}

// Returns true if the directory should be root owned, but is missing or has
// wrong attributes.
bool IsRootDirectoryAndTampered(Platform* platform, DirectoryACL dir) {
  if (dir.uid != kRootUid) {
    // Shouldn't be owned by root - ignore.
    return false;
  }

  base::stat_wrapper_t st;
  if (!platform->Stat(dir.path, &st)) {
    // Couldn't stat it, which means something is wrong, consider tampered.
    return true;
  }

  const mode_t st_mode = st.st_mode & 01777;
  if (S_ISDIR(st.st_mode) && st_mode == dir.mode && st.st_uid == dir.uid &&
      st.st_gid == dir.gid) {
    // Attributes are correct, not tampered
    return false;
  }

  LOG(ERROR) << "Root owned directory was tampered with, will be recreated.";
  return true;
}

void MaybeCorrectUserDirectoryAttrs(Platform* platform, DirectoryACL dir) {
  // Ignore root owned directories - those are recreated if they have wrong
  // attributes.
  if (dir.uid == kRootUid) {
    return;
  }
  // The check is intended to correct, report and fix a group mismatch for the
  // <vault> directories. It is initially required for crbug.com/1205308, but
  // since we are doing the chown anyway, there is no drama to do it for all
  // user directories.
  if (!platform->SafeDirChown(dir.path, dir.uid, dir.gid)) {
    LOG(ERROR) << "Failed to fix ownership of path directory" << dir.path;
  }

  // We make the mode for chronos-access accessible directories more
  // permissive, thus we need to change mode. It is unfortunate we need
  // to do it explicitly, unlike with mountpoints which we could just
  // recreate, but we must preserve user data while doing so.
  if (!platform->SafeDirChmod(dir.path, dir.mode)) {
    PLOG(ERROR) << "Failed to fix mode of path directory: " << dir.path;
  }
}

bool CreateVaultDirectoryStructure(
    Platform* platform, const std::vector<DirectoryACL>& directories) {
  bool success = true;
  for (const auto& subdir : directories) {
    if (platform->DirectoryExists(subdir.path) &&
        !IsRootDirectoryAndTampered(platform, subdir)) {
      MaybeCorrectUserDirectoryAttrs(platform, subdir);
      continue;
    }

    if (!platform->DeletePathRecursively(subdir.path)) {
      LOG(ERROR) << "Couldn't cleanup path element: " << subdir.path;
      success = false;
      continue;
    }

    if (!platform->SafeCreateDirAndSetOwnershipAndPermissions(
            subdir.path, subdir.mode, subdir.uid, subdir.gid)) {
      LOG(ERROR) << "Couldn't create path directory: " << subdir.path;
      ignore_result(platform->DeletePathRecursively(subdir.path));
      success = false;
      continue;
    }
    LOG(INFO) << "Created vault subdirectory: " << subdir.path;
  }
  return success;
}

bool SetTrackingXattr(Platform* platform,
                      const std::vector<DirectoryACL>& directories) {
  bool success = true;
  for (const auto& subdir : directories) {
    std::string name = subdir.path.BaseName().value();
    if (!platform->SetExtendedFileAttribute(subdir.path,
                                            kTrackedDirectoryNameAttribute,
                                            name.data(), name.length())) {
      PLOG(ERROR) << "Unable to set xattr on " << subdir.path;
      success = false;
      continue;
    }
  }
  return success;
}

}  // namespace

const char kDefaultHomeDir[] = "/home/chronos/user";

MountHelper::MountHelper(bool legacy_mount,
                         bool bind_mount_downloads,
                         Platform* platform)
    : legacy_mount_(legacy_mount),
      bind_mount_downloads_(bind_mount_downloads),
      platform_(platform) {}

// static
FilePath MountHelper::GetNewUserPath(const std::string& username) {
  std::string sanitized = SanitizeUserName(username);
  std::string user_dir = StringPrintf("u-%s", sanitized.c_str());
  return FilePath("/home")
      .Append(cryptohome::kDefaultSharedUser)
      .Append(user_dir);
}

FilePath MountHelper::GetMountedUserHomePath(
    const std::string& obfuscated_username) const {
  return GetUserMountDirectory(obfuscated_username).Append(kUserHomeSuffix);
}

FilePath MountHelper::GetMountedRootHomePath(
    const std::string& obfuscated_username) const {
  return GetUserMountDirectory(obfuscated_username).Append(kRootHomeSuffix);
}

bool MountHelper::EnsurePathComponent(const FilePath& check_path,
                                      uid_t uid,
                                      gid_t gid) const {
  base::stat_wrapper_t st;
  if (!platform_->Stat(check_path, &st)) {
    // Dirent not there, so create and set ownership.
    if (!platform_->SafeCreateDirAndSetOwnershipAndPermissions(
            check_path, kPathComponentDirMode, uid, gid)) {
      PLOG(ERROR) << "Can't create: " << check_path.value();
      return false;
    }
  } else {
    // Dirent there; make sure it's acceptable.
    if (!S_ISDIR(st.st_mode)) {
      LOG(ERROR) << "Non-directory path: " << check_path.value();
      return false;
    }
    if (st.st_uid != uid) {
      LOG(ERROR) << "Owner mismatch: " << check_path.value() << " " << st.st_uid
                 << " != " << uid;
      return false;
    }
    if (st.st_gid != gid) {
      LOG(ERROR) << "Group mismatch: " << check_path.value() << " " << st.st_gid
                 << " != " << gid;
      return false;
    }
    if (st.st_mode & S_IWOTH) {
      LOG(ERROR) << "Permissions too lenient: " << check_path.value() << " has "
                 << std::oct << st.st_mode;
      return false;
    }
  }
  return true;
}

bool MountHelper::EnsureMountPointPath(const FilePath& dir) const {
  std::vector<std::string> path_parts;
  dir.GetComponents(&path_parts);
  FilePath check_path(path_parts[0]);
  if (path_parts[0] != "/") {
    return false;
  }
  for (size_t i = 1; i < path_parts.size(); i++) {
    check_path = check_path.Append(path_parts[i]);
    if (!EnsurePathComponent(check_path, kRootUid, kRootGid)) {
      return false;
    }
  }
  return true;
}

bool MountHelper::EnsureUserMountPoints(const std::string& username) const {
  FilePath multi_home_user = GetUserPath(username);
  FilePath multi_home_root = GetRootPath(username);
  FilePath new_user_path = GetNewUserPath(username);

  if (platform_->DirectoryExists(multi_home_user) &&
      (platform_->IsDirectoryMounted(multi_home_user) ||
       !platform_->DeletePathRecursively(multi_home_user))) {
    PLOG(ERROR) << "Failed to remove mount point: " << multi_home_user.value();
    return false;
  }

  if (platform_->DirectoryExists(multi_home_root) &&
      (platform_->IsDirectoryMounted(multi_home_root) ||
       !platform_->DeletePathRecursively(multi_home_root))) {
    PLOG(ERROR) << "Failed to remove mount point: " << multi_home_root.value();
    return false;
  }

  if (platform_->DirectoryExists(new_user_path) &&
      (platform_->IsDirectoryMounted(new_user_path) ||
       !platform_->DeletePathRecursively(new_user_path))) {
    PLOG(ERROR) << "Failed to remove mount point: " << new_user_path.value();
    return false;
  }

  if (!EnsureMountPointPath(multi_home_user.DirName()) ||
      !EnsureMountPointPath(multi_home_root.DirName()) ||
      !EnsureMountPointPath(new_user_path.DirName().DirName()) ||
      !EnsurePathComponent(new_user_path.DirName(), kChronosUid, kChronosGid)) {
    LOG(ERROR) << "The paths to mountpoints are inconsistent";
    return false;
  }

  if (!platform_->SafeCreateDirAndSetOwnershipAndPermissions(
          multi_home_user, kUserMountPointMode, kChronosUid,
          kChronosAccessGid)) {
    PLOG(ERROR) << "Can't create: " << multi_home_user;
    return false;
  }

  if (!platform_->SafeCreateDirAndSetOwnershipAndPermissions(
          new_user_path, kUserMountPointMode, kChronosUid, kChronosAccessGid)) {
    PLOG(ERROR) << "Can't create: " << new_user_path;
    return false;
  }

  if (!platform_->SafeCreateDirAndSetOwnershipAndPermissions(
          multi_home_root, kRootMountPointMode, kRootUid, kRootGid)) {
    PLOG(ERROR) << "Can't create: " << multi_home_root;
    return false;
  }

  return true;
}

void MountHelper::RecursiveCopy(const FilePath& source,
                                const FilePath& destination) const {
  std::unique_ptr<cryptohome::FileEnumerator> file_enumerator(
      platform_->GetFileEnumerator(source, false, base::FileEnumerator::FILES));
  FilePath next_path;

  while (!(next_path = file_enumerator->Next()).empty()) {
    FilePath file_name = next_path.BaseName();

    FilePath destination_file = destination.Append(file_name);

    if (!platform_->Copy(next_path, destination_file) ||
        !platform_->SetOwnership(destination_file, kChronosUid, kChronosGid,
                                 false)) {
      LOG(ERROR) << "Couldn't change owner (" << kChronosUid << ":"
                 << kChronosGid
                 << ") of destination path: " << destination_file.value();
    }
  }

  std::unique_ptr<cryptohome::FileEnumerator> dir_enumerator(
      platform_->GetFileEnumerator(source, false,
                                   base::FileEnumerator::DIRECTORIES));

  while (!(next_path = dir_enumerator->Next()).empty()) {
    FilePath dir_name = FilePath(next_path).BaseName();

    FilePath destination_dir = destination.Append(dir_name);
    VLOG(1) << "RecursiveCopy: " << destination_dir.value();

    if (!platform_->SafeCreateDirAndSetOwnershipAndPermissions(
            destination_dir, kSkeletonSubDirMode, kChronosUid, kChronosGid)) {
      LOG(ERROR) << "SafeCreateDirAndSetOwnership() failed: "
                 << destination_dir.value();
    }

    RecursiveCopy(FilePath(next_path), destination_dir);
  }
}

void MountHelper::CopySkeleton(const FilePath& destination) const {
  RecursiveCopy(SkelDir(), destination);
}

bool MountHelper::IsFirstMountComplete(
    const std::string& obfuscated_username) const {
  const FilePath mount_point = GetUserMountDirectory(obfuscated_username);
  const FilePath user_home = GetMountedUserHomePath(obfuscated_username);

  // Generate the set of the top level nodes that a mount creates.
  std::unordered_set<FilePath> initial_nodes;
  for (const auto& dir : GetCommonSubdirectories(mount_point)) {
    initial_nodes.insert(dir.path);
  }
  std::unique_ptr<cryptohome::FileEnumerator> skel_enumerator(
      platform_->GetFileEnumerator(
          SkelDir(), false,
          base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES));
  for (FilePath next = skel_enumerator->Next(); !next.empty();
       next = skel_enumerator->Next()) {
    initial_nodes.insert(user_home.Append(next.BaseName()));
  }

  // If we have any nodes within the vault that are not in the set created
  // above - it means we have successfully entered a user session prior.
  std::unique_ptr<cryptohome::FileEnumerator> vault_enumerator(
      platform_->GetFileEnumerator(
          user_home, false,
          base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES));
  for (FilePath next = vault_enumerator->Next(); !next.empty();
       next = vault_enumerator->Next()) {
    if (initial_nodes.count(next) == 0) {
      // Found a file not from initial list, first mount was completed.
      // Log the file name to debug in case we ever see problems with
      // something racing the vault creation.
      LOG(INFO) << "Not a first mount, since found: " << next;
      return true;
    }
  }
  return false;
}

bool MountHelper::MountLegacyHome(const FilePath& from) {
  VLOG(1) << "MountLegacyHome from " << from.value();
  // Multiple mounts can't live on the legacy mountpoint.
  if (platform_->IsDirectoryMounted(FilePath(kDefaultHomeDir))) {
    LOG(INFO) << "Skipping binding to /home/chronos/user";
    return true;
  }

  if (!BindAndPush(from, FilePath(kDefaultHomeDir),
                   RemountOption::kMountsFlowIn))
    return false;

  return true;
}

bool MountHelper::HandleMyFilesDownloads(const base::FilePath& user_home) {
  const FilePath downloads = user_home.Append(kDownloadsDir);
  const FilePath downloads_in_myfiles =
      user_home.Append(kMyFilesDir).Append(kDownloadsDir);

  if (!bind_mount_downloads_) {
    MigrateDirectory(downloads_in_myfiles, downloads);
    return true;
  }

  // User could have saved files in MyFiles/Downloads in case cryptohome
  // crashed and bind mounts were removed by error. See crbug.com/1080730.
  // Move the files back to Download unless a file already exists.
  MigrateDirectory(downloads, downloads_in_myfiles);

  if (!BindAndPush(downloads, downloads_in_myfiles))
    return false;

  return true;
}

bool MountHelper::MountAndPush(const base::FilePath& src,
                               const base::FilePath& dest,
                               const std::string& type,
                               const std::string& options) {
  uint32_t mount_flags = kDefaultMountFlags | MS_NOSYMFOLLOW;

  if (!platform_->Mount(src, dest, type, mount_flags, options)) {
    PLOG(ERROR) << "Mount failed: " << src.value() << " -> " << dest.value();
    return false;
  }

  stack_.Push(src, dest);
  return true;
}

bool MountHelper::BindAndPush(const FilePath& src,
                              const FilePath& dest,
                              RemountOption remount) {
  if (!platform_->Bind(src, dest, remount, /*nosymfollow=*/true)) {
    std::string remount_strs[] = {"kNoRemount", "kPrivate", "kShared",
                                  "kMountsFlowIn", "kUnbindable"};
    PLOG(ERROR) << "Bind mount failed: " << src.value() << " -> "
                << dest.value()
                << " remount: " << remount_strs[static_cast<int>(remount)];
    return false;
  }

  stack_.Push(src, dest);
  return true;
}

bool MountHelper::MountDaemonStoreDirectories(
    const FilePath& root_home, const std::string& obfuscated_username) {
  // Iterate over all directories in /etc/daemon-store. This list is on rootfs,
  // so it's tamper-proof and nobody can sneak in additional directories that we
  // blindly mount. The actual mounts happen on /run/daemon-store, though.
  std::unique_ptr<cryptohome::FileEnumerator> file_enumerator(
      platform_->GetFileEnumerator(FilePath(kEtcDaemonStoreBaseDir),
                                   false /* recursive */,
                                   base::FileEnumerator::DIRECTORIES));

  // /etc/daemon-store/<daemon-name>
  FilePath etc_daemon_store_path;
  while (!(etc_daemon_store_path = file_enumerator->Next()).empty()) {
    const FilePath& daemon_name = etc_daemon_store_path.BaseName();

    // /run/daemon-store/<daemon-name>
    FilePath run_daemon_store_path =
        FilePath(kRunDaemonStoreBaseDir).Append(daemon_name);
    if (!platform_->DirectoryExists(run_daemon_store_path)) {
      // The chromeos_startup script should make sure this exist.
      PLOG(ERROR) << "Daemon store directory does not exist: "
                  << run_daemon_store_path.value();
      return false;
    }

    // /home/.shadow/<user_hash>/mount/root/<daemon-name>
    const FilePath mount_source = root_home.Append(daemon_name);

    // /run/daemon-store/<daemon-name>/<user_hash>
    const FilePath mount_target =
        run_daemon_store_path.Append(obfuscated_username);

    // Copy ownership from |etc_daemon_store_path| to |mount_source|. After the
    // bind operation, this guarantees that ownership for |mount_target| is the
    // same as for |etc_daemon_store_path| (usually
    // <daemon_user>:<daemon_group>), which is what the daemon intended.
    // Otherwise, it would end up being root-owned.
    base::stat_wrapper_t etc_daemon_path_stat =
        file_enumerator->GetInfo().stat();

    // TODO(dlunev): add some reporting when we see ACL mismatch.
    if (!platform_->DirectoryExists(mount_source) &&
        !platform_->SafeCreateDirAndSetOwnershipAndPermissions(
            mount_source, etc_daemon_path_stat.st_mode,
            etc_daemon_path_stat.st_uid, etc_daemon_path_stat.st_gid)) {
      LOG(ERROR) << "Failed to create directory " << mount_source.value();
      return false;
    }

    // The target directory's parent exists in the root mount namespace so the
    // directory itself can be created in the root mount namespace and it will
    // be visible in all namespaces.
    if (!platform_->CreateDirectory(mount_target)) {
      PLOG(ERROR) << "Failed to create directory " << mount_target.value();
      return false;
    }

    // Assuming that |run_daemon_store_path| is a shared mount and the daemon
    // runs in a file system namespace with |run_daemon_store_path| mounted as
    // secondary, this mount event propagates into the daemon.
    if (!BindAndPush(mount_source, mount_target))
      return false;
  }

  return true;
}

void MountHelper::MigrateDirectory(const base::FilePath& dst,
                                   const base::FilePath& src) const {
  VLOG(1) << "Migrating directory " << src << " -> " << dst;
  std::unique_ptr<cryptohome::FileEnumerator> enumerator(
      platform_->GetFileEnumerator(
          src, false /* recursive */,
          base::FileEnumerator::DIRECTORIES | base::FileEnumerator::FILES));
  for (base::FilePath src_obj = enumerator->Next(); !src_obj.empty();
       src_obj = enumerator->Next()) {
    base::FilePath dst_obj = dst.Append(src_obj.BaseName());

    // If the destination file exists, or rename failed for whatever reason,
    // then log a warning and delete the source file.
    if (platform_->FileExists(dst_obj) ||
        !platform_->Rename(src_obj, dst_obj)) {
      LOG(WARNING) << "Failed to migrate " << src_obj << " : deleting";
      platform_->DeletePathRecursively(src_obj);
    }
  }
}

bool MountHelper::MountHomesAndDaemonStores(
    const std::string& username,
    const std::string& obfuscated_username,
    const FilePath& user_home,
    const FilePath& root_home) {
  // Bind mount user directory as a shared bind mount.
  // This allows us to set up user mounts as subsidiary mounts without needing
  // to replicate that across multiple mount points.
  if (!BindAndPush(user_home, user_home, RemountOption::kShared))
    return false;

  // Mount /home/chronos/user.
  if (legacy_mount_ && !MountLegacyHome(user_home))
    return false;

  // Mount /home/chronos/u-<user_hash>
  const FilePath new_user_path = GetNewUserPath(username);
  if (!BindAndPush(user_home, new_user_path, RemountOption::kMountsFlowIn))
    return false;

  // Mount /home/user/<user_hash>.
  const FilePath user_multi_home = GetUserPath(username);
  if (!BindAndPush(user_home, user_multi_home, RemountOption::kMountsFlowIn))
    return false;

  // Mount /home/root/<user_hash>.
  const FilePath root_multi_home = GetRootPath(username);
  if (!BindAndPush(root_home, root_multi_home, RemountOption::kMountsFlowIn))
    return false;

  // Mount Downloads to MyFiles/Downloads in the user shadow directory.
  if (!HandleMyFilesDownloads(user_home)) {
    return false;
  }

  // Mount directories used by daemons to store per-user data.
  if (!MountDaemonStoreDirectories(root_home, obfuscated_username))
    return false;

  return true;
}

bool MountHelper::MountCacheSubdirectories(
    const std::string& obfuscated_username,
    const base::FilePath& data_directory) {
  FilePath cache_directory = GetDmcryptUserCacheDirectory(obfuscated_username);

  const FilePath tracked_subdir_paths[] = {
      FilePath(kUserHomeSuffix).Append(kCacheDir),
      FilePath(kUserHomeSuffix).Append(kGCacheDir)};

  for (const auto& tracked_dir : tracked_subdir_paths) {
    FilePath src_dir = cache_directory.Append(tracked_dir);
    FilePath dst_dir = data_directory.Append(tracked_dir);

    if (!BindAndPush(src_dir, dst_dir, RemountOption::kMountsFlowIn)) {
      LOG(ERROR) << "Failed to bind mount " << src_dir;
      return false;
    }
  }

  return true;
}

// The eCryptfs mount is mounted from vault/ --> mount/ except in case of
// migration where the mount point is a temporary directory.
bool MountHelper::SetUpEcryptfsMount(const std::string& obfuscated_username,
                                     const std::string& fek_signature,
                                     const std::string& fnek_signature,
                                     const FilePath& mount_point) {
  const FilePath vault_path = GetEcryptfsUserVaultPath(obfuscated_username);

  // Specify the ecryptfs options for mounting the user's cryptohome.
  std::string ecryptfs_options = StringPrintf(
      "ecryptfs_cipher=aes"
      ",ecryptfs_key_bytes=%d"
      ",ecryptfs_fnek_sig=%s"
      ",ecryptfs_sig=%s"
      ",ecryptfs_unlink_sigs",
      kDefaultEcryptfsKeySize, fnek_signature.c_str(), fek_signature.c_str());

  // Create <vault_path>/user and <vault_path>/root.
  ignore_result(CreateVaultDirectoryStructure(
      platform_, GetCommonSubdirectories(vault_path)));

  // b/115997660: Mount eCryptfs after creating the tracked subdirectories.
  if (!MountAndPush(vault_path, mount_point, "ecryptfs", ecryptfs_options)) {
    LOG(ERROR) << "eCryptfs mount failed";
    return false;
  }

  return true;
}

void MountHelper::SetUpDircryptoMount(const std::string& obfuscated_username) {
  const FilePath mount_point = GetUserMountDirectory(obfuscated_username);

  ignore_result(CreateVaultDirectoryStructure(
      platform_, GetCommonSubdirectories(mount_point)));
  ignore_result(
      SetTrackingXattr(platform_, GetCommonSubdirectories(mount_point)));
}

bool MountHelper::SetUpDmcryptMount(const std::string& obfuscated_username,
                                    const base::FilePath& data_mount_point) {
  const FilePath dmcrypt_data_volume =
      GetDmcryptDataVolume(obfuscated_username);
  const FilePath dmcrypt_cache_volume =
      GetDmcryptCacheVolume(obfuscated_username);

  const FilePath cache_mount_point =
      GetDmcryptUserCacheDirectory(obfuscated_username);

  // Mount the data volume at <vault>/mount and the cache volume at
  // <vault>/cache. The directories are set up by the creation code.
  if (!MountAndPush(dmcrypt_data_volume, data_mount_point,
                    kDmcryptContainerMountType,
                    kDmcryptContainerMountOptions)) {
    LOG(ERROR) << "Failed to mount dmcrypt data volume";
    return false;
  }

  if (!MountAndPush(dmcrypt_cache_volume, cache_mount_point,
                    kDmcryptContainerMountType,
                    kDmcryptContainerMountOptions)) {
    LOG(ERROR) << "Failed to mount dmcrypt cache volume";
    return false;
  }

  ignore_result(CreateVaultDirectoryStructure(
      platform_, GetDmcryptSubdirectories(UserPath(obfuscated_username))));

  return true;
}

MountError MountHelper::PerformMount(MountType mount_type,
                                     const std::string& username,
                                     const std::string& fek_signature,
                                     const std::string& fnek_signature) {
  const std::string obfuscated_username = SanitizeUserName(username);

  if (!EnsureUserMountPoints(username)) {
    LOG(ERROR) << "Error creating mountpoint.";
    return MOUNT_ERROR_CREATE_CRYPTOHOME_FAILED;
  }

  // Since Service::Mount cleans up stale mounts, we should only reach
  // this point if someone attempts to re-mount an in-use mount point.
  if (platform_->IsDirectoryMounted(
          GetUserMountDirectory(obfuscated_username))) {
    LOG(ERROR) << "Mount point is busy: "
               << GetUserMountDirectory(obfuscated_username);
    return MOUNT_ERROR_FATAL;
  }

  switch (mount_type) {
    case MountType::ECRYPTFS:
      if (!SetUpEcryptfsMount(obfuscated_username, fek_signature,
                              fnek_signature,
                              GetUserMountDirectory(obfuscated_username))) {
        return MOUNT_ERROR_MOUNT_ECRYPTFS_FAILED;
      }
      break;
    case MountType::ECRYPTFS_TO_DIR_CRYPTO:
      if (!SetUpEcryptfsMount(
              obfuscated_username, fek_signature, fnek_signature,
              GetUserTemporaryMountDirectory(obfuscated_username))) {
        return MOUNT_ERROR_MOUNT_ECRYPTFS_FAILED;
      }
      SetUpDircryptoMount(obfuscated_username);
      return MOUNT_ERROR_NONE;
    case MountType::ECRYPTFS_TO_DMCRYPT:
      if (!SetUpEcryptfsMount(
              obfuscated_username, fek_signature, fnek_signature,
              GetUserTemporaryMountDirectory(obfuscated_username))) {
        return MOUNT_ERROR_MOUNT_ECRYPTFS_FAILED;
      }
      if (!SetUpDmcryptMount(obfuscated_username,
                             GetUserMountDirectory(obfuscated_username)) ||
          !MountCacheSubdirectories(
              obfuscated_username,
              GetUserMountDirectory(obfuscated_username))) {
        LOG(ERROR) << "Dm-crypt mount failed";
        return MOUNT_ERROR_MOUNT_DMCRYPT_FAILED;
      }
      return MOUNT_ERROR_NONE;
    case MountType::DIR_CRYPTO:
      SetUpDircryptoMount(obfuscated_username);
      break;
    case MountType::DIR_CRYPTO_TO_DMCRYPT:
      SetUpDircryptoMount(obfuscated_username);
      if (!SetUpDmcryptMount(
              obfuscated_username,
              GetUserTemporaryMountDirectory(obfuscated_username)) ||
          !MountCacheSubdirectories(
              obfuscated_username,
              GetUserTemporaryMountDirectory(obfuscated_username))) {
        LOG(ERROR) << "Dm-crypt mount failed";
        return MOUNT_ERROR_MOUNT_DMCRYPT_FAILED;
      }
      return MOUNT_ERROR_NONE;
    case MountType::DMCRYPT:
      if (!SetUpDmcryptMount(obfuscated_username,
                             GetUserMountDirectory(obfuscated_username))) {
        LOG(ERROR) << "Dm-crypt mount failed";
        return MOUNT_ERROR_MOUNT_DMCRYPT_FAILED;
      }
      break;
    case MountType::EPHEMERAL:
    case MountType::NONE:
      NOTREACHED();
  }

  const FilePath user_home = GetMountedUserHomePath(obfuscated_username);
  const FilePath root_home = GetMountedRootHomePath(obfuscated_username);

  if (!IsFirstMountComplete(obfuscated_username)) {
    CopySkeleton(user_home);
  }

  // When migrating, it's better to avoid exposing the new ext4 crypto dir.
  if (!MountHomesAndDaemonStores(username, obfuscated_username, user_home,
                                 root_home)) {
    return MOUNT_ERROR_MOUNT_HOMES_AND_DAEMON_STORES_FAILED;
  }

  // TODO(sarthakkukreti): This can't be moved due to child mount propagation
  // issues. Figure out how to make it propagate properly to move to the switch
  // above.
  if (mount_type == MountType::DMCRYPT &&
      !MountCacheSubdirectories(obfuscated_username,
                                GetUserMountDirectory(obfuscated_username))) {
    LOG(ERROR)
        << "Failed to mount tracked subdirectories from the cache volume";
    return MOUNT_ERROR_MOUNT_DMCRYPT_FAILED;
  }

  return MOUNT_ERROR_NONE;
}

// TODO(dlunev): make specific errors returned. MOUNT_ERROR_FATAL for now
// to preserve the existing expectations..
MountError MountHelper::PerformEphemeralMount(
    const std::string& username, const FilePath& ephemeral_loop_device) {
  const std::string obfuscated_username = SanitizeUserName(username);
  const FilePath mount_point =
      GetUserEphemeralMountDirectory(obfuscated_username);
  LOG(ERROR) << "Directory is" << mount_point.value();

  if (!platform_->CreateDirectory(mount_point)) {
    PLOG(ERROR) << "Directory creation failed for " << mount_point.value();
    return MOUNT_ERROR_FATAL;
  }
  if (!MountAndPush(ephemeral_loop_device, mount_point, kEphemeralMountType,
                    kEphemeralMountOptions)) {
    LOG(ERROR) << "Can't mount ephemeral mount point";
    return MOUNT_ERROR_FATAL;
  }

  // Set SELinux context first, so that the created user & root directory have
  // the correct context.
  if (!SetUpSELinuxContextForEphemeralCryptohome(platform_, mount_point)) {
    // Logging already done in SetUpSELinuxContextForEphemeralCryptohome.
    return MOUNT_ERROR_FATAL;
  }

  if (!EnsureUserMountPoints(username)) {
    return MOUNT_ERROR_FATAL;
  }

  const FilePath user_home =
      GetMountedEphemeralUserHomePath(obfuscated_username);

  const FilePath root_home =
      GetMountedEphemeralRootHomePath(obfuscated_username);

  if (!CreateVaultDirectoryStructure(platform_,
                                     GetCommonSubdirectories(mount_point))) {
    return MOUNT_ERROR_FATAL;
  }

  CopySkeleton(user_home);

  if (!MountHomesAndDaemonStores(username, obfuscated_username, user_home,
                                 root_home)) {
    return MOUNT_ERROR_FATAL;
  }

  return MOUNT_ERROR_NONE;
}

void MountHelper::UnmountAll() {
  FilePath src, dest;
  while (stack_.Pop(&src, &dest)) {
    ForceUnmount(src, dest);
  }

  // Clean up destination directory for ephemeral loop device mounts.
  const FilePath ephemeral_mount_path =
      FilePath(kEphemeralCryptohomeDir).Append(kEphemeralMountDir);
  platform_->DeletePathRecursively(ephemeral_mount_path);
}

void MountHelper::ForceUnmount(const FilePath& src, const FilePath& dest) {
  // Try an immediate unmount.
  bool was_busy;
  if (!platform_->Unmount(dest, false, &was_busy)) {
    LOG(ERROR) << "Couldn't unmount '" << dest.value()
               << "' immediately, was_busy=" << std::boolalpha << was_busy;
    // Failed to unmount immediately, do a lazy unmount.  If |was_busy| we also
    // want to sync before the unmount to help prevent data loss.
    if (was_busy)
      platform_->SyncDirectory(dest);
    platform_->LazyUnmount(dest);
    platform_->SyncDirectory(src);
  }
}

bool MountHelper::CanPerformEphemeralMount() const {
  return !MountPerformed();
}

bool MountHelper::MountPerformed() const {
  return stack_.size() > 0;
}

bool MountHelper::IsPathMounted(const base::FilePath& path) const {
  return stack_.ContainsDest(path);
}

std::vector<base::FilePath> MountHelper::MountedPaths() const {
  return stack_.MountDestinations();
}

}  // namespace cryptohome
