/*
 * router-daemon.c — Persistent iOS skill router
 *
 * This daemon starts once, memory-maps all SKILL.md files,
 * builds routing rules in memory, and listens on a Unix socket.
 * Each hook call is handled by a thin client that connects,
 * sends a binary request, gets a binary response.
 *
 * LIFECYCLE:
 *   1. Start: mmap all skills, build rule table, create socket
 *   2. Run: accept connections, route requests, send responses
 *   3. Stop: unmap files, close socket, clean up
 *
 * WHY A DAEMON:
 *   Process spawn (fork+exec) costs ~1ms even for C.
 *   The Node version costs ~50ms (V8 startup).
 *   A daemon costs 0ms startup — it's already running.
 *   Skills are already in memory — no disk I/O per request.
 *
 * BUILD:
 *   make          (or: cc -O2 -o router-daemon router-daemon.c)
 *
 * RUN:
 *   ./router-daemon                    (foreground, logs to stderr)
 *   ./router-daemon &                  (background)
 *   PLUGIN_ROOT=/path ./router-daemon  (custom skill location)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>      /* signal(), SIGINT, SIGTERM — clean shutdown */
#include <unistd.h>      /* close(), unlink(), read(), write() */
#include <sys/socket.h>  /* socket(), bind(), listen(), accept() */
#include <sys/un.h>      /* struct sockaddr_un — Unix domain socket address */
#include <sys/mman.h>    /* mmap(), munmap() — memory-mapped files */
#include <sys/stat.h>    /* fstat() — get file size for mmap */
#include <fcntl.h>       /* open(), O_RDONLY */
#include <time.h>        /* time() — for uptime tracking */
#include <dirent.h>      /* opendir(), readdir() — scan skill directories */
#include <errno.h>       /* errno — why did that syscall fail? */

#include "protocol.h"

/*
 * ── Constants ──────────────────────────────────────────────────
 */
#define MAX_MAPPED_SKILLS 256     /* max skills we can mmap */
#define MAX_DEDUP_ENTRIES 1024    /* max session+skill dedup pairs */
#define MAX_RULES         64      /* max routing rules */
#define MAX_PATTERNS_PER_RULE 16  /* max pipe-separated patterns per rule */

/*
 * ── Memory-Mapped Skill ────────────────────────────────────────
 *
 * Instead of reading SKILL.md from disk on every request,
 * we mmap it once at startup. The OS maps the file's bytes
 * directly into our address space. Reading the skill content
 * is just dereferencing a pointer — no syscalls, no copying.
 *
 * HOW MMAP WORKS:
 *   Regular file read:
 *     open → read (kernel copies bytes to your buffer) → close
 *     Each read = syscall = context switch to kernel
 *
 *   mmap:
 *     open → mmap (kernel maps file pages into your memory) → close
 *     Reading = normal memory access, no syscall
 *     OS handles paging: if the file is on SSD, pages load on first access
 *     Subsequent reads hit the page cache — as fast as RAM
 *
 *   munmap:
 *     Releases the mapping. The memory is no longer accessible.
 */
typedef struct {
    char  id[128];       /* skill ID: "twostraws--swiftui-pro" */
    char *data;          /* pointer to mmapped file content */
    size_t size;         /* file size in bytes */
} MappedSkill;

/*
 * ── Routing Rule ───────────────────────────────────────────────
 *
 * Same concept as route-skills.c, but patterns are split at
 * startup (not re-split on every request like strtok did).
 */
typedef struct {
    char  patterns[MAX_PATTERNS_PER_RULE][128]; /* pre-split patterns */
    int   pattern_count;
    int   skill_indices[3];  /* indices into the skills array */
    int   skill_count;
    int   priority;
    bool  is_bash;
} Rule;

/*
 * ── Session Dedup ──────────────────────────────────────────────
 *
 * In the old version: write files to ~/.ios-skills/dedup/
 * In the daemon: just keep a table in memory.
 * No filesystem I/O, no cleanup scripts, no race conditions.
 *
 * Each entry is a session_id + skill_id pair.
 * If the pair exists, we already injected that skill this session.
 */
