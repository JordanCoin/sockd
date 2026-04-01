use crate::config::AutoStart;
use crate::error::{Error, Result};
use crate::frame::{DEFAULT_MAX_PAYLOAD_LEN, FrameKind, read_frame, write_frame};
use crate::pidfile::{is_process_alive, read_pid};
use crate::process;
use crate::socket;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::thread;
use std::time::{Duration, Instant};

/// Unix socket client for communicating with a sockd daemon.
///
/// Connects to a daemon via Unix socket, sends framed requests, and
/// returns responses. Optionally auto-starts the daemon if it's not running.
///
/// # Auto-start
///
/// When configured with [`with_auto_start`](Client::with_auto_start), the client
/// detects a dead or missing daemon and spawns it automatically. It waits
/// for the daemon to become ready (verified via a `Ping` round-trip) before
/// sending the actual request. This means the first call may take a few
/// hundred milliseconds while the daemon starts; subsequent calls connect
/// to the already-running daemon instantly.
///
/// # Connection model
///
/// Each `request()` / `ping()` / `shutdown()` call opens a new connection,
/// sends one frame, reads one frame, and closes. This matches the
/// "hook per tool call" pattern where the client is short-lived.
#[derive(Clone, Debug)]
pub struct Client {
    socket_path: PathBuf,
    pid_file: Option<PathBuf>,
    auto_start: Option<AutoStart>,
    max_payload_len: usize,
    shutdown_timeout: Duration,
}

impl Client {
    /// Create a new client targeting the given Unix socket path.
    pub fn new(socket_path: impl Into<PathBuf>) -> Self {
        Self {
            socket_path: socket_path.into(),
            pid_file: None,
            auto_start: None,
            max_payload_len: DEFAULT_MAX_PAYLOAD_LEN,
            shutdown_timeout: Duration::from_secs(2),
        }
    }

    /// Set the PID file path. Used to check if the daemon is alive
    /// before attempting auto-start.
    pub fn with_pid_file(mut self, pid_file: impl Into<PathBuf>) -> Self {
        self.pid_file = Some(pid_file.into());
        self
    }

    /// Enable auto-start with the given daemon binary path.
    ///
    /// If a `request()` or `ping()` fails because the daemon isn't running,
    /// the client will fork/exec this binary, wait for readiness, then retry.
    pub fn with_auto_start(mut self, program: impl Into<PathBuf>) -> Self {
        self.auto_start = Some(AutoStart::new(program));
        self
    }

    /// Enable auto-start with full configuration (custom args, timeouts, backoff).
    pub fn with_auto_start_config(mut self, auto_start: AutoStart) -> Self {
        self.auto_start = Some(auto_start);
        self
    }

    /// Maximum payload size in bytes. Default: 16 MB.
    pub fn with_max_payload_len(mut self, max_payload_len: usize) -> Self {
        self.max_payload_len = max_payload_len;
        self
    }

    /// How long to wait for the daemon to finish shutting down after
    /// receiving a `ShutdownAck`. Default: 2 seconds.
    pub fn with_shutdown_timeout(mut self, shutdown_timeout: Duration) -> Self {
        self.shutdown_timeout = shutdown_timeout;
        self
    }

    /// Send a `Ping` frame and wait for `Pong`. Verifies the daemon is alive.
    ///
    /// If auto-start is configured and the daemon isn't running, this will
    /// start it and wait for readiness.
    pub fn ping(&self) -> Result<()> {
        match self.try_ping_once() {
            Ok(()) => Ok(()),
            Err(err) => self.recover_if_needed(err),
        }
    }

    /// Send a request and return the response bytes.
    ///
    /// If the first attempt fails with a recoverable error (daemon not running)
    /// and auto-start is configured, starts the daemon and retries once.
    ///
    /// The payload is protocol-agnostic: send JSON, protobuf, msgpack, or raw bytes.
    pub fn request(&self, payload: impl AsRef<[u8]>) -> Result<Vec<u8>> {
        let payload = payload.as_ref();

        match self.send_request_once(payload) {
            Ok(response) => Ok(response),
            Err(err) => {
                self.recover_if_needed(err)?;
                self.send_request_once(payload)
            }
        }
    }

    /// Request a graceful shutdown of the daemon.
    ///
    /// Returns `true` if the daemon was running and acknowledged the shutdown.
    /// Returns `false` if the daemon wasn't running (nothing to shut down).
    /// Waits up to `shutdown_timeout` for the daemon to fully exit.
    pub fn shutdown(&self) -> Result<bool> {
        let mut stream = match socket::connect(&self.socket_path) {
            Ok(stream) => stream,
            Err(err) if self.should_treat_connect_error_as_not_running(&err)? => return Ok(false),
            Err(err) => return Err(Error::Io(err)),
        };

        write_frame(&mut stream, FrameKind::Shutdown, &[])?;
        let frame = read_frame(&mut stream, self.max_payload_len)?;

        match frame.kind {
            FrameKind::ShutdownAck => {
                self.wait_for_shutdown()?;
                Ok(true)
            }
            FrameKind::Error => Err(Error::DaemonReturnedError(
                String::from_utf8_lossy(&frame.payload).into_owned(),
            )),
            other => Err(Error::UnexpectedFrameKind {
                expected: FrameKind::ShutdownAck.label(),
                actual: other.label(),
            }),
        }
    }

