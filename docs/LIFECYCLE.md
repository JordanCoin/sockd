# Daemon Lifecycle

## Startup

1. **Kill stale daemon** — If a PID file exists, check if the process is alive.
   If alive, send SIGTERM, wait 100ms, then SIGKILL if still alive. Remove
   the stale PID file.

2. **Claim PID file** — Write the current PID atomically. If the file already
   exists and the process is alive, fail with `DaemonAlreadyRunning`. Retries
   up to 3 times to handle race conditions with concurrent startup attempts.

3. **Call `on_start()`** — Your callback initializes application state. If it
   returns an error, the daemon exits immediately (no socket is created).

4. **Bind socket** — Create and bind the Unix domain socket. Remove any stale
   socket file from a previous crash. If binding fails, `on_stop()` is called
   with the state for cleanup.

5. **Ready** — The daemon is now accepting connections. Clients that send a
   `Ping` frame will receive `Pong`.

## Event Loop

The daemon uses `poll()` (not blocking `accept()`) with a configurable interval
(default: 250ms). This means:

- **Signals are not blocked.** SIGINT/SIGTERM set a flag that the loop checks
  on each iteration. No orphaned processes.

- **Idle timeout is checked periodically.** Every poll interval, the daemon
  compares `now - last_activity` against the idle timeout. If exceeded, the
  loop exits.

- **Connections are non-blocking on the listener.** After `poll()` reports
  readiness, the daemon drains all pending connections. Each accepted stream
  is switched to blocking mode for framed I/O.

## Request Handling

Each connection handles exactly one frame exchange:

1. Read a frame from the stream (header + payload)
2. Dispatch based on frame kind:
   - `Ping` → respond with `Pong`
   - `Shutdown` → respond with `ShutdownAck`, exit the loop
   - `Request` → call `on_request(state, payload)`, respond with `Response`
   - Anything else → respond with `Error` frame
3. Close the connection

If `on_request` returns an error, the daemon sends an `Error` frame with the
error message and continues running (does not crash).

## Shutdown

The daemon exits when any of these happen:

- **Idle timeout** — No connections for `idle_timeout` duration.
- **Signal** — SIGINT (Ctrl+C) or SIGTERM.
- **Client shutdown** — A client sends a `Shutdown` frame.

Shutdown sequence:

1. Stop accepting new connections
2. Call `on_stop(state)` — your cleanup callback
3. Close the Unix socket
4. Remove the socket file
5. Remove the PID file (via RAII `PidGuard` drop)

If `on_stop` returns an error, the daemon still cleans up the socket and PID
file before propagating the error.

## Stale Recovery

The daemon handles three stale-resource scenarios:

- **Stale PID file, dead process** — The PID file points to a process that no
  longer exists. The daemon removes the PID file and claims it.

- **Malformed PID file** — The PID file contains garbage. The daemon removes
  it and claims a fresh one.

- **Stale socket file** — A socket file exists from a crashed daemon. The
  daemon removes it before binding.

## Client Auto-Start

When a client can't connect and auto-start is configured:

1. Check the PID file — if the process is alive, assume the daemon is starting
   and wait (it might just be slow).

2. If the process is dead or no PID file exists, fork/exec the daemon binary
   as a detached process (new session via `setsid`, stdio redirected to null).

3. Poll the socket with exponential backoff (50ms → 100ms → 200ms, up to
   `ready_timeout`). On each attempt, try a `Ping` round-trip — not just
   "does the socket file exist."

4. If readiness is confirmed, send the original request. If timeout expires,
   return `ReadyTimeout`.
