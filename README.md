# sockd

[![CI](https://github.com/JordanCoin/sockd/actions/workflows/ci.yml/badge.svg)](https://github.com/JordanCoin/sockd/actions)
[![License](https://img.shields.io/badge/license-MIT%2FApache--2.0-blue.svg)](LICENSE-MIT)

Local daemon infrastructure for tools with expensive startup.

`sockd` lets a normal CLI command behave like a fast local service.
Your client stays short-lived. Your daemon keeps warm state in memory.
`sockd` handles the daemon plumbing in between.

Use it when your tool is slow because every invocation has to load the
same expensive state again.

```text
short-lived CLI / hook / plugin
              ->
          sockd client
              ->
    local daemon process with warm state
              ->
      your actual application logic
```

In a few words: it is the reusable local daemon layer under your tool.

`sockd` owns sockets, pid files, auto-start, readiness, graceful shutdown,
and idle shutdown.

You own your app logic, your state, and your request format.

It is not a web server, not a remote service, and not your business logic.

## Quickstart

In one terminal:

```bash
cargo run --example echo_daemon
```

In another:

```bash
cargo run --example echo_client -- "hello from rust"
python3 examples/python/client.py "hello from python"
```

Shut it down:

```bash
cargo run --example echo_client -- --shutdown
```

There are full runnable examples in [examples/README.md](examples/README.md).

## Rust Usage

```rust
use sockd::Daemon;
use std::time::Duration;

let daemon = Daemon::builder()
    .socket("/tmp/mytool.sock")
    .pid_file("/tmp/mytool.pid")
    .idle_timeout(Duration::from_secs(300))
    .on_request(|_, req| Ok(handle(req)))
    .build()?;

daemon.run()?;
```

```rust
use sockd::Client;

let resp = Client::new("/tmp/mytool.sock")
    .with_pid_file("/tmp/mytool.pid")
    .with_auto_start("mytool-daemon")
    .request(query_bytes)?;
```

```rust
let stopped = Client::new("/tmp/mytool.sock")
    .with_pid_file("/tmp/mytool.pid")
    .shutdown()?;
```

## Problem

Every developer tool that needs a persistent daemon reimplements the same 6 things:
PID files, Unix sockets, auto-start from client, idle timeout, signal handling,
and a request/response protocol. Gradle, Docker, gopls, Buck2, Redis -- they all
do this. None of them extracted it into a library.

sockd is that library.

## Status

Initial Rust core implemented. Current scope:
- Unix domain sockets only
- single-threaded synchronous daemon loop
- builder API for `Daemon`
- framed request/response transport with internal ping/pong/shutdown control frames
- client auto-start, pid file ownership, graceful shutdown, idle shutdown

Docs:
- [docs/LIFECYCLE.md](docs/LIFECYCLE.md) -- Startup, shutdown, stale recovery, auto-start behavior
- [docs/PROTOCOL.md](docs/PROTOCOL.md) -- Wire format for writing raw-socket clients in any language

## Installation

### Rust consumers

This is the intended primary integration:

```bash
cargo add sockd
```

Then build your own daemon binary and thin client/CLI on top of the crate.

### C / FFI consumers

Build or download the library artifacts:

```bash
cargo build --release
```

This repo emits:
- `target/release/libsockd.a`
- `target/release/libsockd.so` on Linux
- `target/release/libsockd.dylib` on macOS

The C header is at [include/sockd.h](include/sockd.h).
Swift consumers can also use the shipped module map at
[include/module.modulemap](include/module.modulemap) and `import CSockd`.

## Cross-Language Use

The right portability model is:
- use the wire protocol when you want an external daemon process any language can talk to
- use the C ABI when you want to call the client library from Python, Ruby, Node, Swift, Go, C#, etc.

Current scope:
- cross-language client interop via C ABI
- Unix platforms only right now

Windows is still not implemented because the transport layer is Unix sockets today. To make this truly cross-platform, the next step is a transport abstraction with named pipes on Windows and Unix sockets on macOS/Linux.

For open-source consumers, the usual architecture is:

```text
your end users install:      your tool
your tool embeds/links:      sockd
your daemon binary uses:     sockd::Daemon
your CLI/plugin uses:        sockd::Client or the C ABI
```

End users typically do not install `sockd` directly unless they are developing against it.

## Origin

Built while porting an iOS agent skill router from Node.js to C as a learning exercise.
The C daemon achieved 40 microsecond query latency vs 50ms for the Node.js version.
The daemon lifecycle code (PID management, signal handling, idle timeout, auto-start)
was the hardest part to get right -- and it had nothing to do with the routing logic.
That's when we realized: this should be a library.

Reference implementation: [ios-skills-collection/bench/daemon/](https://github.com/JordanCoin/ios-skills-collection/tree/main/bench/daemon)

## Contributing

Issues and PRs welcome. This project uses `cargo fmt` and `cargo clippy`.

## License

Dual-licensed under [MIT](LICENSE-MIT) or [Apache-2.0](LICENSE-APACHE), at your option.
