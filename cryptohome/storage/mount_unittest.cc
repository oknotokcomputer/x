// Copyright 2013 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Unit tests for Mount.

#include "cryptohome/storage/mount.h"

#include <map>
#include <memory>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <pwd.h>
#include <regex>  // NOLINT(build/c++11)
#include <stdlib.h>
#include <string.h>  // For memset(), memcpy()
#include <sys/types.h>
#include <utility>
#include <vector>

#include <base/check.h>
#include <base/check_op.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/functional/bind.h>
#include <base/functional/callback_helpers.h>
#include <base/logging.h>
#include <base/stl_util.h>
#include <base/strings/string_number_conversions.h>
#include <base/time/time.h>
#include <brillo/cryptohome.h>
#include <brillo/process/process_mock.h>
#include <brillo/secure_blob.h>
#include <chromeos/constants/cryptohome.h>
#include <gtest/gtest.h>
#include <libhwsec-foundation/crypto/secure_blob_util.h>
#include <libhwsec-foundation/error/testing_helper.h>
#include <policy/libpolicy.h>

#include "cryptohome/filesystem_layout.h"
#include "cryptohome/mock_keyset_management.h"
#include "cryptohome/mock_platform.h"
#include "cryptohome/mock_vault_keyset.h"
#include "cryptohome/storage/encrypted_container/encrypted_container.h"
#include "cryptohome/storage/encrypted_container/fake_backing_device.h"
#include "cryptohome/storage/encrypted_container/fake_encrypted_container_factory.h"
#include "cryptohome/storage/error_test_helpers.h"
#include "cryptohome/storage/file_system_keyset.h"
#include "cryptohome/storage/homedirs.h"
#include "cryptohome/storage/keyring/fake_keyring.h"
#include "cryptohome/storage/mount_constants.h"
#include "cryptohome/username.h"
#include "cryptohome/vault_keyset.h"

