// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "crash-reporter/crash_collector.h"

#include <dirent.h>
#include <fcntl.h>  // For file creation modes.
#include <inttypes.h>
#include <linux/limits.h>  // PATH_MAX
#include <sys/mman.h>      // for memfd_create
#include <sys/utsname.h>   // For uname.
#include <sys/wait.h>      // For waitpid.
#include <unistd.h>        // For execv and fork.

#include <ctime>
#include <map>
#include <set>
#include <vector>

#include <pcrecpp.h>

#include <base/bind.h>
#include <base/files/file_util.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/posix/eintr_wrapper.h>
#include <base/run_loop.h>
#include <base/scoped_clear_errno.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_split.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <base/threading/thread_task_runner_handle.h>
#include <brillo/key_value_store.h>
#include <brillo/process.h>
#include <brillo/userdb_utils.h>
#include <debugd/dbus-constants.h>
#include <zlib.h>

#include "crash-reporter/paths.h"
#include "crash-reporter/util.h"

namespace {

const char kCollectChromeFile[] =
    "/mnt/stateful_partition/etc/collect_chrome_crashes";
const char kDefaultLogConfig[] = "/etc/crash_reporter_logs.conf";
const char kDefaultUserName[] = "chronos";
const char kShellPath[] = "/bin/sh";
const char kUploadVarPrefix[] = "upload_var_";
const char kUploadTextPrefix[] = "upload_text_";
const char kUploadFilePrefix[] = "upload_file_";
const char kCollectorNameKey[] = "collector";
const char kCrashLoopModeKey[] = "crash_loop_mode";

// Key of the lsb-release entry containing the OS version.
const char kLsbOsVersionKey[] = "CHROMEOS_RELEASE_VERSION";

// Key of the lsb-release entry containing the OS description.
const char kLsbOsDescriptionKey[] = "CHROMEOS_RELEASE_DESCRIPTION";

// Directory mode of the user crash spool directory.
const mode_t kUserCrashPathMode = 0700;

// Directory mode of the system crash spool directory.
const mode_t kSystemCrashDirectoryMode = 0700;

// Directory mode of the run time state directory.
// Since we place flag files in here for checking by tests, we make it readable.
constexpr mode_t kSystemRunStateDirectoryMode = 0755;

// Directory mode of /var/lib/crash_reporter.
constexpr mode_t kCrashReporterStateDirectoryMode = 0700;

const uid_t kRootGroup = 0;

// Buffer size for reading a log into memory.
constexpr size_t kMaxLogSize = 1024 * 1024;

// Limit how many processes we walk back up.  This avoids any possible races
// and loops, and we probably don't need that many in the first place.
constexpr size_t kMaxParentProcessLogs = 8;

}  // namespace

const char* const CrashCollector::kUnknownValue = "unknown";

// Maximum crash reports per crash spool directory.  Note that this is
// a separate maximum from the maximum rate at which we upload these
// diagnostics.  The higher this rate is, the more space we allow for
// core files, minidumps, and kcrash logs, and equivalently the more
// processor and I/O bandwidth we dedicate to handling these crashes when
// many occur at once.  Also note that if core files are configured to
// be left on the file system, we stop adding crashes when either the
// number of core files or minidumps reaches this number.
const int CrashCollector::kMaxCrashDirectorySize = 32;

const uid_t CrashCollector::kRootUid = 0;

using base::FilePath;
using base::StringPrintf;

// Create a directory using the specified mode/user/group, and make sure it
// is actually a directory with the specified permissions.
// static
bool CrashCollector::CreateDirectoryWithSettings(const FilePath& dir,
                                                 mode_t mode,
                                                 uid_t owner,
                                                 gid_t group,
                                                 int* dirfd_out) {
  std::vector<FilePath::StringType> components;
  const FilePath parent_dir = dir.DirName();
  const FilePath final_dir = dir.BaseName();
  int dirfd, parentfd;

  // Walk the directory tree to make sure we avoid symlinks.
  // All parent parts must already exist else we abort.
  parent_dir.GetComponents(&components);
  parentfd = AT_FDCWD;
  for (const auto& component : components) {
    dirfd = openat(parentfd, component.c_str(),
                   O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW | O_PATH);
    if (dirfd < 0) {
      PLOG(ERROR) << "Unable to access crash path: " << dir.value() << " ("
                  << component << ")";
      if (parentfd != AT_FDCWD)
        close(parentfd);
      return false;
    }
    if (parentfd != AT_FDCWD)
      close(parentfd);
    parentfd = dirfd;
  }

  // Now handle the final part of the crash dir.  This one we can initialize.
  // Note: We omit O_CLOEXEC on purpose as children will use it.
  const char* final_dir_str = final_dir.value().c_str();
  dirfd = openat(parentfd, final_dir_str, O_DIRECTORY | O_NOFOLLOW | O_RDONLY);
  if (dirfd < 0) {
    if (errno != ENOENT) {
      // Delete whatever is there.
      if (unlinkat(parentfd, final_dir_str, 0) < 0) {
        PLOG(ERROR) << "Unable to clean up crash path: " << dir.value();
        close(parentfd);
        return false;
      }
    }

    // It doesn't exist, so create it!  We'll recheck the mode below.
    if (mkdirat(parentfd, final_dir_str, mode) < 0) {
      if (errno != EEXIST) {
        PLOG(ERROR) << "Unable to create crash directory: " << dir.value();
        close(parentfd);
        return false;
      }
    }

    // Try once more before we give up.
    // Note: We omit O_CLOEXEC on purpose as children will use it.
    dirfd =
        openat(parentfd, final_dir_str, O_DIRECTORY | O_NOFOLLOW | O_RDONLY);
    if (dirfd < 0) {
      PLOG(ERROR) << "Unable to open crash directory: " << dir.value();
      close(parentfd);
      return false;
    }
  }
  close(parentfd);

  // Make sure the ownership/permissions are correct in case they got reset.
  // We stat it to avoid pointless metadata updates in the common case.
  struct stat st;
  if (fstat(dirfd, &st) < 0) {
    PLOG(ERROR) << "Unable to stat crash path: " << dir.value();
    close(dirfd);
    return false;
  }

  // Change the ownership before we change the mode.
  if (st.st_uid != owner || st.st_gid != group) {
    if (fchown(dirfd, owner, group)) {
      PLOG(ERROR) << "Unable to chown crash directory: " << dir.value();
      close(dirfd);
      return false;
    }
  }

  // Update the mode bits.
  if ((st.st_mode & 07777) != mode) {
    if (fchmod(dirfd, mode)) {
      PLOG(ERROR) << "Unable to chmod crash directory: " << dir.value();
      close(dirfd);
      return false;
    }
  }

  if (dirfd_out)
    *dirfd_out = dirfd;
  else
    close(dirfd);
  return true;
}

