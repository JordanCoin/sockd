/*
 * router-client.c — Thin hook client for Claude Code
 *
 * This is what Claude Code's PreToolUse hook actually spawns.
 * It reads JSON from stdin (the hook protocol), translates it
 * to a binary RequestMsg, sends it to the daemon via Unix socket,
 * receives the binary ResponseMsg, wraps it in the JSON format
 * Claude Code expects, and prints to stdout.
 *
 * This binary should be TINY and FAST. All the heavy lifting
 * (mmap, pattern matching, dedup) happens in the daemon.
 * This client just translates between protocols.
 *
 * LIFECYCLE (per hook call):
 *   1. Read JSON from stdin (~0.01ms)
 *   2. Parse tool_name, file_path, session_id (~0.001ms)
 *   3. Connect to daemon socket (~0.05ms)
 *   4. Send binary RequestMsg (~0.01ms)
 *   5. Receive binary ResponseMsg (~0.01ms)
 *   6. Wrap in JSON, print to stdout (~0.01ms)
 *   Total: ~0.1ms (vs 50ms for Node, <1ms for old C)
 *
 * BUILD:
 *   make client   (or: cc -O2 -o router-client router-client.c)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "protocol.h"

#define MAX_INPUT 16384

/*
 * ── JSON Field Extraction ──────────────────────────────────────
 *
 * Same hand-rolled parser as route-skills.c.
 * Finds "key":"value" in the raw JSON, extracts value.
 */
static int extract_json_string(const char *json, const char *key,
                               char *out, int out_size) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    char pattern2[256];
    snprintf(pattern2, sizeof(pattern2), "\"%s\": \"", key);

    const char *start = strstr(json, pattern);
    int skip = strlen(pattern);
    if (!start) {
        start = strstr(json, pattern2);
        skip = strlen(pattern2);
    }
    if (!start) return 0;

    start += skip;
    int i = 0;
    while (*start && *start != '"' && i < out_size - 1) {
        if (*start == '\\' && *(start + 1)) {
            start++;
            if (*start == 'n') out[i++] = '\n';
            else if (*start == 't') out[i++] = '\t';
            else out[i++] = *start;
        } else {
            out[i++] = *start;
        }
        start++;
    }
    out[i] = '\0';
    return i;
}

/*
 * ── Tool Name → Enum ──────────────────────────────────────────
 *
 * Converts Claude Code's string tool names to our binary enum.
 * This is the boundary between JSON world and binary world.
 */
static ToolType parse_tool_type(const char *name) {
    if (strcmp(name, "Read") == 0)      return TOOL_READ;
    if (strcmp(name, "Edit") == 0)      return TOOL_EDIT;
    if (strcmp(name, "MultiEdit") == 0) return TOOL_MULTIEDIT;
    if (strcmp(name, "Write") == 0)     return TOOL_WRITE;
    if (strcmp(name, "Bash") == 0)      return TOOL_BASH;
    return TOOL_UNKNOWN;
}

/*
 * ── Connect to Daemon ──────────────────────────────────────────
 *
 * Opens a connection to the daemon's Unix socket.
 * If the daemon isn't running, tries to auto-start it.
 * Returns the socket fd, or -1 if all fails.
 */
static int try_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static int connect_to_daemon(void) {
    /* Try connecting first — daemon might already be running */
    int fd = try_connect();
    if (fd >= 0) return fd;

    /*
     * Daemon not running — try to auto-start it.
     *
     * Find the daemon binary relative to this client binary.
     * Both live in the same directory (bench/daemon/).
     * We need CLAUDE_PLUGIN_ROOT set for the daemon to find skills.
     */
    const char *plugin_root = getenv("CLAUDE_PLUGIN_ROOT");
    if (!plugin_root) return -1;  /* can't start without this */

    /* Fork and exec the daemon */
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Child: become the daemon.
         * Detach from parent so it survives the client exiting.
         * Redirect stdio so it doesn't pollute hook output.
         */
        setsid();  /* new session — fully detached */
        freopen("/dev/null", "r", stdin);
        freopen("/tmp/ios-skills-daemon.log", "a", stderr);
        freopen("/dev/null", "w", stdout);

        /* Resolve daemon path from client's argv[0] path */
        execlp("router-daemon", "router-daemon", NULL);
        /* If execlp fails, try with explicit path */
        execl("/tmp/ios-skills-router-daemon", "router-daemon", NULL);
        _exit(1);
    }

    /* Parent: wait for daemon to be ready (up to 2 seconds) */
    for (int attempt = 0; attempt < 40; attempt++) {
        usleep(50000);  /* 50ms */
        fd = try_connect();
        if (fd >= 0) return fd;
    }

    return -1;  /* gave up */
}

