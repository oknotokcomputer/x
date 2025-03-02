// Copyright 2018 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This program collects fine-grained memory stats around events of interest
// (such as browser tab discards) and saves them in a queue of "clip files",
// to be uploaded with other logs.
//
// The program has two modes: slow and fast poll.  In slow-poll mode, the
// program occasionally checks (every 2 seconds right now) whether it should go
// into fast-poll mode, because interesting events become possible.  When in
// fast-poll mode, the program collects memory stats frequently (every 0.1
// seconds right now) and stores them in a circular buffer.  When "interesting"
// events occur, the stats around each event are saved into a "clip" file.
// These files are also maintained as a queue, so only the latest N clips are
// available (N = 20 right now).

extern crate chrono;
extern crate dbus;
extern crate env_logger;
extern crate libc;
#[macro_use]
extern crate log;
extern crate procfs;
extern crate syslog;
extern crate tempfile;

#[cfg(not(test))]
use dbus::ffidisp::{BusType, Connection, ConnectionItem, WatchEvent};
#[cfg(not(test))]
use protobuf::Message;

use std::collections::hash_map::HashMap;
use std::fmt;
use std::fs::{create_dir, File, OpenOptions};
use std::io::Read;
use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::io::AsRawFd;
use std::path::{Path, PathBuf};
use std::time::Duration;
use std::{io, str};

use chrono::DateTime;
use chrono::Local;
#[cfg(not(test))]
use nix::sys::select;
use procfs::LoadAverage;
#[cfg(not(test))]
use system_api::metrics_event;
use tempfile::TempDir;

#[cfg(test)]
mod test;

const LOG_DIRECTORY: &str = "/var/log/memd";
const STATIC_PARAMETERS_LOG: &str = "memd.parameters";
const LOW_MEM_SYSFS: &str = "/sys/kernel/mm/chromeos-low_mem";
const MAX_CLIP_COUNT: usize = 20;

const COLLECTION_DELAY_MS: i64 = 5_000; // Wait after event of interest.
const CLIP_COLLECTION_SPAN_MS: i64 = 10_000; // ms worth of samples in a clip.
const SAMPLES_PER_SECOND: i64 = 10; // Rate of fast sample collection.
const SAMPLING_PERIOD_MS: i64 = 1000 / SAMPLES_PER_SECOND;
// Danger threshold.  When the distance between "available" and "margin" is
// greater than LOW_MEM_DANGER_THRESHOLD_MB, we assume that there's no danger
// of "interesting" events (such as a discard) happening in the next
// SLOW_POLL_PERIOD_DURATION.  In other words, we expect that an allocation of
// more than LOW_MEM_DANGER_THRESHOLD_MB in a SLOW_POLL_PERIOD_DURATION will be
// rare.
const LOW_MEM_DANGER_THRESHOLD_MB: u32 = 600; // Poll fast when closer to margin than this.
const SLOW_POLL_PERIOD_DURATION: Duration = Duration::from_secs(2); // Sleep in slow-poll mode.
const FAST_POLL_PERIOD_DURATION: Duration = Duration::from_millis(SAMPLING_PERIOD_MS as u64); // Sleep duration in fast-poll mode.

// Print a warning if the fast-poll select lasts a lot longer than expected
// (which might happen because of huge load average and swap activity).
const UNREASONABLY_LONG_SLEEP: i64 = 10 * SAMPLING_PERIOD_MS;

// Size of sample queue.  The queue contains mostly timer events, in the amount
// determined by the clip span and the sampling rate.  It also contains other
// events, such as OOM kills etc., whose amount is expected to be smaller than
// the former.  Generously double the number of timer events to leave room for
// non-timer events.
const SAMPLE_QUEUE_LENGTH: usize =
    (CLIP_COLLECTION_SPAN_MS / 1000 * SAMPLES_PER_SECOND * 2) as usize;

// The names of fields of interest in /proc/vmstat.  They must be listed in
// the order in which they appear in /proc/vmstat.  When parsing the file,
// if a mandatory field is missing, the program panics.  A missing optional
// field (for instance, pgmajfault_f for some kernels) results in a value
// of 0. (BAD: This works only for the last field.)
//
// For fields with |accumulate| = true, use the name as a prefix, and add up
// all values with that prefix in consecutive lines.
const VMSTAT_VALUES_COUNT: usize = 5; // Number of vmstat values we're tracking.
#[rustfmt::skip]
const VMSTATS: [(&str, bool, bool); VMSTAT_VALUES_COUNT] = [
    // name                 mandatory   accumulate
    ("pswpin",              true,       false),
    ("pswpout",             true,       false),
    ("pgalloc",             true,       true),   // pgalloc_dma, pgalloc_normal, etc.
    ("pgmajfault",          true,       false),
    ("pgmajfault_f",        false,      false),
];
// The only difference from x86_64 is pgalloc_dma vs. pgalloc_dma32.

#[derive(Debug)]
pub enum Error {
    LowMemFileError(Box<dyn std::error::Error>),
    VmstatFileError(Box<std::io::Error>),
    RunnablesFileError(Box<std::io::Error>),
    AvailableFileError(Box<dyn std::error::Error>),
    CreateLogDirError(Box<std::io::Error>),
    StartingClipCounterMissingError(Box<dyn std::error::Error>),
    LogStaticParametersError(Box<dyn std::error::Error>),
    DbusWatchError(Box<dyn std::error::Error>),
    LowMemFDWatchError(Box<dyn std::error::Error>),
    LowMemWatcherError(Box<dyn std::error::Error>),
    InitSyslogError(Box<dyn std::error::Error>),
}

