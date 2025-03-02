// Copyright 2021 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Implements common functions and definitions used throughout the app and library.

use std::convert::TryInto;
use std::ffi::CString;
use std::ffi::OsStr;
use std::fs;
use std::fs::File;
use std::fs::OpenOptions;
use std::io::prelude::*;
use std::io::BufReader;
use std::mem::MaybeUninit;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::path::PathBuf;
use std::process::exit;
use std::process::Command;
use std::str;
use std::time::Duration;

use anyhow::anyhow;
use anyhow::Context;
use anyhow::Result;

use lazy_static::lazy_static;
use libc::c_ulong;
use libc::c_void;
use log::debug;
use log::error;
use log::info;
use log::warn;
use regex::Regex;
use sha2::Digest;
use sha2::Sha256;
use thiserror::Error as ThisError;

use crate::cookie::set_hibernate_cookie;
use crate::cookie::HibernateCookieValue;
use crate::files::TMPFS_DIR;
use crate::hiberlog::redirect_log;
use crate::hiberlog::HiberlogOut;

const KEYCTL_PATH: &str = "/bin/keyctl";

lazy_static! {
    static ref USER_LOGGED_OUT_PATH: PathBuf = {
        let path = Path::new(TMPFS_DIR);
        path.join("user_logged_out")
    };
    static ref ZRAM_SYSFS_PATH: PathBuf = Path::new("/sys/block/zram0").to_path_buf();
}

/// Define the hibernate stages.
#[derive(Clone, Copy)]
pub enum HibernateStage {
    Suspend,
    Resume,
}

#[derive(Debug, ThisError)]
pub enum HibernateError {
    /// Cookie error
    #[error("Cookie error: {0}")]
    CookieError(String),
    /// Hibernate is not supported.
    #[error("Hibernate is not supported: {0}")]
    HibernateNotSupportedError(String),
    /// Insufficient Memory available.
    #[error("Not enough free memory and swap")]
    InsufficientMemoryAvailableError(),
    /// Insufficient free disk space available.
    #[error("Not enough disk space")]
    InsufficientDiskSpaceError(),
    /// Failed to send metrics
    #[error("Failed to send metrics: {0}")]
    MetricsSendFailure(String),
    /// Hiberimge is not set up.
    #[error("'hiberimage' is not set up")]
    NoHiberimageError(),
    /// Failed to lock process memory.
    #[error("Failed to mlockall: {0}")]
    MlockallError(nix::Error),
    /// Mmap error.
    #[error("mmap error: {0}")]
    MmapError(nix::Error),
    /// Snapshot device error.
    #[error("Snapshot device error: {0}")]
    SnapshotError(String),
    /// Snapshot ioctl error.
    #[error("Snapshot ioctl error: {0}: {1}")]
    SnapshotIoctlError(String, nix::Error),
    /// Mount not found.
    #[error("Mount not found")]
    MountNotFoundError(),
    /// Failed to shut down
    #[error("Failed to shut down: {0}")]
    ShutdownError(nix::Error),
    /// Hibernate volume error
    #[error("Hibernate volume error")]
    HibernateVolumeError(),
    /// Spawned process error
    #[error("Spawned process error: {0}")]
    SpawnedProcessError(i32),
    /// PinWeaver credentials exist.
    #[error("PinWeaver credentials exist")]
    PinWeaverCredentialsExist(),
    /// Index out of range error
    #[error("Index out of range")]
    IndexOutOfRangeError(),
    /// Device mapper error
    #[error("Device mapper error: {0}")]
    DeviceMapperError(String),
    /// Merge timeout error
    #[error("Merge timeout error")]
    MergeTimeoutError(),
    /// Update engine busy error
    #[error("Update engine busy")]
    UpdateEngineBusyError(),
    /// Key retrieve error
    #[error("Unable to retrieve crypto key")]
    KeyRetrievalError(),
    /// Syscall stat error
    #[error("Snapshot stat error: {0}")]
    SnapshotStatDeviceError(nix::Error),
    /// The current user is not one who hibernated
    #[error("User mismatch")]
    UserMismatchError(),
}

/// Options taken from the command line affecting hibernate.
#[derive(Default)]
pub struct HibernateOptions {
    pub dry_run: bool,
    pub reboot: bool,
}

/// Options taken from the command line affecting resume-init.
#[derive(Default)]
pub struct ResumeInitOptions {
    pub force: bool,
}

/// Options taken from the command line affecting resume.
#[derive(Default)]
pub struct ResumeOptions {
    pub dry_run: bool,
}

/// Options taken from the command line affecting abort-resume.
pub struct AbortResumeOptions {
    pub reason: String,
}