namespace cryptohome {

using base::FilePath;
using brillo::SecureBlob;
using hwsec_foundation::SecureBlobToHex;

using ::cryptohome::storage::testing::IsError;
using ::hwsec_foundation::error::testing::IsOk;
using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::InSequence;
using ::testing::NiceMock;
using ::testing::Pointee;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StartsWith;

namespace {

struct Attributes {
  mode_t mode;
  uid_t uid;
  gid_t gid;
};

// TODO(hardigoyal, b:290897808): Cleanup the rest of this file, there is
// duplicated mount_helper_unittest.cc. This would require migrating tests
// to just use MountHelper and not Mount+MountHelper.
constexpr char kEtc[] = "/etc";
constexpr char kEtcSkel[] = "/etc/skel";
constexpr char kEtcDaemonStore[] = "/etc/daemon-store";

constexpr char kRun[] = "/run";
constexpr char kRunCryptohome[] = "/run/cryptohome";
constexpr char kRunDaemonStore[] = "/run/daemon-store";
constexpr char kRunDaemonStoreCache[] = "/run/daemon-store-cache";

constexpr char kHome[] = "/home";
constexpr char kHomeChronos[] = "/home/chronos";
constexpr char kHomeChronosUser[] = "/home/chronos/user";
constexpr char kHomeUser[] = "/home/user";
constexpr char kHomeRoot[] = "/home/root";

constexpr char kDir1[] = "dir1";
constexpr char kFile1[] = "file1";
constexpr char kDir1File2[] = "dir1/file2";
constexpr char kDir1Dir2[] = "dir1/dir2";
constexpr char kDir1Dir2File3[] = "dir1/dir2/file3";

constexpr char kFile1Content[] = "content1";
constexpr char kDir1File2Content[] = "content2";
constexpr char kDir1Dir2File3Content[] = "content3";

constexpr char kSomeDaemon[] = "some_daemon";
constexpr Attributes kSomeDaemonAttributes{01735, 12, 27};
constexpr char kAnotherDaemon[] = "another_daemon";
constexpr Attributes kAnotherDaemonAttributes{0600, 0, 0};

constexpr char kDevLoopPrefix[] = "/dev/loop";

MATCHER_P(DirCryptoReferenceMatcher, reference, "") {
  if (reference.reference != arg.reference) {
    return false;
  }
  if (reference.policy_version != arg.policy_version) {
    return false;
  }
  return true;
}

base::FilePath ChronosHashPath(const Username& username) {
  const ObfuscatedUsername obfuscated_username =
      brillo::cryptohome::home::SanitizeUserName(username);
  return base::FilePath(kHomeChronos)
      .Append(base::StringPrintf("u-%s", obfuscated_username->c_str()));
}

void PrepareDirectoryStructure(Platform* platform) {
  // Create environment as defined in
  // src/platform2/cryptohome/tmpfiles.d/cryptohome.conf
  ASSERT_TRUE(platform->SafeCreateDirAndSetOwnershipAndPermissions(
      base::FilePath(kRun), 0755, kRootUid, kRootGid));
  ASSERT_TRUE(platform->SafeCreateDirAndSetOwnershipAndPermissions(
      base::FilePath(kRunCryptohome), 0700, kRootUid, kRootGid));
  ASSERT_TRUE(platform->SafeCreateDirAndSetOwnershipAndPermissions(
      base::FilePath(kRunDaemonStore), 0755, kRootUid, kRootGid));
  ASSERT_TRUE(platform->SafeCreateDirAndSetOwnershipAndPermissions(
      base::FilePath(kRunDaemonStoreCache), 0755, kRootUid, kRootGid));
  ASSERT_TRUE(platform->SafeCreateDirAndSetOwnershipAndPermissions(
      base::FilePath(kHome), 0755, kRootUid, kRootGid));
  ASSERT_TRUE(platform->SafeCreateDirAndSetOwnershipAndPermissions(
      base::FilePath(kHomeChronos), 0755, kChronosUid, kChronosGid));
  ASSERT_TRUE(platform->SafeCreateDirAndSetOwnershipAndPermissions(
      base::FilePath(kHomeChronosUser), 01755, kChronosUid, kChronosGid));
  ASSERT_TRUE(platform->SafeCreateDirAndSetOwnershipAndPermissions(
      base::FilePath(kHomeUser), 0755, kRootUid, kRootGid));
  ASSERT_TRUE(platform->SafeCreateDirAndSetOwnershipAndPermissions(
      base::FilePath(kHomeRoot), 01751, kRootUid, kRootGid));

  // Setup some skel directories to make sure they are copied over.
  // TODO(dlunev): for now setting permissions is useless, for the code
  // relies on Copy to copy it over for files, meaning we can't intercept it.
  // It can be fixed by setting permissions explicitly in RecursiveCopy.
  ASSERT_TRUE(platform->CreateDirectory(base::FilePath(kEtc)));
  ASSERT_TRUE(platform->CreateDirectory(base::FilePath(kEtcSkel)));
  ASSERT_TRUE(
      platform->CreateDirectory(base::FilePath(kEtcSkel).Append(kDir1)));
  ASSERT_TRUE(platform->WriteStringToFile(
      base::FilePath(kEtcSkel).Append(kFile1), kFile1Content));
  ASSERT_TRUE(platform->WriteStringToFile(
      base::FilePath(kEtcSkel).Append(kDir1File2), kDir1File2Content));
  ASSERT_TRUE(
      platform->CreateDirectory(base::FilePath(kEtcSkel).Append(kDir1Dir2)));
  ASSERT_TRUE(platform->WriteStringToFile(
      base::FilePath(kEtcSkel).Append(kDir1Dir2File3), kDir1Dir2File3Content));

  // Setup daemon-store templates
  ASSERT_TRUE(platform->CreateDirectory(base::FilePath(kEtcDaemonStore)));
  ASSERT_TRUE(platform->SafeCreateDirAndSetOwnershipAndPermissions(
      base::FilePath(kEtcDaemonStore).Append(kSomeDaemon),
      kSomeDaemonAttributes.mode, kSomeDaemonAttributes.uid,
      kSomeDaemonAttributes.gid));
  ASSERT_TRUE(platform->SafeCreateDirAndSetOwnershipAndPermissions(
      base::FilePath(kEtcDaemonStore).Append(kAnotherDaemon),
      kAnotherDaemonAttributes.mode, kAnotherDaemonAttributes.uid,
      kAnotherDaemonAttributes.gid));
  ASSERT_TRUE(platform->CreateDirectory(
      base::FilePath(kRunDaemonStore).Append(kSomeDaemon)));
  ASSERT_TRUE(platform->CreateDirectory(
      base::FilePath(kRunDaemonStoreCache).Append(kSomeDaemon)));
  ASSERT_TRUE(platform->CreateDirectory(
      base::FilePath(kRunDaemonStore).Append(kAnotherDaemon)));
  ASSERT_TRUE(platform->CreateDirectory(
      base::FilePath(kRunDaemonStoreCache).Append(kAnotherDaemon)));
}

void CheckExistanceAndPermissions(Platform* platform,
                                  const base::FilePath& path,
                                  mode_t expected_mode,
                                  uid_t expected_uid,
                                  gid_t expected_gid,
                                  bool expect_present) {
  ASSERT_THAT(platform->FileExists(path), expect_present)
      << "PATH: " << path.value();

  if (!expect_present) {
    return;
  }

  mode_t mode;
  uid_t uid;
  gid_t gid;

  ASSERT_THAT(platform->GetOwnership(path, &uid, &gid, false), true)
      << "PATH: " << path.value();
  ASSERT_THAT(platform->GetPermissions(path, &mode), true)
      << "PATH: " << path.value();

  ASSERT_THAT(mode, expected_mode) << "PATH: " << path.value();
  ASSERT_THAT(uid, expected_uid) << "PATH: " << path.value();
  ASSERT_THAT(gid, expected_gid) << "PATH: " << path.value();
}

void CheckRootAndDaemonStoreMounts(Platform* platform,
                                   const Username& username,
                                   const base::FilePath& vault_mount_point,
                                   bool expect_present) {
  const ObfuscatedUsername obfuscated_username =
      brillo::cryptohome::home::SanitizeUserName(username);
  const std::multimap<const base::FilePath, const base::FilePath>
      expected_root_mount_map{
          {
              vault_mount_point.Append(kRootHomeSuffix),
              vault_mount_point.Append(kRootHomeSuffix),
          },
          {
              vault_mount_point.Append(kRootHomeSuffix),
              brillo::cryptohome::home::GetRootPath(username),
          },
          {
              vault_mount_point.Append(kRootHomeSuffix).Append(kSomeDaemon),
              base::FilePath(kRunDaemonStore)
                  .Append(kSomeDaemon)
                  .Append(*obfuscated_username),
          },
          {
              vault_mount_point.Append(kRootHomeSuffix)
                  .Append(kDaemonStoreCacheDir)
                  .Append(kSomeDaemon),
              base::FilePath(kRunDaemonStoreCache)
                  .Append(kSomeDaemon)
                  .Append(*obfuscated_username),
          },
          {
              vault_mount_point.Append(kRootHomeSuffix).Append(kAnotherDaemon),
              base::FilePath(kRunDaemonStore)
                  .Append(kAnotherDaemon)
                  .Append(*obfuscated_username),
          },
          {
              vault_mount_point.Append(kRootHomeSuffix)
                  .Append(kDaemonStoreCacheDir)
                  .Append(kAnotherDaemon),
              base::FilePath(kRunDaemonStoreCache)
                  .Append(kAnotherDaemon)
                  .Append(*obfuscated_username),
          },
      };
  std::multimap<const base::FilePath, const base::FilePath> root_mount_map;

  ASSERT_THAT(platform->IsDirectoryMounted(
                  brillo::cryptohome::home::GetRootPath(username)),
              expect_present);
  if (expect_present) {
    ASSERT_TRUE(platform->GetMountsBySourcePrefix(
        vault_mount_point.Append(kRootHomeSuffix), &root_mount_map));
    ASSERT_THAT(root_mount_map,
                ::testing::UnorderedElementsAreArray(expected_root_mount_map));
  }
  CheckExistanceAndPermissions(platform,
                               vault_mount_point.Append(kRootHomeSuffix), 01770,
                               kRootUid, kDaemonStoreGid, expect_present);
  CheckExistanceAndPermissions(
      platform, vault_mount_point.Append(kRootHomeSuffix).Append(kSomeDaemon),
      kSomeDaemonAttributes.mode, kSomeDaemonAttributes.uid,
      kSomeDaemonAttributes.gid, expect_present);
  CheckExistanceAndPermissions(
      platform,
      vault_mount_point.Append(kRootHomeSuffix).Append(kAnotherDaemon),
      kAnotherDaemonAttributes.mode, kAnotherDaemonAttributes.uid,
      kAnotherDaemonAttributes.gid, expect_present);

  if (expect_present) {
    // TODO(dlunev): make this directories to go away on unmount.
    ASSERT_THAT(platform->DirectoryExists(base::FilePath(kRunDaemonStore)
                                              .Append(kSomeDaemon)
                                              .Append(*obfuscated_username)),
                expect_present);
    ASSERT_THAT(platform->DirectoryExists(base::FilePath(kRunDaemonStoreCache)
                                              .Append(kSomeDaemon)
                                              .Append(*obfuscated_username)),
                expect_present);
    ASSERT_THAT(platform->DirectoryExists(base::FilePath(kRunDaemonStore)
                                              .Append(kAnotherDaemon)
                                              .Append(*obfuscated_username)),
                expect_present);
    ASSERT_THAT(platform->DirectoryExists(base::FilePath(kRunDaemonStoreCache)
                                              .Append(kAnotherDaemon)
                                              .Append(*obfuscated_username)),
                expect_present);
    CheckExistanceAndPermissions(
        platform, brillo::cryptohome::home::GetRootPath(username), 01770,
        kRootUid, kDaemonStoreGid, expect_present);
  }
}

void CheckUserMountPoints(Platform* platform,
                          const Username& username,
                          const base::FilePath& vault_mount_point,
                          bool expect_present,
                          bool downloads_bind_mount = true) {
  const base::FilePath chronos_hash_user_mount_point =
      ChronosHashPath(username);

  std::multimap<const base::FilePath, const base::FilePath>
      expected_user_mount_map{
          {vault_mount_point.Append(kUserHomeSuffix),
           vault_mount_point.Append(kUserHomeSuffix)},
          {vault_mount_point.Append(kUserHomeSuffix),
           brillo::cryptohome::home::GetUserPath(username)},
          {vault_mount_point.Append(kUserHomeSuffix),
           chronos_hash_user_mount_point},
          {vault_mount_point.Append(kUserHomeSuffix),
           base::FilePath(kHomeChronosUser)},
      };

  if (downloads_bind_mount) {
    expected_user_mount_map.insert(
        {vault_mount_point.Append(kUserHomeSuffix).Append(kDownloadsDir),
         vault_mount_point.Append(kUserHomeSuffix)
             .Append(kMyFilesDir)
             .Append(kDownloadsDir)});
  }
  std::multimap<const base::FilePath, const base::FilePath> user_mount_map;

  ASSERT_THAT(platform->IsDirectoryMounted(base::FilePath(kHomeChronosUser)),
              expect_present);
  ASSERT_THAT(platform->IsDirectoryMounted(
                  brillo::cryptohome::home::GetUserPath(username)),
              expect_present);
  ASSERT_THAT(platform->IsDirectoryMounted(chronos_hash_user_mount_point),
              expect_present);

  ASSERT_THAT(
      platform->IsDirectoryMounted(vault_mount_point.Append(kUserHomeSuffix)
                                       .Append(kMyFilesDir)
                                       .Append(kDownloadsDir)),
      expect_present && downloads_bind_mount);
  if (expect_present) {
    ASSERT_TRUE(platform->GetMountsBySourcePrefix(
        vault_mount_point.Append(kUserHomeSuffix), &user_mount_map));
    ASSERT_THAT(user_mount_map,
                ::testing::UnorderedElementsAreArray(expected_user_mount_map));
  }
}

void CheckUserMountPaths(Platform* platform,
                         const base::FilePath& base_path,
                         bool expect_present,
                         bool downloads_bind_mount) {
  // The path itself.
  // TODO(dlunev): the mount paths should be cleaned up upon unmount.
  if (expect_present) {
    CheckExistanceAndPermissions(platform, base_path, 0750, kChronosUid,
                                 kChronosAccessGid, expect_present);
  }

  // Subdirectories
  if (downloads_bind_mount) {
    CheckExistanceAndPermissions(platform, base_path.Append(kDownloadsDir),
                                 0750, kChronosUid, kChronosAccessGid,
                                 expect_present);
  } else {
    ASSERT_FALSE(platform->DirectoryExists(base_path.Append(kDownloadsDir)));
  }

  CheckExistanceAndPermissions(platform, base_path.Append(kMyFilesDir), 0750,
                               kChronosUid, kChronosAccessGid, expect_present);

  CheckExistanceAndPermissions(
      platform, base_path.Append(kMyFilesDir).Append(kDownloadsDir), 0750,
      kChronosUid, kChronosAccessGid, expect_present);

  CheckExistanceAndPermissions(platform, base_path.Append(kCacheDir), 0700,
                               kChronosUid, kChronosGid, expect_present);

  CheckExistanceAndPermissions(platform, base_path.Append(kGCacheDir), 0750,
                               kChronosUid, kChronosAccessGid, expect_present);

  CheckExistanceAndPermissions(
      platform, base_path.Append(kGCacheDir).Append(kGCacheVersion2Dir), 0770,
      kChronosUid, kChronosAccessGid, expect_present);
}

void CheckSkel(Platform* platform,
               const base::FilePath& base_path,
               bool expect_present) {
  // Presence
  // TODO(dlunev) unfortunately we can not verify if Copy correctly deals with
  // the attributes, because it actually deals with those at the point where
  // we can not intercept it. We can make that explicit by setting those in
  // the copy skel itself.
  CheckExistanceAndPermissions(platform, base_path.Append(kDir1), 0750,
                               kChronosUid, kChronosGid, expect_present);
  CheckExistanceAndPermissions(
      platform, base_path.Append(kFile1),
      0750,  // NOT A PART OF THE CONTRACT, SEE TODO ABOVE.
      kChronosUid, kChronosGid, expect_present);
  CheckExistanceAndPermissions(platform, base_path.Append(kDir1Dir2), 0750,
                               kChronosUid, kChronosGid, expect_present);
  CheckExistanceAndPermissions(
      platform, base_path.Append(kDir1File2),
      0750,  // NOT A PART OF THE CONTRACT, SEE TODO ABOVE.
      kChronosUid, kChronosGid, expect_present);
  CheckExistanceAndPermissions(
      platform, base_path.Append(kDir1Dir2File3),
      0750,  // NOT A PART OF THE CONTRACT, SEE TODO ABOVE.
      kChronosUid, kChronosGid, expect_present);

  // Content
  if (expect_present) {
    std::string result;
    ASSERT_TRUE(platform->ReadFileToString(base_path.Append(kFile1), &result));
    ASSERT_THAT(result, kFile1Content);
    ASSERT_TRUE(
        platform->ReadFileToString(base_path.Append(kDir1File2), &result));
    ASSERT_THAT(result, kDir1File2Content);
    ASSERT_TRUE(
        platform->ReadFileToString(base_path.Append(kDir1Dir2File3), &result));
    ASSERT_THAT(result, kDir1Dir2File3Content);
  }
}

}  // namespace

// TODO(dlunev): add test ecryptfs blasts "mount".
class PersistentSystemTest : public ::testing::Test {
 public:
  const Username kUser{"someuser"};