impl std::error::Error for Error {
    // This function is "soft-deprecated" so
    // we use the fmt() function from Display below.
    fn description(&self) -> &str {
        "memd_error"
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match *self {
            Error::LowMemFileError(ref e) => write!(f, "cannot opening low-mem file: {}", e),
            Error::VmstatFileError(ref e) => write!(f, "cannot open vmstat: {}", e),
            Error::RunnablesFileError(ref e) => write!(f, "cannot open loadavg: {}", e),
            Error::AvailableFileError(ref e) => write!(f, "cannot open available file: {}", e),
            Error::CreateLogDirError(ref e) => write!(f, "cannot create log directory: {}", e),
            Error::StartingClipCounterMissingError(ref e) => {
                write!(f, "cannot find starting clip counter: {}", e)
            }
            Error::LogStaticParametersError(ref e) => {
                write!(f, "cannot log static parameters: {}", e)
            }
            Error::DbusWatchError(ref e) => write!(f, "cannot watch dbus fd: {}", e),
            Error::LowMemFDWatchError(ref e) => write!(f, "cannot watch low-mem fd: {}", e),
            Error::LowMemWatcherError(ref e) => write!(f, "cannot set low-mem watcher: {}", e),
            Error::InitSyslogError(ref e) => write!(f, "cannot init syslog: {}", e),
        }
    }
}

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

// Opens a file if it exists, otherwise returns none.
fn open_maybe(path: &Path) -> Result<Option<File>> {
    if !path.exists() {
        Ok(None)
    } else {
        Ok(Some(File::open(path)?))
    }
}

// Converts the result of an integer expression |e| to modulo |n|. |e| may be
// negative. This differs from plain "%" in that the result of this function
// is always be between 0 and n-1.
fn modulo(e: isize, n: usize) -> usize {
    let nn = n as isize;
    let x = e % nn;
    (if x >= 0 { x } else { x + nn }) as usize
}

// Reads a string from the file named by |path|, representing a u32, and
// returns the value the strings it represents. If there are multiple ints
// in the file, then it returns the first one.
fn read_int(path: &Path) -> Result<u32> {
    let mut file = File::open(path)?;
    let mut content = String::new();
    file.read_to_string(&mut content)?;
    Ok(content
        .split_whitespace()
        .next()
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "empty file"))?
        .parse::<u32>()?)
}

// The Timer trait allows us to mock time for testing.
trait Timer {
    // A wrapper for select() with only ready-to-read fds.
    fn select(
        &mut self,
        dbus_receiver: &crossbeam_channel::Receiver<DbusEvent>,
        timeout: Duration,
    ) -> Result<bool>;
    fn now(&self) -> i64;
    fn quit_request(&self) -> bool;
}

#[cfg(not(test))]
struct GenuineTimer {}

#[cfg(not(test))]
impl Timer for GenuineTimer {
    // Returns current uptime (active time since boot, in milliseconds)
    fn now(&self) -> i64 {
        let ts = nix::time::clock_gettime(nix::time::ClockId::CLOCK_MONOTONIC)
            .expect("clock_gettime() failed!");
        (ts.tv_sec() * 1000 + ts.tv_nsec() / 1_000_000).into()
    }

    // Always returns false, unless testing (see MockTimer).
    fn quit_request(&self) -> bool {
        false
    }

    fn select(
        &mut self,
        dbus_receiver: &crossbeam_channel::Receiver<DbusEvent>,
        timeout: Duration,
    ) -> Result<bool> {
        let mut channel_select = crossbeam_channel::Select::new();
        let _operation = channel_select.recv(dbus_receiver);
        match channel_select.ready_timeout(timeout) {
            Ok(_) => Ok(true),
            Err(_) => Ok(false),
        }
    }
}

#[derive(Clone, Copy, Default)]
struct Sample {
    uptime: i64, // system uptime in ms
    sample_type: SampleType,
    info: Sysinfo,
    runnables: u32, // number of runnable processes
    available: u32, // available RAM from low-mem notifier
    vmstat_values: [i64; VMSTAT_VALUES_COUNT],
}

impl Sample {
    // Outputs a sample.
    fn output(&self, out: &mut File) -> Result<()> {
        writeln!(
            out,
            "{}.{:02} {:6} {} {} {} {} {} {} {} {} {} {} {}",
            self.uptime / 1000,
            self.uptime % 1000 / 10,
            self.sample_type,
            self.info.loads_1_minute,
            self.info.freeram,
            self.info.freeswap,
            self.info.procs,
            self.runnables,
            self.available,
            self.vmstat_values[0],
            self.vmstat_values[1],
            self.vmstat_values[2],
            self.vmstat_values[3],
            self.vmstat_values[4],
        )?;
        Ok(())
    }
}

#[derive(Copy, Clone, Default)]
struct Sysinfo {
    loads_1_minute: f64,
    freeram: u64,
    freeswap: u64,
    procs: u16,
}

fn sysinfo() -> Result<Sysinfo> {
    let sysinfo_result = nix::sys::sysinfo::sysinfo()?;
    Ok(Sysinfo {
        loads_1_minute: sysinfo_result.load_average().0,
        freeram: sysinfo_result.ram_unused(),
        freeswap: sysinfo_result.swap_free(),
        procs: sysinfo_result.process_count(),
    })
}

impl Sysinfo {
    // Fakes sysinfo for testing.
    fn fake_sysinfo() -> Result<Sysinfo> {
        Ok(Sysinfo {
            loads_1_minute: 5.0,
            freeram: 42_000_000,
            freeswap: 84_000_000,
            procs: 1234,
        })
    }
}

