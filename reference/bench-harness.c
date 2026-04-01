/*
 * bench-harness.c — Proper benchmark for the router daemon
 *
 * No python. No shell timing hacks. Just C with clock_gettime.
 *
 * This harness:
 * 1. Starts the daemon (fork + exec)
 * 2. Waits until the socket is ready (poll, not sleep)
 * 3. Sends requests directly via Unix socket (true daemon speed)
 * 4. Also benchmarks the client binary (realistic hook path)
 * 5. Reports with nanosecond precision
 * 6. Cleans up the daemon on exit
 *
 * BUILD:
 *   make bench
 *
 * RUN:
 *   CLAUDE_PLUGIN_ROOT=/path/to/ios-skills-collection ./bench-harness
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>         /* clock_gettime, CLOCK_MONOTONIC */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>     /* waitpid */
#include <sys/stat.h>     /* stat — check if socket file exists */
#include <errno.h>

#include "protocol.h"

/*
 * ── Timing ─────────────────────────────────────────────────────
 *
 * clock_gettime(CLOCK_MONOTONIC) gives nanosecond precision.
 * CLOCK_MONOTONIC means it never jumps (unlike wall clock time
 * which can shift for NTP, daylight savings, etc).
 *
 * This is how you benchmark in C. No python, no date command,
 * no external process. Just a syscall that reads the CPU's
 * high-resolution timer.
 */
static long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static double ns_to_ms(long ns) { return ns / 1000000.0; }
static double ns_to_us(long ns) { return ns / 1000.0; }

/*
 * ── Colors ─────────────────────────────────────────────────────
 */
#define GREEN  "\033[0;32m"
#define BLUE   "\033[0;34m"
#define DIM    "\033[0;90m"
#define YELLOW "\033[1;33m"
#define RESET  "\033[0m"

/*
 * ── Daemon Management ──────────────────────────────────────────
 */
static pid_t daemon_pid = 0;
static char harness_dir[4096] = {0};  /* directory containing this binary */

static void cleanup(void) {
    if (daemon_pid > 0) {
        kill(daemon_pid, SIGTERM);
        waitpid(daemon_pid, NULL, 0);
        daemon_pid = 0;
    }
    /* Don't unlink socket — the daemon does that on shutdown */
}

/* Resolve the directory this binary lives in */
static void resolve_harness_dir(const char *argv0) {
    /* If argv0 has a slash, use its directory */
    const char *last_slash = strrchr(argv0, '/');
    if (last_slash) {
        int len = last_slash - argv0;
        memcpy(harness_dir, argv0, len);
        harness_dir[len] = '\0';
    } else {
        /* Binary invoked via PATH — use current dir */
        getcwd(harness_dir, sizeof(harness_dir));
    }
}

static void signal_handler(int sig) {
    (void)sig;
    cleanup();
    _exit(1);
}

static bool start_daemon(const char *plugin_root) {
    /* Flush all output before fork — unflushed buffers get duplicated */
    fflush(stdout);
    fflush(stderr);

    /* Fork a child process to run the daemon */
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return false;
    }

    if (pid == 0) {
        /* Child: become the daemon.
         * Send daemon logs to a temp file so we can check on failure.
         */
        freopen("/tmp/ios-skills-daemon.log", "w", stderr);

        /* Set env and exec the daemon binary (resolve from harness dir) */
        char daemon_path[4096];
        snprintf(daemon_path, sizeof(daemon_path), "%s/router-daemon", harness_dir);
        setenv("CLAUDE_PLUGIN_ROOT", plugin_root, 1);
        execl(daemon_path, "router-daemon", NULL);
        perror("execl");
        _exit(1);
    }

    /* Parent: wait for the socket to appear */
    daemon_pid = pid;

    for (int attempt = 0; attempt < 100; attempt++) {
        struct stat st;
        if (stat(SOCKET_PATH, &st) == 0) {
            /* Socket file exists — try connecting to verify daemon is ready */
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd >= 0) {
                struct sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

                if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                    /* Send a PING to make sure it's really ready */
                    RequestMsg req;
                    memset(&req, 0, sizeof(req));
                    req.magic = PROTOCOL_MAGIC;
                    req.version = PROTOCOL_VERSION;
                    req.tool_type = TOOL_UNKNOWN;
                    memcpy(req.path, CMD_PING, 4);
                    req.path_len = 4;

                    write(fd, &req, sizeof(req));

                    ResponseMsg resp;
                    read(fd, &resp, sizeof(resp));
                    close(fd);

                    if (resp.magic == PROTOCOL_MAGIC) {
                        return true;  /* Daemon is up and responding */
                    }
                }
                close(fd);
            }
        }
        usleep(10000);  /* 10ms — total max wait 1 second */
    }

    fprintf(stderr, "Daemon failed to start within 1 second\n");
    fprintf(stderr, "Daemon log:\n");
    /* Print the daemon's log so we can see what went wrong */
    FILE *log = fopen("/tmp/ios-skills-daemon.log", "r");
    if (log) {
        char line[512];
        while (fgets(line, sizeof(line), log)) fprintf(stderr, "  %s", line);
        fclose(log);
    }
    cleanup();
    return false;
}

