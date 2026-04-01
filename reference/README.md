# C Reference Implementation

This is the original C implementation that sockd was extracted from.
It is not compiled into the Rust library. It exists here for:

- **Learning** — heavily commented, written as a C teaching exercise
- **Proof of concept** — demonstrates the daemon pattern works in pure C
- **Benchmarking** — includes a harness that measures 40us query latency

## Files

- `protocol.h` — binary frame format (shared between daemon and client)
- `router-daemon.c` — persistent daemon with mmap'd state, Unix socket, signal handling
- `router-client.c` — thin client that translates JSON (from Claude Code hooks) to binary protocol
- `bench-harness.c` — nanosecond-precision benchmark with daemon lifecycle management
- `Makefile` — builds everything

## Build and run

```bash
cd reference
make
CLAUDE_PLUGIN_ROOT=/path/to/ios-skills-collection ./router-daemon &
echo '{"tool_name":"Edit","tool_input":{"file_path":"/a/ContentView.swift"},"session_id":"test"}' | ./router-client
```

## Context

This was built while learning C by porting an iOS agent skill router from
Node.js. The Node version took 50ms per hook call (V8 startup). The C daemon
takes 40us (mmap'd files, compiled routing rules, in-memory dedup).

The daemon lifecycle code (PID files, signal handling, idle timeout, orphan
prevention) was harder to get right than the actual routing logic. That
realization led to extracting the pattern into sockd as a reusable Rust library.