  PersistentSystemTest() {}

  void SetUp() {
    ASSERT_NO_FATAL_FAILURE(PrepareDirectoryStructure(&platform_));
    std::unique_ptr<EncryptedContainerFactory> container_factory =
        std::make_unique<FakeEncryptedContainerFactory>(
            &platform_, std::make_unique<FakeKeyring>());

    vault_factory_ = std::make_unique<CryptohomeVaultFactory>(
        &platform_, std::move(container_factory));
    std::shared_ptr<brillo::LvmCommandRunner> command_runner =
        std::make_shared<brillo::MockLvmCommandRunner>();
    brillo::VolumeGroup vg("STATEFUL", command_runner);
    brillo::Thinpool thinpool("thinpool", "STATEFUL", command_runner);
    vault_factory_->CacheLogicalVolumeObjects(vg, thinpool);

    homedirs_ = std::make_unique<HomeDirs>(
        &platform_, std::make_unique<policy::PolicyProvider>(),
        base::BindRepeating([](const ObfuscatedUsername& unused) {}),
        vault_factory_.get());

    mount_ =
        new Mount(&platform_, homedirs_.get(), /*legacy_mount=*/true,
                  /*bind_mount_downloads=*/true, /*use_local_mounter=*/true);
  }

 protected:
  // Protected for trivial access.
  NiceMock<MockPlatform> platform_;
  std::unique_ptr<CryptohomeVaultFactory> vault_factory_;
  std::unique_ptr<HomeDirs> homedirs_;
  scoped_refptr<Mount> mount_;