/*
 * ── Direct Socket Request ──────────────────────────────────────
 *
 * Sends a binary request directly to the daemon, bypassing the
 * client binary entirely. This measures pure daemon routing speed.
 */
static int send_request(ToolType tool, const char *path,
                        const char *session_id,
                        char *response_buf, int buf_size) {
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

    /* Build and send request */
    RequestMsg req;
    memset(&req, 0, sizeof(req));
    req.magic = PROTOCOL_MAGIC;
    req.version = PROTOCOL_VERSION;
    req.tool_type = tool;
    req.path_len = strlen(path);
    strncpy(req.session_id, session_id, SESSION_ID_LEN - 1);
    strncpy(req.path, path, MAX_PATH_LEN - 1);

    write(fd, &req, sizeof(req));

    /* Read response header */
    ResponseMsg resp;
    ssize_t n = read(fd, &resp, sizeof(resp));
    if (n < (ssize_t)sizeof(resp) || resp.magic != PROTOCOL_MAGIC) {
        close(fd);
        return -1;
    }

    /* Read content */
    int content_read = 0;
    if (resp.content_len > 0 && response_buf) {
        int to_read = resp.content_len;
        if (to_read > buf_size) to_read = buf_size;
        while (content_read < to_read) {
            n = read(fd, response_buf + content_read, to_read - content_read);
            if (n <= 0) break;
            content_read += n;
        }
    }

    close(fd);
    return (resp.status == STATUS_MATCHED) ? content_read : 0;
}

/*
 * ── Test Payloads ──────────────────────────────────────────────
 */
typedef struct {
    const char *label;
    ToolType    tool;
    const char *path;
} TestCase;

static const TestCase STANDARD_CASES[] = {
    {"SwiftUI view",     TOOL_EDIT, "/a/ContentView.swift"},
    {"Networking",       TOOL_EDIT, "/a/NetworkService.swift"},
    {"Data model",       TOOL_EDIT, "/a/UserModel.swift"},
    {"Package manifest", TOOL_READ, "/a/Package.swift"},
    {"xcodebuild",       TOOL_BASH, "xcodebuild build"},
    {"Login view",       TOOL_EDIT, "/a/LoginView.swift"},
    {"HealthKit",        TOOL_EDIT, "/a/HealthKitManager.swift"},
    {"Generic swift",    TOOL_EDIT, "/a/Utils.swift"},
    {"Config file",      TOOL_EDIT, "/a/config.json"},
    {"API client",       TOOL_EDIT, "/a/APIClient.swift"},
};

