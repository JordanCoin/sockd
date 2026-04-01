/*
 * sockd.h — C ABI for sockd Unix IPC daemon client
 *
 * This header exposes the client side of sockd. Use it from C, Swift,
 * Python (ctypes), Go (cgo), Node (ffi-napi), or any language with a
 * C FFI.
 *
 * Link against libsockd.dylib (macOS) or libsockd.so (Linux).
 *
 * OWNERSHIP RULES:
 *   - Strings returned via char **error_out are owned by the caller.
 *     Free them with sockd_string_free().
 *   - Buffers returned via uint8_t **response_ptr_out are owned by the caller.
 *     Free them with sockd_buffer_free(ptr, len).
 *   - SockdClientConfig is owned by the caller. Free with sockd_client_config_free().
 *   - All input pointers (socket_path, pid_file, request_ptr) are borrowed
 *     for the duration of the call only. The caller retains ownership.
 *
 * ERROR CONVENTION:
 *   All functions return int:
 *     SOCKD_OK (0)            — success
 *     SOCKD_ERR_NULL (1)      — null pointer argument
 *     SOCKD_ERR_UTF8 (2)      — string is not valid UTF-8
 *     SOCKD_ERR_OPERATION (3) — sockd operation failed, see error_out
 *
 *   On error, *error_out is set to a human-readable message (or NULL).
 *   Always free it with sockd_string_free() even if NULL.
 */

#ifndef SOCKD_H
#define SOCKD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define SOCKD_OK            0
#define SOCKD_ERR_NULL      1
#define SOCKD_ERR_UTF8      2
#define SOCKD_ERR_OPERATION 3

/* Opaque client configuration. Holds socket path, PID file,
 * auto-start config, and other settings. */
typedef struct SockdClientConfig SockdClientConfig;

/*
 * ── Lifecycle ──────────────────────────────────────────────────
 */

/* Create a new client config targeting the given Unix socket path.
 * Returns NULL on allocation failure. */
SockdClientConfig *sockd_client_config_new(const char *socket_path);

/* Free a client config. Safe to call with NULL. */
void sockd_client_config_free(SockdClientConfig *config);

/*
 * ── Configuration ──────────────────────────────────────────────
 */

/* Set the PID file path. Used to detect if the daemon is alive. */
int sockd_client_config_set_pid_file(
    SockdClientConfig *config,
    const char *pid_file,
    char **error_out
);

/* Enable auto-start. When the daemon isn't running, the client will
 * fork/exec this program and wait for it to become ready.
 * argc/argv are additional arguments (can be 0/NULL). */
int sockd_client_config_set_auto_start(
    SockdClientConfig *config,
    const char *program,
    size_t argc,
    const char *const *argv,
    char **error_out
);

/* Set maximum payload size in bytes. Default: 16 MB. */
int sockd_client_config_set_max_payload_len(
    SockdClientConfig *config,
    size_t max_payload_len,
    char **error_out
);

/* Set shutdown timeout in milliseconds. Default: 2000. */
int sockd_client_config_set_shutdown_timeout_ms(
    SockdClientConfig *config,
    uint64_t shutdown_timeout_ms,
    char **error_out
);

/*
 * ── Operations ─────────────────────────────────────────────────
 */

/* Health check. Returns SOCKD_OK if the daemon responds to a Ping.
 * If auto-start is configured and daemon is not running, starts it. */
int sockd_client_ping(const SockdClientConfig *config, char **error_out);

/* Send a request and receive a response.
 * request_ptr/request_len: your payload bytes (borrowed, not consumed).
 * *response_ptr_out: callee-allocated response buffer. Free with sockd_buffer_free().
 * *response_len_out: length of the response buffer.
 * If auto-start is configured and daemon is not running, starts it. */
int sockd_client_request(
    const SockdClientConfig *config,
    const uint8_t *request_ptr,
    size_t request_len,
    uint8_t **response_ptr_out,
    size_t *response_len_out,
    char **error_out
);

/* Request graceful daemon shutdown.
 * *stopped_out: true if daemon was running and acknowledged shutdown.
 * Does NOT auto-start a daemon. */
int sockd_client_shutdown(
    const SockdClientConfig *config,
    bool *stopped_out,
    char **error_out
);

/*
 * ── Memory management ──────────────────────────────────────────
 */

/* Free a response buffer returned by sockd_client_request(). */
void sockd_buffer_free(uint8_t *ptr, size_t len);

/* Free an error string returned via char **error_out. Safe with NULL. */
void sockd_string_free(char *ptr);

#ifdef __cplusplus
}
#endif

#endif /* SOCKD_H */
