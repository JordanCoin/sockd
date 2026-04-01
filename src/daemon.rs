use crate::config::DaemonConfig;
use crate::error::{CallbackResult, Error, Result};
use crate::frame::{FrameKind, read_frame, write_frame};
use crate::pidfile::PidGuard;
use crate::socket;
use signal_hook::consts::signal::{SIGINT, SIGTERM};
use std::os::fd::AsRawFd;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

type StartCallback<S> = Box<dyn FnMut() -> CallbackResult<S> + Send>;
type RequestCallback<S> = Box<dyn FnMut(&mut S, &[u8]) -> CallbackResult<Vec<u8>> + Send>;
type StopCallback<S> = Box<dyn FnMut(S) -> CallbackResult<()> + Send>;

enum ConnectionOutcome {
    Continue,
    Shutdown,
}

/// A managed Unix IPC daemon.
///
/// `Daemon` handles the full lifecycle: PID file ownership, Unix socket
/// bind/listen, signal-based shutdown (SIGINT/SIGTERM), idle timeout,
/// and framed request/response dispatch.
///
/// The type parameter `S` is your application state, created by `on_start`
/// and passed to every `on_request` call. When the daemon exits (idle
/// timeout, signal, or client shutdown), `on_stop` receives the state
/// for cleanup.
///
/// # Lifecycle
///
/// 1. Claim PID file (kills stale daemon if needed)
/// 2. Call `on_start()` to initialize state
/// 3. Bind Unix socket
/// 4. Enter event loop: accept connections, dispatch frames
/// 5. On exit: close socket, call `on_stop(state)`, remove PID file
///
/// See [`DaemonBuilder`] for configuration options.
pub struct Daemon<S> {
    config: DaemonConfig,
    on_start: StartCallback<S>,
    on_request: RequestCallback<S>,
    on_stop: StopCallback<S>,
}

/// Builder for configuring and constructing a [`Daemon`].
///
/// Start with [`Daemon::builder()`], chain configuration methods,
/// then call [`.build()`](DaemonBuilder::build) to produce a `Daemon`.
///
/// `on_request` is required. All other callbacks and settings have defaults.
pub struct DaemonBuilder<S> {
    config: DaemonConfig,
    on_start: StartCallback<S>,
    on_request: Option<RequestCallback<S>>,
    on_stop: StopCallback<S>,
}

impl Daemon<()> {
    /// Create a new daemon builder with default configuration.
    ///
    /// Defaults: socket at `/tmp/sockd.sock`, no PID file, 300s idle timeout.
    pub fn builder() -> DaemonBuilder<()> {
        DaemonBuilder {
            config: DaemonConfig::default(),
            on_start: Box::new(|| Ok(())),
            on_request: None,
            on_stop: Box::new(|_| Ok(())),
        }
    }
}

impl DaemonBuilder<()> {
    /// Set the startup callback and define the state type.
    ///
    /// Called once when the daemon starts, before the socket is bound.
    /// Return your application state (e.g., loaded config, parsed indexes,
    /// mmap'd files). This state is passed to every `on_request` call.
    pub fn on_start<S, F>(self, on_start: F) -> DaemonBuilder<S>
    where
        S: Send + 'static,
        F: FnMut() -> CallbackResult<S> + Send + 'static,
    {
        DaemonBuilder {
            config: self.config,
            on_start: Box::new(on_start),
            on_request: None,
            on_stop: Box::new(|_| Ok(())),
        }
    }
}