static const TestCase COMPLEX_CASES[] = {
    {"Deep path view",   TOOL_EDIT, "/Users/jordan/Code/MyApp/Sources/Features/Auth/Views/LoginScreenView.swift"},
    {"Multi-match",      TOOL_EDIT, "/a/SubscriptionPaywallView.swift"},
    {"Long bash",        TOOL_BASH, "xcodebuild test -scheme MyApp -destination \"platform=iOS Simulator,name=iPhone 16 Pro\" -resultBundlePath /tmp/results"},
    {"Migration",        TOOL_EDIT, "/a/CoreDataMigrationV2ToV3.swift"},
    {"swift test",       TOOL_BASH, "swift test --parallel --filter HealthKitTests"},
    {"AR view",          TOOL_EDIT, "/a/ARExperienceView.swift"},
    {"pbxproj",          TOOL_READ, "/a/MyApp.xcodeproj/project.pbxproj"},
    {"Localization",     TOOL_EDIT, "/a/Localizable.xcstrings"},
    {"ASC command",      TOOL_BASH, "asc builds list --app-id 123456789 --limit 10"},
    {"No match (.txt)",  TOOL_EDIT, "/a/NoMatchHere.txt"},
};

#define STANDARD_COUNT (sizeof(STANDARD_CASES) / sizeof(STANDARD_CASES[0]))
#define COMPLEX_COUNT  (sizeof(COMPLEX_CASES) / sizeof(COMPLEX_CASES[0]))

/*
 * ── Run a Benchmark Suite ──────────────────────────────────────
 */
static int suite_counter = 0;  /* ensures unique session IDs across suites */

static void run_suite(const char *title, const TestCase *cases, int case_count,
                      int iterations, bool dedup_test) {
    int suite_id = suite_counter++;
    printf(BLUE "%s" RESET " (%d iterations)\n", title, iterations);

    long total_ns = 0;
    long min_ns = 999999999;
    long max_ns = 0;
    int  total_bytes = 0;
    int  match_count = 0;
    int  empty_count = 0;

    char response[MAX_RESPONSE_CONTENT];
    char session_id[64];

    for (int i = 0; i < iterations; i++) {
        const TestCase *tc = &cases[i % case_count];

        /* Unique session per iteration unless testing dedup.
         * Include suite_id so different suites don't collide in dedup table. */
        if (dedup_test) {
            snprintf(session_id, sizeof(session_id), "dedup-s%d", suite_id);
        } else {
            snprintf(session_id, sizeof(session_id), "s%d-i%d", suite_id, i);
        }

        long start = now_ns();
        int bytes = send_request(tc->tool, tc->path, session_id,
                                 response, sizeof(response));
        long elapsed = now_ns() - start;

        total_ns += elapsed;
        if (elapsed < min_ns) min_ns = elapsed;
        if (elapsed > max_ns) max_ns = elapsed;

        if (bytes > 0) {
            total_bytes += bytes;
            match_count++;
        } else {
            empty_count++;
        }
    }

    long avg_ns = total_ns / iterations;
    int avg_bytes = match_count > 0 ? total_bytes / match_count : 0;
    int avg_tokens = avg_bytes / 4;  /* ~4 chars per token estimate */

    printf("  Avg:         " GREEN "%.1fμs" RESET " (%.2fms)\n",
           ns_to_us(avg_ns), ns_to_ms(avg_ns));
    printf("  Min/Max:     %.1fμs / %.1fμs\n",
           ns_to_us(min_ns), ns_to_us(max_ns));
    printf("  Match rate:  %d/%d matched, %d empty\n",
           match_count, iterations, empty_count);

    if (match_count > 0) {
        printf("  Avg payload: %d bytes (~%d tokens per injection)\n",
               avg_bytes, avg_tokens);
        printf("  Total out:   %dKB (~%d tokens)\n",
               total_bytes / 1024, total_bytes / 4);
    }

    if (dedup_test) {
        printf("  " DIM "First %d unique matches, then %d dedup hits" RESET "\n",
               match_count, empty_count);
        printf("  " DIM "Dedup saves ~%d tokens per session (skills not re-injected)" RESET "\n",
               match_count * avg_tokens);
    } else {
        printf("  " DIM "10-edit batch: ~%.1fμs overhead, ~%d tokens" RESET "\n",
               ns_to_us(avg_ns) * 10, avg_tokens * 10);
        printf("  " DIM "Ralph loop (150 calls): ~%.1fms, ~%dK tokens" RESET "\n",
               ns_to_ms(avg_ns) * 150, avg_tokens * 150 / 1000);
    }
    printf("\n");
}