CrashCollector::CrashCollector(const std::string& collector_name)
    : CrashCollector(collector_name,
                     kUseNormalCrashDirectorySelectionMethod,
                     kNormalCrashSendMode) {}

CrashCollector::CrashCollector(
    const std::string& collector_name,
    CrashDirectorySelectionMethod crash_directory_selection_method,
    CrashSendingMode crash_sending_mode)

    : lsb_release_(FilePath(paths::kEtcDirectory).Append(paths::kLsbRelease)),
      system_crash_path_(paths::kSystemCrashDirectory),
      crash_reporter_state_path_(paths::kCrashReporterStateDirectory),
      log_config_path_(kDefaultLogConfig),
      max_log_size_(kMaxLogSize),
      crash_sending_mode_(crash_sending_mode),
      crash_directory_selection_method_(crash_directory_selection_method),
      is_finished_(false),
      bytes_written_(0) {
  AddCrashMetaUploadData(kCollectorNameKey, collector_name);
  if (crash_sending_mode_ == kCrashLoopSendingMode) {
    AddCrashMetaUploadData(kCrashLoopModeKey, "true");
  }
}

CrashCollector::~CrashCollector() {
  if (bus_)
    bus_->ShutdownAndBlock();
}

void CrashCollector::Initialize(
    CrashCollector::IsFeedbackAllowedFunction is_feedback_allowed_function,
    bool early) {
  CHECK(is_feedback_allowed_function);
  is_feedback_allowed_function_ = is_feedback_allowed_function;
  // For early boot crash collectors, the consent file will not be accessible.
  // Instead, collect the crashes into /run and check consent during boot
  // collection.
  if (early) {
    is_feedback_allowed_function_ = []() { return true; };
    system_crash_path_ = base::FilePath(paths::kSystemRunCrashDirectory);
  }
}

void CrashCollector::SetUpDBus() {
  if (bus_)
    return;

  dbus::Bus::Options options;
  options.bus_type = dbus::Bus::SYSTEM;

  bus_ = new dbus::Bus(options);
  CHECK(bus_->Connect());

  session_manager_proxy_.reset(
      new org::chromium::SessionManagerInterfaceProxy(bus_));

  debugd_proxy_.reset(new org::chromium::debugdProxy(bus_));
}

bool CrashCollector::InMemoryFileExists(const base::FilePath& filename) const {
  base::FilePath base_name = filename.BaseName();
  for (const auto& in_memory_file : in_memory_files_) {
    if (std::get<0>(in_memory_file) == base_name.value()) {
      return true;
    }
  }
  return false;
}

base::ScopedFD CrashCollector::GetNewFileHandle(
    const base::FilePath& filename) {
  DCHECK(!is_finished_);
  int fd = -1;
  // Note: Getting the c_str() before calling open or memfd_create ensures that
  // PLOG works correctly -- there won't be intervening standard library calls
  // between the open / memfd_create and PLOG which could overwrite errno.
  std::string filename_string;
  const char* filename_cstr;
  switch (crash_sending_mode_) {
    case kNormalCrashSendMode:
      filename_string = filename.value();
      filename_cstr = filename_string.c_str();
      // The O_NOFOLLOW is redundant with O_CREAT|O_EXCL, but doesn't hurt.
      fd = HANDLE_EINTR(
          open(filename_cstr,
               O_CREAT | O_WRONLY | O_TRUNC | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
               0600));
      if (fd < 0) {
        PLOG(ERROR) << "Could not open " << filename_cstr;
      }
      break;
    case kCrashLoopSendingMode:
      filename_string = filename.BaseName().value();
      filename_cstr = filename_string.c_str();
      fd = memfd_create(filename_cstr, MFD_CLOEXEC);
      if (fd < 0) {
        PLOG(ERROR) << "Could not memfd_create " << filename_cstr;
      }
      break;
    default:
      NOTREACHED();
  }
  return base::ScopedFD(fd);
}

