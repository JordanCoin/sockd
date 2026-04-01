# sockd

[![CI](https://github.com/JordanCoin/sockd/actions/workflows/ci.yml/badge.svg)](https://github.com/JordanCoin/sockd/actions)
[![License](https://img.shields.io/badge/license-MIT%2FApache--2.0-blue.svg)](LICENSE-MIT)

Daemon infrastructure for local-first AI tools.

sockd turns any CLI into a stateful local service.

LLM tools are stateless by default. Every hook, every agent call, every CLI
invocation pays the same startup tax: load the project graph, parse the index,
rebuild the cache. That's why they're slow, expensive, and repetitive.

sockd gives them memory. From 50ms to 40us by keeping state warm.

```
CLI / hook / agent
        |
   sockd client
(connect or auto-start)
        |
   sockd daemon
 (warm state in memory)
        |
    your logic
```

```
codemap → structure code
docmap  → structure docs
sockd   → keep state warm, move context between tools
```

sockd owns sockets, PID files, auto-start, readiness probing, signal handling,
idle shutdown, and framed request/response transport.

You own your state, your logic, and your protocol.

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
and a request/response protocol.

Gradle, Docker, gopls, Buck2, Redis -- they all do this.

**None of them extracted it into a reusable library.**

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

## Install

**Rust:**
```bash
cargo add sockd
```

**Everything else** (Python, Swift, Go, C, Node):
```bash
# macOS (Apple Silicon)
curl -sL https://github.com/JordanCoin/sockd/releases/latest/download/sockd-aarch64-apple-darwin.tar.gz | tar xz -C /usr/local/lib
# macOS (Intel)
curl -sL https://github.com/JordanCoin/sockd/releases/latest/download/sockd-x86_64-apple-darwin.tar.gz | tar xz -C /usr/local/lib
# Linux (x86_64)
curl -sL https://github.com/JordanCoin/sockd/releases/latest/download/sockd-x86_64-unknown-linux-gnu.tar.gz | tar xz -C /usr/local/lib
```

This gives you `libsockd.dylib` (or `.so`), `libsockd.a`, `sockd.h`, and `module.modulemap`.
Link against the library and include the header. See [examples/](examples/) for Python, Swift, and Rust clients.

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