  void VerifyFS(const Username& username,
                MountType type,
                bool expect_present,
                bool downloads_bind_mount) {
    const ObfuscatedUsername obfuscated_username =
        brillo::cryptohome::home::SanitizeUserName(username);
    if (type == MountType::ECRYPTFS) {
      CheckEcryptfsMount(username, expect_present);
    } else if (type == MountType::DIR_CRYPTO) {
      CheckDircryptoMount(username, expect_present);
    } else if (type == MountType::DMCRYPT) {
      CheckDmcryptMount(username, expect_present);
    } else {
      NOTREACHED();
    }
    ASSERT_NO_FATAL_FAILURE(CheckRootAndDaemonStoreMounts(
        &platform_, username, GetUserMountDirectory(obfuscated_username),
        expect_present));
    ASSERT_NO_FATAL_FAILURE(CheckUserMountPoints(
        &platform_, username, GetUserMountDirectory(obfuscated_username),
        expect_present, downloads_bind_mount));

    const std::vector<base::FilePath> user_vault_and_mounts{
        GetUserMountDirectory(obfuscated_username).Append(kUserHomeSuffix),
        base::FilePath(kHomeChronosUser),
        brillo::cryptohome::home::GetUserPath(username),
        ChronosHashPath(username),
    };

    for (const auto& base_path : user_vault_and_mounts) {
      ASSERT_NO_FATAL_FAILURE(CheckUserMountPaths(
          &platform_, base_path, expect_present, downloads_bind_mount));
      ASSERT_NO_FATAL_FAILURE(CheckSkel(&platform_, base_path, expect_present));
    }

    if (type == MountType::DIR_CRYPTO && expect_present) {
      CheckTrackingXattr(username, downloads_bind_mount);
    }
  }

  void MockPreclearKeyring(bool success) {
    EXPECT_CALL(platform_, ClearUserKeyring()).WillOnce(Return(success));
  }

  void MockDircryptoPolicy(const Username& username, bool existing_dir) {
    const ObfuscatedUsername obfuscated_username =
        brillo::cryptohome::home::SanitizeUserName(username);
    const base::FilePath backing_dir =
        GetUserMountDirectory(obfuscated_username);
    EXPECT_CALL(platform_, GetDirectoryPolicyVersion(backing_dir))
        .WillRepeatedly(Return(existing_dir ? FSCRYPT_POLICY_V1 : -1));
    EXPECT_CALL(platform_, GetDirCryptoKeyState(ShadowRoot()))
        .WillRepeatedly(Return(dircrypto::KeyState::NO_KEY));
    EXPECT_CALL(platform_, GetDirCryptoKeyState(backing_dir))
        .WillRepeatedly(Return(existing_dir ? dircrypto::KeyState::ENCRYPTED
                                            : dircrypto::KeyState::NO_KEY));
  }

  void MockDircryptoKeyringSetup(const Username& username,
                                 const FileSystemKeyset& keyset,
                                 bool existing_dir,
                                 bool success) {
    const ObfuscatedUsername obfuscated_username =
        brillo::cryptohome::home::SanitizeUserName(username);
    const base::FilePath backing_dir =
        GetUserMountDirectory(obfuscated_username);
    const dircrypto::KeyReference reference = {
        .policy_version = FSCRYPT_POLICY_V1,
        .reference = keyset.KeyReference().fek_sig,
    };

    MockDircryptoPolicy(username, existing_dir);
    // EXPECT_CALL(platform_,
    // CheckDircryptoKeyIoctlSupport()).WillOnce(Return(true));
    EXPECT_CALL(
        platform_,
        SetDirCryptoKey(backing_dir, DirCryptoReferenceMatcher(reference)))
        .WillOnce(Return(success));
  }

  void SetHomedir(const Username& username) {
    const ObfuscatedUsername obfuscated_username =
        brillo::cryptohome::home::SanitizeUserName(username);
    ASSERT_TRUE(platform_.CreateDirectory(UserPath(obfuscated_username)));
  }

  void SetDmcryptPrereqs(const Username& username) {
    const ObfuscatedUsername obfuscated_username =
        brillo::cryptohome::home::SanitizeUserName(username);
    SetHomedir(username);
    ASSERT_TRUE(
        platform_.TouchFileDurable(GetDmcryptDataVolume(obfuscated_username)));
    ASSERT_TRUE(
        platform_.TouchFileDurable(GetDmcryptCacheVolume(obfuscated_username)));
    ON_CALL(platform_, GetStatefulDevice())
        .WillByDefault(Return(base::FilePath("/dev/somedev")));
    ON_CALL(platform_, GetBlkSize(_, _))
        .WillByDefault(DoAll(SetArgPointee<1>(4096), Return(true)));
    ON_CALL(platform_, UdevAdmSettle(_, _)).WillByDefault(Return(true));
    ON_CALL(platform_, FormatExt4(_, _, _)).WillByDefault(Return(true));
    ON_CALL(platform_, Tune2Fs(_, _)).WillByDefault(Return(true));
  }