/*
 * ── Realistic Coding Session ───────────────────────────────────
 *
 * Simulates what actually happens during a feature build:
 * agent edits multiple files across a feature, reads configs,
 * runs builds, comes back to edit more. All within ONE session.
 *
 * This shows dedup in action over a real workflow.
 */
static void bench_realistic_session(void) {
    printf(YELLOW "═══ Realistic Coding Session ═══" RESET "\n\n");
    printf(BLUE "Simulating: \"Add HealthKit workout tracking feature\"" RESET "\n\n");

    /* All tool calls use the same session — like a real conversation */
    const char *session = "real-session-healthkit";

    typedef struct {
        const char *description;
        ToolType    tool;
        const char *path;
    } Step;

    /*
     * A realistic sequence of tool calls for building a HealthKit feature.
     * Note how the same files get touched multiple times (edit, re-edit,
     * read back). Dedup should fire on the repeated patterns.
     */
    static const Step steps[] = {
        /* Phase 1: Read existing code */
        {"Read Package.swift",          TOOL_READ, "/a/Package.swift"},
        {"Read existing model",         TOOL_READ, "/a/UserModel.swift"},
        {"Read existing view",          TOOL_READ, "/a/ContentView.swift"},

        /* Phase 2: Create new HealthKit files */
        {"Create HealthKitManager",     TOOL_WRITE, "/a/HealthKitManager.swift"},
        {"Create WorkoutSession",       TOOL_WRITE, "/a/WorkoutSession.swift"},
        {"Create WorkoutView",          TOOL_WRITE, "/a/WorkoutView.swift"},

        /* Phase 3: Edit existing files to integrate */
        {"Edit ContentView (add nav)",  TOOL_EDIT, "/a/ContentView.swift"},
        {"Edit UserModel (add health)", TOOL_EDIT, "/a/UserModel.swift"},
        {"Edit Package.swift (deps)",   TOOL_EDIT, "/a/Package.swift"},
        {"Edit Info.plist (permissions)",TOOL_EDIT, "/a/Info.plist"},

        /* Phase 4: Build and fix */
        {"Run xcodebuild",             TOOL_BASH, "xcodebuild build -scheme MyApp"},
        {"Fix HealthKitManager",        TOOL_EDIT, "/a/HealthKitManager.swift"},
        {"Fix WorkoutView",             TOOL_EDIT, "/a/WorkoutView.swift"},
        {"Run xcodebuild again",        TOOL_BASH, "xcodebuild build -scheme MyApp"},

        /* Phase 5: Add tests */
        {"Create HealthKitTests",       TOOL_WRITE, "/a/HealthKitTests.swift"},
        {"Run swift test",              TOOL_BASH, "swift test --filter HealthKit"},
        {"Fix test",                    TOOL_EDIT, "/a/HealthKitTests.swift"},
        {"Run swift test again",        TOOL_BASH, "swift test --filter HealthKit"},

        /* Phase 6: Polish */
        {"Add accessibility labels",    TOOL_EDIT, "/a/WorkoutView.swift"},
        {"Add localization",            TOOL_EDIT, "/a/Localizable.xcstrings"},
        {"Edit ContentView (cleanup)",  TOOL_EDIT, "/a/ContentView.swift"},
        {"Final build",                 TOOL_BASH, "xcodebuild build -scheme MyApp"},

        /* Phase 7: Second feature (Subscription paywall) — same session */
        {"Read StoreKit file",          TOOL_READ, "/a/Subscription.swift"},
        {"Create PaywallView",          TOOL_WRITE, "/a/PaywallView.swift"},
        {"Edit PaywallView",            TOOL_EDIT, "/a/PaywallView.swift"},
        {"Create SubscriptionManager",  TOOL_WRITE, "/a/SubscriptionManager.swift"},
        {"Edit ContentView (paywall)",  TOOL_EDIT, "/a/ContentView.swift"},
        {"Run build",                   TOOL_BASH, "xcodebuild build -scheme MyApp"},
        {"Create PaywallTests",         TOOL_WRITE, "/a/PaywallTests.swift"},
        {"Run all tests",               TOOL_BASH, "swift test"},
    };

    int step_count = sizeof(steps) / sizeof(steps[0]);

    long total_ns = 0;
    int total_bytes = 0;
    int match_count = 0;
    int dedup_count = 0;
    char response[MAX_RESPONSE_CONTENT];

    for (int i = 0; i < step_count; i++) {
        long start = now_ns();
        int bytes = send_request(steps[i].tool, steps[i].path, session,
                                 response, sizeof(response));
        long elapsed = now_ns() - start;
        total_ns += elapsed;

        const char *status;
        if (bytes > 0) {
            total_bytes += bytes;
            match_count++;
            status = GREEN "INJECT" RESET;
        } else {
            dedup_count++;
            status = DIM "dedup" RESET;
        }

        printf("  %2d. %-32s %s  (%.0fμs)\n",
               i + 1, steps[i].description, status, ns_to_us(elapsed));
    }

    int total_tokens = total_bytes / 4;
    int saved_tokens = dedup_count * 4000;  /* ~4K tokens avg per skill injection */

    printf("\n");
    printf("  " BLUE "Session summary:" RESET "\n");
    printf("  Total steps:      %d\n", step_count);
    printf("  Total time:       %.2fms\n", ns_to_ms(total_ns));
    printf("  Avg per step:     %.1fμs\n", ns_to_us(total_ns / step_count));
    printf("  Skills injected:  %d (%.1fKB, ~%d tokens)\n",
           match_count, total_bytes / 1024.0, total_tokens);
    printf("  Dedup hits:       %d (~%dK tokens saved)\n",
           dedup_count, saved_tokens / 1000);
    printf("  " DIM "Without dedup: would have injected ~%dK tokens" RESET "\n",
           step_count * 4000 / 1000);
    printf("  " DIM "With dedup:    actually injected ~%dK tokens (%.0f%% reduction)" RESET "\n",
           total_tokens / 1000,
           step_count > 0 ? (1.0 - (double)total_tokens / (step_count * 4000)) * 100 : 0);
    printf("\n");
}

