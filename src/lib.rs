//! # sockd
//!
//! A managed Unix IPC daemon in 20 lines of code.
//!
//! sockd handles the six things every persistent daemon needs:
//! PID files, Unix sockets, auto-start from client, idle timeout,
//! signal handling, and framed request/response transport.
//!
//! You provide three callbacks:
//! - **on_start** — initialize your state (load files, build indexes, connect to DBs)
//! - **on_request** — handle a query (your business logic, receives bytes, returns bytes)
//! - **on_stop** — cleanup when the daemon exits
//!
//! # Daemon side
//!
//! ```rust,no_run
//! use sockd::Daemon;
//! use std::time::Duration;
//!
//! let daemon = Daemon::builder()
//!     .socket("/tmp/mytool.sock")
//!     .pid_file("/tmp/mytool.pid")
//!     .idle_timeout(Duration::from_secs(300))
//!     .on_request(|_, req| Ok(req.to_ascii_uppercase()))
//!     .build()
//!     .unwrap();
//!
//! daemon.run().unwrap();
//! ```
//!
//! # Client side
//!
//! ```rust,no_run
//! use sockd::Client;
//!
//! let client = Client::new("/tmp/mytool.sock")
//!     .with_pid_file("/tmp/mytool.pid")
//!     .with_auto_start("mytool-daemon");
//!
//! let response = client.request(b"hello")?;
//! # Ok::<(), sockd::Error>(())
//! ```
//!
//! # Cross-language
//!
//! sockd also emits a C-compatible shared library (`libsockd.dylib` / `.so`)
//! and a C header (`include/sockd.h`). Python, Swift, Go, and any language
//! with a C FFI can call `sockd_client_request()` directly.
//!
//! See `examples/` for Python and Swift clients using the C ABI.

mod client;
mod config;
mod daemon;
mod error;
mod ffi;
mod frame;
mod pidfile;
mod process;
mod socket;

pub use client::Client;
pub use config::AutoStart;
pub use daemon::{Daemon, DaemonBuilder};
pub use error::{BoxError, CallbackResult, Error, Result};