 private:
  void CheckEcryptfsMount(const Username& username, bool expect_present) {
    const ObfuscatedUsername obfuscated_username =
        brillo::cryptohome::home::SanitizeUserName(username);
    const base::FilePath ecryptfs_vault =
        GetEcryptfsUserVaultPath(obfuscated_username);
    const base::FilePath ecryptfs_mount_point =
        GetUserMountDirectory(obfuscated_username);
    const std::multimap<const base::FilePath, const base::FilePath>
        expected_ecryptfs_mount_map{
            {ecryptfs_vault, ecryptfs_mount_point},
        };
    std::multimap<const base::FilePath, const base::FilePath>
        ecryptfs_mount_map;
    ASSERT_THAT(platform_.IsDirectoryMounted(ecryptfs_mount_point),
                expect_present);
    if (expect_present) {
      ASSERT_THAT(platform_.DirectoryExists(ecryptfs_mount_point),
                  expect_present);
      ASSERT_TRUE(platform_.GetMountsBySourcePrefix(ecryptfs_vault,
                                                    &ecryptfs_mount_map));
      ASSERT_THAT(ecryptfs_mount_map, ::testing::UnorderedElementsAreArray(
                                          expected_ecryptfs_mount_map));
    }
  }

  void CheckDircryptoMount(const Username& username, bool expect_present) {
    const ObfuscatedUsername obfuscated_username =
        brillo::cryptohome::home::SanitizeUserName(username);
    const base::FilePath dircrypto_mount_point =
        GetUserMountDirectory(obfuscated_username);
    if (expect_present) {
      ASSERT_THAT(platform_.DirectoryExists(dircrypto_mount_point),
                  expect_present);
    }
  }

  void CheckDmcryptMount(const Username& username, bool expect_present) {
    const base::FilePath kDevMapperPath(kDeviceMapperDir);
    const ObfuscatedUsername obfuscated_username =
        brillo::cryptohome::home::SanitizeUserName(username);
    const std::multimap<const base::FilePath, const base::FilePath>
        expected_volume_mount_map{
            {GetDmcryptDataVolume(obfuscated_username),
             GetUserMountDirectory(obfuscated_username)},
            {GetDmcryptCacheVolume(obfuscated_username),
             GetDmcryptUserCacheDirectory(obfuscated_username)},
        };
    const std::multimap<const base::FilePath, const base::FilePath>
        expected_cache_mount_map{
            {GetDmcryptUserCacheDirectory(obfuscated_username)
                 .Append(kUserHomeSuffix)
                 .Append(kCacheDir),
             GetUserMountDirectory(obfuscated_username)
                 .Append(kUserHomeSuffix)
                 .Append(kCacheDir)},
            {GetDmcryptUserCacheDirectory(obfuscated_username)
                 .Append(kUserHomeSuffix)
                 .Append(kGCacheDir),
             GetUserMountDirectory(obfuscated_username)
                 .Append(kUserHomeSuffix)
                 .Append(kGCacheDir)},
            {GetDmcryptUserCacheDirectory(obfuscated_username)
                 .Append(kRootHomeSuffix)
                 .Append(kDaemonStoreCacheDir),
             GetUserMountDirectory(obfuscated_username)
                 .Append(kRootHomeSuffix)
                 .Append(kDaemonStoreCacheDir)},
        };
    std::multimap<const base::FilePath, const base::FilePath> volume_mount_map;
    std::multimap<const base::FilePath, const base::FilePath> cache_mount_map;
    ASSERT_THAT(platform_.IsDirectoryMounted(
                    GetUserMountDirectory(obfuscated_username)),
                expect_present);
    ASSERT_THAT(platform_.IsDirectoryMounted(
                    GetDmcryptUserCacheDirectory(obfuscated_username)),
                expect_present);
    ASSERT_THAT(
        platform_.IsDirectoryMounted(GetUserMountDirectory(obfuscated_username)
                                         .Append(kUserHomeSuffix)
                                         .Append(kCacheDir)),
        expect_present);
    ASSERT_THAT(
        platform_.IsDirectoryMounted(GetUserMountDirectory(obfuscated_username)
                                         .Append(kUserHomeSuffix)
                                         .Append(kGCacheDir)),
        expect_present);
    if (expect_present) {
      ASSERT_TRUE(
          platform_.GetMountsBySourcePrefix(kDevMapperPath, &volume_mount_map));
      ASSERT_THAT(volume_mount_map, ::testing::UnorderedElementsAreArray(
                                        expected_volume_mount_map));
      ASSERT_TRUE(platform_.GetMountsBySourcePrefix(
          GetDmcryptUserCacheDirectory(obfuscated_username), &cache_mount_map));
      ASSERT_THAT(cache_mount_map, ::testing::UnorderedElementsAreArray(
                                       expected_cache_mount_map));
    }
  }