/*
 * ── Client Spawn Benchmark ─────────────────────────────────────
 *
 * Measures the realistic path: fork+exec the client binary,
 * pipe JSON to it, read JSON output. This is what Claude Code
 * actually experiences.
 */
static void bench_client_spawn(int iterations) {
    printf(BLUE "Client spawn (realistic hook path)" RESET " (%d iterations)\n", iterations);

    long total_ns = 0;
    long min_ns = 999999999;
    long max_ns = 0;

    const char *payload_template =
        "{\"tool_name\":\"Edit\",\"tool_input\":{\"file_path\":\"/a/ContentView.swift\","
        "\"old_string\":\"x\",\"new_string\":\"y\"},\"session_id\":\"client-bench-%d\"}";

    for (int i = 0; i < iterations; i++) {
        char payload[512];
        snprintf(payload, sizeof(payload), payload_template, i);

        /*
         * popen: starts a child process with a shell pipe.
         * We pipe our JSON into the client binary and read its output.
         * This measures the full path: fork + exec + socket + response.
         */
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "echo '%s' | %s/router-client 2>/dev/null", payload, harness_dir);

        long start = now_ns();
        FILE *fp = popen(cmd, "r");
        if (fp) {
            char buf[4096];
            while (fgets(buf, sizeof(buf), fp)) {}  /* drain output */
            pclose(fp);
        }
        long elapsed = now_ns() - start;

        total_ns += elapsed;
        if (elapsed < min_ns) min_ns = elapsed;
        if (elapsed > max_ns) max_ns = elapsed;
    }

    long avg_ns = total_ns / iterations;
    printf("  Avg:     " GREEN "%.2fms" RESET "\n", ns_to_ms(avg_ns));
    printf("  Min/Max: %.2fms / %.2fms\n", ns_to_ms(min_ns), ns_to_ms(max_ns));
    printf("  " DIM "This includes: shell + fork + exec client + socket + response" RESET "\n");
    printf("\n");
}