struct SampleQueue {
    samples: [Sample; SAMPLE_QUEUE_LENGTH],
    head: usize,  // points to latest entry
    count: usize, // count of valid entries (min=0, max=SAMPLE_QUEUE_LENGTH)
}

impl SampleQueue {
    fn new() -> SampleQueue {
        let s: Sample = Default::default();
        SampleQueue {
            samples: [s; SAMPLE_QUEUE_LENGTH],
            head: 0,
            count: 0,
        }
    }

    // Returns self.head as isize, to make index calculations behave correctly
    // on underflow.
    fn ihead(&self) -> isize {
        self.head as isize
    }

    fn reset(&mut self) {
        self.head = 0;
        self.count = 0;
    }

    // Returns the next slot in the queue.  Always succeeds, since on overflow
    // it discards the LRU slot.
    fn next_slot(&mut self) -> &mut Sample {
        let slot = self.head;
        self.head = modulo(self.ihead() + 1, SAMPLE_QUEUE_LENGTH);
        if self.count < SAMPLE_QUEUE_LENGTH {
            self.count += 1;
        }
        &mut self.samples[slot]
    }

    fn sample(&self, i: isize) -> &Sample {
        assert!(i >= 0);
        // Subtract 1 because head points to the next free slot.
        assert!(
            modulo(self.ihead() - 1 - i, SAMPLE_QUEUE_LENGTH) <= self.count,
            "bad queue index: i {}, head {}, count {}, queue len {}",
            i,
            self.head,
            self.count,
            SAMPLE_QUEUE_LENGTH
        );
        &self.samples[i as usize]
    }

    // Outputs to file |f| samples from |start_time| to the head.  Uses a start
    // time rather than a start index because when we start a clip we have to
    // do a time-based search anyway.
    fn output_from_time(&self, f: &mut File, start_time: i64) -> Result<()> {
        // For now just do a linear search. ;)
        let mut start_index = modulo(self.ihead() - 1, SAMPLE_QUEUE_LENGTH);
        debug!(
            "output_from_time: start_time {}, head {}",
            start_time, start_index
        );
        loop {
            let sample = self.samples[start_index];
            // Ignore samples from external sources because their time stamp may be off.
            if sample.uptime <= start_time && sample.sample_type.has_internal_timestamp() {
                break;
            }
            debug!(
                "output_from_time: seeking uptime {}, index {}",
                sample.uptime, start_index
            );
            start_index = modulo(start_index as isize - 1, SAMPLE_QUEUE_LENGTH);
            if modulo(self.ihead() - 1 - start_index as isize, SAMPLE_QUEUE_LENGTH) > self.count {
                warn!("too many events in requested interval");
                break;
            }
        }

        let mut index = modulo(start_index as isize + 1, SAMPLE_QUEUE_LENGTH) as isize;
        while index != self.ihead() {
            debug!("output_from_time: outputting index {}", index);
            self.sample(index).output(f)?;
            index = modulo(index + 1, SAMPLE_QUEUE_LENGTH) as isize;
        }
        Ok(())
    }
}

// Returns the number of processes currently runnable (running or on ready queue).
// Rule of thumb:
// runnable / CPU_count < 3, CPU loading is not high.
// runnable / CPU_count > 5, CPU loading is very high.
fn get_runnables() -> Result<u32> {
    Ok(parse_runnables(LoadAverage::new()?))
}

fn parse_runnables(load_average: LoadAverage) -> u32 {
    load_average.cur
}

fn get_vmstats() -> Result<[i64; VMSTAT_VALUES_COUNT]> {
    let vmstats = procfs::vmstat()?;
    parse_vmstats(&vmstats)
}

fn parse_vmstats(vmstats: &HashMap<String, i64>) -> Result<[i64; VMSTAT_VALUES_COUNT]> {
    let mut result = [0i64; VMSTAT_VALUES_COUNT];
    for (i, &(field_name, mandatory, accumulate)) in VMSTATS.iter().enumerate() {
        if accumulate {
            for (sub_field_name, value) in vmstats {
                if sub_field_name.starts_with(field_name) {
                    result[i] += value;
                }
            }
        } else {
            match vmstats.get(field_name) {
                Some(value) => result[i] = *value,
                None => {
                    if mandatory {
                        return Err(format!("vmstat: missing value: {}", field_name).into());
                    }
                }
            }
        }
    }
    Ok(result)
}

fn pread_u32(f: &File) -> Result<u32> {
    let mut buffer: [u8; 20] = [0; 20];
    let length = nix::sys::uio::pread(f.as_raw_fd(), &mut buffer, 0)?;
    if length == 0 {
        return Err("empty pread_u32".into());
    }
    Ok(String::from_utf8_lossy(&buffer[..length as usize])
        .trim()
        .parse::<u32>()?)
}

struct Watermarks {
    min: u32,
    low: u32,
    high: u32,
}

struct ZoneinfoFile(File);

impl ZoneinfoFile {
    // Computes and returns the watermark values from /proc/zoneinfo.
    fn read_watermarks(&mut self) -> Result<Watermarks> {
        let mut min = 0;
        let mut low = 0;
        let mut high = 0;
        let mut content = String::new();
        self.0.read_to_string(&mut content)?;
        for line in content.lines() {
            let items = line.split_whitespace().collect::<Vec<_>>();
            match items[0] {
                "min" => min += items[1].parse::<u32>()?,
                "low" => low += items[1].parse::<u32>()?,
                "high" => high += items[1].parse::<u32>()?,
                _ => {}
            }
        }
        Ok(Watermarks { min, low, high })
    }
}