/*
 * ── JSON-Escape and Print ──────────────────────────────────────
 *
 * The daemon sends us raw UTF-8 skill content (from mmap).
 * Claude Code expects it inside a JSON string, so we need
 * to escape special characters.
 */
static void print_json_escaped(const char *data, int len) {
    for (int i = 0; i < len; i++) {
        switch (data[i]) {
            case '"':  printf("\\\""); break;
            case '\\': printf("\\\\"); break;
            case '\n': printf("\\n");  break;
            case '\r': printf("\\r");  break;
            case '\t': printf("\\t");  break;
            default:
                if ((unsigned char)data[i] >= 32)
                    putchar(data[i]);
                break;
        }
    }
}

/*
 * ── Main ───────────────────────────────────────────────────────
 */
int main(void) {
    /* 1. Read JSON from stdin */
    char input[MAX_INPUT];
    int input_len = fread(input, 1, sizeof(input) - 1, stdin);
    input[input_len] = '\0';

    if (input_len == 0) {
        printf("{}");
        return EXIT_SUCCESS;
    }

    /* 2. Extract fields */
    char tool_name[64] = {0};
    char file_path[MAX_PATH_LEN] = {0};
    char command[MAX_PATH_LEN] = {0};
    char session_id[SESSION_ID_LEN] = {0};

    extract_json_string(input, "tool_name", tool_name, sizeof(tool_name));
    extract_json_string(input, "file_path", file_path, sizeof(file_path));
    extract_json_string(input, "command", command, sizeof(command));
    extract_json_string(input, "session_id", session_id, sizeof(session_id));

    /* 3. Build binary request */
    RequestMsg req;
    memset(&req, 0, sizeof(req));
    req.magic = PROTOCOL_MAGIC;
    req.version = PROTOCOL_VERSION;
    req.tool_type = parse_tool_type(tool_name);

    strncpy(req.session_id, session_id, SESSION_ID_LEN - 1);

    /* Pick the right path field based on tool type */
    const char *path = (req.tool_type == TOOL_BASH) ? command : file_path;
    int path_len = strlen(path);
    if (path_len > MAX_PATH_LEN - 1) path_len = MAX_PATH_LEN - 1;
    memcpy(req.path, path, path_len);
    req.path_len = path_len;

    /* 4. Connect to daemon */
    int sock = connect_to_daemon();
    if (sock < 0) {
        /* Daemon not running — silent failure, same as empty response */
        printf("{}");
        return EXIT_SUCCESS;
    }

    /* 5. Send request */
    write(sock, &req, sizeof(req));

    /* 6. Receive response header */
    ResponseMsg resp;
    ssize_t n = read(sock, &resp, sizeof(resp));
    if (n < (ssize_t)sizeof(resp) ||
        resp.magic != PROTOCOL_MAGIC ||
        resp.version != PROTOCOL_VERSION) {
        close(sock);
        printf("{}");
        return EXIT_SUCCESS;
    }

    /* 7. Receive content body (if any) */
    if (resp.content_len == 0 || resp.status != STATUS_MATCHED) {
        close(sock);
        printf("{}");
        return EXIT_SUCCESS;
    }

    char *content = malloc(resp.content_len + 1);
    if (!content) {
        close(sock);
        printf("{}");
        return EXIT_SUCCESS;
    }

    int total_read = 0;
    while (total_read < (int)resp.content_len) {
        n = read(sock, content + total_read, resp.content_len - total_read);
        if (n <= 0) break;
        total_read += n;
    }
    close(sock);

    /* 8. Wrap in Claude Code's expected JSON format and print */
    printf("{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"additionalContext\":\"");
    print_json_escaped(content, total_read);
    printf("\"}}");

    free(content);
    return EXIT_SUCCESS;
}
