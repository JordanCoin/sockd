/*
 * protocol.h — Binary message format for router IPC
 *
 * This replaces JSON between the client and daemon.
 * Both sides include this file so they agree on the exact
 * byte layout of messages.
 *
 * WHY BINARY:
 * JSON means: build a string, send it, scan for keys, extract values.
 * Binary means: fill a struct, send the bytes, cast bytes back to struct.
 * The CPU doesn't care about human readability — it just wants offsets.
 *
 * MESSAGE FLOW:
 *   Client → Daemon:  RequestMsg (fixed-size header + paths)
 *   Daemon → Client:  ResponseMsg (header + skill content bytes)
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>    /* uint8_t, uint16_t, uint32_t — exact-width integers */

/*
 * ── Socket Path ────────────────────────────────────────────────
 *
 * Unix domain sockets live on the filesystem as a special file.
 * Both client and daemon need to agree on where it is.
 * /tmp is readable by everyone — fine for a local dev tool.
 */
#define SOCKET_PATH "/tmp/ios-skills-router.sock"
#define PID_PATH    "/tmp/ios-skills-router.pid"

/*
 * ── Idle Timeout ───────────────────────────────────────────────
 *
 * If no requests arrive within this many seconds, the daemon
 * shuts itself down. Prevents orphans from lingering forever.
 * The client auto-starts the daemon if it's not running, so
 * the next hook call will just spin up a fresh one.
 */
#define IDLE_TIMEOUT_SECS 300  /* 5 minutes */

/*
 * ── Magic Number ───────────────────────────────────────────────
 *
 * First 4 bytes of every message. If we get garbage on the socket
 * (wrong client, corruption, half-sent data), the magic number
 * lets us detect it immediately instead of interpreting random
 * bytes as a file path.
 *
 * 0x494F5352 = "IOSR" in ASCII (iOS Router).
 */
#define PROTOCOL_MAGIC 0x494F5352

/*
 * ── Protocol Version ───────────────────────────────────────────
 *
 * If we change the message format later, bump this.
 * Daemon rejects messages with wrong version instead of
 * misinterpreting fields.
 */
#define PROTOCOL_VERSION 1

/*
 * ── Tool Type Enum ─────────────────────────────────────────────
 *
 * Instead of sending "Edit" as a 4-byte string and scanning it,
 * we send a single byte: 0x01.
 *
 * The client maps Claude Code's tool_name string to this enum
 * once, then it's just integer comparison on the daemon side.
 *
 * Remember our discussion about is_bash? Here it's implicit —
 * TOOL_BASH is just another enum value, no separate bool needed.
 */
typedef enum {
    TOOL_UNKNOWN   = 0x00,
    TOOL_READ      = 0x01,
    TOOL_EDIT      = 0x02,
    TOOL_MULTIEDIT = 0x03,
    TOOL_WRITE     = 0x04,
    TOOL_BASH      = 0x05,
} ToolType;

/*
 * ── Request Message ────────────────────────────────────────────
 *
 * What the client sends to the daemon.
 *
 * Fixed-size struct = no parsing. The daemon reads exactly
 * sizeof(RequestMsg) bytes and the fields are at known offsets.
 *
 * Layout in memory (assuming no padding):
 *   Bytes 0-3:    magic (0x494F5352)
 *   Byte  4:      version (1)
 *   Byte  5:      tool_type (enum)
 *   Bytes 6-7:    path_len (how many bytes of path follow)
 *   Bytes 8-71:   session_id (fixed 64 chars, null-padded)
 *   Bytes 72-end: path data (variable length, up to 4096)
 *
 * WHY FIXED session_id:
 * Session IDs are short strings. By fixing the size, the entire
 * header is fixed-size — we always read exactly the same number
 * of bytes. The path is variable because file paths vary wildly.
 *
 * TOTAL: 72-byte header + up to 4096 bytes of path = ~4KB max
 * Compare to JSON: ~200 bytes of {"tool_name":"Edit","tool_input":...}
 * The binary is actually LARGER here, but that's fine — the win
 * is zero parsing, not smaller messages.
 */
#define MAX_PATH_LEN    4096
#define SESSION_ID_LEN  64

typedef struct __attribute__((packed)) {
    uint32_t magic;                     /* PROTOCOL_MAGIC */
    uint8_t  version;                   /* PROTOCOL_VERSION */
    uint8_t  tool_type;                 /* ToolType enum */
    uint16_t path_len;                  /* length of path data */
    char     session_id[SESSION_ID_LEN]; /* null-padded session ID */
    char     path[MAX_PATH_LEN];        /* file_path or command */
} RequestMsg;

/*
 * ── Response Message ───────────────────────────────────────────
 *
 * What the daemon sends back to the client.
 *
 * Layout:
 *   Bytes 0-3:    magic
 *   Byte  4:      version
 *   Byte  5:      status (0=no match, 1=matched, 2=deduped)
 *   Byte  6:      skill_count (how many skills matched)
 *   Byte  7:      (reserved/padding)
 *   Bytes 8-11:   content_len (total bytes of skill content)
 *   Bytes 12-end: content data (the actual SKILL.md text)
 *
 * The client receives this, wraps it in the hookSpecificOutput
 * JSON that Claude Code expects, and prints to stdout.
 *
 * WHY THE CLIENT DOES THE JSON WRAPPING:
 * The daemon deals in pure binary — fast, clean, no escaping.
 * The client is the "translation layer" between Claude Code's
 * JSON world and the daemon's binary world. This separation
 * means if Claude Code changes its JSON format, we only update
 * the client, not the daemon.
 */
typedef enum {
    STATUS_NO_MATCH = 0,
    STATUS_MATCHED  = 1,
    STATUS_DEDUPED  = 2,   /* skills matched but already injected this session */
    STATUS_ERROR    = 3,
} ResponseStatus;

typedef struct __attribute__((packed)) {
    uint32_t magic;                     /* PROTOCOL_MAGIC */
    uint8_t  version;                   /* PROTOCOL_VERSION */
    uint8_t  status;                    /* ResponseStatus */
    uint8_t  skill_count;               /* number of skills in response */
    uint8_t  _reserved;                 /* alignment padding */
    uint32_t content_len;               /* bytes of content following this header */
    /* Followed by content_len bytes of skill content (UTF-8 text) */
} ResponseMsg;

/*
 * ── Daemon Control ─────────────────────────────────────────────
 *
 * Special messages for managing the daemon itself.
 * Client sends TOOL_UNKNOWN with a command in the path field.
 */
#define CMD_PING     "PING"      /* health check — daemon responds with empty MATCHED */
#define CMD_RELOAD   "RELOAD"    /* re-mmap all skill files */
#define CMD_SHUTDOWN "SHUTDOWN"  /* graceful exit */
#define CMD_STATS    "STATS"     /* return hit counts, uptime, etc. */

/*
 * ── Size Limits ────────────────────────────────────────────────
 */
#define MAX_SKILLS_PER_RESPONSE 3
#define MAX_RESPONSE_CONTENT    24576   /* 24KB, same as Node version */

/*
 * ── Helper: header sizes ───────────────────────────────────────
 *
 * These tell you how many bytes to read/write for just the header
 * (before the variable-length content).
 */
#define REQUEST_HEADER_SIZE  sizeof(RequestMsg)
#define RESPONSE_HEADER_SIZE sizeof(ResponseMsg)

#endif /* PROTOCOL_H */