  void CheckTrackingXattr(const Username& username, bool downloads_bind_mount) {
    const ObfuscatedUsername obfuscated_username =
        brillo::cryptohome::home::SanitizeUserName(username);
    const base::FilePath mount_point =
        GetUserMountDirectory(obfuscated_username);

    std::string result;
    ASSERT_TRUE(platform_.GetExtendedFileAttributeAsString(
        mount_point.Append(kRootHomeSuffix), kTrackedDirectoryNameAttribute,
        &result));
    ASSERT_THAT(result, Eq(kRootHomeSuffix));

    ASSERT_TRUE(platform_.GetExtendedFileAttributeAsString(
        mount_point.Append(kUserHomeSuffix), kTrackedDirectoryNameAttribute,
        &result));
    ASSERT_THAT(result, Eq(kUserHomeSuffix));

    ASSERT_TRUE(platform_.GetExtendedFileAttributeAsString(
        mount_point.Append(kUserHomeSuffix).Append(kGCacheDir),
        kTrackedDirectoryNameAttribute, &result));
    ASSERT_THAT(result, Eq(kGCacheDir));

    ASSERT_TRUE(platform_.GetExtendedFileAttributeAsString(
        mount_point.Append(kUserHomeSuffix)
            .Append(kGCacheDir)
            .Append(kGCacheVersion2Dir),
        kTrackedDirectoryNameAttribute, &result));
    ASSERT_THAT(result, Eq(kGCacheVersion2Dir));

    ASSERT_TRUE(platform_.GetExtendedFileAttributeAsString(
        mount_point.Append(kUserHomeSuffix).Append(kCacheDir),
        kTrackedDirectoryNameAttribute, &result));
    ASSERT_THAT(result, Eq(kCacheDir));

    if (downloads_bind_mount) {
      ASSERT_TRUE(platform_.GetExtendedFileAttributeAsString(
          mount_point.Append(kUserHomeSuffix).Append(kDownloadsDir),
          kTrackedDirectoryNameAttribute, &result));
      ASSERT_THAT(result, Eq(kDownloadsDir));
    }

    ASSERT_TRUE(platform_.GetExtendedFileAttributeAsString(
        mount_point.Append(kUserHomeSuffix).Append(kMyFilesDir),
        kTrackedDirectoryNameAttribute, &result));
    ASSERT_THAT(result, Eq(kMyFilesDir));

    ASSERT_TRUE(platform_.GetExtendedFileAttributeAsString(
        mount_point.Append(kUserHomeSuffix)
            .Append(kMyFilesDir)
            .Append(kDownloadsDir),
        kTrackedDirectoryNameAttribute, &result));
    ASSERT_THAT(result, Eq(kDownloadsDir));
  }
};

namespace {

TEST_F(PersistentSystemTest, NoEcryptfsMountWhenForcedDircrypto) {
  // Verify force_dircrypto flag prohibits ecryptfs mounts.
  const FileSystemKeyset keyset = FileSystemKeyset::CreateRandom();

  CryptohomeVault::Options options = {
      .force_type = EncryptedContainerType::kEcryptfs,
  };

  MockPreclearKeyring(/*success=*/true);
  ASSERT_THAT(mount_->MountCryptohome(kUser, keyset, options), IsOk());
  VerifyFS(kUser, MountType::ECRYPTFS, /*expect_present=*/true,
           /*downloads_bind_mount=*/true);

  ASSERT_TRUE(mount_->UnmountCryptohome());
  VerifyFS(kUser, MountType::ECRYPTFS, /*expect_present=*/false,
           /*downloads_bind_mount=*/true);

  options = {
      .block_ecryptfs = true,
  };
  ASSERT_THAT(mount_->MountCryptohome(kUser, keyset, options),
              IsError(MOUNT_ERROR_OLD_ENCRYPTION));
}

TEST_F(PersistentSystemTest, MigrateEcryptfsToFscrypt) {
  // Verify ecryptfs->dircrypto migration.
  const std::string kContent{"some_content"};
  const base::FilePath kFile{"some_file"};
  const FileSystemKeyset keyset = FileSystemKeyset::CreateRandom();

  // Create ecryptfs
  CryptohomeVault::Options options = {
      .force_type = EncryptedContainerType::kEcryptfs,
  };
  MockPreclearKeyring(/*success=*/true);
  ASSERT_THAT(mount_->MountCryptohome(kUser, keyset, options), IsOk());

  ASSERT_TRUE(platform_.WriteStringToFile(
      base::FilePath(kHomeChronosUser).Append(kFile), kContent));

  ASSERT_TRUE(mount_->UnmountCryptohome());

  // Start migration
  options = {
      .migrate = true,
  };
  MockPreclearKeyring(/*success=*/true);
  MockDircryptoKeyringSetup(kUser, keyset, /*existing_dir=*/false,
                            /*success=*/true);
  ASSERT_THAT(mount_->MountCryptohome(kUser, keyset, options), IsOk());

  ASSERT_TRUE(mount_->UnmountCryptohome());

  // We can't mount in progress migration regularly
  options = {};
  MockDircryptoPolicy(kUser, /*existing_dir=*/true);
  ASSERT_THAT(mount_->MountCryptohome(kUser, keyset, options),
              IsError(MOUNT_ERROR_PREVIOUS_MIGRATION_INCOMPLETE));

  // We haven't migrated anything really, so we are in continuation.
  // Create a new mount object, because interface rises a flag prohibiting
  // migration on unmount.
  // TODO(dlunev): fix the behaviour.
  scoped_refptr<Mount> new_mount =
      new Mount(&platform_, homedirs_.get(), /*legacy_mount=*/true,
                /*bind_mount_downloads=*/true, /*use_local_mounter=*/true);
  options = {
      .migrate = true,
  };
  MockPreclearKeyring(/*success=*/true);
  MockDircryptoKeyringSetup(kUser, keyset, /*existing_dir=*/true,
                            /*success=*/true);
  ASSERT_THAT(new_mount->MountCryptohome(kUser, keyset, options), IsOk());

  ASSERT_TRUE(new_mount->MigrateEncryption(
      base::BindRepeating(
          [](const user_data_auth::DircryptoMigrationProgress& unused) {}),
      MigrationType::FULL));
  // TODO(dlunev): figure out how to properly abstract the unmount on dircrypto
  // VerifyFS(kUser, MountType::ECRYPTFS, /*expect_present=*/false,
  // /*downloads_bind_mount=*/true); VerifyFS(kUser, MountType::DIR_CRYPTO,
  // /*expect_present=*/false, /*downloads_bind_mount=*/true);

  // "vault" should be gone.
  const ObfuscatedUsername obfuscated_username =
      brillo::cryptohome::home::SanitizeUserName(kUser);
  const base::FilePath ecryptfs_vault =
      GetEcryptfsUserVaultPath(obfuscated_username);
  ASSERT_FALSE(platform_.DirectoryExists(ecryptfs_vault));

  // Now we should be able to mount with dircrypto.
  options = {
      .force_type = EncryptedContainerType::kFscrypt,
  };
  MockPreclearKeyring(/*success=*/true);
  MockDircryptoKeyringSetup(kUser, keyset, /*existing_dir=*/true,
                            /*success=*/true);
  ASSERT_THAT(mount_->MountCryptohome(kUser, keyset, options), IsOk());
  VerifyFS(kUser, MountType::DIR_CRYPTO, /*expect_present=*/true,
           /*downloads_bind_mount=*/true);

  std::string result;
  ASSERT_TRUE(platform_.ReadFileToString(
      base::FilePath(kHomeChronosUser).Append(kFile), &result));
  ASSERT_THAT(result, kContent);

  ASSERT_TRUE(mount_->UnmountCryptohome());
  // TODO(dlunev): figure out how to properly abstract the unmount on dircrypto
  // VerifyFS(kUser, MountType::DIR_CRYPTO, /*expect_present=*/false,
  // /*downloads_bind_mount=*/true);
}

TEST_F(PersistentSystemTest, MigrateEcryptfsToDmcrypt) {
  // Verify ecryptfs->dircrypto migration.
  const std::string kContent{"some_content"};
  const base::FilePath kFile{"some_file"};
  const FileSystemKeyset keyset = FileSystemKeyset::CreateRandom();

  homedirs_->set_lvm_migration_enabled(true);

  // Create ecryptfs
  CryptohomeVault::Options options = {
      .force_type = EncryptedContainerType::kEcryptfs,
  };
  MockPreclearKeyring(/*success=*/true);
  ASSERT_THAT(mount_->MountCryptohome(kUser, keyset, options), IsOk());

  ASSERT_TRUE(platform_.WriteStringToFile(
      base::FilePath(kHomeChronosUser).Append(kFile), kContent));

  ASSERT_TRUE(mount_->UnmountCryptohome());

  // Start migration
  // Create a new mount object, because interface rises a flag prohibiting
  // migration on unmount.
  // TODO(dlunev): fix the behaviour.
  scoped_refptr<Mount> new_mount =
      new Mount(&platform_, homedirs_.get(), /*legacy_mount=*/true,
                /*bind_mount_downloads=*/true, /*use_local_mounter=*/true);
  options = {
      .migrate = true,
  };
  MockPreclearKeyring(/*success=*/true);
  SetDmcryptPrereqs(kUser);
  ASSERT_THAT(new_mount->MountCryptohome(kUser, keyset, options), IsOk());

  ASSERT_TRUE(new_mount->MigrateEncryption(
      base::BindRepeating(
          [](const user_data_auth::DircryptoMigrationProgress& unused) {}),
      MigrationType::FULL));
  VerifyFS(kUser, MountType::ECRYPTFS, /*expect_present=*/false,
           /*downloads_bind_mount=*/true);
  VerifyFS(kUser, MountType::DMCRYPT, /*expect_present=*/false,
           /*downloads_bind_mount=*/true);

  // "vault" should be gone.
  const ObfuscatedUsername obfuscated_username =
      brillo::cryptohome::home::SanitizeUserName(kUser);
  const base::FilePath ecryptfs_vault =
      GetEcryptfsUserVaultPath(obfuscated_username);
  ASSERT_FALSE(platform_.DirectoryExists(ecryptfs_vault));

  // Now we should be able to mount with dircrypto.
  options = {
      .force_type = EncryptedContainerType::kDmcrypt,
  };
  MockPreclearKeyring(/*success=*/true);
  ASSERT_THAT(mount_->MountCryptohome(kUser, keyset, options), IsOk());
  VerifyFS(kUser, MountType::DMCRYPT, /*expect_present=*/true,
           /*downloads_bind_mount=*/true);

  std::string result;
  ASSERT_TRUE(platform_.ReadFileToString(
      base::FilePath(kHomeChronosUser).Append(kFile), &result));
  ASSERT_THAT(result, kContent);

  ASSERT_TRUE(mount_->UnmountCryptohome());
  VerifyFS(kUser, MountType::DMCRYPT, /*expect_present=*/false,
           /*downloads_bind_mount=*/true);
}

TEST_F(PersistentSystemTest, MigrateFscryptToDmcrypt) {
  // Verify ecryptfs->dircrypto migration.
  const std::string kContent{"some_content"};
  const base::FilePath kFile{"some_file"};
  const FileSystemKeyset keyset = FileSystemKeyset::CreateRandom();

  homedirs_->set_lvm_migration_enabled(true);

  // Create ecryptfs
  CryptohomeVault::Options options = {
      .force_type = EncryptedContainerType::kFscrypt,
  };
  MockPreclearKeyring(/*success=*/true);
  MockDircryptoKeyringSetup(kUser, keyset, /*existing_dir=*/false,
                            /*success=*/true);
  ASSERT_THAT(mount_->MountCryptohome(kUser, keyset, options), IsOk());

  ASSERT_TRUE(platform_.WriteStringToFile(
      base::FilePath(kHomeChronosUser).Append(kFile), kContent));

  ASSERT_TRUE(mount_->UnmountCryptohome());

  // Start migration
  // Create a new mount object, because interface rises a flag prohibiting
  // migration on unmount.
  // TODO(dlunev): fix the behaviour.
  scoped_refptr<Mount> new_mount =
      new Mount(&platform_, homedirs_.get(), /*legacy_mount=*/true,
                /*bind_mount_downloads=*/true, /*use_local_mounter=*/true);
  options = {
      .migrate = true,
  };
  MockPreclearKeyring(/*success=*/true);
  MockDircryptoKeyringSetup(kUser, keyset, /*existing_dir=*/true,
                            /*success=*/true);
  SetDmcryptPrereqs(kUser);
  ASSERT_THAT(new_mount->MountCryptohome(kUser, keyset, options), IsOk());

  ASSERT_TRUE(new_mount->MigrateEncryption(
      base::BindRepeating(
          [](const user_data_auth::DircryptoMigrationProgress& unused) {}),
      MigrationType::FULL));
  // VerifyFS(kUser, MountType::DIR_CRYPTO, /*expect_present=*/false,
  // /*downloads_bind_mount=*/true);
  VerifyFS(kUser, MountType::DMCRYPT, /*expect_present=*/false,
           /*downloads_bind_mount=*/true);

  // "vault" should be gone.
  const ObfuscatedUsername obfuscated_username =
      brillo::cryptohome::home::SanitizeUserName(kUser);
  const base::FilePath ecryptfs_vault =
      GetEcryptfsUserVaultPath(obfuscated_username);
  ASSERT_FALSE(platform_.DirectoryExists(ecryptfs_vault));

  // Now we should be able to mount with dircrypto.
  options = {
      .force_type = EncryptedContainerType::kDmcrypt,
  };
  MockPreclearKeyring(/*success=*/true);
  ASSERT_THAT(mount_->MountCryptohome(kUser, keyset, options), IsOk());
  VerifyFS(kUser, MountType::DMCRYPT, /*expect_present=*/true,
           /*downloads_bind_mount=*/true);

  std::string result;
  ASSERT_TRUE(platform_.ReadFileToString(
      base::FilePath(kHomeChronosUser).Append(kFile), &result));
  ASSERT_THAT(result, kContent);

  ASSERT_TRUE(mount_->UnmountCryptohome());
  VerifyFS(kUser, MountType::DMCRYPT, /*expect_present=*/false,
           /*downloads_bind_mount=*/true);
}

}  // namespace

class EphemeralSystemTest : public ::testing::Test {
 public:
  const Username kUser{"someuser"};

