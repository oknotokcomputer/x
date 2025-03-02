// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::fmt::Display;
use std::io;
use std::os::fd::FromRawFd;
use std::os::fd::OwnedFd;
use std::path::Path;
use std::sync::Arc;

use dbus::MethodErr;
use log::error;
use log::info;
use schedqos::cgroups::setup_cpu_cgroup;
use schedqos::cgroups::setup_cpuset_cgroup;
use schedqos::CgroupContext;
use schedqos::Config;
use schedqos::ProcessKey;
use schedqos::ProcessState;
use schedqos::ThreadState;
use tokio::io::unix::AsyncFd;
use tokio::io::Interest;
use tokio::sync::Mutex;
use tokio::task::JoinHandle;

use crate::config::ConfigProvider;
use crate::cpu_utils::Cpuset;
use crate::proc::load_ruid;

pub type SchedQosContext = schedqos::RestorableSchedQosContext;

const STATE_FILE_PATH: &str = "/run/resourced/schedqos_states";

/// Error of parsing /proc/pid/status
#[derive(Debug)]
pub enum Error {
    ProcessForbidden,
    ProcessNotFound,
    InvalidState,
    SchedQoS(schedqos::Error),
    Pidfd(io::Error),
    Proc(crate::proc::Error),
}

impl Error {
    pub fn to_dbus_error(&self) -> MethodErr {
        match self {
            Self::ProcessForbidden => MethodErr::failed("process is not allowed"),
            Self::ProcessNotFound => MethodErr::failed("process not found"),
            Self::InvalidState => MethodErr::invalid_arg("invalid state"),
            Self::SchedQoS(e) => match e {
                schedqos::Error::ProcessNotRegistered => {
                    MethodErr::failed("process not registered")
                }
                schedqos::Error::ThreadNotFound => MethodErr::failed("thread not found"),
                _ => MethodErr::failed("failed to set qos state"),
            },
            Self::Pidfd(_) => MethodErr::failed("failed to create pidfd"),
            Self::Proc(_) => MethodErr::failed("failed to read /proc/pid/status"),
        }
    }
}

impl From<schedqos::Error> for Error {
    fn from(e: schedqos::Error) -> Self {
        match e {
            schedqos::Error::ProcessNotFound => Self::ProcessNotFound,
            _ => Self::SchedQoS(e),
        }
    }
}

impl From<crate::proc::Error> for Error {
    fn from(e: crate::proc::Error) -> Self {
        match e {
            crate::proc::Error::NotFound(_) => Self::ProcessNotFound,
            _ => Self::Proc(e),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::ProcessForbidden => None,
            Self::ProcessNotFound => None,
            Self::InvalidState => None,
            Self::SchedQoS(e) => Some(e),
            Self::Pidfd(e) => Some(e),
            Self::Proc(e) => Some(e),
        }
    }
}

impl Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::ProcessForbidden => write!(f, "process forbidden"),
            Self::ProcessNotFound => write!(f, "process not found"),
            Self::InvalidState => write!(f, "invalid state"),
            Self::SchedQoS(e) => write!(f, "failed to set qos state: {:#}", e),
            Self::Pidfd(e) => write!(f, "failed to create pidfd: {:#}", e),
            Self::Proc(e) => write!(f, "failed to read /proc/pid/status: {:#}", e),
        }
    }
}

pub type Result<T> = std::result::Result<T, Error>;

pub fn create_schedqos_context(
    root: &Path,
    config_provider: &ConfigProvider,
) -> anyhow::Result<SchedQosContext> {
    let mut normal_cpu_share = 1024;
    let mut background_cpu_share = 10;
    let mut thread_configs = Config::default_thread_config();
    match config_provider.read_sched_qos_config("default") {
        Ok(Some(local_config)) => {
            if let Some(share) = local_config.normal_cpu_share {
                normal_cpu_share = share;
            }
            if let Some(share) = local_config.background_cpu_share {
                background_cpu_share = share;
            }
            local_config.merge_thread_configs_into(&mut thread_configs);
        }
        Ok(None) => {
            info!("no schedqos config to override.");
        }
        Err(e) => {
            error!("Failed to read sched qos config: {:?}", e);
        }
    }

    let cpu_normal = setup_cpu_cgroup("resourced/normal", normal_cpu_share)?;
    let cpu_background = setup_cpu_cgroup("resourced/background", background_cpu_share)?;
    let cpuset_all = Cpuset::all_cores(root)?;
    let cpuset_all = setup_cpuset_cgroup("resourced/all", &cpuset_all.to_string())?;
    let cpuset_efficient = Cpuset::little_cores(root)?;
    let cpuset_efficient =
        setup_cpuset_cgroup("resourced/efficient", &cpuset_efficient.to_string())?;

    let config = Config {
        cgroup_context: CgroupContext {
            cpu_normal,
            cpu_background,
            cpuset_all,
            cpuset_efficient,
        },
        process_configs: Config::default_process_config(),
        thread_configs,
    };

    let file_path = Path::new(STATE_FILE_PATH);
    let ctx = if file_path.exists() {
        info!("Loading schedqos state from {:?}", file_path);
        SchedQosContext::load_from_file(config, file_path)?
    } else {
        info!("Initialize schedqos state at {:?}", file_path);
        SchedQosContext::new_file(config, file_path)?
    };
    Ok(ctx)
}