enum DbusEvent {
    TabDiscard { time: i64 },
    OomKill { time: i64 },
    OomKillKernel { time: i64 },
    CriticalMemoryPressure,
}

// The main object.
struct Sampler<'a> {
    always_poll_fast: bool, // When true, program stays in fast poll mode.
    paths: &'a Paths,       // Paths of files used by the program.
    dbus_receiver: crossbeam_channel::Receiver<DbusEvent>,
    low_mem_margin_mb: u32, // Low-memory margin, assumed to remain constant in a boot session.
    sample_header: String,  // The text at the beginning of each clip file.
    files: Files,           // Files kept open by the program.
    clip_counter: usize,    // Index of next clip file (also part of file name).
    sample_queue: SampleQueue, // A running queue of samples of vm quantities.
    current_available: u32, // Amount of "available" memory (in MB) at last reading.
    current_time: i64,      // Wall clock in ms at last reading.
    collecting: bool,       // True if we're in the middle of collecting a clip.
    timer: Box<dyn Timer>,  // Real or mock timer.
    quit_request: bool,     // Signals a quit-and-restart request when testing.
}

impl<'a> Sampler<'a> {
    fn new(
        always_poll_fast: bool,
        paths: &'a Paths,
        timer: Box<dyn Timer>,
        dbus_receiver: crossbeam_channel::Receiver<DbusEvent>,
        low_mem_margin_mb: u32,
    ) -> Result<Sampler> {
        let mut low_mem_file_flags = OpenOptions::new();
        low_mem_file_flags.custom_flags(libc::O_NONBLOCK);
        low_mem_file_flags.read(true);

        let files = Files {
            available_file_option: open_maybe(&paths.available)
                .map_err(Error::AvailableFileError)?,
        };

        let sample_header = build_sample_header();

        let mut sampler = Sampler {
            always_poll_fast,
            dbus_receiver,
            low_mem_margin_mb,
            paths,
            sample_header,
            files,
            sample_queue: SampleQueue::new(),
            clip_counter: 0,
            collecting: false,
            current_available: 0,
            current_time: 0,
            timer,
            quit_request: false,
        };
        sampler
            .find_starting_clip_counter()
            .map_err(Error::StartingClipCounterMissingError)?;
        sampler
            .log_static_parameters(low_mem_margin_mb)
            .map_err(Error::LogStaticParametersError)?;
        Ok(sampler)
    }

    // Refresh cached time.  This should be called after system calls, which
    // can potentially block, but not if current_time is unused before the next call.
    fn refresh_time(&mut self) {
        self.current_time = self.timer.now();
    }

    // Collect a sample using the latest time snapshot.
    fn enqueue_sample(&mut self, sample_type: SampleType) -> Result<()> {
        let time = self.current_time; // to pacify the borrow checker
        self.enqueue_sample_at_time(sample_type, time)
    }

    // Collect a sample with an externally-generated time stamp.
    fn enqueue_sample_external(&mut self, sample_type: SampleType, time: i64) -> Result<()> {
        assert!(!sample_type.has_internal_timestamp());
        self.enqueue_sample_at_time(sample_type, time)
    }

    // Collects a sample of memory manager stats and adds it to the sample
    // queue, possibly overwriting an old value.  |sample_type| indicates the
    // type of sample, and |time| the system uptime at the time the sample was
    // collected.
    fn enqueue_sample_at_time(&mut self, sample_type: SampleType, time: i64) -> Result<()> {
        {
            let sample: &mut Sample = self.sample_queue.next_slot();
            sample.uptime = time;
            sample.sample_type = sample_type;
            sample.available = self.current_available;
            sample.runnables = get_runnables()?;
            sample.info = if cfg!(test) {
                Sysinfo::fake_sysinfo()?
            } else {
                sysinfo()?
            };
            sample.vmstat_values = get_vmstats()?;
        }
        self.refresh_time();
        Ok(())
    }

    // Creates or overwrites a file in the memd log directory containing
    // quantities of interest.
    fn log_static_parameters(&self, low_mem_margin_mb: u32) -> Result<()> {
        let out = &mut OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(&self.paths.static_parameters)?;
        fprint_datetime(out)?;
        writeln!(out, "margin {}", low_mem_margin_mb)?;

        let psv = &self.paths.procsysvm;
        log_from_procfs(out, psv, "min_filelist_kbytes")?;
        log_from_procfs(out, psv, "min_free_kbytes")?;
        log_from_procfs(out, psv, "extra_free_kbytes")?;

        let mut zoneinfo = ZoneinfoFile(File::open(&self.paths.zoneinfo)?);
        let watermarks = zoneinfo.read_watermarks()?;
        writeln!(out, "min_water_mark_kbytes {}", watermarks.min * 4)?;
        writeln!(out, "low_water_mark_kbytes {}", watermarks.low * 4)?;
        writeln!(out, "high_water_mark_kbytes {}", watermarks.high * 4)?;
        Ok(())
    }

    // Returns true if the program should go back to slow-polling mode (or stay
    // in that mode).  Returns false otherwise.  Relies on |self.collecting|
    // and |self.current_available| being up-to-date.  If the kernel does not
    // have the cros low-mem notifier, "margin" and "available" are both 0, and
    // this always returns false. (XXX should use a different way of detecting
    // medium pressure, but it's not critical since all cros devices have a
    // low-mem device.)
    fn should_poll_slowly(&self) -> bool {
        !self.collecting
            && !self.always_poll_fast
            && self.low_mem_margin_mb > 0
            && self.current_available > self.low_mem_margin_mb + LOW_MEM_DANGER_THRESHOLD_MB
    }