typedef struct {
    char session_id[SESSION_ID_LEN];
    char skill_id[128];
    time_t timestamp;    /* for expiring old sessions */
} DedupEntry;

/*
 * ── Global State ───────────────────────────────────────────────
 *
 * The daemon is single-threaded (one request at a time).
 * Global state is fine here — no concurrency issues.
 * This is all the state that persists across requests.
 */
static struct {
    /* Skills */
    MappedSkill skills[MAX_MAPPED_SKILLS];
    int         skill_count;

    /* Routing rules */
    Rule file_rules[MAX_RULES];
    int  file_rule_count;
    Rule bash_rules[MAX_RULES];
    int  bash_rule_count;

    /* Dedup */
    DedupEntry dedup[MAX_DEDUP_ENTRIES];
    int        dedup_count;

    /* Socket */
    int  server_fd;

    /* Stats */
    time_t start_time;
    long   total_requests;
    long   total_matches;
    long   total_deduped;

    /* Config */
    char plugin_root[MAX_PATH_LEN];

    /* Shutdown flag */
    volatile bool running;
} G;

/*
 * ── Signal Handler ─────────────────────────────────────────────
 *
 * When you hit Ctrl+C (SIGINT) or the system sends SIGTERM,
 * this sets the running flag to false. The main loop checks
 * this flag and exits cleanly — unmapping files, closing the
 * socket, deleting the socket file.
 *
 * volatile: tells the compiler "this variable can change at any
 * time (from a signal), don't optimize away reads of it."
 */
static void handle_signal(int sig) {
    (void)sig;  /* unused parameter — cast to void to suppress warning */
    G.running = false;
}

/*
 * ── Skill Loading (mmap) ───────────────────────────────────────
 *
 * Scans the skills/ directory, finds every SKILL.md,
 * and mmaps each one into memory.
 */
static int find_skill_index(const char *skill_id) {
    for (int i = 0; i < G.skill_count; i++) {
        if (strcmp(G.skills[i].id, skill_id) == 0) return i;
    }
    return -1;
}

static bool mmap_skill(const char *skill_id) {
    if (G.skill_count >= MAX_MAPPED_SKILLS) return false;

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/skills/%s/SKILL.md",
             G.plugin_root, skill_id);

    /* Open the file */
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    /* Get file size */
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size == 0) {
        close(fd);
        return false;
    }

    /* mmap it — this is the key operation.
     *
     * Arguments:
     *   NULL      — let the OS choose the virtual address
     *   st.st_size — map the whole file
     *   PROT_READ — read-only (we never modify skills)
     *   MAP_PRIVATE — changes (if any) don't affect the file
     *   fd        — the file descriptor
     *   0         — start from beginning of file
     *
     * Returns: pointer to the mapped memory, or MAP_FAILED.
     * After this, skill->data[0..size-1] IS the file content.
     * No read() call needed — the bytes are just... there.
     */
    char *data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);  /* can close fd after mmap — the mapping stays */

    if (data == MAP_FAILED) return false;

    MappedSkill *skill = &G.skills[G.skill_count];
    strncpy(skill->id, skill_id, sizeof(skill->id) - 1);
    skill->data = data;
    skill->size = st.st_size;
    G.skill_count++;

    return true;
}

static void load_all_skills(void) {
    char skills_dir[MAX_PATH_LEN];
    snprintf(skills_dir, sizeof(skills_dir), "%s/skills", G.plugin_root);

    DIR *dir = opendir(skills_dir);
    if (!dir) {
        fprintf(stderr, "[daemon] cannot open skills dir: %s\n", skills_dir);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. and _router */
        if (entry->d_name[0] == '.' || entry->d_name[0] == '_') continue;

        /* Check if SKILL.md exists in this directory */
        char check_path[MAX_PATH_LEN];
        snprintf(check_path, sizeof(check_path), "%s/%s/SKILL.md",
                 skills_dir, entry->d_name);

        if (access(check_path, R_OK) == 0) {
            mmap_skill(entry->d_name);
        }
    }

    closedir(dir);
    fprintf(stderr, "[daemon] loaded %d skills into memory\n", G.skill_count);
}