int CrashCollector::WriteNewFile(const FilePath& filename,
                                 const char* data,
                                 int size) {
  base::ScopedFD fd = GetNewFileHandle(filename);
  if (!fd.is_valid()) {
    return -1;
  }

  if (!base::WriteFileDescriptor(fd.get(), data, size)) {
    base::ScopedClearErrno restore_error;
    fd.reset();
    return -1;
  }

  if (crash_sending_mode_ == kCrashLoopSendingMode) {
    if (InMemoryFileExists(filename)) {
      LOG(ERROR)
          << "Duplicate file names not allowed in crash loop sending mode: "
          << filename.value();
      errno = EEXIST;
      return -1;
    }
    in_memory_files_.emplace_back(filename.BaseName().value(), std::move(fd));
  }
  bytes_written_ += size;
  return size;
}

bool CrashCollector::WriteNewCompressedFile(const FilePath& filename,
                                            const char* data,
                                            size_t size) {
  DCHECK_EQ(filename.FinalExtension(), ".gz")
      << filename.value() << " must end in .gz";
  base::ScopedFD fd = GetNewFileHandle(filename);
  if (!fd.is_valid()) {
    LOG(ERROR) << "Failed to open " << filename.value();
    return false;
  }
  // No way to stop gzclose_w from closing the file descriptor, but we need a
  // copy to send to debugd if crash_sending_mode_ == kCrashLoopSendingMode, so
  // duplicate first. We don't *need* a duplicate for kNormalCrashSendMode, but
  // it makes the bytes_written_-updating code easier below, so we duplicate in
  // both crash sending modes.
  base::ScopedFD fd_dup(dup(fd.get()));
  if (!fd_dup.is_valid()) {
    PLOG(ERROR) << "Failed to dup file descriptor";
    return false;
  }

  gzFile compressed_output = gzdopen(fd.get(), "wb");
  if (compressed_output == nullptr) {
    LOG(ERROR) << "Failed to gzip " << filename.value();
    return false;
  }

  // zlib now owns the file descriptor; we must not close it past this point.
  // Note that if gzdopen fails, we are still responsible for closing the file,
  // so we can't just put the release() call inside gzdopen().
  (void)fd.release();
  int result = gzwrite(compressed_output, data, size);
  if (result != size) {
    if (result <= 0) {
      int saved_errno = errno;
      int errnum = 0;
      const char* error_msg = gzerror(compressed_output, &errnum);
      if (errnum == Z_ERRNO) {
        LOG(ERROR) << "gzwrite failed with file system error: "
                   << strerror(saved_errno);
      } else {
        LOG(ERROR) << "gzwrite failed: error code " << errnum << ", error msg: "
                   << (error_msg == nullptr ? "None" : error_msg);
      }
    } else {
      LOG(ERROR) << "gzwrite wrote partial output";
    }
    gzclose_w(compressed_output);
    return false;
  }

  result = gzclose_w(compressed_output);
  if (result != Z_OK) {
    LOG(ERROR) << "gzclose_w failed with error code " << result;
    return false;
  }

  struct stat compressed_output_stats;
  if (fstat(fd_dup.get(), &compressed_output_stats) < 0) {
    PLOG(WARNING) << "Failed to fstat compressed file";
    // Make sure st_size is set so we don't add junk to bytes_written.
    compressed_output_stats.st_size = 0;
  }

  if (crash_sending_mode_ == kCrashLoopSendingMode) {
    if (InMemoryFileExists(filename)) {
      LOG(ERROR)
          << "Duplicate file names not allowed in crash loop sending mode: "
          << filename.value();
      return false;
    }
    in_memory_files_.emplace_back(filename.BaseName().value(),
                                  std::move(fd_dup));
  }
  bytes_written_ += compressed_output_stats.st_size;
  return true;
}

bool CrashCollector::RemoveNewFile(const base::FilePath& file_name) {
  switch (crash_sending_mode_) {
    case kNormalCrashSendMode: {
      if (!base::PathExists(file_name)) {
        return false;
      }
      int64_t file_size = 0;
      if (base::GetFileSize(file_name, &file_size)) {
        bytes_written_ -= file_size;
      }
      return base::DeleteFile(file_name, false /*recursive*/);
    }
    case kCrashLoopSendingMode: {
      base::FilePath base_name = file_name.BaseName();
      for (auto it = in_memory_files_.begin(); it != in_memory_files_.end();
           ++it) {
        if (std::get<0>(*it) == base_name.value()) {
          struct stat file_stat;
          const brillo::dbus_utils::FileDescriptor& fd = std::get<1>(*it);
          if (fstat(fd.get(), &file_stat) == 0) {
            bytes_written_ -= file_stat.st_size;
          }
          // Resources for memfd_create files are automatically released once
          // the last file descriptor is closed, and this will close what should
          // be the last file descriptor, so we are effectively deleting the
          // file by erasing the vector entry.
          in_memory_files_.erase(it);
          return true;
        }
      }
      return false;
    }
    default:
      NOTREACHED();
      return false;
  }
}

std::string CrashCollector::Sanitize(const std::string& name) {
  // Make sure the sanitized name does not include any periods.
  // The logic in crash_sender relies on this.
  std::string result = name;
  for (size_t i = 0; i < name.size(); ++i) {
    if (!isalnum(result[i]) && result[i] != '_')
      result[i] = '_';
  }
  return result;
}

void CrashCollector::StripSensitiveData(std::string* contents) {
  // At the moment, the only sensitive data we strip is MAC addresses and
  // emails.
  StripMacAddresses(contents);
  StripEmailAddresses(contents);
}