    // Sits mostly idle and checks available RAM at low frequency.  Returns
    // when available memory gets "close enough" to the tab discard margin.
    fn slow_poll(&mut self) -> Result<()> {
        debug!("entering slow poll at {}", self.current_time);
        // Idiom for do ... while.
        while {
            let event_ready = self
                .timer
                .select(&self.dbus_receiver, SLOW_POLL_PERIOD_DURATION)?;
            debug!("event_ready: {} at {}", event_ready, self.timer.now());
            self.quit_request = self.timer.quit_request();
            if let Some(available_file) = self.files.available_file_option.as_ref() {
                self.current_available = pread_u32(available_file)?;
            }
            self.refresh_time();
            self.should_poll_slowly() && !self.quit_request && !event_ready
        } {}
        Ok(())
    }

    // Collects timer samples at fast rate.  Also collects event samples.
    // Samples contain various system stats related to kernel memory
    // management.  The samples are placed in a circular buffer.  When
    // something "interesting" happens, (for instance a tab discard, or a
    // kernel OOM-kill) the samples around that event are saved into a "clip
    // file".
    fn fast_poll(&mut self) -> Result<()> {
        let mut earliest_start_time = self.current_time;
        debug!("entering fast poll at {}", earliest_start_time);

        // Collect the first timer sample immediately upon entering.
        self.enqueue_sample(SampleType::Timer)?;
        // Keep track if we're in a low-mem state.  Initially assume we are
        // not.
        let mut was_in_low_mem = false;
        // |clip_{start,end}_time| are the collection start and end time for
        // the current clip.  Their value is valid only when |self.collecting|
        // is true.
        let mut clip_start_time = self.current_time;
        let mut clip_end_time = self.current_time;
        // |final_collection_time| indicates when we should stop collecting
        // samples for any clip (either the current one, or the next one).  Its
        // value is valid only when |self.collecting| is true.
        let mut final_collection_time = self.current_time;

        // |self.collecting| is true when we're in the middle of collecting a clip
        // because something interesting has happened.
        self.collecting = false;

        // Poll/select loop.
        loop {
            // Assume event is not interesting (since most events
            // aren't). Change this to true for some event types.
            let mut event_is_interesting = false;

            let watch_start_time = self.current_time;
            let event_ready = self
                .timer
                .select(&self.dbus_receiver, FAST_POLL_PERIOD_DURATION)?;
            // Check for dbus events.
            while let Ok(dbus_event) = self.dbus_receiver.try_recv() {
                let (sample_type, sample_time) = match dbus_event {
                    DbusEvent::TabDiscard { time } => (SampleType::TabDiscard, time),
                    DbusEvent::OomKill { time } => (SampleType::OomKillBrowser, time),
                    DbusEvent::OomKillKernel { time } => (SampleType::OomKillKernel, time),
                    DbusEvent::CriticalMemoryPressure => (SampleType::Uninitialized, 0),
                };
                if sample_type != SampleType::Uninitialized {
                    debug!("enqueue {:?}, {:?}", sample_type, sample_time);
                    self.enqueue_sample_external(sample_type, sample_time)?;
                }
                event_is_interesting = true;
            }
            self.quit_request = self.timer.quit_request();
            if let Some(available_file) = self.files.available_file_option.as_ref() {
                self.current_available = pread_u32(available_file)?;
            }
            let in_low_mem = self.current_available < self.low_mem_margin_mb;
            self.refresh_time();

            // Record a sample when we sleep too long.  Such samples are
            // somewhat redundant as they could be deduced from the log, but we
            // wish to make it easier to detect such (rare, we hope)
            // occurrences.
            if watch_start_time > self.current_time + UNREASONABLY_LONG_SLEEP {
                warn!(
                    "woke up at {} after unreasonable {} sleep",
                    self.current_time,
                    self.current_time - watch_start_time
                );
                self.enqueue_sample(SampleType::Sleeper)?;
            }

            if was_in_low_mem && !in_low_mem {
                // Refresh time since we may have blocked.  (That should
                // not happen often because currently the run times between
                // sleeps are well below the minimum timeslice.)
                self.current_time = self.timer.now();
                debug!("leaving low mem at {}", self.current_time);
                was_in_low_mem = false;
                self.enqueue_sample(SampleType::LeaveLowMem)?;
            }

            if !event_ready {
                // Timer event.
                self.enqueue_sample(SampleType::Timer)?;
            } else {
                // See comment above about watching low_mem.
                let low_mem_has_fired = !was_in_low_mem && in_low_mem;
                if low_mem_has_fired {
                    debug!("entering low mem at {}", self.current_time);
                    was_in_low_mem = true;
                    self.enqueue_sample(SampleType::EnterLowMem)?;
                    // Make this interesting at least until chrome events are
                    // plumbed, maybe permanently.
                    event_is_interesting = true;
                }
            }

            // Arrange future saving of samples around interesting events.
            if event_is_interesting {
                // Update the time intervals to ensure we include all samples
                // of interest in a clip.  If we're not in the middle of
                // collecting a clip, start one.  If we're in the middle of
                // collecting a clip which can be extended, do that.
                final_collection_time = self.current_time + COLLECTION_DELAY_MS;
                if self.collecting {
                    // Check if the current clip can be extended.
                    if clip_end_time < clip_start_time + CLIP_COLLECTION_SPAN_MS {
                        clip_end_time =
                            final_collection_time.min(clip_start_time + CLIP_COLLECTION_SPAN_MS);
                        debug!("extending clip to {}", clip_end_time);
                    }
                } else {
                    // Start the clip collection.
                    self.collecting = true;
                    clip_start_time =
                        earliest_start_time.max(self.current_time - COLLECTION_DELAY_MS);
                    clip_end_time = self.current_time + COLLECTION_DELAY_MS;
                    debug!(
                        "starting new clip from {} to {}",
                        clip_start_time, clip_end_time
                    );
                }
            }

            // Check if it is time to save the samples into a file.
            if self.collecting && self.current_time > clip_end_time - SAMPLING_PERIOD_MS {
                // Save the clip to disk.
                debug!(
                    "[{}] saving clip: ({} ... {}), final {}",
                    self.current_time, clip_start_time, clip_end_time, final_collection_time
                );
                let res = self.save_clip(clip_start_time);
                // Don't panic if there's an error writing to disk, log it instead.
                if res.is_err() {
                    warn!("Error saving clip: {:?}", res);
                }
                self.collecting = false;
                earliest_start_time = clip_end_time;
                // Need to schedule another collection?
                if final_collection_time > clip_end_time {
                    // Continue collecting by starting a new clip.  Note that
                    // the clip length may be less than CLIP_COLLECTION_SPAN.
                    // This happens when event spans overlap, and also if we
                    // started fast sample collection just recently.
                    assert!(final_collection_time <= clip_end_time + CLIP_COLLECTION_SPAN_MS);
                    clip_start_time = clip_end_time;
                    clip_end_time = final_collection_time;
                    self.collecting = true;
                    debug!(
                        "continue collecting with new clip ({} {})",
                        clip_start_time, clip_end_time
                    );
                    // If we got stuck in the select() for a very long time
                    // because of system slowdown, it may be time to collect
                    // this second clip as well.  But we don't bother, since
                    // this is unlikely, and we can collect next time around.
                    if self.current_time > clip_end_time {
                        debug!(
                            "heavy slowdown: postponing collection of ({}, {})",
                            clip_start_time, clip_end_time
                        );
                    }
                }
            }
            if self.should_poll_slowly() || (self.quit_request && !self.collecting) {
                break;
            }
        }
        Ok(())
    }