  EphemeralSystemTest() {}

  void SetUp() {
    ASSERT_NO_FATAL_FAILURE(PrepareDirectoryStructure(&platform_));
    std::unique_ptr<EncryptedContainerFactory> container_factory =
        std::make_unique<EncryptedContainerFactory>(
            &platform_, std::make_unique<FakeKeyring>(),
            std::make_unique<FakeBackingDeviceFactory>(&platform_));
    vault_factory_ = std::make_unique<CryptohomeVaultFactory>(
        &platform_, std::move(container_factory));
    homedirs_ = std::make_unique<HomeDirs>(
        &platform_, std::make_unique<policy::PolicyProvider>(),
        base::BindRepeating([](const ObfuscatedUsername& unused) {}),
        vault_factory_.get());

    mount_ =
        new Mount(&platform_, homedirs_.get(), /*legacy_mount=*/true,
                  /*bind_mount_downloads=*/true, /*use_local_mounter=*/true);
  }

 protected:
  // Protected for trivial access.
  NiceMock<MockPlatform> platform_;
  std::unique_ptr<CryptohomeVaultFactory> vault_factory_;
  std::unique_ptr<HomeDirs> homedirs_;
  scoped_refptr<Mount> mount_;
  struct statvfs ephemeral_statvfs_;