void CrashCollector::StripMacAddresses(std::string* contents) {
  std::ostringstream result;
  pcrecpp::StringPiece input(*contents);
  std::string pre_re_str;
  std::string re_str;

  // Get rid of things that look like MAC addresses, since they could possibly
  // give information about where someone has been.  This is strings that look
  // like this: 11:22:33:44:55:66
  // Complications:
  // - Within a given log, we want to be able to tell when the same MAC
  //   was used more than once.  Thus, we'll consistently replace the first
  //   MAC found with 00:00:00:00:00:01, the second with ...:02, etc.
  // - ACPI commands look like MAC addresses.  We'll specifically avoid getting
  //   rid of those.
  std::map<std::string, std::string> mac_map;

  // This RE will find the next MAC address and can return us the data preceding
  // the MAC and the MAC itself.
  pcrecpp::RE mac_re(
      "(.*?)("
      "[0-9a-fA-F][0-9a-fA-F]:"
      "[0-9a-fA-F][0-9a-fA-F]:"
      "[0-9a-fA-F][0-9a-fA-F]:"
      "[0-9a-fA-F][0-9a-fA-F]:"
      "[0-9a-fA-F][0-9a-fA-F]:"
      "[0-9a-fA-F][0-9a-fA-F])",
      pcrecpp::RE_Options().set_multiline(true).set_dotall(true));

  // This RE will identify when the 'pre_mac_str' shows that the MAC address
  // was really an ACPI cmd.  The full string looks like this:
  //   ata1.00: ACPI cmd ef/10:03:00:00:00:a0 (SET FEATURES) filtered out
  pcrecpp::RE acpi_re(
      "ACPI cmd ef/$",
      pcrecpp::RE_Options().set_multiline(true).set_dotall(true));

  // Keep consuming, building up a result string as we go.
  while (mac_re.Consume(&input, &pre_re_str, &re_str)) {
    if (acpi_re.PartialMatch(pre_re_str)) {
      // We really saw an ACPI command; add to result w/ no stripping.
      result << pre_re_str << re_str;
    } else {
      // Found a MAC address; look up in our hash for the mapping.
      std::string replacement_mac = mac_map[re_str];
      if (replacement_mac == "") {
        // It wasn't present, so build up a replacement string.
        int mac_id = mac_map.size();

        // Handle up to 2^32 unique MAC address; overkill, but doesn't hurt.
        replacement_mac = StringPrintf(
            "00:00:%02x:%02x:%02x:%02x", (mac_id & 0xff000000) >> 24,
            (mac_id & 0x00ff0000) >> 16, (mac_id & 0x0000ff00) >> 8,
            (mac_id & 0x000000ff));
        mac_map[re_str] = replacement_mac;
      }

      // Dump the string before the MAC and the fake MAC address into result.
      result << pre_re_str << replacement_mac;
    }
  }

  // One last bit of data might still be in the input.
  result << input;

  // We'll just assign right back to |contents|.
  *contents = result.str();
}

void CrashCollector::StripEmailAddresses(std::string* contents) {
  std::ostringstream result;
  pcrecpp::StringPiece input(*contents);
  std::string pre_re_str;
  std::string re_str;

  // Email regex according RFC 5322. I feel dirty after this...
  pcrecpp::RE email_re(
      "(.*?)(\\b"
      "(?:[a-z0-9!#$%&'*+/=?^_`{|}~-]+(?:\\.[a-z0-9!#$%&'*+/=?^_`{|}~-]+)*"
      "|\"(?:[\\x01-\\x08\\x0b\\x0c\\x0e-\\x1f\\x21\\x23-\\x5b\\x5d-\\x7f]"
      "|\\\\[\\x01-\\x09\\x0b\\x0c\\x0e-\\x7f])*\")"
      "@(?:(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\\.)+"
      "[a-z0-9](?:[a-z0-9-]*[a-z0-9])?|\\[(?:(?:(2(5[0-5]|[0-4][0-9])"
      "|1[0-9][0-9]|[1-9]?[0-9]))\\.){3}(?:(2(5[0-5]|[0-4][0-9])|1[0-9][0-9]"
      "|[1-9]?[0-9])|[a-z0-9-]*[a-z0-9]:"
      "(?:[\\x01-\\x08\\x0b\\x0c\\x0e-\\x1f\\x21-"
      "\\x5a\\x53-\\x7f]|\\\\[\\x01-\\x09\\x0b\\x0c\\x0e-\\x7f])+)\\])"
      "\\b)",
      pcrecpp::RE_Options().set_multiline(true).set_dotall(true));
  CHECK_EQ("", email_re.error());

  while (email_re.Consume(&input, &pre_re_str, &re_str)) {
    result << pre_re_str << "<redacted email address>";
  }
  result << input;
  *contents = result.str();
}

std::string CrashCollector::FormatDumpBasename(const std::string& exec_name,
                                               time_t timestamp,
                                               pid_t pid) {
  struct tm tm;
  localtime_r(&timestamp, &tm);
  std::string sanitized_exec_name = Sanitize(exec_name);
  return StringPrintf("%s.%04d%02d%02d.%02d%02d%02d.%d",
                      sanitized_exec_name.c_str(), tm.tm_year + 1900,
                      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                      tm.tm_sec, pid);
}

FilePath CrashCollector::GetCrashPath(const FilePath& crash_directory,
                                      const std::string& basename,
                                      const std::string& extension) {
  return crash_directory.Append(
      StringPrintf("%s.%s", basename.c_str(), extension.c_str()));
}

bool CrashCollector::GetUserCrashDirectories(
    std::vector<FilePath>* directories) {
  SetUpDBus();
  return util::GetUserCrashDirectories(session_manager_proxy_.get(),
                                       directories);
}