/*
 * ── Main ───────────────────────────────────────────────────────
 */
int main(int argc, char *argv[]) {
    int iterations = 100;
    if (argc > 1) iterations = atoi(argv[1]);
    if (iterations < 1) iterations = 100;

    resolve_harness_dir(argv[0]);

    /* Force line-buffered stdout — without this, output disappears
     * when the harness is run from a pipe or background process.
     * fork() duplicates buffered output, causing chaos. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    const char *plugin_root = getenv("CLAUDE_PLUGIN_ROOT");
    if (!plugin_root) plugin_root = getenv("PLUGIN_ROOT");
    if (!plugin_root) {
        fprintf(stderr, "Set CLAUDE_PLUGIN_ROOT\n");
        return 1;
    }

    /* Clean shutdown on Ctrl+C */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    atexit(cleanup);

    printf("\n");
    printf(BLUE "iOS Skills Daemon Benchmark" RESET "\n");
    printf(DIM "%d iterations, nanosecond precision, no python" RESET "\n\n", iterations);

    /* Start daemon */
    printf("Starting daemon...\n");
    if (!start_daemon(plugin_root)) {
        fprintf(stderr, "Failed to start daemon\n");
        return 1;
    }
    printf("Daemon ready (202 skills mmapped)\n\n");

    /* Warmup — prime the page cache */
    printf(DIM "Warmup (10 requests)..." RESET "\n");
    char buf[MAX_RESPONSE_CONTENT];
    for (int i = 0; i < 10; i++) {
        send_request(TOOL_EDIT, "/a/ContentView.swift", "warmup",
                     buf, sizeof(buf));
    }
    printf("\n");

    printf(YELLOW "═══ Direct Socket (pure daemon speed) ═══" RESET "\n\n");

    /* Standard payloads */
    run_suite("Standard payloads", STANDARD_CASES, STANDARD_COUNT,
              iterations, false);

    /* Complex payloads */
    run_suite("Complex payloads", COMPLEX_CASES, COMPLEX_COUNT,
              iterations, false);

    /* Dedup stress */
    run_suite("Dedup stress (same session)", STANDARD_CASES, STANDARD_COUNT,
              iterations, true);

    printf(YELLOW "═══ Realistic Coding Session ═══" RESET "\n\n");
    bench_realistic_session();

    printf(YELLOW "═══ Client Spawn (what Claude Code sees) ═══" RESET "\n\n");

    /* Client spawn — limited iterations since it's slower */
    int client_iters = iterations > 30 ? 30 : iterations;
    bench_client_spawn(client_iters);

    /* Summary */
    printf(YELLOW "═══ Comparison ═══" RESET "\n\n");
    printf("  " DIM "%-20s  %-12s  %-12s  %-12s" RESET "\n",
           "Implementation", "Avg/call", "10-edit", "Ralph (150)");
    printf("  " DIM "%-20s  %-12s  %-12s  %-12s" RESET "\n",
           "────────────────", "──────────", "──────────", "──────────");
    printf("  %-20s  ~50ms        ~500ms       ~7.5s\n", "Node.js");
    printf("  %-20s  ~1ms         ~10ms        ~0.15s\n", "C (spawn/call)");
    printf("  %-20s  ~2-5ms       ~25-50ms     ~0.3-0.7s\n", "Daemon (via client)");
    printf("  %-20s  ~50-200μs    ~0.5-2ms     ~7-30ms\n", "Daemon (direct)");
    printf("\n");
    printf("  " DIM "Token cost: ~3,000-5,000 tokens per skill injection" RESET "\n");
    printf("  " DIM "Dedup saves ~4K tokens per repeated file edit" RESET "\n");
    printf("  " DIM "Without dedup: ralph loop ≈ 600K tokens of skill content" RESET "\n");
    printf("  " DIM "With dedup:    ralph loop ≈ 15K tokens (first injection only)" RESET "\n");
    printf("\n");

    /* Cleanup happens via atexit */
    return 0;
}
