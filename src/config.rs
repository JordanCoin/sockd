use crate::frame::DEFAULT_MAX_PAYLOAD_LEN;
use std::ffi::OsString;
use std::path::PathBuf;
use std::time::Duration;

#[derive(Clone, Debug)]
pub(crate) struct DaemonConfig {
    pub(crate) socket_path: PathBuf,
    pub(crate) pid_file: Option<PathBuf>,
    pub(crate) idle_timeout: Duration,
    pub(crate) poll_interval: Duration,
    pub(crate) max_payload_len: usize,
}

impl Default for DaemonConfig {
    fn default() -> Self {
        Self {
            socket_path: PathBuf::from("/tmp/sockd.sock"),
            pid_file: None,
            idle_timeout: Duration::from_secs(300),
            poll_interval: Duration::from_millis(250),
            max_payload_len: DEFAULT_MAX_PAYLOAD_LEN,
        }
    }
}

/// Configuration for auto-starting a daemon from the client.
///
/// When the client can't connect to the daemon, it uses this config
/// to fork/exec the daemon binary and wait for it to become ready.
///
/// ```rust
/// use sockd::AutoStart;
/// use std::time::Duration;
///
/// let auto = AutoStart::new("/usr/local/bin/mytool-daemon")
///     .arg("--socket=/tmp/mytool.sock")
///     .ready_timeout(Duration::from_secs(5));
/// ```
#[derive(Clone, Debug)]
pub struct AutoStart {
    pub(crate) program: PathBuf,
    pub(crate) args: Vec<OsString>,
    pub(crate) ready_timeout: Duration,
    pub(crate) initial_backoff: Duration,
    pub(crate) max_backoff: Duration,
}

impl AutoStart {
    /// Create auto-start config for the given daemon binary path.
    ///
    /// Defaults: 2s ready timeout, 50ms initial backoff, 250ms max backoff.
    pub fn new(program: impl Into<PathBuf>) -> Self {
        Self {
            program: program.into(),
            args: Vec::new(),
            ready_timeout: Duration::from_secs(2),
            initial_backoff: Duration::from_millis(50),
            max_backoff: Duration::from_millis(250),
        }
    }

    pub fn arg(mut self, arg: impl Into<OsString>) -> Self {
        self.args.push(arg.into());
        self
    }

    pub fn args<I, S>(mut self, args: I) -> Self
    where
        I: IntoIterator<Item = S>,
        S: Into<OsString>,
    {
        self.args.extend(args.into_iter().map(Into::into));
        self
    }

    pub fn ready_timeout(mut self, timeout: Duration) -> Self {
        self.ready_timeout = timeout;
        self
    }

    pub fn initial_backoff(mut self, duration: Duration) -> Self {
        self.initial_backoff = duration;
        self
    }

    pub fn max_backoff(mut self, duration: Duration) -> Self {
        self.max_backoff = duration;
        self
    }
}