impl<S> DaemonBuilder<S>
where
    S: Send + 'static,
{
    /// Set the Unix socket path. Default: `/tmp/sockd.sock`.
    pub fn socket(mut self, socket_path: impl Into<PathBuf>) -> Self {
        self.config.socket_path = socket_path.into();
        self
    }

    /// Set the PID file path. Optional.
    ///
    /// When set, the daemon claims this file on startup (killing any stale
    /// process that holds it) and removes it on shutdown. Enables client
    /// auto-start and stale recovery.
    pub fn pid_file(mut self, pid_file: impl Into<PathBuf>) -> Self {
        self.config.pid_file = Some(pid_file.into());
        self
    }

    /// How long the daemon waits with no connections before exiting.
    /// Default: 300 seconds (5 minutes).
    pub fn idle_timeout(mut self, idle_timeout: Duration) -> Self {
        self.config.idle_timeout = idle_timeout;
        self
    }

    /// How often to wake up and check for idle timeout. Default: 250ms.
    /// Lower values mean more responsive idle exit but slightly more CPU.
    pub fn poll_interval(mut self, poll_interval: Duration) -> Self {
        self.config.poll_interval = poll_interval;
        self
    }

    /// Maximum payload size in bytes. Default: 16 MB.
    /// Frames with larger payloads are rejected with `PayloadTooLarge`.
    pub fn max_payload_len(mut self, max_payload_len: usize) -> Self {
        self.config.max_payload_len = max_payload_len;
        self
    }

    /// Set the request handler. **Required.**
    ///
    /// Called for every incoming `Request` frame. Receives mutable state
    /// and the raw payload bytes. Return the response bytes.
    ///
    /// sockd is protocol-agnostic: you parse/serialize however you want
    /// (JSON, protobuf, msgpack, raw structs).
    pub fn on_request<F>(mut self, on_request: F) -> Self
    where
        F: FnMut(&mut S, &[u8]) -> CallbackResult<Vec<u8>> + Send + 'static,
    {
        self.on_request = Some(Box::new(on_request));
        self
    }

    /// Set the shutdown callback. Optional.
    ///
    /// Called when the daemon exits (idle timeout, signal, or client shutdown).
    /// Receives ownership of the state for cleanup (flush to disk, close
    /// connections, etc.).
    pub fn on_stop<F>(mut self, on_stop: F) -> Self
    where
        F: FnMut(S) -> CallbackResult<()> + Send + 'static,
    {
        self.on_stop = Box::new(on_stop);
        self
    }

    /// Build the daemon. Fails if `on_request` was not set.
    pub fn build(self) -> Result<Daemon<S>> {
        Ok(Daemon {
            config: self.config,
            on_start: self.on_start,
            on_request: self
                .on_request
                .ok_or(Error::MissingCallback("on_request"))?,
            on_stop: self.on_stop,
        })
    }
}

impl<S> Daemon<S>
where
    S: Send + 'static,
{
    /// Run the daemon. Blocks until shutdown.
    ///
    /// The daemon exits when any of these happen:
    /// - Idle timeout expires (no connections for `idle_timeout` duration)
    /// - SIGINT or SIGTERM received
    /// - A client sends a `Shutdown` frame
    ///
    /// On exit, the socket and PID files are cleaned up, and `on_stop`
    /// is called with the application state.
    pub fn run(mut self) -> Result<()> {
        let shutdown = install_shutdown_flag()?;
        let _pid_guard = match self.config.pid_file.as_deref() {
            Some(path) => Some(PidGuard::claim(path)?),
            None => None,
        };

        let mut state = (self.on_start)().map_err(Error::Startup)?;
        let listener = match socket::bind(&self.config.socket_path) {
            Ok(listener) => listener,
            Err(err) => {
                let stop_result = (self.on_stop)(state).map_err(Error::Shutdown);
                return match stop_result {
                    Ok(()) => Err(err),
                    Err(stop_err) => Err(stop_err),
                };
            }
        };

        let loop_result = self.run_event_loop(&listener, &mut state, &shutdown);

        drop(listener);
        let cleanup_result = socket::cleanup(&self.config.socket_path);
        let stop_result = (self.on_stop)(state).map_err(Error::Shutdown);

        loop_result?;
        cleanup_result?;
        stop_result?;
        Ok(())
    }

    fn run_event_loop(
        &mut self,
        listener: &UnixListener,
        state: &mut S,
        shutdown: &AtomicBool,
    ) -> Result<()> {
        let mut last_activity = Instant::now();

        while !shutdown.load(Ordering::Relaxed) {
            let idle_remaining = self
                .config
                .idle_timeout
                .saturating_sub(last_activity.elapsed());
            if idle_remaining.is_zero() {
                break;
            }

            let timeout = idle_remaining.min(self.config.poll_interval);
            if !poll_listener(listener, timeout)? {
                continue;
            }

            loop {
                match listener.accept() {
                    Ok((stream, _)) => {
                        stream.set_nonblocking(false).map_err(Error::Io)?;
                        last_activity = Instant::now();
                        match self.process_connection(stream, state)? {
                            ConnectionOutcome::Continue => {}
                            ConnectionOutcome::Shutdown => return Ok(()),
                        }
                    }
                    Err(err) if err.kind() == std::io::ErrorKind::WouldBlock => break,
                    Err(err) => return Err(Error::Io(err)),
                }
            }
        }

        Ok(())
    }

    fn process_connection(
        &mut self,
        mut stream: UnixStream,
        state: &mut S,
    ) -> Result<ConnectionOutcome> {
        let frame = read_frame(&mut stream, self.config.max_payload_len)?;

        match frame.kind {
            FrameKind::Ping => {
                write_frame(&mut stream, FrameKind::Pong, &[])?;
                Ok(ConnectionOutcome::Continue)
            }
            FrameKind::Shutdown => {
                write_frame(&mut stream, FrameKind::ShutdownAck, &[])?;
                Ok(ConnectionOutcome::Shutdown)
            }
            FrameKind::Request => match (self.on_request)(state, &frame.payload) {
                Ok(response) => write_frame(&mut stream, FrameKind::Response, &response)
                    .map(|_| ConnectionOutcome::Continue),
                Err(err) => {
                    let message = err.to_string();
                    write_frame(&mut stream, FrameKind::Error, message.as_bytes())
                }
                .map_err(|io_err| match io_err {
                    Error::Io(err) => Error::Io(err),
                    other => other,
                })
                .map(|_| ConnectionOutcome::Continue),
            },
            other => {
                let message = format!("unexpected frame kind: {other:?}");
                write_frame(&mut stream, FrameKind::Error, message.as_bytes())?;
                Ok(ConnectionOutcome::Continue)
            }
        }
    }
}