/// Validate the ruid of process_id is the same as the sender euid.
///
/// Use ruid of the target process to compare because euid can be changed by the process itself
/// without any capabilities. Sender should be aware of its euid when sending the QoS request.
/// This can support the case a parent process forks a third-party process which change its euid and
/// the parent process sends the QoS request for the third-party process..
fn validate_pid(process_id: u32, sender_euid: u32) -> Result<()> {
    let target_process_ruid = load_ruid(process_id)?;
    if target_process_ruid == sender_euid {
        Ok(())
    } else {
        Err(Error::ProcessForbidden)
    }
}

pub async fn set_thread_state(
    sched_ctx: Arc<Mutex<SchedQosContext>>,
    process_id: u32,
    thread_id: u32,
    state: u8,
    sender_euid: u32,
) -> Result<()> {
    let state = ThreadState::try_from(state).map_err(|_| Error::InvalidState)?;

    validate_pid(process_id, sender_euid)?;

    let mut ctx = sched_ctx.lock().await;

    ctx.set_thread_state(process_id.into(), thread_id.into(), state)?;

    Ok(())
}

/// The returned [JoinHandle] is used for testing purpose.
pub async fn set_process_state(
    sched_ctx: Arc<Mutex<SchedQosContext>>,
    process_id: u32,
    state: u8,
    sender_euid: u32,
) -> Result<Option<JoinHandle<()>>> {
    let state = ProcessState::try_from(state).map_err(|_| Error::InvalidState)?;

    validate_pid(process_id, sender_euid)?;

    let mut ctx = sched_ctx.lock().await;

    if let Some(process_key) = ctx.set_process_state(process_id.into(), state)? {
        match create_async_pidfd(process_id) {
            Ok(pidfd) => Ok(Some(monitor_process(sched_ctx.clone(), pidfd, process_key))),
            Err(e) => {
                ctx.remove_process(process_key);
                if e.raw_os_error() == Some(libc::ESRCH) {
                    Err(Error::ProcessNotFound)
                } else {
                    Err(Error::Pidfd(e))
                }
            }
        }
    } else {
        Ok(None)
    }
}

fn create_async_pidfd(pid: u32) -> std::io::Result<AsyncFd<OwnedFd>> {
    // SAFETY: pidfd_open(2) does not modify userspace memory.
    let res = unsafe { libc::syscall(libc::SYS_pidfd_open, pid, 0) } as libc::c_int;

    if res < 0 {
        return Err(io::Error::last_os_error());
    }

    // SAFETY:: The new pidfd is not owned by anything.
    let pidfd = unsafe { OwnedFd::from_raw_fd(res) };

    AsyncFd::with_interest(pidfd, Interest::READABLE)
}