    fn send_request_once(&self, payload: &[u8]) -> Result<Vec<u8>> {
        let mut stream = socket::connect(&self.socket_path).map_err(Error::Io)?;
        write_frame(&mut stream, FrameKind::Request, payload)?;
        let frame = read_frame(&mut stream, self.max_payload_len)?;
        match frame.kind {
            FrameKind::Response => Ok(frame.payload),
            FrameKind::Error => Err(Error::DaemonReturnedError(
                String::from_utf8_lossy(&frame.payload).into_owned(),
            )),
            other => Err(Error::UnexpectedFrameKind {
                expected: FrameKind::Response.label(),
                actual: other.label(),
            }),
        }
    }

    fn recover_if_needed(&self, err: Error) -> Result<()> {
        let Some(auto_start) = &self.auto_start else {
            return Err(err);
        };

        if !can_attempt_recovery(&err) {
            return Err(err);
        }

        if !self.pid_file_points_to_live_process()? {
            process::spawn_detached(auto_start)?;
        }
        self.wait_for_ready(auto_start)
    }

    fn wait_for_ready(&self, auto_start: &AutoStart) -> Result<()> {
        let deadline = Instant::now() + auto_start.ready_timeout;
        let mut backoff = auto_start.initial_backoff;

        loop {
            match self.try_ping_once() {
                Ok(()) => return Ok(()),
                Err(err) if can_attempt_recovery(&err) && Instant::now() < deadline => {
                    thread::sleep(backoff);
                    backoff = backoff.saturating_mul(2).min(auto_start.max_backoff);
                }
                Err(err) if can_attempt_recovery(&err) => {
                    return Err(Error::ReadyTimeout {
                        socket: self.socket_path.clone(),
                        timeout: auto_start.ready_timeout,
                    });
                }
                Err(other) => return Err(other),
            }
        }
    }

    fn try_ping_once(&self) -> Result<()> {
        let mut stream = socket::connect(&self.socket_path).map_err(Error::Io)?;
        write_frame(&mut stream, FrameKind::Ping, &[])?;
        let frame = read_frame(&mut stream, self.max_payload_len)?;

        match frame.kind {
            FrameKind::Pong => Ok(()),
            other => Err(Error::UnexpectedFrameKind {
                expected: FrameKind::Pong.label(),
                actual: other.label(),
            }),
        }
    }

    fn pid_file_points_to_live_process(&self) -> Result<bool> {
        let Some(pid_file) = self.pid_file.as_deref() else {
            return Ok(false);
        };

        match read_pid(pid_file)? {
            Some(pid) => Ok(is_process_alive(pid)),
            None => Ok(false),
        }
    }

    pub fn socket_path(&self) -> &Path {
        &self.socket_path
    }

    fn wait_for_shutdown(&self) -> Result<()> {
        let deadline = Instant::now() + self.shutdown_timeout;

        loop {
            if self.shutdown_complete()? {
                return Ok(());
            }

            if Instant::now() >= deadline {
                return Err(Error::ShutdownTimeout {
                    socket: self.socket_path.clone(),
                    timeout: self.shutdown_timeout,
                });
            }

            thread::sleep(Duration::from_millis(20));
        }
    }

    fn shutdown_complete(&self) -> Result<bool> {
        let socket_gone = match fs::metadata(&self.socket_path) {
            Ok(_) => false,
            Err(err) if err.kind() == io::ErrorKind::NotFound => true,
            Err(err) => return Err(Error::Io(err)),
        };

        if !socket_gone {
            return Ok(false);
        }

        Ok(!self.pid_file_points_to_live_process()?)
    }

    fn should_treat_connect_error_as_not_running(&self, err: &io::Error) -> Result<bool> {
        if !matches_recoverable_connect_error(err) {
            return Ok(false);
        }

        Ok(!self.pid_file_points_to_live_process()?)
    }
}

fn can_attempt_recovery(err: &Error) -> bool {
    match err {
        Error::Io(err) => matches_recoverable_connect_error(err),
        _ => false,
    }
}

fn matches_recoverable_connect_error(err: &io::Error) -> bool {
    matches!(
        err.kind(),
        io::ErrorKind::NotFound
            | io::ErrorKind::ConnectionRefused
            | io::ErrorKind::ConnectionAborted
            | io::ErrorKind::NotConnected
    )
}