fn install_shutdown_flag() -> Result<Arc<AtomicBool>> {
    let flag = Arc::new(AtomicBool::new(false));
    signal_hook::flag::register(SIGINT, Arc::clone(&flag)).map_err(Error::Io)?;
    signal_hook::flag::register(SIGTERM, Arc::clone(&flag)).map_err(Error::Io)?;
    Ok(flag)
}

fn poll_listener(listener: &UnixListener, timeout: Duration) -> Result<bool> {
    let mut poll_fd = libc::pollfd {
        fd: listener.as_raw_fd(),
        events: libc::POLLIN,
        revents: 0,
    };
    let timeout_ms = timeout.as_millis().min(i32::MAX as u128) as i32;

    let ready = unsafe { libc::poll(&mut poll_fd, 1, timeout_ms) };
    if ready < 0 {
        let err = std::io::Error::last_os_error();
        if err.kind() == std::io::ErrorKind::Interrupted {
            return Ok(false);
        }
        return Err(Error::Io(err));
    }

    Ok(ready > 0 && (poll_fd.revents & libc::POLLIN) != 0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        Client,
        frame::{DEFAULT_MAX_PAYLOAD_LEN, FrameKind},
    };
    use std::thread;
    use tempfile::tempdir;

    #[test]
    fn accepts_client_that_writes_after_connecting() {
        let temp = tempdir().unwrap();
        let socket_path = temp.path().join("sockd.sock");
        let pid_path = temp.path().join("sockd.pid");

        let daemon = Daemon::builder()
            .socket(&socket_path)
            .pid_file(&pid_path)
            .idle_timeout(Duration::from_secs(2))
            .poll_interval(Duration::from_millis(10))
            .on_request(|_, req| Ok(req.iter().map(u8::to_ascii_uppercase).collect()))
            .build()
            .unwrap();

        let handle = thread::spawn(move || daemon.run());
        let client = Client::new(&socket_path).with_pid_file(&pid_path);

        let deadline = Instant::now() + Duration::from_secs(1);
        while Instant::now() < deadline {
            if socket_path.exists() {
                break;
            }
            thread::sleep(Duration::from_millis(10));
        }

        let mut stream = loop {
            match std::os::unix::net::UnixStream::connect(&socket_path) {
                Ok(stream) => break stream,
                Err(_) if Instant::now() < deadline => thread::sleep(Duration::from_millis(10)),
                Err(err) => panic!("failed to connect to daemon socket: {err}"),
            }
        };

        thread::sleep(Duration::from_millis(50));
        write_frame(&mut stream, FrameKind::Request, b"hello after connect").unwrap();
        let response = read_frame(&mut stream, DEFAULT_MAX_PAYLOAD_LEN).unwrap();

        assert_eq!(response.kind, FrameKind::Response);
        assert_eq!(response.payload, b"HELLO AFTER CONNECT");

        assert!(client.shutdown().unwrap());
        let daemon_result = handle.join().unwrap();
        assert!(daemon_result.is_ok(), "{daemon_result:?}");
    }
}