    // Returns the clip file pathname to be used after the current one,
    // and advances the clip counter.
    fn next_clip_path(&mut self) -> PathBuf {
        let name = format!("memd.clip{:03}.log", self.clip_counter);
        self.clip_counter = modulo(self.clip_counter as isize + 1, MAX_CLIP_COUNT);
        self.paths.log_directory.join(name)
    }

    // Finds and sets the starting value for the clip counter in this session.
    // The goal is to preserve the most recent clips (if any) from previous
    // sessions.
    fn find_starting_clip_counter(&mut self) -> Result<()> {
        self.clip_counter = 0;
        let mut previous_time = std::time::UNIX_EPOCH;
        loop {
            let path = self.next_clip_path();
            if !path.exists() {
                break;
            }
            let md = std::fs::metadata(path)?;
            let time = md.modified()?;
            if time < previous_time {
                break;
            } else {
                previous_time = time;
            }
        }
        // We found the starting point, but the counter has already advanced so we
        // need to backtrack one step.
        self.clip_counter = modulo(self.clip_counter as isize - 1, MAX_CLIP_COUNT);
        Ok(())
    }

    // Stores samples of interest in a new clip log, and remove those samples
    // from the queue.
    fn save_clip(&mut self, start_time: i64) -> Result<()> {
        let path = self.next_clip_path();
        let out = &mut OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(path)?;

        // Print courtesy header.  The first line is the current time.  The
        // second line lists the names of the variables in the following lines,
        // in the same order.
        fprint_datetime(out)?;
        out.write_all(self.sample_header.as_bytes())?;
        // Output samples from |start_time| to the head.
        self.sample_queue.output_from_time(out, start_time)?;
        // The queue is now empty.
        self.sample_queue.reset();
        Ok(())
    }
}

// Prints |name| and value of entry /pros/sys/vm/|name| (or 0, if the entry is
// missing) to file |out|.
fn log_from_procfs(out: &mut File, dir: &Path, name: &str) -> Result<()> {
    let procfs_path = dir.join(name);
    let value = read_int(&procfs_path).unwrap_or(0);
    writeln!(out, "{} {}", name, value)?;
    Ok(())
}

// Outputs readable date and time to file |out|.
fn fprint_datetime(out: &mut File) -> Result<()> {
    let local: DateTime<Local> = Local::now();
    writeln!(out, "{}", local)?;
    Ok(())
}

#[derive(Copy, Clone, Debug, Default, PartialEq)]
enum SampleType {
    EnterLowMem,    // Entering low-memory state, from the kernel low-mem notifier.
    LeaveLowMem,    // Leaving low-memory state, from the kernel low-mem notifier.
    OomKillBrowser, // Chrome browser letting us know it detected OOM kill.
    OomKillKernel,  // Anomaly detector letting us know it detected OOM kill.
    Sleeper,        // Memd was not running for a long time.
    TabDiscard,     // Chrome browser letting us know about a tab discard.
    Timer,          // Event was produced after FAST_POLL_PERIOD_DURATION with no other events.
    #[default]
    Uninitialized, // Internal use.
}

impl fmt::Display for SampleType {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{}", self.name())
    }
}

impl SampleType {
    // Returns true if the timestamp for this sample type is generated
    // internally.  This knowledge is used in outputting samples to clip files,
    // because the timestamps of samples generated externally may be skewed.
    fn has_internal_timestamp(&self) -> bool {
        !matches!(
            self,
            &SampleType::TabDiscard | &SampleType::OomKillBrowser | &SampleType::OomKillKernel
        )
    }
}