static void unload_all_skills(void) {
    for (int i = 0; i < G.skill_count; i++) {
        if (G.skills[i].data) {
            munmap(G.skills[i].data, G.skills[i].size);
            G.skills[i].data = NULL;
        }
    }
    G.skill_count = 0;
}

/*
 * ── Rule Loading ───────────────────────────────────────────────
 *
 * Adds compiled routing rules.
 * In the future, these could be loaded from skills-index.json
 * (embedded at compile time or read from disk once at startup).
 * For now, they're hardcoded like the original C version.
 */
static void add_rule(Rule *rules, int *count, bool is_bash,
                     const char *patterns, int priority,
                     const char *skill_ids[], int skill_id_count) {
    if (*count >= MAX_RULES) return;

    Rule *r = &rules[*count];
    r->is_bash = is_bash;
    r->priority = priority;
    r->pattern_count = 0;
    r->skill_count = 0;

    /* Pre-split patterns by pipe — do this once, not per request */
    char buf[1024];
    strncpy(buf, patterns, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = strtok(buf, "|");
    while (token && r->pattern_count < MAX_PATTERNS_PER_RULE) {
        strncpy(r->patterns[r->pattern_count], token, 127);
        r->pattern_count++;
        token = strtok(NULL, "|");
    }

    /* Resolve skill IDs to indices in our mmap'd skill array */
    for (int i = 0; i < skill_id_count && i < 3; i++) {
        int idx = find_skill_index(skill_ids[i]);
        if (idx >= 0) {
            r->skill_indices[r->skill_count++] = idx;
        }
    }

    (*count)++;
}

static void load_rules(void) {
    /* File rules — same as route-skills.c */
    const char *s1[] = {"twostraws--swiftui-pro", "dadederk--ios-accessibility"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "View.swift|Screen.swift|Page.swift|ContentView", 10, s1, 2);

    const char *s2[] = {"twostraws--swift-testing-pro", "avdlee--swift-testing-expert"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Tests.swift|Test.swift|Spec.swift", 10, s2, 2);

    const char *s3[] = {"twostraws--swiftdata-pro", "avdlee--core-data-expert"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Model.swift|Entity.swift|.xcdatamodeld|Schema.swift|Migration.swift", 10, s3, 2);

    const char *s4[] = {"avdlee--spm-build-analysis"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Package.swift|Package.resolved", 9, s4, 1);

    const char *s5[] = {"avdlee--xcode-project-analyzer"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             ".xcodeproj|.pbxproj|.xcworkspace|.xcscheme", 8, s5, 1);

    const char *s6[] = {"truongduy2611--app-store-preflight"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Info.plist|.entitlements|PrivacyInfo.xcprivacy", 9, s6, 1);

    const char *s7[] = {"dpearson2699--widgetkit"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Widget.swift|TimelineProvider.swift|TimelineEntry.swift", 9, s7, 1);

    const char *s8[] = {"dpearson2699--app-intents"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Intent.swift|AppShortcut.swift", 9, s8, 1);

    const char *s9[] = {"dpearson2699--healthkit"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "HealthKit.swift|HealthStore.swift|HKQuery.swift|HKSample.swift|WorkoutSession.swift", 8, s9, 1);

    const char *s10[] = {"dpearson2699--mapkit"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "MapKit.swift|MapView.swift|MKAnnotation.swift|CLLocation.swift", 8, s10, 1);

    const char *s11[] = {"dpearson2699--storekit"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "StoreKit.swift|InAppPurchase.swift|Subscription.swift|Paywall.swift", 8, s11, 1);

    const char *s12[] = {"dpearson2699--push-notifications"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Notification.swift|Push.swift|UNUser.swift", 8, s12, 1);

    const char *s13[] = {"dpearson2699--coreml"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "CoreML.swift|MLModel.swift|VNRequest.swift|Prediction.swift|FoundationModel.swift", 8, s13, 1);

    const char *s14[] = {"dpearson2699--ios-networking", "twostraws--swift-concurrency-pro"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Network.swift|API.swift|Client.swift|Endpoint.swift|URLSession.swift|Request.swift|Response.swift", 6, s14, 2);

    const char *s15[] = {"dpearson2699--authentication"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Auth.swift|Login.swift|SignIn.swift|Credential.swift", 8, s15, 1);

    const char *s16[] = {"dadederk--ios-accessibility"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Accessibility.swift|A11y.swift|VoiceOver.swift", 9, s16, 1);

    const char *s17[] = {"twostraws--swift-concurrency-pro", "efremidze--swift-architecture-skill"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Manager.swift|Service.swift|Provider.swift|Coordinator.swift|Repository.swift|DataSource.swift|UseCase.swift", 5, s17, 2);

    const char *s18[] = {"dpearson2699--ios-localization"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             ".strings|.stringsdict|Localizable|.xcstrings", 8, s18, 1);

    const char *s19[] = {"dpearson2699--realitykit"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "ARView.swift|ARSession.swift|RealityKit.swift|AnchorEntity.swift|RealityView.swift", 8, s19, 1);

    const char *s20[] = {"dpearson2699--swiftui-uikit-interop"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "ViewController.swift|UIKit.swift|Representable.swift", 7, s20, 1);

    const char *s21[] = {"dpearson2699--ios-security"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Security.swift|Keychain.swift|Crypto.swift|Biometric.swift", 8, s21, 1);

    const char *s22[] = {"dpearson2699--swiftui-liquid-glass", "harryworld--xcode-26"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             "Glass.swift|LiquidGlass.swift", 9, s22, 2);

    const char *s23[] = {"martinlasek--swift-coding-guideline"};
    add_rule(G.file_rules, &G.file_rule_count, false,
             ".swift", 1, s23, 1);

    /* Bash rules */
    const char *b1[] = {"getsentry--xcodebuildmcp-cli", "avdlee--xcode-build-orchestrator"};
    add_rule(G.bash_rules, &G.bash_rule_count, true,
             "xcodebuild", 10, b1, 2);

    const char *b2[] = {"twostraws--swift-testing-pro"};
    add_rule(G.bash_rules, &G.bash_rule_count, true,
             "swift test", 10, b2, 1);

    const char *b3[] = {"avdlee--spm-build-analysis"};
    add_rule(G.bash_rules, &G.bash_rule_count, true,
             "swift build", 8, b3, 1);

    const char *b4[] = {"getsentry--xcodebuildmcp-cli"};
    add_rule(G.bash_rules, &G.bash_rule_count, true,
             "simctl", 9, b4, 1);

    const char *b5[] = {"rudrankriyam--asc-cli-usage", "rudrankriyam--asc-workflow"};
    add_rule(G.bash_rules, &G.bash_rule_count, true,
             "asc ", 10, b5, 2);

    const char *b6[] = {"rudrankriyam--asc-release-flow"};
    add_rule(G.bash_rules, &G.bash_rule_count, true,
             "fastlane", 8, b6, 1);

    const char *b7[] = {"martinlasek--swift-coding-guideline"};
    add_rule(G.bash_rules, &G.bash_rule_count, true,
             "swiftlint|swift-format", 7, b7, 1);

    const char *b8[] = {"dpearson2699--debugging-instruments"};
    add_rule(G.bash_rules, &G.bash_rule_count, true,
             "instruments|xctrace", 9, b8, 1);

    fprintf(stderr, "[daemon] loaded %d file rules, %d bash rules\n",
            G.file_rule_count, G.bash_rule_count);
}

/*
 * ── Dedup ──────────────────────────────────────────────────────
 */
static bool is_deduped(const char *session_id, const char *skill_id) {
    for (int i = 0; i < G.dedup_count; i++) {
        if (strcmp(G.dedup[i].session_id, session_id) == 0 &&
            strcmp(G.dedup[i].skill_id, skill_id) == 0) {
            return true;
        }
    }
    return false;
}

static void mark_deduped(const char *session_id, const char *skill_id) {
    if (G.dedup_count >= MAX_DEDUP_ENTRIES) {
        /* Evict oldest entries (simple: just reset) */
        /* A real system would use LRU or a hash table */
        G.dedup_count = 0;
    }
    DedupEntry *e = &G.dedup[G.dedup_count++];
    strncpy(e->session_id, session_id, SESSION_ID_LEN - 1);
    strncpy(e->skill_id, skill_id, 127);
    e->timestamp = time(NULL);
}

/*
 * ── Pattern Matching ───────────────────────────────────────────
 *
 * Same logic as route-skills.c but uses pre-split patterns
 * (no strtok per request — patterns were split at startup).
 */
static const char *get_basename(const char *path) {
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}

static bool rule_matches(const Rule *rule, const char *str, const char *basename) {
    for (int p = 0; p < rule->pattern_count; p++) {
        const char *pat = rule->patterns[p];
        int plen = strlen(pat);

        if (rule->is_bash) {
            if (strstr(str, pat)) return true;
        } else {
            int blen = strlen(basename);
            int slen = strlen(str);
            if (blen >= plen && strcmp(basename + blen - plen, pat) == 0) return true;
            if (slen >= plen && strcmp(str + slen - plen, pat) == 0) return true;
        }
    }
    return false;
}

/*
 * ── TODO(human): Handle Request ────────────────────────────────
 *
 * This is the core routing function. It receives a parsed request,
 * matches against rules, checks dedup, and builds the response.
 *
 * You need to:
 * 1. Decide which rule array to search (file_rules vs bash_rules)
 *    based on req->tool_type
 * 2. Collect matches with their priorities
 * 3. Sort by priority (highest first) and deduplicate skill IDs
 * 4. Check session dedup (skip skills already injected this session)
 * 5. Build the response content from mmapped skill data
 *
 * Available helpers:
 *   rule_matches(rule, path, basename) — does a rule match this path?
 *   get_basename(path)                 — "/a/b/Foo.swift" → "Foo.swift"
 *   is_deduped(session_id, skill_id)   — already injected?
 *   mark_deduped(session_id, skill_id) — record injection
 *   G.skills[idx].data / .size / .id   — mmapped skill content
 *   G.file_rules / G.bash_rules        — rule arrays
 *   MAX_SKILLS_PER_RESPONSE            — cap at 3
 *   MAX_RESPONSE_CONTENT               — 24KB byte budget
 *
 * Return: number of bytes written to content_buf
 *         Set *out_skill_count to number of skills included
 *         Set *out_status to STATUS_MATCHED, STATUS_DEDUPED, or STATUS_NO_MATCH
 */
static int handle_request(const RequestMsg *req,
                          char *content_buf, int buf_size,
                          uint8_t *out_skill_count,
                          uint8_t *out_status) {

    /* Determine which rule array to search */
    bool is_file_tool = (req->tool_type == TOOL_READ ||
                         req->tool_type == TOOL_EDIT ||
                         req->tool_type == TOOL_MULTIEDIT ||
                         req->tool_type == TOOL_WRITE);
    bool is_bash = (req->tool_type == TOOL_BASH);

    Rule *rules = NULL;
    int rule_count = 0;

    if (is_file_tool && req->path[0]) {
        rules = G.file_rules;
        rule_count = G.file_rule_count;
    } else if (is_bash && req->path[0]) {
        rules = G.bash_rules;
        rule_count = G.bash_rule_count;
    } else {
        *out_status = STATUS_NO_MATCH;
        return 0;
    }

    const char *basename = is_bash ? "" : get_basename(req->path);

    /*
     * Phase 1: Collect all matching skill indices with priorities.
     *
     * We can't just take the first matches — we need the HIGHEST
     * priority ones. A .swift file matches both "View.swift" (priority 10)
     * and ".swift" (priority 1). We want the priority 10 match.
     */
    typedef struct { int skill_idx; int priority; } Hit;
    Hit hits[64];
    int hit_count = 0;

    for (int r = 0; r < rule_count; r++) {
        if (!rule_matches(&rules[r], req->path, basename)) continue;

        for (int s = 0; s < rules[r].skill_count; s++) {
            if (hit_count >= 64) break;
            hits[hit_count].skill_idx = rules[r].skill_indices[s];
            hits[hit_count].priority = rules[r].priority;
            hit_count++;
        }
    }

    if (hit_count == 0) {
        *out_status = STATUS_NO_MATCH;
        return 0;
    }

    /*
     * Phase 2: Sort by priority (highest first).
     *
     * Simple insertion sort — hit_count is small (< 64),
     * and insertion sort beats qsort for tiny arrays.
     */
    for (int i = 1; i < hit_count; i++) {
        Hit tmp = hits[i];
        int j = i - 1;
        while (j >= 0 && hits[j].priority < tmp.priority) {
            hits[j + 1] = hits[j];
            j--;
        }
        hits[j + 1] = tmp;
    }

    /*
     * Phase 3: Deduplicate and select top skills.
     *
     * Walk the sorted hits. Skip duplicates (same skill from
     * multiple rules). Skip already-injected (session dedup).
     * Stop at MAX_SKILLS_PER_RESPONSE or when byte budget is full.
     */
    int total_bytes = 0;
    int selected = 0;
    bool all_deduped = true;  /* track if every match was deduped */

    for (int i = 0; i < hit_count && selected < MAX_SKILLS_PER_RESPONSE; i++) {
        int idx = hits[i].skill_idx;
        const char *skill_id = G.skills[idx].id;

        /* Skip if we already selected this skill (same skill, different rule) */
        bool already_selected = false;
        for (int j = 0; j < i; j++) {
            if (hits[j].skill_idx == idx) { already_selected = true; break; }
        }
        if (already_selected) continue;

        /* Skip if already injected this session */
        if (is_deduped(req->session_id, skill_id)) continue;

        /* This skill is new — it matched and wasn't deduped */
        all_deduped = false;

        /* Check byte budget — don't overflow content_buf */
        MappedSkill *skill = &G.skills[idx];
        if (total_bytes + (int)skill->size > buf_size) continue;

        /*
         * Copy the mmapped skill content into the response buffer.
         *
         * THIS is the payoff of mmap. skill->data is already in memory.
         * memcpy just copies bytes from one address to another.
         * No open(), no read(), no syscall — just a memory copy.
         *
         * On a modern CPU with the page already cached, this copies
         * ~10GB/s. A 15KB skill file takes ~1.5 microseconds.
         */
        if (selected > 0) {
            /* Add separator between skills */
            const char *sep = "\n\n---\n\n";
            int sep_len = 7;
            if (total_bytes + sep_len + (int)skill->size > buf_size) continue;
            memcpy(content_buf + total_bytes, sep, sep_len);
            total_bytes += sep_len;
        }

        memcpy(content_buf + total_bytes, skill->data, skill->size);
        total_bytes += skill->size;

        /* Mark as injected so future calls this session skip it */
        mark_deduped(req->session_id, skill_id);
        selected++;
    }

    /* Set status */
    if (selected > 0) {
        *out_skill_count = selected;
        *out_status = STATUS_MATCHED;
    } else if (all_deduped && hit_count > 0) {
        *out_skill_count = 0;
        *out_status = STATUS_DEDUPED;
    } else {
        *out_skill_count = 0;
        *out_status = STATUS_NO_MATCH;
    }

    return total_bytes;
}

/*
 * ── Socket Setup ───────────────────────────────────────────────
 *
 * Creates a Unix domain socket and starts listening.
 *
 * Unix domain sockets are like TCP sockets but local-only.
 * Instead of an IP:port, they use a filesystem path.
 * Faster than TCP (no network stack overhead) and secure
 * (only processes on this machine can connect).
 */
static int setup_socket(void) {
    /* Remove stale socket file from a previous crash */
    unlink(SOCKET_PATH);

    /* Create the socket
     * AF_UNIX   = Unix domain (local, not network)
     * SOCK_STREAM = reliable byte stream (like TCP)
     * 0         = default protocol
     */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[daemon] socket");
        return -1;
    }

    /* Bind to the filesystem path
     * This creates /tmp/ios-skills-router.sock
     * The client connects to this same path.
     */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[daemon] bind");
        close(fd);
        return -1;
    }

    /* Start listening — allow 5 pending connections
     * In practice we only get 1 at a time (Claude Code is sequential)
     * but the backlog handles bursts.
     */
    if (listen(fd, 5) < 0) {
        perror("[daemon] listen");
        close(fd);
        unlink(SOCKET_PATH);
        return -1;
    }

    return fd;
}

/*
 * ── Process One Connection ─────────────────────────────────────
 *
 * Reads a RequestMsg from the client, routes it, and sends
 * a ResponseMsg back. Then closes the connection.
 */
static void process_connection(int client_fd) {
    G.total_requests++;

    /* Read the request */
    RequestMsg req;
    ssize_t n = read(client_fd, &req, sizeof(req));
    if (n < (ssize_t)RESPONSE_HEADER_SIZE) {
        fprintf(stderr, "[daemon] short read: %zd bytes\n", n);
        close(client_fd);
        return;
    }

    /* Validate magic and version */
    if (req.magic != PROTOCOL_MAGIC || req.version != PROTOCOL_VERSION) {
        fprintf(stderr, "[daemon] bad magic/version: 0x%08x v%d\n",
                req.magic, req.version);
        close(client_fd);
        return;
    }

    /* Handle control commands */
    if (req.tool_type == TOOL_UNKNOWN) {
        if (strncmp(req.path, CMD_PING, 4) == 0) {
            ResponseMsg resp = {
                .magic = PROTOCOL_MAGIC,
                .version = PROTOCOL_VERSION,
                .status = STATUS_MATCHED,
                .skill_count = 0,
                .content_len = 0,
            };
            write(client_fd, &resp, sizeof(resp));
            close(client_fd);
            return;
        }
        if (strncmp(req.path, CMD_SHUTDOWN, 8) == 0) {
            G.running = false;
            ResponseMsg resp = {
                .magic = PROTOCOL_MAGIC,
                .version = PROTOCOL_VERSION,
                .status = STATUS_MATCHED,
                .content_len = 0,
            };
            write(client_fd, &resp, sizeof(resp));
            close(client_fd);
            return;
        }
        if (strncmp(req.path, CMD_RELOAD, 6) == 0) {
            unload_all_skills();
            load_all_skills();
            G.file_rule_count = 0;
            G.bash_rule_count = 0;
            load_rules();
            ResponseMsg resp = {
                .magic = PROTOCOL_MAGIC,
                .version = PROTOCOL_VERSION,
                .status = STATUS_MATCHED,
                .content_len = 0,
            };
            write(client_fd, &resp, sizeof(resp));
            close(client_fd);
            return;
        }
    }

    /* Route the request */
    char content_buf[MAX_RESPONSE_CONTENT];
    uint8_t skill_count = 0;
    uint8_t status = STATUS_NO_MATCH;

    int content_len = handle_request(&req, content_buf, sizeof(content_buf),
                                     &skill_count, &status);

    if (status == STATUS_MATCHED) G.total_matches++;
    if (status == STATUS_DEDUPED) G.total_deduped++;

    /* Send response header */
    ResponseMsg resp = {
        .magic = PROTOCOL_MAGIC,
        .version = PROTOCOL_VERSION,
        .status = status,
        .skill_count = skill_count,
        .content_len = content_len,
    };
    write(client_fd, &resp, sizeof(resp));

    /* Send content body (if any) */
    if (content_len > 0) {
        write(client_fd, content_buf, content_len);
    }

    close(client_fd);
}

/*
 * ── Main ───────────────────────────────────────────────────────
 */
/*
 * ── PID File ───────────────────────────────────────────────────
 *
 * A PID file records this daemon's process ID so:
 * 1. The client can check if a daemon is running before starting one
 * 2. A new daemon can kill a stale one on startup
 * 3. Cleanup scripts can find and kill it
 *
 * Without this, every crashed run leaves an orphan.
 */
static void write_pid_file(void) {
    FILE *f = fopen(PID_PATH, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

static void remove_pid_file(void) {
    unlink(PID_PATH);
}

static void kill_stale_daemon(void) {
    FILE *f = fopen(PID_PATH, "r");
    if (!f) return;

    int old_pid = 0;
    if (fscanf(f, "%d", &old_pid) == 1 && old_pid > 0) {
        /* Check if the process is actually running */
        if (kill(old_pid, 0) == 0) {
            fprintf(stderr, "[daemon] killing stale daemon (pid %d)\n", old_pid);
            kill(old_pid, SIGTERM);
            usleep(100000);  /* 100ms for graceful shutdown */
            if (kill(old_pid, 0) == 0) {
                kill(old_pid, SIGKILL);  /* force if still alive */
                usleep(50000);
            }
        }
    }
    fclose(f);
}

int main(void) {
    /* Initialize global state */
    memset(&G, 0, sizeof(G));
    G.running = true;
    G.start_time = time(NULL);

    /* Get plugin root */
    const char *root = getenv("CLAUDE_PLUGIN_ROOT");
    if (!root) root = getenv("PLUGIN_ROOT");
    if (!root) {
        fprintf(stderr, "[daemon] set CLAUDE_PLUGIN_ROOT or PLUGIN_ROOT\n");
        return EXIT_FAILURE;
    }
    strncpy(G.plugin_root, root, sizeof(G.plugin_root) - 1);

    fprintf(stderr, "[daemon] starting — plugin root: %s\n", G.plugin_root);

    /* Kill any stale daemon from a previous crash */
    kill_stale_daemon();

    /* Load skills into memory (mmap) */
    load_all_skills();
    load_rules();

    /* Set up signal handlers for clean shutdown.
     *
     * sigaction instead of signal — more reliable on Unix.
     * SA_RESTART is NOT set, so accept() will return -1 with
     * errno=EINTR when a signal arrives. This is what we want:
     * the main loop checks G.running and exits cleanly.
     */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Create and bind socket */
    G.server_fd = setup_socket();
    if (G.server_fd < 0) return EXIT_FAILURE;

    /* Write PID file so others can find/kill us */
    write_pid_file();

    fprintf(stderr, "[daemon] listening on %s (pid %d)\n", SOCKET_PATH, getpid());
    fprintf(stderr, "[daemon] ready — %d skills, %d file rules, %d bash rules\n",
            G.skill_count, G.file_rule_count, G.bash_rule_count);
    fprintf(stderr, "[daemon] idle timeout: %ds\n", IDLE_TIMEOUT_SECS);

    /*
     * Main event loop with idle timeout.
     *
     * Instead of blocking forever in accept(), we use select()
     * with a timeout. If no connection arrives within the timeout,
     * the daemon shuts itself down. This prevents orphans.
     *
     * select() is the classic Unix way to wait on multiple things
     * at once (or wait with a timeout). It watches file descriptors
     * and returns when one is ready or when time expires.
     */
    time_t last_activity = time(NULL);

    while (G.running) {
        /* Set up select() to watch the server socket with a timeout */
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(G.server_fd, &readfds);

        struct timeval tv;
        tv.tv_sec = 30;   /* check every 30 seconds */
        tv.tv_usec = 0;

        int ready = select(G.server_fd + 1, &readfds, NULL, NULL, &tv);

        if (ready < 0) {
            /* select() interrupted by signal — check G.running */
            if (errno == EINTR) continue;
            perror("[daemon] select");
            break;
        }

        if (ready == 0) {
            /* Timeout — no connections. Check idle time. */
            if (time(NULL) - last_activity > IDLE_TIMEOUT_SECS) {
                fprintf(stderr, "[daemon] idle for %ds, shutting down\n",
                        IDLE_TIMEOUT_SECS);
                break;
            }
            continue;
        }

        /* Connection ready — accept it */
        int client_fd = accept(G.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;  /* signal interrupted accept */
            if (G.running) perror("[daemon] accept");
            continue;
        }

        last_activity = time(NULL);
        process_connection(client_fd);
    }

    /* Clean shutdown */
    fprintf(stderr, "\n[daemon] shutting down — %ld requests, %ld matches, %ld deduped\n",
            G.total_requests, G.total_matches, G.total_deduped);
    fprintf(stderr, "[daemon] uptime: %lds\n", time(NULL) - G.start_time);

    close(G.server_fd);
    unlink(SOCKET_PATH);
    remove_pid_file();
    unload_all_skills();

    return EXIT_SUCCESS;
}