impl Default for AbortResumeOptions {
    fn default() -> Self {
        Self {
            reason: "Manually aborted by hiberman abort-resume".to_string(),
        }
    }
}

/// Get a device id from the path.
pub fn get_device_id<P: AsRef<std::path::Path>>(path: P) -> Result<u32> {
    let path_str_c = CString::new(path.as_ref().as_os_str().as_bytes())?;
    let mut stats: MaybeUninit<libc::stat> = MaybeUninit::zeroed();

    // This is safe because only stats is modified.
    let res = unsafe { libc::stat(path_str_c.as_ptr(), stats.as_mut_ptr()) };
    if res != 0 {
        return Err(HibernateError::SnapshotStatDeviceError(nix::Error::last()))
            .context("Failed to stat device");
    }

    // Safe because the syscall just initialized it, and we just verified
    // the return was successful.
    unsafe { Ok(stats.assume_init().st_rdev as u32) }
}

/// Get the page size on this system.
pub fn get_page_size() -> usize {
    // Safe because sysconf() returns a long and has no other side effects.
    unsafe { libc::sysconf(libc::_SC_PAGESIZE) as usize }
}

/// Get the amount of free memory (in pages) on this system.
pub fn get_available_pages() -> usize {
    // Safe because sysconf() returns a long and has no other side effects.
    unsafe { libc::sysconf(libc::_SC_AVPHYS_PAGES) as usize }
}

/// Helper function to get the amount of free physical memory on this system,
/// in megabytes.
pub fn get_available_memory_mb() -> u32 {
    let pagesize = get_page_size() as u64;
    let pagecount = get_available_pages() as u64;

    let mb = pagecount * pagesize / (1024 * 1024);
    mb.try_into().unwrap_or(u32::MAX)
}

/// Look through /proc/mounts to find the block device supporting the
/// given directory. The directory must be the root of a mount.
pub fn get_device_mounted_at_dir(mount_path: &str) -> Result<String> {
    // Go look through the mounts to see where the given mount is.
    let f = File::open("/proc/mounts")?;
    let buf_reader = BufReader::new(f);
    for line in buf_reader.lines().flatten() {
        let mut split = line.split_whitespace();
        let blk = split.next();
        let path = split.next();
        if let Some(path) = path {
            if path == mount_path {
                if let Some(blk) = blk {
                    return Ok(blk.to_string());
                }
            }
        }
    }

    Err(HibernateError::MountNotFoundError())
        .context(format!("Failed to find mount at {}", mount_path))
}

/// Return the path to partition one (stateful) on the root block device.
pub fn stateful_block_partition_one() -> Result<String> {
    let rootdev = path_to_stateful_block()?;
    let last = rootdev.chars().last();
    if let Some(last) = last {
        if last.is_numeric() {
            return Ok(format!("{}p1", rootdev));
        }
    }

    Ok(format!("{}1", rootdev))
}