FilePath CrashCollector::GetUserCrashDirectory() {
  FilePath user_directory = FilePath(paths::kFallbackUserCrashDirectory);
  // When testing, store crashes in the fallback crash directory; otherwise,
  // the test framework can't get to them after logging the user out.
  if (ShouldHandleChromeCrashes()) {
    return user_directory;
  }
  // In this multiprofile world, there is no one-specific user dir anymore.
  // Ask the session manager for the active ones, then just run with the
  // first result we get back.
  std::vector<FilePath> directories;
  if (!GetUserCrashDirectories(&directories) || directories.empty()) {
    LOG(ERROR) << "Could not get user crash directories, using default.";
    return user_directory;
  }

  user_directory = directories[0];
  return user_directory;
}

FilePath CrashCollector::GetCrashDirectoryInfo(uid_t process_euid,
                                               uid_t default_user_id,
                                               gid_t default_user_group,
                                               mode_t* mode,
                                               uid_t* directory_owner,
                                               gid_t* directory_group) {
  // User crashes should go into the cryptohome, since they may contain PII.
  // For system crashes, there may not be a cryptohome mounted, so we use the
  // system crash path.
  if (process_euid == default_user_id ||
      crash_directory_selection_method_ == kAlwaysUseUserCrashDirectory) {
    *mode = kUserCrashPathMode;
    *directory_owner = default_user_id;
    *directory_group = default_user_group;
    return GetUserCrashDirectory();
  } else {
    *mode = kSystemCrashDirectoryMode;
    *directory_owner = kRootUid;
    *directory_group = kRootGroup;
    return system_crash_path_;
  }
}

bool CrashCollector::GetCreatedCrashDirectoryByEuid(uid_t euid,
                                                    FilePath* crash_directory,
                                                    bool* out_of_capacity) {
  base::FilePath full_path;
  uid_t default_user_id;
  gid_t default_user_group;

  if (out_of_capacity)
    *out_of_capacity = false;

  // In crash loop mode, we don't actually need a crash directory, so don't
  // bother creating one.
  if (crash_sending_mode_ == kCrashLoopSendingMode) {
    crash_directory->clear();
    return true;
  }

  // For testing.
  if (!forced_crash_directory_.empty()) {
    *crash_directory = forced_crash_directory_;
    return true;
  }

  if (!brillo::userdb::GetUserInfo(kDefaultUserName, &default_user_id,
                                   &default_user_group)) {
    LOG(ERROR) << "Could not find default user info";
    return false;
  }
  mode_t directory_mode;
  uid_t directory_owner;
  gid_t directory_group;
  full_path = GetCrashDirectoryInfo(euid, default_user_id, default_user_group,
                                    &directory_mode, &directory_owner,
                                    &directory_group);

  // Note: We "leak" dirfd to children so the /proc symlink below stays valid
  // in their own context.  We can't pass other /proc paths as they might not
  // be accessible in the children (when dropping privs), and we don't want to
  // pass the direct path in the filesystem as it'd be subject to TOCTOU.
  int dirfd;
  if (!CreateDirectoryWithSettings(full_path, directory_mode, directory_owner,
                                   directory_group, &dirfd)) {
    return false;
  }

  // Have all the rest of the tools access the directory by file handle.  This
  // avoids any TOCTOU races in case the underlying dir is changed on us.
  const FilePath crash_dir_procfd =
      FilePath("/proc/self/fd").Append(std::to_string(dirfd));
  LOG(INFO) << "Accessing crash dir '" << full_path.value()
            << "' via symlinked handle '" << crash_dir_procfd.value() << "'";

  if (!CheckHasCapacity(crash_dir_procfd, full_path.value())) {
    if (out_of_capacity)
      *out_of_capacity = true;
    return false;
  }

  *crash_directory = crash_dir_procfd;
  return true;
}

// static
FilePath CrashCollector::GetProcessPath(pid_t pid) {
  return FilePath(StringPrintf("/proc/%d", pid));
}

// static
bool CrashCollector::GetUptime(base::TimeDelta* uptime) {
  timespec boot_time;
  if (clock_gettime(CLOCK_BOOTTIME, &boot_time) != 0) {
    PLOG(ERROR) << "Failed to get boot time.";
    return false;
  }

  *uptime = base::TimeDelta::FromSeconds(boot_time.tv_sec) +
            base::TimeDelta::FromMicroseconds(
                boot_time.tv_nsec / base::Time::kNanosecondsPerMicrosecond);
  return true;
}

// static
bool CrashCollector::GetUptimeAtProcessStart(pid_t pid,
                                             base::TimeDelta* uptime) {
  std::string stat;
  if (!base::ReadFileToString(GetProcessPath(pid).Append("stat"), &stat)) {
    PLOG(ERROR) << "Failed to read process status.";
    return false;
  }

  uint64_t ticks;
  if (!ParseProcessTicksFromStat(stat, &ticks)) {
    LOG(ERROR) << "Failed to parse process status: " << stat;
    return false;
  }

  *uptime = base::TimeDelta::FromSecondsD(static_cast<double>(ticks) /
                                          sysconf(_SC_CLK_TCK));

  return true;
}