fn monitor_process(
    sched_ctx: Arc<Mutex<SchedQosContext>>,
    pidfd: AsyncFd<OwnedFd>,
    process: ProcessKey,
) -> JoinHandle<()> {
    tokio::spawn(async move {
        match pidfd.readable().await {
            Ok(_guard) => {}
            Err(e) => {
                error!("pidfd readable fails: {:?}", e);
            }
        };
        sched_ctx.lock().await.remove_process(process);
    })
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use super::*;
    use crate::test_utils::*;

    fn create_schedqos_context_for_test() -> Arc<Mutex<SchedQosContext>> {
        let dir = tempfile::tempdir().unwrap();
        let file_path = dir.path().join("states");
        let config = Config {
            cgroup_context: CgroupContext {
                cpu_normal: tempfile::tempfile().unwrap(),
                cpu_background: tempfile::tempfile().unwrap(),
                cpuset_all: tempfile::tempfile().unwrap(),
                cpuset_efficient: tempfile::tempfile().unwrap(),
            },
            process_configs: Config::default_process_config(),
            thread_configs: Config::default_thread_config(),
        };
        Arc::new(Mutex::new(
            SchedQosContext::new_file(config, &file_path).unwrap(),
        ))
    }

    #[tokio::test]
    async fn test_set_process_state() {
        let sched_ctx = create_schedqos_context_for_test();

        let (process_id, process) = fork_process_for_test();

        let uid = load_ruid(process_id).unwrap();

        let result = set_process_state(
            sched_ctx.clone(),
            process_id,
            ProcessState::Normal as u8,
            uid,
        )
        .await;
        assert!(result.is_ok());
        let result = result.unwrap();
        assert!(result.is_some());
        let join_handle = result.unwrap();

        tokio::time::sleep(Duration::from_millis(1)).await;

        assert!(!join_handle.is_finished());

        drop(process);

        // The remove_process() is executed. Otherwise this test times out.
        let _ = join_handle.await;
    }

    #[tokio::test]
    async fn test_set_process_state_invalid_state() {
        let sched_ctx = create_schedqos_context_for_test();

        let (process_id, _process) = fork_process_for_test();

        let uid = load_ruid(process_id).unwrap();

        let result = set_process_state(sched_ctx.clone(), process_id, 255, uid).await;
        assert!(matches!(result.err().unwrap(), Error::InvalidState));
    }

    #[tokio::test]
    async fn test_set_process_state_invalid_pid() {
        let sched_ctx = create_schedqos_context_for_test();

        let (process_id, process) = fork_process_for_test();

        let uid = load_ruid(process_id).unwrap();

        let result = set_process_state(
            sched_ctx.clone(),
            process_id,
            ProcessState::Normal as u8,
            !uid,
        )
        .await;
        assert!(matches!(result.err().unwrap(), Error::ProcessForbidden));

        drop(process);

        let result = set_process_state(
            sched_ctx.clone(),
            process_id,
            ProcessState::Normal as u8,
            uid,
        )
        .await;
        assert!(matches!(result.err().unwrap(), Error::ProcessNotFound));
    }

    #[tokio::test]
    async fn test_set_thread_state() {
        let sched_ctx = create_schedqos_context_for_test();

        let (process_id, _process) = fork_process_for_test();

        let uid = load_ruid(process_id).unwrap();

        set_process_state(
            sched_ctx.clone(),
            process_id,
            ProcessState::Normal as u8,
            uid,
        )
        .await
        .unwrap();

        let result = set_thread_state(
            sched_ctx.clone(),
            process_id,
            process_id,
            ThreadState::Balanced as u8,
            uid,
        )
        .await;
        result.as_ref().unwrap();
        assert!(result.is_ok());
    }

    #[tokio::test]
    async fn test_set_thread_state_invalid_state() {
        let sched_ctx = create_schedqos_context_for_test();

        let (process_id, _process) = fork_process_for_test();

        let uid = load_ruid(process_id).unwrap();

        let result = set_thread_state(sched_ctx.clone(), process_id, process_id, 255, uid).await;
        assert!(matches!(result.err().unwrap(), Error::InvalidState));
    }

    #[tokio::test]
    async fn test_set_thread_state_invalid_pid() {
        let sched_ctx = create_schedqos_context_for_test();

        let (process_id, process) = fork_process_for_test();

        let uid = load_ruid(process_id).unwrap();

        let result = set_thread_state(
            sched_ctx.clone(),
            process_id,
            process_id,
            ThreadState::Balanced as u8,
            !uid,
        )
        .await;
        assert!(matches!(result.err().unwrap(), Error::ProcessForbidden));

        drop(process);

        let result = set_thread_state(
            sched_ctx.clone(),
            process_id,
            process_id,
            ThreadState::Balanced as u8,
            uid,
        )
        .await;
        assert!(matches!(result.err().unwrap(), Error::ProcessNotFound));
    }

    #[tokio::test]
    async fn test_create_async_pidfd() {
        assert!(create_async_pidfd(std::process::id()).is_ok());

        let (process_id, process) = fork_process_for_test();
        assert!(create_async_pidfd(process_id).is_ok());

        drop(process);
        assert_eq!(
            create_async_pidfd(process_id).err().unwrap().raw_os_error(),
            Some(libc::ESRCH)
        );

        assert_eq!(
            create_async_pidfd(0).err().unwrap().raw_os_error(),
            Some(libc::EINVAL)
        );
    }
}
