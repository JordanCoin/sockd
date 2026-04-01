use std::error::Error as StdError;
use std::fmt;
use std::io;
use std::path::PathBuf;
use std::time::Duration;

/// A boxed error type for user callbacks. Your `on_start`, `on_request`,
/// and `on_stop` callbacks return `CallbackResult<T>`, letting you use
/// `?` with any error type.
pub type BoxError = Box<dyn StdError + Send + Sync + 'static>;

/// Result type for user callbacks (`on_start`, `on_request`, `on_stop`).
pub type CallbackResult<T> = std::result::Result<T, BoxError>;

/// Result type for sockd library operations.
pub type Result<T> = std::result::Result<T, Error>;

/// All errors that sockd can produce.
///
/// Covers I/O failures, protocol violations, daemon lifecycle issues,
/// and callback errors. Implements `std::error::Error` with source chaining.
#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    MissingCallback(&'static str),
    InvalidFrameMagic(u32),
    UnsupportedFrameVersion(u8),
    UnknownFrameKind(u8),
    UnexpectedFrameKind {
        expected: &'static str,
        actual: &'static str,
    },
    PayloadTooLarge {
        len: usize,
        max: usize,
    },
    DaemonAlreadyRunning {
        pid: Option<u32>,
        socket: Option<PathBuf>,
    },
    ReadyTimeout {
        socket: PathBuf,
        timeout: Duration,
    },
    ShutdownTimeout {
        socket: PathBuf,
        timeout: Duration,
    },
    DaemonReturnedError(String),
    Startup(BoxError),
    Request(BoxError),
    Shutdown(BoxError),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(err) => write!(f, "{err}"),
            Self::MissingCallback(name) => write!(f, "missing required callback: {name}"),
            Self::InvalidFrameMagic(magic) => write!(f, "invalid frame magic: 0x{magic:08x}"),
            Self::UnsupportedFrameVersion(version) => {
                write!(f, "unsupported frame version: {version}")
            }
            Self::UnknownFrameKind(kind) => write!(f, "unknown frame kind: {kind}"),
            Self::UnexpectedFrameKind { expected, actual } => {
                write!(
                    f,
                    "unexpected frame kind: expected {expected}, got {actual}"
                )
            }
            Self::PayloadTooLarge { len, max } => {
                write!(f, "payload length {len} exceeds maximum {max}")
            }
            Self::DaemonAlreadyRunning { pid, socket } => match (pid, socket) {
                (Some(pid), Some(socket)) => write!(
                    f,
                    "daemon already running with pid {pid} on socket {}",
                    socket.display()
                ),
                (Some(pid), None) => write!(f, "daemon already running with pid {pid}"),
                (None, Some(socket)) => {
                    write!(f, "daemon already running on socket {}", socket.display())
                }
                (None, None) => write!(f, "daemon already running"),
            },
            Self::ReadyTimeout { socket, timeout } => write!(
                f,
                "daemon did not become ready on {} within {:?}",
                socket.display(),
                timeout
            ),
            Self::ShutdownTimeout { socket, timeout } => write!(
                f,
                "daemon did not shut down on {} within {:?}",
                socket.display(),
                timeout
            ),
            Self::DaemonReturnedError(message) => write!(f, "daemon returned error: {message}"),
            Self::Startup(err) => write!(f, "daemon startup failed: {err}"),
            Self::Request(err) => write!(f, "request handler failed: {err}"),
            Self::Shutdown(err) => write!(f, "daemon shutdown failed: {err}"),
        }
    }
}

impl StdError for Error {
    fn source(&self) -> Option<&(dyn StdError + 'static)> {
        match self {
            Self::Io(err) => Some(err),
            Self::Startup(err) => Some(&**err),
            Self::Request(err) => Some(&**err),
            Self::Shutdown(err) => Some(&**err),
            _ => None,
        }
    }
}

impl From<io::Error> for Error {
    fn from(value: io::Error) -> Self {
        Self::Io(value)
    }
}