bool CrashCollector::GetExecutableBaseNameFromPid(pid_t pid,
                                                  std::string* base_name) {
  FilePath target;
  FilePath process_path = GetProcessPath(pid);
  FilePath exe_path = process_path.Append("exe");
  if (!base::ReadSymbolicLink(exe_path, &target)) {
    LOG(INFO) << "ReadSymbolicLink failed - Path " << process_path.value()
              << " DirectoryExists: " << base::DirectoryExists(process_path);
    // Try to further diagnose exe readlink failure cause.
    struct stat buf;
    int stat_result = stat(exe_path.value().c_str(), &buf);
    int saved_errno = errno;
    if (stat_result < 0) {
      LOG(INFO) << "stat " << exe_path.value() << " failed: " << stat_result
                << " " << saved_errno;
    } else {
      LOG(INFO) << "stat " << exe_path.value()
                << " succeeded: st_mode=" << buf.st_mode;
    }
    return false;
  }
  *base_name = target.BaseName().value();
  return true;
}

// Return true if the given crash directory has not already reached
// maximum capacity.
bool CrashCollector::CheckHasCapacity(const FilePath& crash_directory,
                                      const std::string& display_path) {
  DIR* dir = opendir(crash_directory.value().c_str());
  if (!dir) {
    PLOG(ERROR) << "Unable to open directory to check capacity: "
                << crash_directory.value();
    return false;
  }
  struct dirent* ent;
  bool full = false;
  std::set<std::string> basenames;
  // readdir_r is deprecated from glibc and we need to use readdir instead.
  // readdir is safe for glibc because it guarantees readdir is thread safe,
  // and atm we aren't supporting other C libraries
  while ((ent = readdir(dir))) {
    // Only count crash reports.  Ignore all other supplemental files.
    // We define "crash reports" as .meta, .dmp, or .core files.
    // This does mean that we ignore random files that might accumulate but
    // didn't come from us, but not a lot we can do about that.  Our crash
    // sender process should clean up unknown files independently.
    const base::FilePath filename(ent->d_name);
    const std::string ext = filename.FinalExtension();
    if (ext != ".core" && ext != ".dmp" && ext != ".meta")
      continue;

    // Track the basenames as our unique identifiers.  When the core/dmp files
    // are part of a single report, this will count them as one report.
    const std::string basename = filename.RemoveFinalExtension().value();
    basenames.insert(basename);

    if (basenames.size() >= static_cast<size_t>(kMaxCrashDirectorySize)) {
      LOG(WARNING) << "Crash directory " << display_path
                   << " already full with " << kMaxCrashDirectorySize
                   << " pending reports";
      full = true;
      break;
    }
  }
  closedir(dir);
  return !full;
}

bool CrashCollector::CheckHasCapacity(const FilePath& crash_directory) {
  return CheckHasCapacity(crash_directory, crash_directory.value());
}

bool CrashCollector::GetLogContents(const FilePath& config_path,
                                    const std::string& exec_name,
                                    const FilePath& output_file) {
  brillo::KeyValueStore store;
  if (!store.Load(config_path)) {
    LOG(WARNING) << "Unable to read log configuration file "
                 << config_path.value();
    return false;
  }

  std::string command;
  if (!store.GetString(exec_name, &command))
    return false;

  FilePath raw_output_file;
  if (!base::CreateTemporaryFile(&raw_output_file)) {
    PLOG(WARNING) << "Failed to create temporary file for raw log output.";
    return false;
  }

  brillo::ProcessImpl diag_process;
  diag_process.AddArg(kShellPath);
  diag_process.AddStringOption("-c", command);
  diag_process.RedirectOutput(raw_output_file.value());

  const int result = diag_process.Run();

  std::string log_contents;
  const bool fully_read = base::ReadFileToStringWithMaxSize(
      raw_output_file, &log_contents, max_log_size_);
  base::DeleteFile(raw_output_file, false);

  if (!fully_read) {
    if (log_contents.empty()) {
      LOG(WARNING) << "Failed to read raw log contents.";
      return false;
    }
    // If ReadFileToStringWithMaxSize returned false and log_contents is
    // non-empty, this means the log is larger than max_log_size_.
    LOG(WARNING) << "Log is larger than " << max_log_size_
                 << " bytes. Truncating.";
    log_contents.append("\n<TRUNCATED>\n");
  }

  // If the registered command failed, we include any (partial) output it might
  // have produced to improve crash reports.  But make a note of the failure.
  if (result != 0) {
    const std::string warning = StringPrintf(
        "\nLog command \"%s\" exited with %i\n", command.c_str(), result);
    log_contents.append(warning);
    LOG(WARNING) << warning;
  }

  // Always do this after log_contents is "finished" so we don't accidentally
  // leak data.
  StripSensitiveData(&log_contents);

  if (output_file.FinalExtension() == ".gz") {
    if (!WriteNewCompressedFile(output_file, log_contents.data(),
                                log_contents.size())) {
      LOG(WARNING) << "Error writing sanitized log to " << output_file.value();
      return false;
    }
  } else {
    if (WriteNewFile(output_file, log_contents.data(), log_contents.size()) !=
        static_cast<int>(log_contents.length())) {
      PLOG(WARNING) << "Error writing sanitized log to " << output_file.value();
      return false;
    }
  }

  return true;
}

