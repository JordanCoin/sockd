# Examples

These examples all talk to the same daemon contract:

- socket: `/tmp/sockd-example.sock`
- pid file: `/tmp/sockd-example.pid`

## 1. Start the daemon

```bash
cargo run --example echo_daemon
```

## 2. Talk to it from Rust

```bash
cargo run --example echo_client -- "hello from rust"
```

## 3. Talk to it from Python

Build the library first:

```bash
cargo build --release
python3 examples/python/client.py "hello from python"
```

If the dynamic library is not in the default search path, point at it explicitly:

```bash
python3 examples/python/client.py --lib target/release/libsockd.dylib "hello"
```

## 4. Talk to it from Swift

```bash
cargo build --release
swiftc \
  -I include \
  examples/swift/main.swift \
  -L target/release \
  -lsockd \
  -o /tmp/sockd-swift-client
DYLD_LIBRARY_PATH=target/release /tmp/sockd-swift-client "hello from swift"
```

`include/module.modulemap` ships with the repo, so Swift can import `CSockd`
directly instead of relying on a bridging header.

On Linux, use `LD_LIBRARY_PATH` instead of `DYLD_LIBRARY_PATH`.

## 5. Shut it down

Rust:

```bash
cargo run --example echo_client -- --shutdown
```

Python:

```bash
python3 examples/python/client.py --shutdown
```

Swift:

```bash
DYLD_LIBRARY_PATH=target/release /tmp/sockd-swift-client --shutdown
```