impl SampleType {
    // Returns the 6-character(max) identifier for a sample type.
    fn name(&self) -> &'static str {
        match *self {
            SampleType::EnterLowMem => "lowmem",
            SampleType::LeaveLowMem => "lealow",
            SampleType::OomKillBrowser => "oomkll", // OOM from chrome
            SampleType::OomKillKernel => "keroom",  // OOM from kernel
            SampleType::Sleeper => "sleepr",
            SampleType::TabDiscard => "discrd",
            SampleType::Timer => "timer",
            SampleType::Uninitialized => "UNINIT",
        }
    }
}

// Path names of various system files, mostly in /proc, /sys, and /dev.  They
// are collected into this struct because they get special values when testing.
#[derive(Clone)]
pub struct Paths {
    available: PathBuf,
    log_directory: PathBuf,
    static_parameters: PathBuf,
    zoneinfo: PathBuf,
    procsysvm: PathBuf,
    testing_root: PathBuf,
}

// Returns a file name that replaces |name| when testing.
fn test_filename(testing: bool, testing_root: &str, name: &str) -> String {
    if testing {
        testing_root.to_string() + name
    } else {
        name.to_owned()
    }
}

// This macro constructs a "normal" Paths object when |testing| is false, and
// one that mimics a root filesystem in a temporary directory when |testing| is
// true.
macro_rules! make_paths {
    ($testing:expr, $root:expr,
     $($name:ident : $e:expr,)*
    ) => (
        Paths {
            testing_root: PathBuf::from($root),
            $($name: PathBuf::from(test_filename($testing, $root, &($e).to_string()))),*
        }
    )
}

// All files that are to be left open go here.  We keep them open to reduce the
// number of syscalls.  They are mostly files in /proc and /sys.
struct Files {
    // These files might not exist.
    available_file_option: Option<File>,
}

fn build_sample_header() -> String {
    let mut s = "uptime type load freeram freeswap procs runnables available".to_string();
    for vmstat in VMSTATS {
        s = s + " " + vmstat.0;
    }
    s + "\n"
}

fn main() -> Result<()> {
    let mut always_poll_fast = false;
    let mut debug_log = false;

    libchromeos::panic_handler::install_memfd_handler();
    let args: Vec<String> = std::env::args().collect();
    for arg in &args[1..] {
        match arg.as_ref() {
            "always-poll-fast" => always_poll_fast = true,
            "debug-log" => debug_log = true,
            _ => panic!("usage: memd [always-poll-fast|debug-log]*"),
        }
    }

    let log_level = if debug_log {
        log::LevelFilter::Debug
    } else {
        log::LevelFilter::Warn
    };
    syslog::init_unix(syslog::Facility::LOG_USER, log_level)
        .map_err(|e| Error::InitSyslogError(Box::new(e)))?;

    // Unlike log!(), warn!() etc., panic!() is not redirected by the syslog
    // facility, instead always goes to stderr, which can get lost.  Here we
    // provide our own panic hook to make sure that the panic message goes to
    // the syslog as well.
    let default_panic = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |panic_info| {
        error!("memd: {}", panic_info);
        default_panic(panic_info);
    }));

    warn!("memd started");
    run_memory_daemon(always_poll_fast)
}

// Creates a directory for testing, if testing.  Otherwise
// returns None
fn make_testing_dir() -> Option<TempDir> {
    if cfg!(test) {
        Some(TempDir::new().expect("cannot create temp dir"))
    } else {
        None
    }
}

fn get_paths(root: Option<TempDir>) -> Paths {
    // make_paths! returns a Paths object initializer with these fields.
    let testing_root = match root {
        Some(tmpdir) => tmpdir.path().to_str().unwrap().to_string(),
        None => "/".to_string(),
    };
    make_paths!(
        cfg!(test),
        &testing_root,
        available:         LOW_MEM_SYSFS.to_string() + "/available",
        log_directory:     LOG_DIRECTORY,
        static_parameters: LOG_DIRECTORY.to_string() + "/" + STATIC_PARAMETERS_LOG,
        zoneinfo:          "/proc/zoneinfo",
        procsysvm:         "/proc/sys/vm",
    )
}

