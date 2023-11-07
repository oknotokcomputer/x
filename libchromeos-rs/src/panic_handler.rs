// Copyright 2022 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A panic handler for better crash signatures for rust apps.

use nix::sys::memfd::memfd_create;
use nix::sys::memfd::MemFdCreateFlag;
use std::ffi::CString;
use std::fmt::Write;
use std::fs::File;
use std::io;
use std::mem;
use std::os::unix::prelude::FromRawFd;
use std::panic;
use std::process::abort;

const PANIC_MEMFD_NAME: &str = "RUST_PANIC_SIG";

fn create_panic_memfd() -> nix::Result<File> {
    let fd: i32 = memfd_create(
        &CString::new(PANIC_MEMFD_NAME).unwrap(),
        MemFdCreateFlag::MFD_CLOEXEC | MemFdCreateFlag::MFD_ALLOW_SEALING,
    )?;
    // SAFETY: Safe since the fd is newly created and not owned yet. `File` will own the fd.
    Ok(unsafe { File::from_raw_fd(fd) })
}

// TODO(b/309651697): This was written to be compatible with existing PanicInfo formatting, but it
// should probably be made more stable.
fn format_panic_info(panic_info: &panic::PanicInfo<'_>) -> String {
    // 128 is arbitrary, but should be enough to cover most cases.
    let mut result = String::with_capacity(128);
    result += "panicked at '";
    if let Some(message) = panic_info.message() {
        let _ = write!(&mut result, "{}", message);
    } else if let Some(message) = panic_info.payload().downcast_ref::<&'static str>() {
        result += message;
    }
    result += "', ";

    // At the time of writing, `PanicInfo::location()` cannot return `None`.
    match panic_info.location() {
        Some(location) => {
            let _ = write!(&mut result, "{}", location);
        }
        None => {
            result += "no location info";
        }
    }
    result
}

/// Inserts a panic handler that writes the panic info to a memfd called
/// "RUST_PANIC_SIG" before calling the original panic handler. This
/// makes it possible for external crash handlers to recover the panic info.
pub fn install_memfd_handler() {
    let hook = panic::take_hook();
    panic::set_hook(Box::new(move |p| {
        // On failure, ignore the error and call the original handler.
        if let Ok(mut panic_memfd) = create_panic_memfd() {
            let _ = io::Write::write_all(&mut panic_memfd, format_panic_info(p).as_bytes());
            // Intentionally leak panic_memfd so it is picked up by the crash handler.
            mem::forget(panic_memfd);
        }
        hook(p);

        // If this is a multithreaded program, a panic in one thread will not kill the whole
        // process. Abort so the entire process gets killed and produces a core dump.
        abort();
    }));
}