/// Determine the path to the block device containing the stateful partition.
/// Farm this out to rootdev to keep the magic in one place.
pub fn path_to_stateful_block() -> Result<String> {
    let output = checked_command_output(Command::new("/usr/bin/rootdev").args(["-d", "-s"]))
        .context("Cannot get rootdev")?;
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

/// Determines if the stateful-rw snapshot is active, indicating a resume boot.
pub fn is_snapshot_active() -> bool {
    fs::metadata("/dev/mapper/stateful-rw").is_ok()
}

pub struct LockedProcessMemory {}

impl Drop for LockedProcessMemory {
    fn drop(&mut self) {
        unlock_process_memory();
    }
}

/// Lock all present and future memory belonging to this process, preventing it
/// from being paged out. Returns a LockedProcessMemory token, which undoes the
/// operation when dropped.
pub fn lock_process_memory() -> Result<LockedProcessMemory> {
    // This is safe because mlockall() does not modify memory, it only ensures
    // it doesn't get swapped out, which maintains Rust's safety guarantees.
    let rc = unsafe { libc::mlockall(libc::MCL_CURRENT | libc::MCL_FUTURE) };

    if rc < 0 {
        return Err(HibernateError::MlockallError(nix::Error::last()))
            .context("Cannot lock process memory");
    }

    Ok(LockedProcessMemory {})
}

/// Unlock memory belonging to this process, allowing it to be paged out once
/// more.
fn unlock_process_memory() {
    // This is safe because while munlockall() is a foreign function, it has
    // no immediately observable side effects on program execution.
    unsafe {
        libc::munlockall();
    };
}

/// Log a duration with level info in the form: <action> in X.YYY seconds.
pub fn log_duration(action: &str, duration: Duration) {
    info!(
        "{} in {}.{:03} seconds",
        action,
        duration.as_secs(),
        duration.subsec_millis()
    );
}

/// Log a duration with an I/O rate at level info in the form:
/// <action> in X.YYY seconds, N bytes, A.BB MB/s.
pub fn log_io_duration(action: &str, io_bytes: u64, duration: Duration) {
    let rate = ((io_bytes as f64) / duration.as_secs_f64()) / 1048576.0;
    info!(
        "{} in {}.{:03} seconds, {} bytes, {:.3} MB/s",
        action,
        duration.as_secs(),
        duration.subsec_millis(),
        io_bytes,
        rate
    );
}

/// Wait for a std::process::Command, and convert the exit status into a Result
pub fn checked_command(command: &mut std::process::Command) -> Result<()> {
    let mut child = command.spawn().context("Failed to spawn child process")?;
    let exit_status = child.wait().context("Failed to wait for child")?;
    if exit_status.success() {
        Ok(())
    } else {
        let code = exit_status.code().unwrap_or(-2);
        Err(HibernateError::SpawnedProcessError(code)).context(format!(
            "Command {} failed with code {}",
            command.get_program().to_string_lossy(),
            &code
        ))
    }
}

/// Wait for a std::process::Command, convert its exit status into a Result, and
/// collect the output on success.
pub fn checked_command_output(command: &mut std::process::Command) -> Result<std::process::Output> {
    let output = command
        .output()
        .context("Failed to get output for child process")?;
    let exit_status = output.status;
    if exit_status.success() {
        Ok(output)
    } else {
        let code = exit_status.code().unwrap_or(-2);
        Err(HibernateError::SpawnedProcessError(code)).context(format!(
            "Command {} failed with code {}\nstderr: {}",
            command.get_program().to_string_lossy(),
            &code,
            String::from_utf8_lossy(&output.stderr)
        ))
    }
}

/// Perform emergency bailout procedures (like syncing logs), set the cookie to
/// indicate something went very wrong, and reboot the system.
pub fn emergency_reboot(reason: &str) {
    error!("Performing emergency reboot: {}", reason);
    // Attempt to set the cookie, but do not stop if it fails.
    if let Err(e) = set_hibernate_cookie::<PathBuf>(None, HibernateCookieValue::EmergencyReboot) {
        error!("Failed to set cookie to EmergencyReboot: {}", e);
    }
    // Redirect the log to in-memory, which flushes out any pending logs if
    // logging is already directed to a file.
    redirect_log(HiberlogOut::BufferInMemory);
    reboot_system();
    // Exit with a weird error code to avoid going through this path multiple
    // times.
    exit(9);
}

/// Perform an orderly reboot.
fn reboot_system() {
    error!("Rebooting system!");
    checked_command(&mut Command::new("/sbin/reboot")).expect("Failed to reboot system");
}

pub fn mount_filesystem<P: AsRef<OsStr>>(
    block_device: P,
    mountpoint: P,
    fs_type: &str,
    flags: u64,
    data: &str,
) -> Result<()> {
    let bdev_cstr = CString::new(block_device.as_ref().as_bytes())?;
    let mp_cstr = CString::new(mountpoint.as_ref().as_bytes())?;
    let fs_cstr = CString::new(fs_type)?;
    let data_cstr = CString::new(data)?;

    debug!(
        "Mounting {} to {}",
        bdev_cstr.to_string_lossy(),
        mp_cstr.to_string_lossy()
    );

    // This is safe because mount does not affect memory layout.
    unsafe {
        let rc = libc::mount(
            bdev_cstr.as_ptr(),
            mp_cstr.as_ptr(),
            fs_cstr.as_ptr(),
            flags as c_ulong,
            data_cstr.as_ptr() as *const c_void,
        );

        if rc < 0 {
            return Err(nix::Error::last())
                .context(format!("Failed to mount {}", bdev_cstr.to_string_lossy()));
        }
    }

    Ok(())
}

pub fn unmount_filesystem<P: AsRef<OsStr>>(mountpoint: P) -> Result<()> {
    let mp_cstr = CString::new(mountpoint.as_ref().as_bytes())?;

    debug!("Unmounting {}", mp_cstr.to_string_lossy());

    // This is safe because unmount does not affect memory.
    unsafe {
        let rc = libc::umount(mp_cstr.as_ptr());
        if rc < 0 {
            return Err(nix::Error::last())
                .context(format!("Failed to unmount {}", mp_cstr.to_string_lossy()));
        }
    }

    Ok(())
}

/// Get the size of the system RAM
pub fn get_ram_size() -> u64 {
    let f = File::open("/proc/meminfo").unwrap();
    let reader = BufReader::new(f);

    for l in reader.lines() {
        let l = l.unwrap();
        if l.starts_with("MemTotal:") {
            let size_kb = l.split_whitespace().nth(1).unwrap().parse::<u64>().unwrap();
            return size_kb * 1024;
        }
    }

    panic!("Could not determine RAM size");
}

/// Struct with zram writeback stats.
pub struct ZramWritebackStats {
    pub bytes_on_disk: u64,
    pub total_bytes_read: u64,
    pub total_bytes_written: u64,
}

/// Get zram write back stats.
pub fn zram_get_bd_stats() -> Result<ZramWritebackStats> {
    let path = ZRAM_SYSFS_PATH.join("bd_stat");
    let content = std::fs::read_to_string(&path).context(format!("Failed to read {:?}", path))?;

    let values: Vec<_> = content
        .split_whitespace()
        .map(|s| s.parse::<u64>().unwrap() * 4096)
        .collect();

    Ok(ZramWritebackStats {
        bytes_on_disk: values[0],
        total_bytes_read: values[1],
        total_bytes_written: values[2],
    })
}

/// Checks if zram writeback is enabled.
pub fn zram_is_writeback_enabled() -> Result<bool> {
    let path = ZRAM_SYSFS_PATH.join("backing_dev");

    let content = std::fs::read_to_string(&path).context(format!("Failed to read {:?}", path))?;

    Ok(content.trim_end() != "none")
}

/// Get the time needed by the hibernated kernel to restore the system.
pub fn get_kernel_restore_time() -> Result<Duration> {
    let output =
        checked_command_output(Command::new("/bin/dmesg").args(["-P", "--since", "1 minute ago"]))
            .context("Failed to execute 'dmesg'")?;

    // regular expression for extracting the kernel timestamp.
    let re = Regex::new(r"^\[\s*(\d+)\.(\d{6})\]").unwrap();

    let mut restore_start: Option<Duration> = None;

    for line in output.stdout.lines() {
        if line.is_err() {
            continue;
        }

        let line = line.unwrap();

        match restore_start {
            None => {
                if line.contains("Enabling non-boot CPUs ...") {
                    let cap = re.captures(&line).unwrap();
                    restore_start = Some(Duration::new(
                        cap[1].parse()?,
                        cap[2].parse::<u32>()? * 1000,
                    ));
                }
            }

            Some(restore_start) => {
                if line.contains("PM: restore of devices complete after") {
                    let cap = re.captures(&line).unwrap();
                    let restore_done =
                        Duration::new(cap[1].parse()?, cap[2].parse::<u32>()? * 1000);

                    return Ok(restore_done - restore_start);
                }
            }
        }
    }

    Err(anyhow!(
        "Could not find log entries to determine kernel restore time"
    ))
}

/// Records a user logout.
pub fn record_user_logout() {
    // create (empty) sentinel file
    if let Err(e) = OpenOptions::new()
        .write(true)
        .create(true)
        .open(USER_LOGGED_OUT_PATH.as_path())
    {
        warn!(
            "Failed to open/create '{}': {e:?}",
            USER_LOGGED_OUT_PATH.display()
        );
    }
}

/// Returns true if the hiberimage was torn down because the user logged out.
pub fn has_user_logged_out() -> bool {
    USER_LOGGED_OUT_PATH.exists()
}

/// Obfuscates the given username.
///
/// We can't use cryptohome's SanitizeUserName() because hiberman does not have
/// access to /home/.shadow/ where the salt is stored.
pub fn sanitize_username(username: &str) -> Result<String> {
    let username_lc = username.to_lowercase();
    let mut hasher = Sha256::new();

    hasher.update(username_lc);
    let res = hasher.finalize();

    // hex::encode() wants an array, not a slice.
    let mut hash = [0_u8; 32];
    hash.copy_from_slice(&res[0..32]);

    Ok(hex::encode(hash))
}

/// Add a logon key to the kernel key ring
pub fn keyctl_add_key(description: &str, key_data: &[u8]) -> Result<()> {
    checked_command(Command::new(KEYCTL_PATH).args([
        "add",
        "-x",
        "logon",
        description,
        &hex::encode(key_data),
        "@s",
    ]))
    .context(format!(
        "Failed to add key '{description}' to the kernel key ring"
    ))
}

/// Remove a logon key from the kernel key ring
pub fn keyctl_remove_key(description: &str) -> Result<()> {
    checked_command(Command::new(KEYCTL_PATH).args(["purge", "-s", "logon", description])).context(
        format!("Failed to remove key '{description}' from the kernel key ring"),
    )
}