// Receive D-Bus events and resend via channel.
#[cfg(not(test))]
fn receive_dbus_events(sender: crossbeam_channel::Sender<DbusEvent>) -> Result<()> {
    let connection = Connection::get_private(BusType::System)?;
    let _m = connection.add_match(concat!(
        "type='signal',",
        "interface='org.chromium.AnomalyEventServiceInterface',",
        "member='AnomalyEvent'"
    ));
    let _m = connection.add_match(concat!(
        "type='signal',",
        "interface='org.chromium.MetricsEventServiceInterface',",
        "member='ChromeEvent'"
    ));
    let _m = connection.add_match(concat!(
        "type='signal',",
        "interface='org.chromium.ResourceManager',",
        "member='MemoryPressureChrome'"
    ));

    let mut watch_fdset = select::FdSet::new();
    for fd in connection.watch_fds() {
        watch_fdset.insert(fd.fd());
    }
    let highest = watch_fdset.highest().ok_or("The fd set is empty")?;

    loop {
        let mut inout_fdset = watch_fdset;
        let _n = select::select(highest + 1, &mut inout_fdset, None, None, None)?;

        for fd in inout_fdset.fds(None) {
            let handle = connection.watch_handle(fd, WatchEvent::Readable as libc::c_uint);
            for connection_item in handle {
                // Only consider signals.
                if let ConnectionItem::Signal(ref message) = connection_item {
                    // Only consider signals with "ChromeEvent" or "AnomalyEvent" members.
                    if let Some(member) = message.member() {
                        if &*member == "ChromeEvent" || &*member == "AnomalyEvent" {
                            // Read first item in signal message as byte blob and
                            // parse blob into protobuf.
                            let raw_buffer: Vec<u8> = message.read1()?;
                            let mut protobuf = metrics_event::Event::new();
                            protobuf.merge_from_bytes(&raw_buffer)?;

                            let event_type = protobuf.type_.enum_value_or_default();
                            let time_stamp = protobuf.timestamp;
                            match event_type {
                                metrics_event::event::Type::TAB_DISCARD => {
                                    sender.send(DbusEvent::TabDiscard { time: time_stamp })?;
                                }
                                metrics_event::event::Type::OOM_KILL => {
                                    sender.send(DbusEvent::OomKill { time: time_stamp })?;
                                }
                                metrics_event::event::Type::OOM_KILL_KERNEL => {
                                    sender.send(DbusEvent::OomKillKernel { time: time_stamp })?;
                                }
                                _ => {
                                    warn!("unknown event type {:?}", event_type);
                                }
                            }
                        } else if &*member == "MemoryPressureChrome" {
                            let pressure_level: u8 = message.read1()?;
                            const PRESSURE_LEVEL_CHROME_CRITICAL: u8 = 2;
                            if pressure_level == PRESSURE_LEVEL_CHROME_CRITICAL {
                                sender.send(DbusEvent::CriticalMemoryPressure)?;
                            }
                        } else if &*member != "NameAcquired" {
                            // Do not report spurious "NameAcquired" signal to avoid spam.
                            warn!("unexpected dbus signal member {}", &*member);
                        }
                    }
                }
            }
        }
    }
}

// Get the memory margin by calling resource manager D-Bus method.
#[cfg(not(test))]
fn get_memory_margin_mb() -> Result<u32> {
    const RESOURCED_SERVICE_NAME: &str = "org.chromium.ResourceManager";
    const RESOURCED_PATH_NAME: &str = "/org/chromium/ResourceManager";
    const RESOURCED_INTERFACE_NAME: &str = RESOURCED_SERVICE_NAME;

    let conn = dbus::blocking::Connection::new_system()?;
    let proxy = conn.with_proxy(
        RESOURCED_INTERFACE_NAME,
        RESOURCED_PATH_NAME,
        Duration::from_millis(5000),
    );
    let (critical, _moderate): (u64, u64) =
        proxy.method_call(RESOURCED_SERVICE_NAME, "GetMemoryMarginsKB", ())?;
    Ok((critical / 1024) as u32)
}

fn run_memory_daemon(always_poll_fast: bool) -> Result<()> {
    let test_dir_option = make_testing_dir();
    let paths = get_paths(test_dir_option);
    debug!("Using root: {}", paths.testing_root.display());

    #[cfg(test)]
    {
        test::setup_test_environment(&paths);
        let var_log = &paths.log_directory.parent().unwrap();
        std::fs::create_dir_all(var_log).map_err(|e| Error::CreateLogDirError(Box::new(e)))?;
    }

    // Make sure /var/log/memd exists.  Create it if not.  Assume /var/log
    // exists.  Panic on errors.
    if !paths.log_directory.exists() {
        create_dir(&paths.log_directory).map_err(|e| Error::CreateLogDirError(Box::new(e)))?
    }

    #[cfg(test)]
    {
        test::test_loop(always_poll_fast, &paths);
        test::teardown_test_environment(&paths);
        Ok(())
    }

    #[cfg(not(test))]
    {
        let timer = Box::new(GenuineTimer {});
        let (send, recv) = crossbeam_channel::unbounded();
        let _sender_thread = std::thread::spawn(move || {
            let _ = receive_dbus_events(send);
        });
        let mut sampler = Sampler::new(
            always_poll_fast,
            &paths,
            timer,
            recv,
            get_memory_margin_mb()?,
        )?;
        loop {
            // Run forever, alternating between slow and fast poll.
            sampler.slow_poll()?;
            sampler.fast_poll()?;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_vmstats() {
        let vmstats: HashMap<String, i64> = HashMap::from([
            ("noop".to_string(), 600),
            ("pswpin".to_string(), 100),
            ("pswpout".to_string(), 200),
            ("pgalloc_dma".to_string(), 1000),
            ("pgalloc_dma32".to_string(), 2000),
            ("pgalloc_normal".to_string(), 3000),
            ("pgalloc_movable".to_string(), 4000),
            ("pgmajfault".to_string(), 1500),
            ("pgmajfault_f".to_string(), 550),
        ]);
        let result = parse_vmstats(&vmstats).unwrap();
        assert_eq!(result[0], 100); // pswpin
        assert_eq!(result[1], 200); // pswpout
        assert_eq!(result[2], 10000); // paalloc
        assert_eq!(result[3], 1500); // pgmajfault
        assert_eq!(result[4], 550); // pgmajfault_f
    }

    #[test]
    fn test_parse_runnables() {
        // See also https://docs.kernel.org/filesystems/proc.html for field values of
        // /proc/loadavg.
        let load_average =
            LoadAverage::from_reader("3.15 2.15 1.15 5/990 1270".as_bytes()).unwrap();
        let runnables = parse_runnables(load_average);
        assert_eq!(runnables, 5);
    }

    /// Regression test for https://crbug.com/1058463. Ensures that output_from_time doesn't read
    /// samples outside of the valid range.
    #[test]
    fn queue_loop_test() {
        test::queue_loop();
    }

    #[test]
    fn memory_daemon_test() {
        env_logger::init();
        run_memory_daemon(false).expect("run_memory_daemon error");
    }
}