bool CrashCollector::GetProcessTree(pid_t pid,
                                    const base::FilePath& output_file) {
  std::ostringstream stream;

  // Grab a limited number of parent process details.
  for (size_t depth = 0; depth < kMaxParentProcessLogs; ++depth) {
    std::string contents;

    stream << "### Process " << pid << std::endl;

    const FilePath proc_path = GetProcessPath(pid);
    const FilePath status_path = proc_path.Append("status");

    // Read the command line and append it to the log.
    if (!base::ReadFileToString(proc_path.Append("cmdline"), &contents))
      break;
    base::ReplaceChars(contents, std::string(1, '\0'), " ", &contents);
    stream << "cmdline: " << contents << std::endl;

    // Read the status file and append it to the log.
    if (!base::ReadFileToString(proc_path.Append("status"), &contents))
      break;
    stream << contents << std::endl;

    // Pull out the parent pid from the status file.  The line will look like:
    // PPid:\t1234
    base::StringPairs pairs;
    if (!base::SplitStringIntoKeyValuePairs(contents, ':', '\n', &pairs))
      break;
    pid = 0;
    for (const auto& key_value : pairs) {
      if (key_value.first == "PPid") {
        std::string value;
        int ppid;

        // Parse the parent pid.  Set it only if it's valid.
        base::TrimWhitespaceASCII(key_value.second, base::TRIM_ALL, &value);
        if (base::StringToInt(value, &ppid))
          pid = ppid;
        break;
      }
    }
    // If we couldn't find the parent pid, break out.
    if (pid == 0)
      break;
  }

  // Always do this after log collection is "finished" so we don't accidentally
  // leak data.
  std::string log = stream.str();
  StripSensitiveData(&log);

  if (WriteNewFile(output_file, log.data(), log.size()) !=
      static_cast<int>(log.size())) {
    PLOG(WARNING) << "Error writing sanitized log to " << output_file.value();
    return false;
  }

  return true;
}

void CrashCollector::AddCrashMetaData(const std::string& key,
                                      const std::string& value) {
  extra_metadata_.append(StringPrintf("%s=%s\n", key.c_str(), value.c_str()));
}

void CrashCollector::AddCrashMetaUploadFile(const std::string& key,
                                            const std::string& path) {
  if (!path.empty()) {
    FilePath file_path = FilePath(path);
    switch (crash_sending_mode_) {
      case kNormalCrashSendMode:
        // TODO(vapier): Make it fatal if the name is not relative.
        if (!NormalizeFilePath(file_path, &file_path))
          PLOG(WARNING) << "Could not normalize " << path;
        break;
      case kCrashLoopSendingMode:
        // Only the base name is recorded in the metadata file.
        file_path = file_path.BaseName();
        break;
    }
    AddCrashMetaData(kUploadFilePrefix + key, file_path.value());
  }
}

void CrashCollector::AddCrashMetaUploadData(const std::string& key,
                                            const std::string& value) {
  if (!value.empty())
    AddCrashMetaData(kUploadVarPrefix + key, value);
}

void CrashCollector::AddCrashMetaUploadText(const std::string& key,
                                            const std::string& path) {
  if (!path.empty()) {
    FilePath file_path = FilePath(path);
    switch (crash_sending_mode_) {
      case kNormalCrashSendMode:
        // TODO(vapier): Make it fatal if the name is not relative.
        if (!NormalizeFilePath(file_path, &file_path))
          PLOG(WARNING) << "Could not normalize " << path;
        break;
      case kCrashLoopSendingMode:
        // Only the base name is recorded in the metadata file.
        file_path = file_path.BaseName();
        break;
    }
    AddCrashMetaData(kUploadTextPrefix + key, file_path.value());
  }
}

std::string CrashCollector::GetLsbReleaseValue(const std::string& key) const {
  std::vector<base::FilePath> directories = {crash_reporter_state_path_,
                                             lsb_release_.DirName()};

  std::string value;
  if (util::GetCachedKeyValue(lsb_release_.BaseName(), key, directories,
                              &value)) {
    return value;
  }
  return kUnknownValue;
}

std::string CrashCollector::GetOsVersion() const {
  return GetLsbReleaseValue(kLsbOsVersionKey);
}

std::string CrashCollector::GetOsDescription() const {
  return GetLsbReleaseValue(kLsbOsDescriptionKey);
}

std::string CrashCollector::GetKernelName() const {
  struct utsname buf;
  if (!test_kernel_name_.empty())
    return test_kernel_name_;

  if (uname(&buf))
    return kUnknownValue;

  return buf.sysname;
}

std::string CrashCollector::GetKernelVersion() const {
  struct utsname buf;
  if (!test_kernel_version_.empty())
    return test_kernel_version_;

  if (uname(&buf))
    return kUnknownValue;

  // 3.8.11 #1 SMP Wed Aug 22 02:18:30 PDT 2018
  return StringPrintf("%s %s", buf.release, buf.version);
}

// Callback for CallMethodWithErrorCallback(). Discards the response pointer
// and just calls |callback|.
static void IgnoreResponsePointer(base::Callback<void()> callback,
                                  dbus::Response*) {
  callback.Run();
}

// Error callback for CallMethodWithErrorCallback(). Discards the error pointer
// and just calls |callback|.
static void IgnoreErrorResponsePointer(base::Callback<void()> callback,
                                       dbus::ErrorResponse*) {
  // We set the timeout to 0, so of course we time out before we get a response.
  // 99% of the time, the ErrorResponse is just "NoReply". Don't spam the error
  // log with that information, just discard the error response.
  callback.Run();
}