  base::FilePath EphemeralBackingFile(const Username& username) {
    const ObfuscatedUsername obfuscated_username =
        brillo::cryptohome::home::SanitizeUserName(username);
    return base::FilePath(kEphemeralCryptohomeDir)
        .Append(kSparseFileDir)
        .Append(*obfuscated_username);
  }

  base::FilePath EphemeralMountPoint(const Username& username) {
    const ObfuscatedUsername obfuscated_username =
        brillo::cryptohome::home::SanitizeUserName(username);
    return base::FilePath(kEphemeralCryptohomeDir)
        .Append(kEphemeralMountDir)
        .Append(*obfuscated_username);
  }

  void VerifyFS(const Username& username, bool expect_present) {
    CheckLoopDev(username, expect_present);
    ASSERT_NO_FATAL_FAILURE(CheckRootAndDaemonStoreMounts(
        &platform_, username, EphemeralMountPoint(username), expect_present));
    ASSERT_NO_FATAL_FAILURE(CheckUserMountPoints(
        &platform_, username, EphemeralMountPoint(username), expect_present));

    const std::vector<base::FilePath> user_vault_and_mounts{
        EphemeralMountPoint(username).Append(kUserHomeSuffix),
        base::FilePath(kHomeChronosUser),
        brillo::cryptohome::home::GetUserPath(username),
        ChronosHashPath(username),
    };

    for (const auto& base_path : user_vault_and_mounts) {
      ASSERT_NO_FATAL_FAILURE(
          CheckUserMountPaths(&platform_, base_path, expect_present, true));
      ASSERT_NO_FATAL_FAILURE(CheckSkel(&platform_, base_path, expect_present));
    }
  }

  base::FilePath GetLoopDevice() {
    return platform_.GetLoopDeviceManager()
        ->GetAttachedDeviceByName("ephemeral")
        ->GetDevicePath();
  }

 private:
  void CheckLoopDev(const Username& username, bool expect_present) {
    const base::FilePath ephemeral_backing_file =
        EphemeralBackingFile(username);
    const base::FilePath ephemeral_mount_point = EphemeralMountPoint(username);

    ASSERT_THAT(platform_.FileExists(ephemeral_backing_file), expect_present);
    ASSERT_THAT(platform_.DirectoryExists(ephemeral_mount_point),
                expect_present);
    ASSERT_THAT(platform_.IsDirectoryMounted(ephemeral_mount_point),
                expect_present);
    if (expect_present) {
      const std::multimap<const base::FilePath, const base::FilePath>
          expected_ephemeral_mount_map{
              {GetLoopDevice(), ephemeral_mount_point},
          };
      std::multimap<const base::FilePath, const base::FilePath>
          ephemeral_mount_map;
      ASSERT_TRUE(platform_.GetMountsBySourcePrefix(GetLoopDevice(),
                                                    &ephemeral_mount_map));
      ASSERT_THAT(ephemeral_mount_map, ::testing::UnorderedElementsAreArray(
                                           expected_ephemeral_mount_map));
    }
  }
};

namespace {

TEST_F(EphemeralSystemTest, EpmeneralMount_VFSFailure) {
  // Checks the case when ephemeral statvfs call fails.
  ON_CALL(platform_, StatVFS(base::FilePath(kEphemeralCryptohomeDir), _))
      .WillByDefault(Return(false));

  ASSERT_THAT(mount_->MountEphemeralCryptohome(kUser),
              IsError(MOUNT_ERROR_FATAL));

  VerifyFS(kUser, /*expect_present=*/false);
}

TEST_F(EphemeralSystemTest, EphemeralMount_CreateSparseDirFailure) {
  // Checks the case when directory for ephemeral sparse files fails to be
  // created.
  EXPECT_CALL(platform_, CreateDirectory(EphemeralBackingFile(kUser).DirName()))
      .WillOnce(Return(false));

  ASSERT_THAT(mount_->MountEphemeralCryptohome(kUser),
              IsError(MOUNT_ERROR_KEYRING_FAILED));

  VerifyFS(kUser, /*expect_present=*/false);
}

TEST_F(EphemeralSystemTest, EphemeralMount_CreateSparseFailure) {
  // Checks the case when ephemeral sparse file fails to create.
  EXPECT_CALL(platform_, CreateSparseFile(EphemeralBackingFile(kUser), _))
      .WillOnce(Return(false));

  ASSERT_THAT(mount_->MountEphemeralCryptohome(kUser),
              IsError(MOUNT_ERROR_KEYRING_FAILED));

  VerifyFS(kUser, /*expect_present=*/false);
}

TEST_F(EphemeralSystemTest, EphemeralMount_FormatFailure) {
  // Checks that when ephemeral loop device fails to be formatted, clean up
  // happens appropriately.
  EXPECT_CALL(platform_, FormatExt4(Property(&base::FilePath::value,
                                             StartsWith(kDevLoopPrefix)),
                                    _, _))
      .WillOnce(Return(false));

  ASSERT_THAT(mount_->MountEphemeralCryptohome(kUser),
              IsError(MOUNT_ERROR_KEYRING_FAILED));

  VerifyFS(kUser, /*expect_present=*/false);
}

TEST_F(EphemeralSystemTest, EphemeralMount_EnsureUserMountFailure) {
  // Checks that when ephemeral mount fails to ensure mount points, clean up
  // happens appropriately.
  EXPECT_CALL(platform_, FormatExt4(Property(&base::FilePath::value,
                                             StartsWith(kDevLoopPrefix)),
                                    _, _))
      .WillOnce(Return(true));
  EXPECT_CALL(platform_, Mount(Property(&base::FilePath::value,
                                        StartsWith(kDevLoopPrefix)),
                               EphemeralMountPoint(kUser), _, _, _))
      .WillOnce(Return(false));

  ASSERT_THAT(mount_->MountEphemeralCryptohome(kUser),
              IsError(MOUNT_ERROR_FATAL));

  VerifyFS(kUser, /*expect_present=*/false);
}

}  // namespace

}  // namespace cryptohome