void CrashCollector::FinishCrash(const FilePath& meta_path,
                                 const std::string& exec_name,
                                 const std::string& payload_name) {
  DCHECK(!is_finished_);

  // TODO(vapier): Make it fatal if the name is not relative.
  FilePath payload_path = FilePath(payload_name);
  switch (crash_sending_mode_) {
    case kNormalCrashSendMode:
      payload_path = meta_path.DirName().Append(payload_path.BaseName());
      if (!NormalizeFilePath(payload_path, &payload_path))
        PLOG(WARNING) << "Could not normalize " << payload_name;
      break;
    case kCrashLoopSendingMode:
      // Only the base name of the payload is recorded in the metadata file.
      payload_path = payload_path.BaseName();
      break;
  }

  const std::string version = GetOsVersion();
  const std::string description = GetOsDescription();
  const std::string kernel_name = GetKernelName();
  const std::string kernel_version = GetKernelVersion();
  base::Time now = test_clock_ ? test_clock_->Now() : base::Time::Now();
  int64_t now_millis = (now - base::Time::UnixEpoch()).InMilliseconds();
  base::Time os_timestamp = util::GetOsTimestamp();
  std::string os_timestamp_str;
  if (!os_timestamp.is_null()) {
    os_timestamp_str =
        StringPrintf("os_millis=%" PRId64 "\n",
                     (os_timestamp - base::Time::UnixEpoch()).InMilliseconds());
  }
  std::string meta_data =
      StringPrintf("%supload_var_reportTimeMillis=%" PRId64
                   "\n"
                   "upload_var_lsb-release=%s\n"
                   "upload_var_osName=%s\n"
                   "upload_var_osVersion=%s\n"
                   "exec_name=%s\n"
                   "ver=%s\n"
                   "payload=%s\n"
                   "%s"
                   "done=1\n",
                   extra_metadata_.c_str(), now_millis, description.c_str(),
                   kernel_name.c_str(), kernel_version.c_str(),
                   exec_name.c_str(), version.c_str(),
                   payload_path.value().c_str(), os_timestamp_str.c_str());
  // We must use WriteNewFile instead of base::WriteFile as we
  // do not want to write with root access to a symlink that an attacker
  // might have created.
  if (WriteNewFile(meta_path, meta_data.c_str(), meta_data.size()) < 0) {
    PLOG(ERROR) << "Unable to write " << meta_path.value();
  }

  if (crash_sending_mode_ == kCrashLoopSendingMode) {
    SetUpDBus();

    // We'd like to call debugd_proxy_->UploadSingleCrash here; that seems like
    // the simplest method. However, calling debugd_proxy_->UploadSingleCrash
    // with a timeout of zero will spam the error log with messages about timing
    // out and not receiving a response. Going through
    // CallMethodWithErrorCallback avoids the error messages, but it does mean
    // we need a RunLoop.
    base::RunLoop run_loop;
    auto quit_closure = run_loop.QuitClosure();
    dbus::MethodCall method_call(debugd::kDebugdInterface,
                                 debugd::kUploadSingleCrash);
    dbus::MessageWriter writer(&method_call);
    brillo::dbus_utils::DBusParamWriter::Append(&writer,
                                                std::move(in_memory_files_));
    debugd_proxy_->GetObjectProxy()->CallMethodWithErrorCallback(
        &method_call, 0 /*timeout_ms*/,
        base::Bind(IgnoreResponsePointer, quit_closure),
        base::Bind(IgnoreErrorResponsePointer, quit_closure));
    run_loop.Run();
  }

  is_finished_ = true;
}

bool CrashCollector::ShouldHandleChromeCrashes() {
  // If we're testing crash reporter itself, we don't want to allow an
  // override for chrome crashes.  And, let's be conservative and only
  // allow an override for developer images.
  if (!util::IsCrashTestInProgress() && util::IsDeveloperImage()) {
    // Check if there's an override to indicate we should indeed collect
    // chrome crashes.  This allows the crashes to still be tracked when
    // they occur in autotests.  See "crosbug.com/17987".
    if (base::PathExists(FilePath(kCollectChromeFile)))
      return true;
  }
  // We default to ignoring chrome crashes.
  return false;
}

// Hash a string to a number.  We define our own hash function to not
// be dependent on a C++ library that might change.  This function
// uses basically the same approach as tr1/functional_hash.h but with
// a larger prime number (16127 vs 131).
unsigned CrashCollector::HashString(base::StringPiece input) {
  unsigned hash = 0;
  for (auto c : input)
    hash = hash * 16127 + c;
  return hash;
}

bool CrashCollector::InitializeSystemCrashDirectories(bool early) {
  if (!CreateDirectoryWithSettings(FilePath(paths::kSystemRunStateDirectory),
                                   kSystemRunStateDirectoryMode, kRootUid,
                                   kRootGroup, nullptr))
    return false;

  if (early) {
    if (!CreateDirectoryWithSettings(FilePath(paths::kSystemRunCrashDirectory),
                                     kSystemRunStateDirectoryMode, kRootUid,
                                     kRootGroup, nullptr))
      return false;
  } else {
    if (!CreateDirectoryWithSettings(FilePath(paths::kSystemCrashDirectory),
                                     kSystemCrashDirectoryMode, kRootUid,
                                     kRootGroup, nullptr))
      return false;

    if (!CreateDirectoryWithSettings(
            FilePath(paths::kCrashReporterStateDirectory),
            kCrashReporterStateDirectoryMode, kRootUid, kRootGroup, nullptr))
      return false;
  }
  return true;
}

// static
bool CrashCollector::ParseProcessTicksFromStat(base::StringPiece stat,
                                               uint64_t* ticks) {
  // Skip "pid" and "comm" fields. See format in proc(5).
  const auto pos = stat.find_last_of(')');
  if (pos == base::StringPiece::npos)
    return false;

  stat.remove_prefix(pos + 1);
  const auto fields = base::SplitStringPiece(stat, " ", base::TRIM_WHITESPACE,
                                             base::SPLIT_WANT_NONEMPTY);

  constexpr size_t kStartTimePos = 19;
  return fields.size() > kStartTimePos &&
         base::StringToUint64(fields[kStartTimePos], ticks);
}
