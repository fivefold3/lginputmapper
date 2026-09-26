/*
 * lginputmapperd - remote button remapper for rooted LG webOS TVs (LG Input Mapper).
 *
 * Grabs the virtual input devices that lginput2 / micomservice create for the
 * Magic Remote and IR remote (EVIOCGRAB), applies the user's mappings, and
 * re-emits the resulting events through a uinput clone of each device (or into
 * an existing sibling device). No process injection, no hooks: only the evdev
 * and uinput kernel ABIs are used, so it works on any webOS version and on
 * both 32-bit and 64-bit ARM.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <linux/uinput.h>

#include "vendor/cJSON.h"

#ifndef LGINPUTMAPPERD_VERSION
#define LGINPUTMAPPERD_VERSION "1.1.0"
#endif

#define DEFAULT_CONFIG "/home/root/.config/lginputmapper/config.json"
#define DEFAULT_STATE_DIR "/tmp/lginputmapperd"
#define DEV_INPUT "/dev/input"
#define CLONE_PHYS_PREFIX "lginputmapperd/"
#define DEFAULT_INCLUDE "LGE *"
/* surface-manager only reads the input devices that existed when it started,
 * so remapped events are injected into a sibling device it already has open
 * (lginput2 creates several identical ones) rather than into a fresh clone. */
#define DEFAULT_OUTPUT_DEVICE "LGE M-RCU - Builtin [2]"

/* LG kernels carry key codes well above mainline KEY_MAX (0x2ff), e.g. 1123
 * for the C5 "home hub" button, so every table here is sized for a bigger
 * bitmap and the real limit is read back from the kernel at runtime. */
#define MAX_KEY_CNT 4096
#define MAX_EV_CNT 32
#define MAX_ABS_CNT 64
#define MAX_REL_CNT 16
#define MAX_MSC_CNT 8
#define MAX_LED_CNT 16
#define MAX_SND_CNT 8
#define MAX_SW_CNT 32
#define MAX_PROP_CNT 32
#define MAX_DEVS 32

#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define NLONGS(bits) (((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bit, arr) (((arr)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

/* The kernel's struct input_event: two __kernel_ulong_t fields followed by
 * type/code/value. Spelled out here so the layout does not depend on the C
 * library's time_t width (musl 1.2 has a 64-bit time_t on 32-bit ARM). */
struct raw_event {
    unsigned long sec;
    unsigned long usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

enum act_type { ACT_PASS = 0, ACT_DISABLE, ACT_REPLACE, ACT_EXEC, ACT_LAUNCH };
enum out_mode { OUT_AUTO = 0, OUT_DEVICE, OUT_CLONE };

struct action {
    enum act_type type;
    int to;            /* ACT_REPLACE */
    char *command;     /* ACT_EXEC */
    int on_release;    /* ACT_EXEC: also run on release */
    char *app;         /* ACT_LAUNCH */
    char *params;      /* ACT_LAUNCH: JSON text or NULL */
};

struct mapping {
    int key;
    int enabled;
    int looped;        /* part of a loop of "acts as" mappings: the button does nothing */
    int standby;       /* 0: the button must not turn the TV on from standby */
    char *label;
    struct action press;
    int has_hold;
    struct action hold;
    int hold_ms;
};

struct config {
    struct mapping *list;
    int nmaps;
    struct mapping *by_key[MAX_KEY_CNT];
    char **include; int ninclude;
    char **exclude; int nexclude;
    int output_mode;           /* OUT_AUTO, OUT_DEVICE or OUT_CLONE */
    char *output_device;
    int hold_ms;
    int loaded_ok;
    char error[256];
};

struct device {
    int fd;                    /* grabbed source device, -1 when slot unused */
    int out_fd;                /* clone (or sibling) we write into */
    int out_is_clone;
    int out_has_rep;
    unsigned long out_keybits[NLONGS(MAX_KEY_CNT)]; /* keys the output device accepts */
    unsigned long out_down[NLONGS(MAX_KEY_CNT)];    /* keys currently pressed on the output */
    unsigned long eaten[NLONGS(MAX_KEY_CNT)];       /* presses used to wake the TV: drop their release too */
    int64_t out_retry_at;
    char path[64];
    char name[128];
    char phys[128];
    struct input_id id;
    int nkeys;
    /* pending hold detection */
    int pend_code;
    int64_t pend_at;
    struct mapping *pend_map;
    /* key currently resolved as "held" */
    int held_code;
    struct mapping *held_map;
    /* output frame buffer */
    struct raw_event out[256];
    int nout;
    int out_real;
};

struct capture {
    int64_t until_ms;          /* wall clock ms, 0 = inactive */
    int swallow;
    int pass[16];              /* keys forwarded even while swallowing (e.g. Back) */
    int npass;
};

static struct device *device_by_path(const char *path);
static int count_remote_keys(int fd, int *kernel_keylen);
static int name_matches(const char *name);

static struct config g_cfg;
static struct device g_devs[MAX_DEVS];
static struct capture g_capture;
static char *g_config_path = DEFAULT_CONFIG;
static char g_config_dir[512];
static char *g_state_dir = DEFAULT_STATE_DIR;
static int g_kernel_key_cnt = MAX_KEY_CNT;   /* refined from EVIOCGBIT */
static int g_verbose = 0;
static int g_dry_run = 0;                    /* observe only, never grab */
static FILE *g_events = NULL;
static long g_events_bytes = 0;
static int64_t g_started_ms = 0;
static volatile sig_atomic_t g_quit = 0;

/* ------------------------------------------------------------------ util */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logf_(const char *level, const char *fmt, ...) {
    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "%s [%s] ", ts, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
#define LOG(...) logf_("info", __VA_ARGS__)
#define WARN(...) logf_("warn", __VA_ARGS__)
#define ERR(...) logf_("error", __VA_ARGS__)
#define DBG(...) do { if (g_verbose) logf_("debug", __VA_ARGS__); } while (0)

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    char *d = strdup(s);
    if (!d) { ERR("out of memory"); exit(1); }
    return d;
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_file_atomic(const char *path, const char *data) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int rc = write_all(fd, data, strlen(data));
    close(fd);
    if (rc < 0 || rename(tmp, path) < 0) { unlink(tmp); return -1; }
    return 0;
}

static char *read_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
        }
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if (n < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NULL; }
        if (n == 0) break;
        len += (size_t)n;
    }
    buf[len] = 0;
    close(fd);
    return buf;
}

/* ---------------------------------------------------------------- config */

static void free_action(struct action *a) {
    free(a->command); free(a->app); free(a->params);
    memset(a, 0, sizeof *a);
}

static void free_config_contents(struct config *c) {
    for (int i = 0; i < c->nmaps; i++) {
        free(c->list[i].label);
        free_action(&c->list[i].press);
        free_action(&c->list[i].hold);
    }
    free(c->list);
    for (int i = 0; i < c->ninclude; i++) free(c->include[i]);
    for (int i = 0; i < c->nexclude; i++) free(c->exclude[i]);
    free(c->include); free(c->exclude); free(c->output_device);
    memset(c, 0, sizeof *c);
}

static int parse_action(cJSON *j, struct action *a, char *err, size_t errlen) {
    memset(a, 0, sizeof *a);
    if (!cJSON_IsObject(j)) { snprintf(err, errlen, "action must be an object"); return -1; }
    cJSON *type = cJSON_GetObjectItemCaseSensitive(j, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "pass";
    if (!strcmp(t, "pass")) a->type = ACT_PASS;
    else if (!strcmp(t, "disable") || !strcmp(t, "ignore")) a->type = ACT_DISABLE;
    else if (!strcmp(t, "replace")) {
        a->type = ACT_REPLACE;
        cJSON *to = cJSON_GetObjectItemCaseSensitive(j, "to");
        if (!cJSON_IsNumber(to) || to->valueint <= 0 || to->valueint >= MAX_KEY_CNT) {
            snprintf(err, errlen, "replace action needs a valid 'to' key code"); return -1;
        }
        a->to = to->valueint;
    } else if (!strcmp(t, "exec")) {
        a->type = ACT_EXEC;
        cJSON *cmd = cJSON_GetObjectItemCaseSensitive(j, "command");
        if (!cJSON_IsString(cmd)) { snprintf(err, errlen, "exec action needs 'command'"); return -1; }
        a->command = xstrdup(cmd->valuestring);
        a->on_release = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "onRelease"));
    } else if (!strcmp(t, "launch")) {
        a->type = ACT_LAUNCH;
        cJSON *app = cJSON_GetObjectItemCaseSensitive(j, "app");
        if (!cJSON_IsString(app)) { snprintf(err, errlen, "launch action needs 'app'"); return -1; }
        a->app = xstrdup(app->valuestring);
        cJSON *params = cJSON_GetObjectItemCaseSensitive(j, "params");
        if (cJSON_IsObject(params)) {
            char *s = cJSON_PrintUnformatted(params);
            a->params = s; /* cJSON uses malloc; freed with free() */
        }
    } else {
        snprintf(err, errlen, "unknown action type '%s'", t); return -1;
    }
    return 0;
}

static void parse_patterns(cJSON *arr, char ***out, int *n) {
    *out = NULL; *n = 0;
    if (!cJSON_IsArray(arr)) return;
    int cnt = cJSON_GetArraySize(arr);
    *out = calloc((size_t)cnt, sizeof(char *));
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it)) (*out)[(*n)++] = xstrdup(it->valuestring);
    }
}

static void reset_key_states(void);

/* Keys a mapping sends in place of its own ("acts as", on press or hold). */
static int replace_targets(const struct mapping *m, int out[2]) {
    int n = 0;
    if (!m || !m->enabled) return 0;
    if (m->press.type == ACT_REPLACE) out[n++] = m->press.to;
    if (m->has_hold && m->hold.type == ACT_REPLACE) out[n++] = m->hold.to;
    return n;
}

/* Marks the mappings whose "acts as" chain leads back to their own key
 * (Netflix acts as LG Channels, LG Channels acts as Netflix). They stay in
 * the config, shown as a loop by the app, and the buttons do nothing until the
 * loop is broken. Returns how many were marked. */
static int mark_loops(struct config *c) {
    static unsigned long seen[NLONGS(MAX_KEY_CNT)];
    int stack[MAX_KEY_CNT];
    int loops = 0;
    for (int i = 0; i < c->nmaps; i++) {
        struct mapping *start = &c->list[i];
        start->looped = 0;
        memset(seen, 0, sizeof seen);
        int sp = replace_targets(start, stack);
        while (sp > 0 && !start->looped) {
            int k = stack[--sp];
            if (k == start->key) { start->looped = 1; break; }
            if (k <= 0 || k >= MAX_KEY_CNT || TEST_BIT(k, seen)) continue;
            seen[k / BITS_PER_LONG] |= 1UL << (k % BITS_PER_LONG);
            int t[2], n = replace_targets(c->by_key[k], t);
            for (int j = 0; j < n && sp < MAX_KEY_CNT; j++) stack[sp++] = t[j];
        }
        if (start->looped) loops++;
    }
    return loops;
}

static int load_config(void) {
    struct config nc;
    memset(&nc, 0, sizeof nc);
    nc.output_mode = OUT_AUTO;
    nc.output_device = xstrdup(DEFAULT_OUTPUT_DEVICE);
    nc.hold_ms = 500;

    char *text = read_file(g_config_path);
    if (!text) {
        if (errno == ENOENT) {
            /* No config yet: run with defaults (everything passes through). */
            nc.include = calloc(1, sizeof(char *));
            nc.include[nc.ninclude++] = xstrdup(DEFAULT_INCLUDE);
            nc.loaded_ok = 1;
            free_config_contents(&g_cfg);
            g_cfg = nc;
            LOG("config %s not found, using defaults", g_config_path);
            return 0;
        }
        snprintf(g_cfg.error, sizeof g_cfg.error, "cannot read config: %s", strerror(errno));
        g_cfg.loaded_ok = 0;
        ERR("%s", g_cfg.error);
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root || !cJSON_IsObject(root)) {
        const char *ep = cJSON_GetErrorPtr();
        snprintf(g_cfg.error, sizeof g_cfg.error, "config is not valid JSON%s%.40s",
                 ep ? " near: " : "", ep ? ep : "");
        g_cfg.loaded_ok = 0;
        ERR("%s (keeping previous mappings)", g_cfg.error);
        cJSON_Delete(root);
        return -1;
    }

    cJSON *hold = cJSON_GetObjectItemCaseSensitive(root, "holdMs");
    if (cJSON_IsNumber(hold) && hold->valueint >= 100 && hold->valueint <= 5000) nc.hold_ms = hold->valueint;

    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (cJSON_IsObject(devices)) {
        parse_patterns(cJSON_GetObjectItemCaseSensitive(devices, "include"), &nc.include, &nc.ninclude);
        parse_patterns(cJSON_GetObjectItemCaseSensitive(devices, "exclude"), &nc.exclude, &nc.nexclude);
    }
    if (nc.ninclude == 0) {
        free(nc.include);
        nc.include = calloc(1, sizeof(char *));
        nc.include[nc.ninclude++] = xstrdup(DEFAULT_INCLUDE);
    }

    cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
    if (cJSON_IsObject(output)) {
        cJSON *mode = cJSON_GetObjectItemCaseSensitive(output, "mode");
        cJSON *dev = cJSON_GetObjectItemCaseSensitive(output, "device");
        if (cJSON_IsString(dev) && dev->valuestring[0]) {
            free(nc.output_device);
            nc.output_device = xstrdup(dev->valuestring);
        }
        if (cJSON_IsString(mode)) {
            if (!strcmp(mode->valuestring, "clone")) nc.output_mode = OUT_CLONE;
            else if (!strcmp(mode->valuestring, "device")) nc.output_mode = OUT_DEVICE;
        }
    }

    char err[160] = "";
    cJSON *maps = cJSON_GetObjectItemCaseSensitive(root, "mappings");
    if (cJSON_IsArray(maps)) {
        int cnt = cJSON_GetArraySize(maps);
        nc.list = calloc((size_t)cnt, sizeof(struct mapping));
        cJSON *it;
        int idx = 0;
        cJSON_ArrayForEach(it, maps) {
            idx++;
            if (!cJSON_IsObject(it)) continue;
            struct mapping m;
            memset(&m, 0, sizeof m);
            cJSON *key = cJSON_GetObjectItemCaseSensitive(it, "key");
            if (!cJSON_IsNumber(key) || key->valueint <= 0 || key->valueint >= MAX_KEY_CNT) {
                snprintf(err, sizeof err, "mapping #%d has an invalid 'key'", idx);
                break;
            }
            m.key = key->valueint;
            cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "enabled");
            m.enabled = en ? cJSON_IsTrue(en) : 1;
            m.standby = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(it, "standby"));
            cJSON *label = cJSON_GetObjectItemCaseSensitive(it, "label");
            if (cJSON_IsString(label)) m.label = xstrdup(label->valuestring);
            cJSON *act = cJSON_GetObjectItemCaseSensitive(it, "action");
            if (act) {
                if (parse_action(act, &m.press, err, sizeof err) < 0) {
                    free(m.label);
                    break;
                }
            }
            cJSON *hm = cJSON_GetObjectItemCaseSensitive(it, "hold");
            if (cJSON_IsObject(hm)) {
                if (parse_action(hm, &m.hold, err, sizeof err) < 0) {
                    free(m.label); free_action(&m.press);
                    break;
                }
                m.has_hold = 1;
                cJSON *hms = cJSON_GetObjectItemCaseSensitive(it, "holdMs");
                m.hold_ms = (cJSON_IsNumber(hms) && hms->valueint >= 100 && hms->valueint <= 5000)
                            ? hms->valueint : nc.hold_ms;
            }
            if (nc.by_key[m.key]) {
                WARN("duplicate mapping for key %d, later one wins", m.key);
            }
            nc.list[nc.nmaps] = m;
            nc.by_key[m.key] = &nc.list[nc.nmaps];
            nc.nmaps++;
        }
    }
    cJSON_Delete(root);

    if (err[0]) {
        snprintf(g_cfg.error, sizeof g_cfg.error, "%s", err);
        g_cfg.loaded_ok = 0;
        ERR("config error: %s (keeping previous mappings)", err);
        free_config_contents(&nc);
        return -1;
    }

    /* Pending/held keys reference mappings that are about to be freed: settle
     * them first (release anything we pressed on the output). */
    reset_key_states();

    /* by_key holds pointers into nc.list; rebuild after the struct copy. */
    free_config_contents(&g_cfg);
    g_cfg = nc;
    memset(g_cfg.by_key, 0, sizeof g_cfg.by_key);
    for (int i = 0; i < g_cfg.nmaps; i++) g_cfg.by_key[g_cfg.list[i].key] = &g_cfg.list[i];
    if (mark_loops(&g_cfg)) {
        for (int i = 0; i < g_cfg.nmaps; i++)
            if (g_cfg.list[i].looped) WARN("key %d is part of a loop of \"acts as\" mappings: it does nothing until the loop is broken", g_cfg.list[i].key);
    }
    g_cfg.loaded_ok = 1;
    g_cfg.error[0] = 0;
    LOG("config loaded: %d mapping(s), output=%s%s, holdMs=%d", g_cfg.nmaps,
        g_cfg.output_mode == OUT_CLONE ? "clone" : g_cfg.output_device,
        g_cfg.output_mode == OUT_AUTO ? " (auto, clone fallback)" : "", g_cfg.hold_ms);
    return 0;
}

static void load_capture(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/capture.json", g_state_dir);
    char *text = read_file(path);
    g_capture.until_ms = 0;
    g_capture.swallow = 1;
    if (!text) return;
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return;
    cJSON *until = cJSON_GetObjectItemCaseSensitive(root, "until");
    if (cJSON_IsNumber(until)) g_capture.until_ms = (int64_t)until->valuedouble;
    cJSON *sw = cJSON_GetObjectItemCaseSensitive(root, "swallow");
    if (sw) g_capture.swallow = cJSON_IsTrue(sw);
    g_capture.npass = 0;
    cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "passthrough");
    if (cJSON_IsArray(pass)) {
        cJSON *it;
        cJSON_ArrayForEach(it, pass) {
            if (cJSON_IsNumber(it) && g_capture.npass < 16) g_capture.pass[g_capture.npass++] = it->valueint;
        }
    }
    cJSON_Delete(root);
    if (g_capture.until_ms > now_ms())
        LOG("capture mode active for %lld ms (swallow=%d)", (long long)(g_capture.until_ms - now_ms()), g_capture.swallow);
}

static int capture_active(void) {
    return g_capture.until_ms > 0 && now_ms() < g_capture.until_ms;
}

static void open_events_log(void);
static void close_events_log(void);

static void apply_capture(void) {
    load_capture();
    if (capture_active()) open_events_log();
    else close_events_log();
}

/* ---------------------------------------------------------------- events log */

/* Key presses are only recorded while capture mode is on: the app's key picker
 * and key monitor are the only readers. The file is removed as soon as capture
 * ends, so no history of button presses is kept. */
static void close_events_log(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/events.jsonl", g_state_dir);
    if (g_events) { fclose(g_events); g_events = NULL; }
    unlink(path);
}

static void open_events_log(void) {
    if (g_events) return;
    char path[512];
    snprintf(path, sizeof path, "%s/events.jsonl", g_state_dir);
    unlink(path);
    /* close-on-exec keeps it away from exec'd commands */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { WARN("cannot open %s: %s", path, strerror(errno)); return; }
    g_events = fdopen(fd, "w");
    if (!g_events) { WARN("cannot open %s: %s", path, strerror(errno)); close(fd); unlink(path); return; }
    g_events_bytes = 0;
}

static void rotate_events_log_if_needed(void) {
    if (g_events_bytes < 256 * 1024) return;
    /* A monitor left open for hours: start over (the service notices the new file). */
    close_events_log();
    open_events_log();
}

static void json_escape(const char *s, char *out, size_t outlen) {
    size_t o = 0;
    for (; *s && o + 6 < outlen; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { o += (size_t)snprintf(out + o, outlen - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static void log_event(struct device *d, const struct raw_event *e, const char *act, int to) {
    if (!g_events) return;
    char name[256];
    json_escape(d->name, name, sizeof name);
    int n = fprintf(g_events, "{\"t\":%lld,\"dev\":\"%s\",\"path\":\"%s\",\"type\":%u,\"code\":%u,\"value\":%d,\"act\":\"%s\"",
                    (long long)now_ms(), name, d->path, e->type, e->code, e->value, act);
    if (to > 0) n += fprintf(g_events, ",\"to\":%d", to);
    n += fprintf(g_events, "}\n");
    fflush(g_events);
    g_events_bytes += n;
    rotate_events_log_if_needed();
}

/* ---------------------------------------------------------------- status */

static void standby_status(cJSON *root);

static void write_status(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", LGINPUTMAPPERD_VERSION);
    cJSON_AddNumberToObject(root, "pid", getpid());
    cJSON_AddNumberToObject(root, "started", (double)g_started_ms);
    cJSON_AddNumberToObject(root, "updated", (double)now_ms());
    cJSON_AddNumberToObject(root, "kernelKeyCount", g_kernel_key_cnt);
    cJSON_AddBoolToObject(root, "dryRun", g_dry_run);

    cJSON *cfg = cJSON_AddObjectToObject(root, "config");
    cJSON_AddStringToObject(cfg, "path", g_config_path);
    cJSON_AddBoolToObject(cfg, "ok", g_cfg.loaded_ok);
    cJSON_AddStringToObject(cfg, "error", g_cfg.error);
    cJSON_AddNumberToObject(cfg, "mappings", g_cfg.nmaps);
    cJSON_AddStringToObject(cfg, "output", g_cfg.output_mode == OUT_CLONE ? "clone" : g_cfg.output_device);
    cJSON *loops = cJSON_AddArrayToObject(cfg, "loops");
    for (int i = 0; i < g_cfg.nmaps; i++)
        if (g_cfg.list[i].looped) cJSON_AddItemToArray(loops, cJSON_CreateNumber(g_cfg.list[i].key));

    cJSON *cap = cJSON_AddObjectToObject(root, "capture");
    cJSON_AddBoolToObject(cap, "active", capture_active());
    cJSON_AddNumberToObject(cap, "until", (double)g_capture.until_ms);

    standby_status(root);

    cJSON *devs = cJSON_AddArrayToObject(root, "devices");
    for (int i = 0; i < MAX_DEVS; i++) {
        struct device *d = &g_devs[i];
        if (d->fd < 0) continue;
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "path", d->path);
        cJSON_AddStringToObject(j, "name", d->name);
        cJSON_AddStringToObject(j, "phys", d->phys);
        cJSON_AddNumberToObject(j, "bus", d->id.bustype);
        cJSON_AddNumberToObject(j, "vendor", d->id.vendor);
        cJSON_AddNumberToObject(j, "product", d->id.product);
        cJSON_AddNumberToObject(j, "keys", d->nkeys);
        cJSON_AddBoolToObject(j, "grabbed", !g_dry_run);
        cJSON_AddStringToObject(j, "output", d->out_fd < 0 ? "none" : d->out_is_clone ? "clone" : g_cfg.output_device);
        cJSON_AddItemToArray(devs, j);
    }

    /* Every input device on the system, with what we did about it. */
    cJSON *all = cJSON_AddArrayToObject(root, "inputDevices");
    DIR *dir = opendir(DEV_INPUT);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir))) {
            if (strncmp(de->d_name, "event", 5)) continue;
            char path[64], name[128] = "", phys[128] = "";
            snprintf(path, sizeof path, DEV_INPUT "/%s", de->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
            ioctl(fd, EVIOCGPHYS(sizeof phys - 1), phys);
            int nkeys = count_remote_keys(fd, NULL);
            close(fd);
            const char *state;
            if (!strncmp(phys, CLONE_PHYS_PREFIX, strlen(CLONE_PHYS_PREFIX))) state = "clone";
            else if (device_by_path(path)) state = g_dry_run ? "observing" : "grabbed";
            else if (g_cfg.output_mode != OUT_CLONE && g_cfg.output_device && !strcmp(name, g_cfg.output_device)) state = "output";
            else if (nkeys == 0) state = "no-keys";
            else if (!name_matches(name)) state = "filtered";
            else state = "unsupported";
            cJSON *j = cJSON_CreateObject();
            cJSON_AddStringToObject(j, "path", path);
            cJSON_AddStringToObject(j, "name", name);
            cJSON_AddNumberToObject(j, "keys", nkeys);
            cJSON_AddStringToObject(j, "state", state);
            cJSON_AddItemToArray(all, j);
        }
        closedir(dir);
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return;
    char path[512];
    snprintf(path, sizeof path, "%s/status.json", g_state_dir);
    if (write_file_atomic(path, text) < 0) WARN("cannot write %s: %s", path, strerror(errno));
    free(text);
}

/* ---------------------------------------------------------------- commands */

static void run_detached(char *const argv[], int key, int value) {
    pid_t pid = fork();
    if (pid < 0) { ERR("fork failed: %s", strerror(errno)); return; }
    if (pid == 0) {
        setsid();
        char k[32], v[32];
        snprintf(k, sizeof k, "%d", key);
        snprintf(v, sizeof v, "%d", value);
        setenv("LGINPUTMAPPER_KEY", k, 1);
        setenv("LGINPUTMAPPER_VALUE", v, 1);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); if (devnull > 2) close(devnull); }
        /* stdout/stderr stay on our log so command output is visible */
        signal(SIGCHLD, SIG_DFL);
        execvp(argv[0], argv);
        fprintf(stderr, "lginputmapperd: exec %s failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
}

static void run_exec(const char *command, int key, int value) {
    LOG("key %d value %d: exec: %s", key, value, command);
    char *argv[] = { "/bin/sh", "-c", (char *)command, NULL };
    run_detached(argv, key, value);
}

static void run_launch(const char *app, const char *params, int key) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", app);
    if (params) {
        cJSON *p = cJSON_Parse(params);
        if (p) cJSON_AddItemToObject(j, "params", p);
    }
    char *payload = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!payload) return;
    LOG("key %d: launch app %s", key, app);
    char *argv[] = { "luna-send", "-n", "1", "luna://com.webos.applicationManager/launch", payload, NULL };
    run_detached(argv, key, 1);
    free(payload);
}

/* ---------------------------------------------------------------- uinput */

static int ioctl_bits(int fd, int type, unsigned long *bits, size_t nbytes) {
    memset(bits, 0, nbytes);
    int n = ioctl(fd, EVIOCGBIT(type, (int)nbytes), bits);
    return n; /* bytes the kernel filled, -1 on error */
}

static int create_clone(struct device *d) {
    unsigned long evbits[NLONGS(MAX_EV_CNT)];
    unsigned long keybits[NLONGS(MAX_KEY_CNT)];
    unsigned long relbits[NLONGS(MAX_REL_CNT)];
    unsigned long absbits[NLONGS(MAX_ABS_CNT)];
    unsigned long mscbits[NLONGS(MAX_MSC_CNT)];
    unsigned long ledbits[NLONGS(MAX_LED_CNT)];
    unsigned long sndbits[NLONGS(MAX_SND_CNT)];
    unsigned long swbits[NLONGS(MAX_SW_CNT)];
    unsigned long propbits[NLONGS(MAX_PROP_CNT)];

    if (ioctl_bits(d->fd, 0, evbits, sizeof evbits) < 0) { ERR("%s: EVIOCGBIT(0): %s", d->path, strerror(errno)); return -1; }
    int keylen = ioctl_bits(d->fd, EV_KEY, keybits, sizeof keybits);
    if (keylen > 0) g_kernel_key_cnt = keylen * 8;
    ioctl_bits(d->fd, EV_REL, relbits, sizeof relbits);
    ioctl_bits(d->fd, EV_ABS, absbits, sizeof absbits);
    ioctl_bits(d->fd, EV_MSC, mscbits, sizeof mscbits);
    ioctl_bits(d->fd, EV_LED, ledbits, sizeof ledbits);
    ioctl_bits(d->fd, EV_SND, sndbits, sizeof sndbits);
    ioctl_bits(d->fd, EV_SW, swbits, sizeof swbits);
    memset(propbits, 0, sizeof propbits);
    ioctl(d->fd, EVIOCGPROP((int)sizeof propbits), propbits);

    int ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ufd < 0) ufd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ufd < 0) { ERR("cannot open /dev/uinput: %s", strerror(errno)); return -1; }

    for (int t = 0; t < MAX_EV_CNT; t++) {
        if (!TEST_BIT(t, evbits)) continue;
        /* EV_REP is deliberately not copied: the kernel would auto-repeat on the
         * clone as well and the source's repeats are already forwarded. */
        if (t == EV_REP || t == EV_FF || t == EV_PWR || t == EV_FF_STATUS) continue;
        if (ioctl(ufd, UI_SET_EVBIT, t) < 0) WARN("UI_SET_EVBIT %d: %s", t, strerror(errno));
    }
    if (TEST_BIT(EV_KEY, evbits)) {
        for (int c = 0; c < keylen * 8 && c < MAX_KEY_CNT; c++)
            if (TEST_BIT(c, keybits) && ioctl(ufd, UI_SET_KEYBIT, c) < 0)
                DBG("UI_SET_KEYBIT %d: %s", c, strerror(errno));
    }
    if (TEST_BIT(EV_REL, evbits))
        for (int c = 0; c < MAX_REL_CNT; c++) if (TEST_BIT(c, relbits)) ioctl(ufd, UI_SET_RELBIT, c);
    if (TEST_BIT(EV_MSC, evbits))
        for (int c = 0; c < MAX_MSC_CNT; c++) if (TEST_BIT(c, mscbits)) ioctl(ufd, UI_SET_MSCBIT, c);
    if (TEST_BIT(EV_LED, evbits))
        for (int c = 0; c < MAX_LED_CNT; c++) if (TEST_BIT(c, ledbits)) ioctl(ufd, UI_SET_LEDBIT, c);
    if (TEST_BIT(EV_SND, evbits))
        for (int c = 0; c < MAX_SND_CNT; c++) if (TEST_BIT(c, sndbits)) ioctl(ufd, UI_SET_SNDBIT, c);
    if (TEST_BIT(EV_SW, evbits))
        for (int c = 0; c < MAX_SW_CNT; c++) if (TEST_BIT(c, swbits)) ioctl(ufd, UI_SET_SWBIT, c);
    for (int c = 0; c < MAX_PROP_CNT; c++) if (TEST_BIT(c, propbits)) ioctl(ufd, UI_SET_PROPBIT, c);

    char phys[160];
    snprintf(phys, sizeof phys, CLONE_PHYS_PREFIX "%s", d->phys);
    ioctl(ufd, UI_SET_PHYS, phys);

    int has_abs = TEST_BIT(EV_ABS, evbits);
    int legacy = 0;

    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id = d->id;
    snprintf(us.name, sizeof us.name, "%s", d->name);
    if (ioctl(ufd, UI_DEV_SETUP, &us) < 0) {
        legacy = 1;
        DBG("UI_DEV_SETUP unsupported (%s), using legacy uinput_user_dev", strerror(errno));
    } else if (has_abs) {
        for (int c = 0; c < MAX_ABS_CNT && c < ABS_CNT; c++) {
            if (!TEST_BIT(c, absbits)) continue;
            struct uinput_abs_setup as;
            memset(&as, 0, sizeof as);
            as.code = (uint16_t)c;
            if (ioctl(d->fd, EVIOCGABS(c), &as.absinfo) < 0) continue;
            if (ioctl(ufd, UI_ABS_SETUP, &as) < 0) WARN("UI_ABS_SETUP %d: %s", c, strerror(errno));
        }
    }

    if (legacy) {
        struct uinput_user_dev ud;
        memset(&ud, 0, sizeof ud);
        ud.id = d->id;
        snprintf(ud.name, sizeof ud.name, "%s", d->name);
        if (has_abs) {
            for (int c = 0; c < ABS_CNT; c++) {
                if (!TEST_BIT(c, absbits)) continue;
                struct input_absinfo ai;
                if (ioctl(d->fd, EVIOCGABS(c), &ai) < 0) continue;
                ud.absmin[c] = ai.minimum; ud.absmax[c] = ai.maximum;
                ud.absfuzz[c] = ai.fuzz; ud.absflat[c] = ai.flat;
            }
        }
        if (write(ufd, &ud, sizeof ud) != (ssize_t)sizeof ud) {
            ERR("legacy uinput setup write failed: %s", strerror(errno));
            close(ufd);
            return -1;
        }
    }

    if (ioctl(ufd, UI_DEV_CREATE) < 0) {
        ERR("UI_DEV_CREATE for %s failed: %s", d->name, strerror(errno));
        close(ufd);
        return -1;
    }
    d->out_fd = ufd;
    d->out_is_clone = 1;
    d->out_has_rep = 0;
    return 0;
}

static int find_device_by_name(const char *name, char *path_out, size_t plen);

static int open_output(struct device *d) {
    if (g_cfg.output_mode == OUT_CLONE) return create_clone(d);
    char path[64];
    if (find_device_by_name(g_cfg.output_device, path, sizeof path) < 0) {
        if (g_cfg.output_mode == OUT_AUTO) {
            WARN("output device '%s' not found, falling back to a uinput clone", g_cfg.output_device);
            return create_clone(d);
        }
        ERR("output device '%s' not found", g_cfg.output_device);
        return -1;
    }
    int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { ERR("cannot open output device %s: %s", path, strerror(errno)); return -1; }
    unsigned long evbits[NLONGS(MAX_EV_CNT)];
    ioctl_bits(fd, 0, evbits, sizeof evbits);
    ioctl_bits(fd, EV_KEY, d->out_keybits, sizeof d->out_keybits);
    d->out_fd = fd;
    d->out_is_clone = 0;
    d->out_has_rep = TEST_BIT(EV_REP, evbits) ? 1 : 0;
    return 0;
}

static void close_output(struct device *d) {
    if (d->out_fd < 0) return;
    if (d->out_is_clone) ioctl(d->out_fd, UI_DEV_DESTROY);
    close(d->out_fd);
    d->out_fd = -1;
}

/* ---------------------------------------------------------------- devices */

/* Patterns match either literally (LG device names contain "[0]", which is a
 * bracket expression to fnmatch) or as a shell glob. */
static int pattern_matches(const char *pattern, const char *name) {
    return !strcmp(pattern, name) || !fnmatch(pattern, name, 0);
}

static int name_matches(const char *name) {
    int inc = 0;
    for (int i = 0; i < g_cfg.ninclude; i++) if (pattern_matches(g_cfg.include[i], name)) { inc = 1; break; }
    if (!inc) return 0;
    for (int i = 0; i < g_cfg.nexclude; i++) if (pattern_matches(g_cfg.exclude[i], name)) return 0;
    return 1;
}

static int count_remote_keys(int fd, int *kernel_keylen) {
    unsigned long keybits[NLONGS(MAX_KEY_CNT)];
    int keylen = ioctl_bits(fd, EV_KEY, keybits, sizeof keybits);
    if (keylen <= 0) return 0;
    if (kernel_keylen) *kernel_keylen = keylen;
    int n = 0;
    for (int c = 1; c < keylen * 8 && c < MAX_KEY_CNT; c++) {
        if (c >= BTN_MISC && c < KEY_OK) continue; /* mouse/joystick buttons only */
        if (TEST_BIT(c, keybits)) n++;
    }
    return n;
}

static int find_device_by_name(const char *name, char *path_out, size_t plen) {
    DIR *dir = opendir(DEV_INPUT);
    if (!dir) return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(dir))) {
        if (strncmp(de->d_name, "event", 5)) continue;
        char path[64], dname[128] = "", phys[128] = "";
        snprintf(path, sizeof path, DEV_INPUT "/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        ioctl(fd, EVIOCGNAME(sizeof dname - 1), dname);
        ioctl(fd, EVIOCGPHYS(sizeof phys - 1), phys);
        close(fd);
        if (!strcmp(dname, name) && strncmp(phys, CLONE_PHYS_PREFIX, strlen(CLONE_PHYS_PREFIX))) {
            snprintf(path_out, plen, "%s", path);
            found = 0;
            break;
        }
    }
    closedir(dir);
    return found;
}

static struct device *device_by_path(const char *path) {
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i].fd >= 0 && !strcmp(g_devs[i].path, path)) return &g_devs[i];
    return NULL;
}

static void close_device(struct device *d) {
    if (d->fd < 0) return;
    LOG("releasing %s (%s)", d->path, d->name);
    if (!g_dry_run) ioctl(d->fd, EVIOCGRAB, (void *)0);
    close(d->fd);
    close_output(d);
    memset(d, 0, sizeof *d);
    d->fd = -1;
    d->out_fd = -1;
}

/* Returns 1 if the device was taken over, 0 if skipped, -1 on error. */
static int try_add_device(const char *path, int quiet) {
    if (device_by_path(path)) return 0;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (!quiet) DBG("%s: open: %s", path, strerror(errno));
        return -1;
    }
    char name[128] = "", phys[128] = "";
    struct input_id id;
    memset(&id, 0, sizeof id);
    ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
    ioctl(fd, EVIOCGPHYS(sizeof phys - 1), phys);
    ioctl(fd, EVIOCGID, &id);

    if (!strncmp(phys, CLONE_PHYS_PREFIX, strlen(CLONE_PHYS_PREFIX))) { close(fd); return 0; }
    if (g_cfg.output_mode != OUT_CLONE && g_cfg.output_device && !strcmp(name, g_cfg.output_device)) { close(fd); return 0; }
    int keylen = 0;
    int nkeys = count_remote_keys(fd, &keylen);
    if (keylen > 0) g_kernel_key_cnt = keylen * 8;
    if (nkeys == 0) { DBG("%s (%s): no remote keys, skipping", path, name); close(fd); return 0; }
    if (!name_matches(name)) { DBG("%s (%s): filtered out by device patterns", path, name); close(fd); return 0; }

    struct device *d = NULL;
    for (int i = 0; i < MAX_DEVS; i++) if (g_devs[i].fd < 0) { d = &g_devs[i]; break; }
    if (!d) { WARN("too many devices, ignoring %s", path); close(fd); return -1; }
    memset(d, 0, sizeof *d);
    d->fd = fd;
    d->out_fd = -1;
    snprintf(d->path, sizeof d->path, "%s", path);
    snprintf(d->name, sizeof d->name, "%s", name);
    snprintf(d->phys, sizeof d->phys, "%s", phys);
    d->id = id;
    d->nkeys = nkeys;
    d->pend_code = -1;
    d->held_code = -1;

    if (g_dry_run) {
        LOG("observing %s: \"%s\" phys=\"%s\" bus=%u vendor=%04x product=%04x keys=%d",
            path, name, phys, id.bustype, id.vendor, id.product, nkeys);
        return 1;
    }
    if (open_output(d) < 0) { close(fd); d->fd = -1; return -1; }
    if (!d->out_is_clone) {
        /* Every key this device can send must be deliverable through the output
         * device, otherwise unmapped buttons would silently vanish. */
        unsigned long keybits[NLONGS(MAX_KEY_CNT)];
        int kl = ioctl_bits(fd, EV_KEY, keybits, sizeof keybits);
        int missing = 0;
        for (int c = 1; c < kl * 8 && c < MAX_KEY_CNT; c++)
            if (TEST_BIT(c, keybits) && !TEST_BIT(c, d->out_keybits)) missing++;
        if (missing) {
            /* Rescans happen on every /dev/input change; say this once per node. */
            static unsigned long warned_nodes[4];
            int idx = atoi(path + strlen(DEV_INPUT "/event"));
            int seen = idx >= 0 && idx < 256 && TEST_BIT(idx, warned_nodes);
            if (!seen) {
                if (idx >= 0 && idx < 256) warned_nodes[idx / BITS_PER_LONG] |= 1UL << (idx % BITS_PER_LONG);
                WARN("%s (%s): %d of its keys cannot be delivered through '%s', leaving it alone",
                     path, name, missing, g_cfg.output_device);
            }
            close_output(d);
            close(fd);
            d->fd = -1;
            return 0;
        }
    }
    if (ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
        ERR("%s (%s): EVIOCGRAB failed: %s", path, name, strerror(errno));
        close_device(d);
        return -1;
    }
    LOG("took over %s: \"%s\" phys=\"%s\" bus=%u vendor=%04x product=%04x keys=%d -> %s",
        path, name, phys, id.bustype, id.vendor, id.product, nkeys,
        d->out_is_clone ? "uinput clone" : g_cfg.output_device);
    return 1;
}

static void scan_devices(void) {
    DIR *dir = opendir(DEV_INPUT);
    if (!dir) { ERR("cannot open " DEV_INPUT ": %s", strerror(errno)); return; }
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (strncmp(de->d_name, "event", 5)) continue;
        char path[64];
        snprintf(path, sizeof path, DEV_INPUT "/%s", de->d_name);
        try_add_device(path, 1);
    }
    closedir(dir);
}

static void list_devices(void) {
    DIR *dir = opendir(DEV_INPUT);
    if (!dir) { fprintf(stderr, "cannot open " DEV_INPUT ": %s\n", strerror(errno)); return; }
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (strncmp(de->d_name, "event", 5)) continue;
        char path[64], name[128] = "", phys[128] = "", uniq[64] = "";
        snprintf(path, sizeof path, DEV_INPUT "/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { printf("%-20s (cannot open: %s)\n", path, strerror(errno)); continue; }
        struct input_id id; memset(&id, 0, sizeof id);
        ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        ioctl(fd, EVIOCGPHYS(sizeof phys - 1), phys);
        ioctl(fd, EVIOCGUNIQ(sizeof uniq - 1), uniq);
        ioctl(fd, EVIOCGID, &id);
        unsigned long evbits[NLONGS(MAX_EV_CNT)];
        ioctl_bits(fd, 0, evbits, sizeof evbits);
        int keylen = 0;
        int nkeys = count_remote_keys(fd, &keylen);
        printf("%s\n  name=\"%s\" phys=\"%s\" uniq=\"%s\" bus=%u vendor=%04x product=%04x version=%04x\n  ev=",
               path, name, phys, uniq, id.bustype, id.vendor, id.product, id.version);
        for (int t = 0; t < MAX_EV_CNT; t++) if (TEST_BIT(t, evbits)) printf("%d ", t);
        printf(" remoteKeys=%d keyBitmapBytes=%d", nkeys, keylen);
        if (nkeys) {
            unsigned long keybits[NLONGS(MAX_KEY_CNT)];
            ioctl_bits(fd, EV_KEY, keybits, sizeof keybits);
            printf("\n  keys=");
            for (int c = 0; c < keylen * 8 && c < MAX_KEY_CNT; c++) if (TEST_BIT(c, keybits)) printf("%d ", c);
        }
        printf("\n");
        close(fd);
    }
    closedir(dir);
}

/* ---------------------------------------------------------------- output */

static void out_push(struct device *d, uint16_t type, uint16_t code, int32_t value) {
    if (d->nout >= (int)(sizeof d->out / sizeof d->out[0]) - 1) {
        WARN("%s: output frame overflow, dropping events", d->path);
        return;
    }
    if (type == EV_KEY && !d->out_is_clone && d->out_fd >= 0 && code < MAX_KEY_CNT && !TEST_BIT(code, d->out_keybits)) {
        static unsigned long warned[NLONGS(MAX_KEY_CNT)];
        if (!TEST_BIT(code, warned)) {
            warned[code / BITS_PER_LONG] |= 1UL << (code % BITS_PER_LONG);
            WARN("output device '%s' does not support key %u; the kernel will drop it", g_cfg.output_device, code);
        }
    }
    struct raw_event *e = &d->out[d->nout++];
    e->sec = 0; e->usec = 0;
    e->type = type; e->code = code; e->value = value;
    if (type != EV_SYN && type != EV_MSC) d->out_real = 1;
    if (type == EV_KEY && code < MAX_KEY_CNT) {
        if (value == 1) d->out_down[code / BITS_PER_LONG] |= 1UL << (code % BITS_PER_LONG);
        else if (value == 0) d->out_down[code / BITS_PER_LONG] &= ~(1UL << (code % BITS_PER_LONG));
    }
}

static void out_flush(struct device *d) {
    if (d->out_fd < 0 && d->out_retry_at <= mono_ms()) {
        /* the sibling device went away (e.g. lginput2 restarted): try again */
        d->out_retry_at = mono_ms() + 2000;
        if (open_output(d) == 0) { LOG("%s: output reopened", d->path); write_status(); }
    }
    if (d->out_fd >= 0 && d->out_real && d->nout > 0) {
        out_push(d, EV_SYN, SYN_REPORT, 0);
        if (write_all(d->out_fd, d->out, (size_t)d->nout * sizeof d->out[0]) < 0) {
            WARN("%s: write to output failed: %s", d->path, strerror(errno));
            if (errno == ENODEV || errno == EBADF || errno == EIO) { close_output(d); d->out_retry_at = 0; write_status(); }
        }
    }
    d->nout = 0;
    d->out_real = 0;
}

static void out_key_now(struct device *d, int code, int value) {
    out_push(d, EV_KEY, (uint16_t)code, value);
    out_flush(d);
}

static void out_tap(struct device *d, int code) {
    out_key_now(d, code, 1);
    out_key_now(d, code, 0);
}

/* ---------------------------------------------------------------- key logic */

static void out_key_now(struct device *d, int code, int value);

static void reset_key_states(void) {
    for (int i = 0; i < MAX_DEVS; i++) {
        struct device *d = &g_devs[i];
        if (d->fd < 0) continue;
        if (d->held_map) {
            const struct action *a = &d->held_map->hold;
            if (a->type == ACT_PASS) out_key_now(d, d->held_code, 0);
            else if (a->type == ACT_REPLACE) out_key_now(d, a->to, 0);
        }
        d->pend_code = -1; d->pend_map = NULL;
        d->held_code = -1; d->held_map = NULL;
    }
}

static void perform_action(struct device *d, const struct action *a, int code, int value, const struct raw_event *e) {
    switch (a->type) {
        case ACT_PASS:
            log_event(d, e, "pass", 0);
            out_push(d, EV_KEY, (uint16_t)code, value);
            break;
        case ACT_DISABLE:
            log_event(d, e, "disable", 0);
            break;
        case ACT_REPLACE:
            log_event(d, e, "replace", a->to);
            if (value == 2 && !d->out_is_clone && d->out_has_rep) break;
            out_push(d, EV_KEY, (uint16_t)a->to, value);
            break;
        case ACT_EXEC:
            log_event(d, e, "exec", 0);
            if (value == 1 || (value == 0 && a->on_release)) run_exec(a->command, code, value);
            break;
        case ACT_LAUNCH:
            log_event(d, e, "launch", 0);
            if (value == 1) run_launch(a->app, a->params, code);
            break;
    }
}

/* A pending key was released before the hold threshold: play it as a tap. */
static void resolve_short_press(struct device *d, const struct raw_event *e) {
    struct mapping *m = d->pend_map;
    int code = d->pend_code;
    d->pend_code = -1;
    d->pend_map = NULL;
    if (!m) return;
    const struct action *a = &m->press;
    struct raw_event fake = *e;
    fake.code = (uint16_t)code;
    LOG("key %d: short press", code);
    switch (a->type) {
        case ACT_PASS:
            fake.value = 1; log_event(d, &fake, "pass", 0);
            fake.value = 0; log_event(d, &fake, "pass", 0);
            out_tap(d, code);
            break;
        case ACT_REPLACE:
            fake.value = 1; log_event(d, &fake, "replace", a->to);
            fake.value = 0; log_event(d, &fake, "replace", a->to);
            out_tap(d, a->to);
            break;
        case ACT_DISABLE:
            fake.value = 1; log_event(d, &fake, "disable", 0);
            break;
        case ACT_EXEC:
            fake.value = 1; log_event(d, &fake, "exec", 0);
            run_exec(a->command, code, 1);
            if (a->on_release) run_exec(a->command, code, 0);
            break;
        case ACT_LAUNCH:
            fake.value = 1; log_event(d, &fake, "launch", 0);
            run_launch(a->app, a->params, code);
            break;
    }
}

/* The pending key crossed the hold threshold: start the hold action. */
static void resolve_hold(struct device *d) {
    struct mapping *m = d->pend_map;
    int code = d->pend_code;
    d->pend_code = -1;
    d->pend_map = NULL;
    if (!m) return;
    LOG("key %d: hold", code);
    struct raw_event fake;
    memset(&fake, 0, sizeof fake);
    fake.type = EV_KEY; fake.code = (uint16_t)code; fake.value = 1;
    d->held_code = code;
    d->held_map = m;
    const struct action *a = &m->hold;
    switch (a->type) {
        case ACT_PASS:
            log_event(d, &fake, "hold:pass", 0);
            out_key_now(d, code, 1);
            break;
        case ACT_REPLACE:
            log_event(d, &fake, "hold:replace", a->to);
            out_key_now(d, a->to, 1);
            break;
        case ACT_DISABLE:
            log_event(d, &fake, "hold:disable", 0);
            break;
        case ACT_EXEC:
            log_event(d, &fake, "hold:exec", 0);
            run_exec(a->command, code, 1);
            break;
        case ACT_LAUNCH:
            log_event(d, &fake, "hold:launch", 0);
            run_launch(a->app, a->params, code);
            break;
    }
}

static int standby_wake_key_pressed(int code);

static void handle_key(struct device *d, const struct raw_event *e) {
    int code = e->code;
    int value = e->value;

    /* A press that went out must always be followed by its release, whatever
     * mode we are in now (capture may have started or the config changed in
     * between). Otherwise the kernel keeps the key "down" on the output device
     * and silently drops the next press of it. */
    if (value == 0 && code < MAX_KEY_CNT && TEST_BIT(code, d->out_down)) {
        log_event(d, e, "pass", 0);
        out_push(d, EV_KEY, (uint16_t)code, 0);
        if (d->held_code == code) { d->held_code = -1; d->held_map = NULL; }
        if (d->pend_code == code) { d->pend_code = -1; d->pend_map = NULL; }
        return;
    }

    if (capture_active()) {
        if (value == 0) {
            /* a key pressed before capture started is simply forgotten */
            if (d->pend_code == code) { d->pend_code = -1; d->pend_map = NULL; }
            if (d->held_code == code) { d->held_code = -1; d->held_map = NULL; }
        }
        int pass = !g_capture.swallow;
        for (int i = 0; i < g_capture.npass && !pass; i++) if (g_capture.pass[i] == code) pass = 1;
        log_event(d, e, pass ? "capture:pass" : "capture", 0);
        if (pass) out_push(d, EV_KEY, (uint16_t)code, value);
        return;
    }

    /* A hotkey pressed while the TV is in active standby (on, screen dark)
     * arrives here instead of at the micom: turn the TV on the way a wake from
     * standby would, and drop the rest of the press. */
    if (code < MAX_KEY_CNT && TEST_BIT(code, d->eaten)) {
        if (value == 0) d->eaten[code / BITS_PER_LONG] &= ~(1UL << (code % BITS_PER_LONG));
        log_event(d, e, "wake", 0);
        return;
    }
    if (value == 1 && code < MAX_KEY_CNT && standby_wake_key_pressed(code)) {
        d->eaten[code / BITS_PER_LONG] |= 1UL << (code % BITS_PER_LONG);
        log_event(d, e, "wake", 0);
        return;
    }

    /* Key currently held (hold action already fired). */
    if (d->held_code == code && d->held_map) {
        const struct action *a = &d->held_map->hold;
        if (value == 1) return; /* should not happen */
        if (a->type == ACT_PASS) { log_event(d, e, "hold:pass", 0); out_push(d, EV_KEY, (uint16_t)code, value); }
        else if (a->type == ACT_REPLACE) {
            log_event(d, e, "hold:replace", a->to);
            if (!(value == 2 && !d->out_is_clone && d->out_has_rep)) out_push(d, EV_KEY, (uint16_t)a->to, value);
        } else if (a->type == ACT_EXEC && value == 0 && a->on_release) {
            log_event(d, e, "hold:exec", 0);
            run_exec(a->command, code, 0);
        } else {
            log_event(d, e, value == 0 ? "hold:end" : "hold:repeat", 0);
        }
        if (value == 0) { d->held_code = -1; d->held_map = NULL; }
        return;
    }

    /* Key pending hold detection. */
    if (d->pend_code == code && d->pend_map) {
        int64_t elapsed = mono_ms() - d->pend_at;
        if (value == 0) {
            if (elapsed >= d->pend_map->hold_ms) {
                resolve_hold(d);
                /* and immediately release the held key */
                struct raw_event rel = *e;
                handle_key(d, &rel);
            } else {
                resolve_short_press(d, e);
            }
        } else if (value == 2 && elapsed >= d->pend_map->hold_ms) {
            resolve_hold(d);
        } else {
            log_event(d, e, "pending", 0);
        }
        return;
    }

    struct mapping *m = (code >= 0 && code < MAX_KEY_CNT) ? g_cfg.by_key[code] : NULL;
    if (!m || !m->enabled) {
        log_event(d, e, "pass", 0);
        out_push(d, EV_KEY, (uint16_t)code, value);
        return;
    }
    if (m->looped) {
        log_event(d, e, "loop", 0); /* a button in a loop does nothing */
        return;
    }

    if (m->has_hold) {
        if (value == 1) {
            if (d->pend_code >= 0) {
                /* another key arrived while one was pending: settle the old one as a tap */
                struct raw_event fake = *e;
                fake.code = (uint16_t)d->pend_code;
                fake.value = 0;
                resolve_short_press(d, &fake);
            }
            d->pend_code = code;
            d->pend_map = m;
            d->pend_at = mono_ms();
            log_event(d, e, "pending", 0);
            return;
        }
        /* release/repeat of a hold key that is not pending (e.g. pressed before a config reload) */
        log_event(d, e, "pass", 0);
        out_push(d, EV_KEY, (uint16_t)code, value);
        return;
    }

    perform_action(d, &m->press, code, value, e);
}

static void check_hold_timeouts(void) {
    int64_t now = mono_ms();
    for (int i = 0; i < MAX_DEVS; i++) {
        struct device *d = &g_devs[i];
        if (d->fd < 0 || d->pend_code < 0 || !d->pend_map) continue;
        if (now - d->pend_at >= d->pend_map->hold_ms) resolve_hold(d);
    }
}

static int next_timeout_ms(void) {
    int64_t now = mono_ms();
    int64_t best = -1;
    for (int i = 0; i < MAX_DEVS; i++) {
        struct device *d = &g_devs[i];
        if (d->fd < 0 || d->pend_code < 0 || !d->pend_map) continue;
        int64_t left = d->pend_map->hold_ms - (now - d->pend_at);
        if (left < 0) left = 0;
        if (best < 0 || left < best) best = left;
    }
    if (g_capture.until_ms > 0) {
        int64_t left = g_capture.until_ms - now_ms();
        if (left < 0) left = 0;
        if (best < 0 || left < best) best = left;
    }
    return best < 0 ? -1 : (int)(best + 1);
}

static void handle_device_input(struct device *d) {
    struct raw_event buf[64];
    for (;;) {
        ssize_t n = read(d->fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) break;
            WARN("%s: read failed (%s), dropping device", d->path, strerror(errno));
            close_device(d);
            write_status();
            return;
        }
        if (n == 0) break;
        int cnt = (int)(n / (ssize_t)sizeof buf[0]);
        for (int i = 0; i < cnt; i++) {
            struct raw_event *e = &buf[i];
            if (g_dry_run) {
                if (e->type == EV_KEY || e->type == EV_REL) log_event(d, e, "observe", 0);
                continue;
            }
            if (e->type == EV_SYN) {
                if (e->code == SYN_REPORT) out_flush(d);
                /* SYN_DROPPED etc. are not forwarded */
            } else if (e->type == EV_KEY) {
                handle_key(d, e);
            } else {
                out_push(d, e->type, e->code, e->value);
            }
        }
    }
}

/* ---------------------------------------------------------------- standby */

/* A hotkey pressed while the TV is in standby never reaches the input devices:
 * the micom wakes the TV with a power-on reason named after the key, and bootd
 * opens the app that LG's setting other.mapping_info lists for that reason
 * (the table is re-read from /var/luna/preferences/other on every wake).
 *
 * So the entries of remapped hotkeys are rewritten through the settings
 * service: a launch mapping opens its app straight away, anything else wakes
 * the TV like the power button (isActive false). Replace and exec actions then
 * run once the system has resumed with that power-on reason. LG's own values
 * are kept in standby.json and given back as soon as a mapping stops needing
 * the change. LG's server also resets the table after every reboot; the
 * rewrite is simply applied again. */

#ifndef LG_PREFS_DIR   /* overridable for tests off the TV */
#define LG_PREFS_DIR "/var/luna/preferences"
#endif
#define LG_PREFS_NAME "other"
#define LG_PREFS_FILE LG_PREFS_DIR "/" LG_PREFS_NAME
#define STANDBY_STATE_NAME "standby.json"
/* A ready-made request that gives LG's table back, for the boot script to
 * send if the app was uninstalled while the daemon could not do it itself. */
#define STANDBY_RESTORE_NAME "standby-restore.json"
/* The same for the micom's hotkey locks. */
#define STANDBY_RESTORE_MICOM_NAME "standby-restore-micom.json"
#ifndef INIT_SCRIPT
#define INIT_SCRIPT "/var/lib/webosbrew/init.d/lginputmapper"
#endif

/* Hotkeys the micom can wake the TV with: Linux key code (LG's libStarfishInput
 * key table), the power-on reason tvpowerd reports for it and the micom's lock
 * for it (lowlevelstorage db "micom"; 1 = the key does not wake the TV). */
static const struct { int key; const char *reason; const char *lock; } WAKE_KEYS[] = {
    { 1037, "netflix", "NetflixKeyLock" }, { 1038, "amazon", "AmazonKeyLock" },
    { 1039, "ivi", "IVIKeyLock" }, { 1041, "hotstar", "HotstarKeyLock" },
    { 1042, "disneyplus", "DisneyplusKeyLock" }, { 1043, "lgchannels", "LGchannelsKeyLock" },
    { 1044, "rakutentv", "RakutentvKeyLock" }, { 1045, "globoplay", "GloboplayKeyLock" },
    { 1047, "okko", "OkkoKeyLock" }, { 1088, "kinopoisk", "KinopoiskKeyLock" },
    { 1089, "watchaplay", "WatchaplayKeyLock" }, { 1090, "unext", "UnextKeyLock" },
    { 1091, "fptplay", "FptplayKeyLock" }, { 1092, "shahid", "ShahidKeyLock" },
    { 1095, "hulu", "HuluKeyLock" }, { 1096, "nhkplus", "NhkplusKeyLock" },
    { 1097, "tod", "TodKeyLock" }, { 1099, "freeviewplay", "FreeviewplayKeyLock" },
    { 1102, "sonyliv", "SonylivKeyLock" }, { 1107, "slingtv", "SlingtvKeyLock" },
    { 1108, "tver", "TverKeyLock" }, { 1109, "wavve", "WavveKeyLock" },
    { 1110, "coupangplay", "CoupangplayKeyLock" }, { 1111, "stan", "StanKeyLock" },
    { 1120, "tv360", "Tv360KeyLock" }, { 1121, "vtvgo", "VtvgoKeyLock" },
    { 1125, "vkvideo", "VkvideoKeyLock" }, { 1126, "premier", "PremierKeyLock" },
};
#define NWAKE ((int)(sizeof WAKE_KEYS / sizeof WAKE_KEYS[0]))
/* Also in the micom's lock list, without a key code we know of. */
static const char *OTHER_LOCK_REASONS[] = { "iplayer" };

/* What a wake by each hotkey does right now, for status.json. */
enum wake_mode { WM_ABSENT = 0, WM_LG, WM_INACTIVE, WM_LAUNCH, WM_NORMAL, WM_OFF };
static const char *WAKE_MODE_NAMES[] = { "absent", "lg", "inactive", "launch", "normal", "off" };

/* {"entries": {reason: {"lg": {...}, "ours": {...}, "lock": true}},
 *  "micom": {reason: isActive, ...}}  (the lock list we sent, while ours apply) */
static cJSON *g_sb_state;
static enum wake_mode g_wake_mode[NWAKE];
static char g_sb_error[200];
static int64_t g_sb_retry_at;              /* mono ms, 0 = none */
static int64_t g_sb_recheck_at;            /* mono ms: look at our key locks again, 0 = none */
static int64_t g_sb_window_start;          /* write rate guard */
static int g_sb_window_writes;
static int g_sb_paused;

static struct {
    int active;
    int stage;                             /* 0: wait until the TV is active, 1: run the action */
    int key;
    int64_t next;                          /* mono ms */
    int64_t until;
} g_wake;

static struct {
    char reason[32];
    int key;
    int64_t at;                            /* wall clock ms */
    const char *action;
} g_last_wake;

/* A hotkey the daemon used to turn the TV on from active standby, in case
 * tvpower reports a different reason for that wake. */
static int g_standby_press_key;
static int64_t g_standby_press_at;          /* mono ms */

static char g_install_dir[512];            /* our service directory, when running from an installed package */
static int g_uninstalled;                  /* remove our config and state dirs on the way out */
static int64_t g_install_missing_since;
static int64_t g_install_next_check;

static int64_t boot_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void config_dir_file(char *out, size_t len, const char *name) {
    snprintf(out, len, "%s/%s", g_config_dir, name);
}

/* Runs luna-send and returns what it printed (malloc'd), or NULL. Bounded by
 * luna-send's own exit timeout, and killed should it hang past that. */
static char *luna_call(const char *uri, const char *payload, int timeout_ms) {
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) < 0) { WARN("pipe: %s", strerror(errno)); return NULL; }
    char wait_arg[16];
    snprintf(wait_arg, sizeof wait_arg, "%d", timeout_ms);
    pid_t pid = fork();
    if (pid < 0) { WARN("fork failed: %s", strerror(errno)); close(pfd[0]); close(pfd[1]); return NULL; }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 2); }
        dup2(pfd[1], 1);
        signal(SIGCHLD, SIG_DFL);
        char *argv[] = { "luna-send", "-n", "1", "-w", wait_arg, (char *)uri, (char *)payload, NULL };
        execv("/usr/bin/luna-send", argv);
        execvp("luna-send", argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    int64_t deadline = mono_ms() + timeout_ms + 1000;
    while (buf) {
        int left = (int)(deadline - mono_ms());
        if (left <= 0) {
            kill(pid, SIGKILL);
            WARN("luna-send %s did not finish, killed it", uri);
            free(buf); buf = NULL;
            break;
        }
        struct pollfd p = { .fd = pfd[0], .events = POLLIN };
        int rc = poll(&p, 1, left);
        if (rc < 0 && errno != EINTR) { free(buf); buf = NULL; break; }
        if (rc <= 0) continue;
        if (len + 1024 > cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb; cap *= 2;
        }
        ssize_t n = read(pfd[0], buf + len, cap - len - 1);
        if (n < 0) { if (errno == EINTR) continue; free(buf); buf = NULL; break; }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(pfd[0]); /* SIGCHLD is ignored: the child reaps itself */
    if (buf) buf[len] = 0;
    return buf;
}

/* The response object when the call succeeded (returnValue true), else NULL. */
static cJSON *luna_json(const char *uri, const char *payload, int timeout_ms) {
    char *out = luna_call(uri, payload, timeout_ms);
    if (!out) return NULL;
    cJSON *r = cJSON_Parse(out);
    if (!r || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "returnValue"))) {
        WARN("%s failed: %.200s", uri, out);
        cJSON_Delete(r);
        r = NULL;
    }
    free(out);
    return r;
}

static void standby_load_state(void) {
    char path[600];
    config_dir_file(path, sizeof path, STANDBY_STATE_NAME);
    char *text = read_file(path);
    cJSON_Delete(g_sb_state);
    g_sb_state = text ? cJSON_Parse(text) : NULL;
    if (text && !cJSON_IsObject(g_sb_state)) WARN("%s is unreadable, LG's original hotkey entries are lost", path);
    free(text);
    if (!cJSON_IsObject(g_sb_state)) { cJSON_Delete(g_sb_state); g_sb_state = cJSON_CreateObject(); }
    if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(g_sb_state, "entries"))) {
        cJSON_DeleteItemFromObjectCaseSensitive(g_sb_state, "entries");
        cJSON_AddObjectToObject(g_sb_state, "entries");
    }
}

/* Saves standby.json and the restore request, or removes both once nothing of
 * LG's table is changed any more. */
static cJSON *micom_payload(cJSON *table, int ours);

static void standby_save_state(cJSON *table) {
    char path[600], restore[600], restore_micom[600];
    config_dir_file(path, sizeof path, STANDBY_STATE_NAME);
    config_dir_file(restore, sizeof restore, STANDBY_RESTORE_NAME);
    config_dir_file(restore_micom, sizeof restore_micom, STANDBY_RESTORE_MICOM_NAME);
    cJSON *entries = cJSON_GetObjectItemCaseSensitive(g_sb_state, "entries");
    int micom = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(g_sb_state, "micom"));
    if (micom && table) {
        cJSON *lg = micom_payload(table, 0);
        char *t = cJSON_PrintUnformatted(lg);
        cJSON_Delete(lg);
        if (!t || write_file_atomic(restore_micom, t) < 0) WARN("cannot write %s: %s", restore_micom, strerror(errno));
        free(t);
    } else if (!micom) {
        unlink(restore_micom);
    }
    if (!cJSON_GetArraySize(entries) && !micom) {
        unlink(path);
        unlink(restore);
        return;
    }
    char *text = cJSON_PrintUnformatted(g_sb_state);
    if (!text || write_file_atomic(path, text) < 0) WARN("cannot write %s: %s", path, strerror(errno));
    free(text);
    if (!table) return;
    /* the current table with LG's values in place of ours */
    cJSON *orig = cJSON_Duplicate(table, 1);
    cJSON *slot;
    cJSON_ArrayForEach(slot, orig) {
        cJSON *rec;
        cJSON_ArrayForEach(rec, entries) {
            cJSON *lg = cJSON_GetObjectItemCaseSensitive(rec, "lg");
            if (lg && cJSON_GetObjectItemCaseSensitive(slot, rec->string))
                cJSON_ReplaceItemInObjectCaseSensitive(slot, rec->string, cJSON_Duplicate(lg, 1));
        }
    }
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "category", "other");
    cJSON_AddItemToObject(cJSON_AddObjectToObject(req, "settings"), "mapping_info", orig);
    text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!text || write_file_atomic(restore, text) < 0) WARN("cannot write %s: %s", restore, strerror(errno));
    free(text);
}

/* The entry a mapping needs, built from LG's entry, or NULL if the mapping
 * leaves the wake alone. *lock is set when the key must not wake the TV. */
static cJSON *standby_want(const struct mapping *m, const cJSON *lg, int *lock) {
    *lock = 0;
    if (!m || !m->enabled) return NULL;
    /* LG marks hotkeys that are not serviced in this country inactive: those
     * already wake the TV like the power button, or do not wake it at all. */
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(lg, "isActive"))) return NULL;
    const struct action *a = &m->press;
    /* a button in a loop does nothing, like a disabled one, and so does one
     * whose "allow from standby" is off (in standby only) */
    enum act_type type = m->looped || !m->standby ? ACT_DISABLE : a->type;
    /* Doing nothing includes not turning the TV on: the micom gets told to
     * ignore the key. The entry still says "wake normally", which is what
     * happens should the lock ever be missing. */
    if (type == ACT_DISABLE) *lock = 1;
    cJSON *w;
    switch (type) {
        case ACT_LAUNCH: {
            w = cJSON_Duplicate(lg, 1);
            cJSON_DeleteItemFromObjectCaseSensitive(w, "app_id");
            cJSON_AddStringToObject(w, "app_id", a->app);
            cJSON_DeleteItemFromObjectCaseSensitive(w, "launch_param");
            cJSON *p = a->params ? cJSON_Parse(a->params) : NULL;
            cJSON_AddItemToObject(w, "launch_param", p ? p : cJSON_CreateNull());
            return w;
        }
        case ACT_DISABLE:
        case ACT_REPLACE:
        case ACT_EXEC:
            w = cJSON_Duplicate(lg, 1);
            cJSON_DeleteItemFromObjectCaseSensitive(w, "isActive");
            cJSON_AddBoolToObject(w, "isActive", 0);
            return w;
        default:
            return NULL;
    }
}

static cJSON *table_slot(cJSON *table, const char *reason) {
    cJSON *slot;
    cJSON_ArrayForEach(slot, table)
        if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(slot, reason))) return slot;
    return NULL;
}

/* What each hotkey does on a wake, read from the table as it is. */
static void standby_update_modes(cJSON *table) {
    cJSON *entries = cJSON_GetObjectItemCaseSensitive(g_sb_state, "entries");
    for (int i = 0; i < NWAKE; i++) {
        const char *reason = WAKE_KEYS[i].reason;
        cJSON *slot = table_slot(table, reason);
        cJSON *cur = slot ? cJSON_GetObjectItemCaseSensitive(slot, reason) : NULL;
        cJSON *ours = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(entries, reason), "ours");
        int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cur, "isActive"));
        if (!cur) g_wake_mode[i] = WM_ABSENT;
        else if (ours && cJSON_Compare(cur, ours, 1))
            g_wake_mode[i] = active ? WM_LAUNCH
                : cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(entries, reason), "lock")) &&
                  cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(g_sb_state, "micom"), reason))
                ? WM_OFF : WM_NORMAL;
        else g_wake_mode[i] = active ? WM_LG : WM_INACTIVE;
    }
}

/* After a failed write: forget what was not written and report the table as it is. */
static void standby_reload(void) {
    standby_load_state();
    char *text = read_file(LG_PREFS_FILE);
    cJSON *prefs = text ? cJSON_Parse(text) : NULL;
    free(text);
    standby_update_modes(cJSON_GetObjectItemCaseSensitive(prefs, "mapping_info"));
    cJSON_Delete(prefs);
}

/* The micom's hotkey locks as LG's own code sends them: every key it knows,
 * true = may wake the TV. setCPHotKeyListLock unlocks any key left out, so the
 * list is always complete. LG's value comes from its table (from our record of
 * LG's entry where the table holds ours); with ours set, our locks apply. */
static cJSON *micom_payload(cJSON *table, int ours) {
    cJSON *entries = cJSON_GetObjectItemCaseSensitive(g_sb_state, "entries");
    cJSON *p = cJSON_CreateObject();
    int nother = (int)(sizeof OTHER_LOCK_REASONS / sizeof OTHER_LOCK_REASONS[0]);
    for (int i = 0; i < NWAKE + nother; i++) {
        const char *reason = i < NWAKE ? WAKE_KEYS[i].reason : OTHER_LOCK_REASONS[i - NWAKE];
        cJSON *slot = table_slot(table, reason);
        cJSON *rec = cJSON_GetObjectItemCaseSensitive(entries, reason);
        cJSON *lg = rec ? cJSON_GetObjectItemCaseSensitive(rec, "lg") : slot ? cJSON_GetObjectItemCaseSensitive(slot, reason) : NULL;
        int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(lg, "isActive"));
        if (ours && rec && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec, "lock"))) active = 0;
        cJSON_AddBoolToObject(p, reason, active);
    }
    return p;
}

/* 1 locked, 0 not, -1 unknown. (One item per request: a batch fails as a
 * whole if the TV lacks any of the items.) */
static int micom_lock_value(const char *item) {
    char payload[160];
    snprintf(payload, sizeof payload, "{\"dbgroups\":[{\"items\":[\"%s\"],\"dbid\":\"micom\"}]}", item);
    cJSON *r = luna_json("luna://com.webos.service.lowlevelstorage/getData", payload, 2000);
    cJSON *groups = cJSON_GetObjectItemCaseSensitive(r, "dbgroups");
    cJSON *v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(groups, 0), "items"), item);
    int val = cJSON_IsNumber(v) ? (v->valueint != 0) : -1;
    cJSON_Delete(r);
    return val;
}

/* Locks the keys of "do nothing" mappings in the micom so they do not wake the
 * TV, and gives the locks back to LG once no mapping needs them. LG unlocks
 * them after a reboot (with the same server sync that resets the table), so
 * our locks are checked whenever the table changes. */
static int standby_micom_sync(cJSON *table, int *state_changed) {
    cJSON *entries = cJSON_GetObjectItemCaseSensitive(g_sb_state, "entries");
    cJSON *sent = cJSON_GetObjectItemCaseSensitive(g_sb_state, "micom");
    int want = 0;
    cJSON *rec;
    cJSON_ArrayForEach(rec, entries) if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec, "lock"))) want = 1;
    if (!want && !sent) return 0; /* the micom is LG's alone */
    cJSON *payload = micom_payload(table, 1);
    int need = !sent || !cJSON_Compare(sent, payload, 1);
    for (int i = 0; i < NWAKE && !need && want; i++) {
        rec = cJSON_GetObjectItemCaseSensitive(entries, WAKE_KEYS[i].reason);
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec, "lock")) && micom_lock_value(WAKE_KEYS[i].lock) == 0) {
            LOG("the TV unlocked '%s' in the micom, locking it again", WAKE_KEYS[i].reason);
            need = 1;
        }
    }
    if (need) {
        char *text = cJSON_PrintUnformatted(payload);
        cJSON *res = text ? luna_json("luna://com.webos.service.micomservice/setCPHotKeyListLock", text, 5000) : NULL;
        free(text);
        if (!res) {
            snprintf(g_sb_error, sizeof g_sb_error, "could not update the TV's standby key locks; retrying");
            ERR("%s", g_sb_error);
            cJSON_Delete(payload);
            g_sb_retry_at = mono_ms() + 30000;
            return -1;
        }
        cJSON_Delete(res);
        LOG(want ? "standby key locks updated" : "standby key locks given back to LG");
    }
    if (want) {
        if (need) {
            cJSON_DeleteItemFromObjectCaseSensitive(g_sb_state, "micom");
            cJSON_AddItemToObject(g_sb_state, "micom", payload);
            *state_changed = 1;
            payload = NULL;
        }
    } else {
        cJSON_DeleteItemFromObjectCaseSensitive(g_sb_state, "micom");
        *state_changed = 1;
    }
    cJSON_Delete(payload);
    return 0;
}

/* Brings LG's hotkey table in line with the mappings (release_all: give every
 * entry back to LG). Returns 0 when the table is as it should be. */
static int standby_sync(int release_all) {
    if (g_dry_run) return 0;
    if (g_sb_paused && !release_all) return 0;
    g_sb_retry_at = 0;
    char *text = read_file(LG_PREFS_FILE);
    cJSON *prefs = text ? cJSON_Parse(text) : NULL;
    free(text);
    cJSON *table = cJSON_GetObjectItemCaseSensitive(prefs, "mapping_info");
    if (!cJSON_IsArray(table)) {
        for (int i = 0; i < NWAKE; i++) g_wake_mode[i] = WM_ABSENT;
        if (!cJSON_IsObject(prefs)) {
            /* missing or being rewritten: look again later */
            snprintf(g_sb_error, sizeof g_sb_error, "the TV's hotkey table (%s) is not readable", LG_PREFS_FILE);
            g_sb_retry_at = mono_ms() + 60000;
        } else {
            g_sb_error[0] = 0; /* this TV has no such table (webOS 5 and older) */
        }
        cJSON_Delete(prefs);
        return -1;
    }

    cJSON *entries = cJSON_GetObjectItemCaseSensitive(g_sb_state, "entries");
    int changed = 0, state_changed = 0, lg_reset = 0;
    for (int i = 0; i < NWAKE; i++) {
        const char *reason = WAKE_KEYS[i].reason;
        cJSON *slot = table_slot(table, reason);
        cJSON *cur = slot ? cJSON_GetObjectItemCaseSensitive(slot, reason) : NULL;
        cJSON *rec = cJSON_GetObjectItemCaseSensitive(entries, reason);
        if (!cur) {
            if (rec) { cJSON_DeleteItemFromObjectCaseSensitive(entries, reason); state_changed = 1; }
            continue;
        }
        cJSON *ours = rec ? cJSON_GetObjectItemCaseSensitive(rec, "ours") : NULL;
        if (rec && !cJSON_Compare(cur, ours, 1)) {
            /* Not what we wrote: the TV set it (LG's server resets the table
             * after a reboot), so this is LG's value now. */
            cJSON_ReplaceItemInObjectCaseSensitive(rec, "lg", cJSON_Duplicate(cur, 1));
            state_changed = 1;
            lg_reset = 1;
        }
        cJSON *lg = rec ? cJSON_GetObjectItemCaseSensitive(rec, "lg") : cur;
        const struct mapping *m = release_all ? NULL : g_cfg.by_key[WAKE_KEYS[i].key];
        int lock;
        cJSON *want = standby_want(m, lg, &lock);
        if (want) {
            if (!rec) {
                rec = cJSON_AddObjectToObject(entries, reason);
                cJSON_AddItemToObject(rec, "lg", cJSON_Duplicate(cur, 1));
                state_changed = 1;
            }
            if (!cJSON_Compare(cur, want, 1)) {
                cJSON_ReplaceItemInObjectCaseSensitive(slot, reason, cJSON_Duplicate(want, 1));
                changed = 1;
            }
            if (!cJSON_Compare(cJSON_GetObjectItemCaseSensitive(rec, "ours"), want, 1)) {
                cJSON_DeleteItemFromObjectCaseSensitive(rec, "ours");
                cJSON_AddItemToObject(rec, "ours", cJSON_Duplicate(want, 1));
                state_changed = 1;
            }
            if (lock != cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rec, "lock"))) {
                cJSON_DeleteItemFromObjectCaseSensitive(rec, "lock");
                if (lock) cJSON_AddBoolToObject(rec, "lock", 1);
                state_changed = 1;
            }
            cJSON_Delete(want);
        } else {
            if (rec) {
                if (cJSON_Compare(cur, cJSON_GetObjectItemCaseSensitive(rec, "ours"), 1)) {
                    cJSON_ReplaceItemInObjectCaseSensitive(slot, reason, cJSON_Duplicate(lg, 1));
                    changed = 1;
                }
                cJSON_DeleteItemFromObjectCaseSensitive(entries, reason);
                state_changed = 1;
            }
        }
    }

    if (changed) {
        int64_t now = mono_ms();
        if (now - g_sb_window_start > 10 * 60 * 1000) { g_sb_window_start = now; g_sb_window_writes = 0; }
        if (++g_sb_window_writes > 10 && !release_all) {
            /* Something keeps putting the table back: stop fighting over it. */
            snprintf(g_sb_error, sizeof g_sb_error, "the TV keeps changing its hotkey table back; standby remapping paused until the mappings change");
            ERR("%s", g_sb_error);
            g_sb_paused = 1;
            standby_reload();
            cJSON_Delete(prefs);
            return -1;
        }
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "category", "other");
        cJSON_AddItemReferenceToObject(cJSON_AddObjectToObject(req, "settings"), "mapping_info", table);
        char *payload = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        cJSON *res = payload ? luna_json("luna://com.webos.settingsservice/setSystemSettings", payload, 5000) : NULL;
        free(payload);
        if (!res) {
            snprintf(g_sb_error, sizeof g_sb_error, "could not update the TV's hotkey table; retrying");
            ERR("%s", g_sb_error);
            standby_reload();
            cJSON_Delete(prefs);
            g_sb_retry_at = mono_ms() + 30000;
            return -1;
        }
        cJSON_Delete(res);
        LOG("hotkey table updated for standby wakes");
    }
    int micom_rc = standby_micom_sync(table, &state_changed);
    /* LG's sync sends the key locks right after the table: look at ours again
     * once that has landed. */
    if (lg_reset && micom_rc == 0 && !release_all) g_sb_recheck_at = mono_ms() + 10000;
    if (changed || state_changed) standby_save_state(table);
    standby_update_modes(table);
    if (!g_sb_paused && micom_rc == 0) g_sb_error[0] = 0;
    cJSON_Delete(prefs);
    return micom_rc;
}

/* Gives LG's table back and forgets our changes (stop, uninstall).
 * Returns 0 once the TV is LG's again. */
static int standby_release(void) {
    if (standby_sync(1) != 0) return -1;
    char path[600];
    config_dir_file(path, sizeof path, STANDBY_STATE_NAME); unlink(path);
    config_dir_file(path, sizeof path, STANDBY_RESTORE_NAME); unlink(path);
    config_dir_file(path, sizeof path, STANDBY_RESTORE_MICOM_NAME); unlink(path);
    return 0;
}

static void standby_status(cJSON *root) {
    cJSON *sb = cJSON_AddObjectToObject(root, "standby");
    cJSON_AddBoolToObject(sb, "enabled", !g_dry_run);
    cJSON_AddStringToObject(sb, "error", g_sb_error);
    cJSON *keys = cJSON_AddObjectToObject(sb, "keys");
    for (int i = 0; i < NWAKE; i++) {
        if (g_wake_mode[i] == WM_ABSENT) continue;
        char k[16];
        snprintf(k, sizeof k, "%d", WAKE_KEYS[i].key);
        cJSON *j = cJSON_AddObjectToObject(keys, k);
        cJSON_AddStringToObject(j, "reason", WAKE_KEYS[i].reason);
        cJSON_AddStringToObject(j, "wake", WAKE_MODE_NAMES[g_wake_mode[i]]);
    }
    if (g_last_wake.at) {
        cJSON *w = cJSON_AddObjectToObject(sb, "lastWake");
        cJSON_AddStringToObject(w, "reason", g_last_wake.reason);
        cJSON_AddNumberToObject(w, "at", (double)g_last_wake.at);
        if (g_last_wake.key) cJSON_AddNumberToObject(w, "key", g_last_wake.key);
        cJSON_AddStringToObject(w, "action", g_last_wake.action ? g_last_wake.action : "none");
    }
}

/* Replace and exec mappings of wake hotkeys act after the wake; the others are
 * handled by the table alone. */
static int standby_has_wake_actions(void) {
    if (g_dry_run) return 0;
    for (int i = 0; i < NWAKE; i++) {
        const struct mapping *m = g_cfg.by_key[WAKE_KEYS[i].key];
        if (m && m->enabled && !m->looped && m->standby && (m->press.type == ACT_REPLACE || m->press.type == ACT_EXEC)) return 1;
    }
    return 0;
}

static void standby_start_wake(void) {
    if (!standby_has_wake_actions() && !g_standby_press_key) return;
    memset(&g_wake, 0, sizeof g_wake);
    g_wake.active = 1;
    g_wake.next = mono_ms() + 500;
    g_wake.until = mono_ms() + 30000;
}

/* Wakes are noticed through tvpower's power state: a luna-send subscription
 * runs alongside while any mapping needs to act after a wake. (The C5 does
 * suspend in standby, but its CLOCK_BOOTTIME does not count the time asleep,
 * so the clocks cannot tell.) */
static struct {
    int fd;                /* luna-send's stdout, -1 when not running */
    pid_t pid;
    int64_t restart_at;    /* mono ms */
    char line[1024];
    size_t len;
    char state[40];        /* last power state reported */
} g_pw = { .fd = -1 };

static int is_standby_state(const char *s) {
    return strstr(s, "Standby") || strstr(s, "Suspend") || !strcmp(s, "Power Off");
}

static void power_watch_stop(void) {
    if (g_pw.fd < 0) return;
    if (g_pw.pid > 0) kill(g_pw.pid, SIGTERM);
    close(g_pw.fd);
    g_pw.fd = -1;
    g_pw.pid = 0;
    g_pw.len = 0;
    g_pw.state[0] = 0;
}

static void power_watch_start(void) {
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) < 0) { WARN("pipe: %s", strerror(errno)); return; }
    pid_t pid = fork();
    if (pid < 0) { WARN("fork failed: %s", strerror(errno)); close(pfd[0]); close(pfd[1]); return; }
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 2); }
        dup2(pfd[1], 1);
        signal(SIGCHLD, SIG_DFL);
        char *argv[] = { "luna-send", "-i", "luna://com.webos.service.tvpower/power/getPowerState",
                         "{\"subscribe\":true}", NULL };
        execv("/usr/bin/luna-send", argv);
        execvp("luna-send", argv);
        _exit(127);
    }
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    g_pw.fd = pfd[0];
    g_pw.pid = pid;
    g_pw.len = 0;
    g_pw.state[0] = 0;
}

/* The watcher is needed while any hotkey's wake is ours: in active standby
 * those keys reach the daemon, which then turns the TV on itself. */
static int standby_needs_watch(void) {
    if (g_dry_run) return 0;
    for (int i = 0; i < NWAKE; i++)
        if (g_wake_mode[i] == WM_LAUNCH || g_wake_mode[i] == WM_NORMAL || g_wake_mode[i] == WM_OFF) return 1;
    return standby_has_wake_actions();
}

/* Runs the watcher exactly while it is needed. */
static void power_watch_update(void) {
    int want = standby_needs_watch();
    if (!want) { power_watch_stop(); return; }
    if (g_pw.fd < 0 && mono_ms() >= g_pw.restart_at) power_watch_start();
}

static void power_watch_line(char *line) {
    cJSON *r = cJSON_Parse(line);
    cJSON *st = cJSON_GetObjectItemCaseSensitive(r, "state");
    if (cJSON_IsString(st)) {
        if (g_pw.state[0] && is_standby_state(g_pw.state) && !strcmp(st->valuestring, "Active")) {
            LOG("the TV turned on (was '%s')", g_pw.state);
            standby_start_wake();
        }
        snprintf(g_pw.state, sizeof g_pw.state, "%s", st->valuestring);
    }
    cJSON_Delete(r);
}

static void power_watch_read(void) {
    for (;;) {
        if (g_pw.len >= sizeof g_pw.line - 1) g_pw.len = 0; /* an absurd line: drop it */
        ssize_t n = read(g_pw.fd, g_pw.line + g_pw.len, sizeof g_pw.line - 1 - g_pw.len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return;
        if (n <= 0) {
            WARN("power state subscription ended, restarting it in 10 s");
            power_watch_stop();
            g_pw.restart_at = mono_ms() + 10000;
            return;
        }
        g_pw.len += (size_t)n;
        g_pw.line[g_pw.len] = 0;
        char *nl;
        while ((nl = strchr(g_pw.line, '\n'))) {
            *nl = 0;
            power_watch_line(g_pw.line);
            size_t rest = g_pw.len - (size_t)(nl + 1 - g_pw.line);
            memmove(g_pw.line, nl + 1, rest + 1);
            g_pw.len = rest;
        }
    }
}

static void standby_poll_wake(void) {
    if (!g_wake.active || mono_ms() < g_wake.next) return;
    if (g_wake.stage == 0) {
        /* tvpowerd sets the power-on reason before the TV turns active. */
        cJSON *r = luna_json("luna://com.webos.service.tvpower/power/getPowerState", "{}", 2000);
        cJSON *st = cJSON_GetObjectItemCaseSensitive(r, "state");
        int ready = cJSON_IsString(st) && !strcmp(st->valuestring, "Active") &&
                    !cJSON_GetObjectItemCaseSensitive(r, "processing");
        cJSON_Delete(r);
        if (!ready) {
            if (mono_ms() > g_wake.until) { LOG("the TV did not turn active, no wake action"); g_wake.active = 0; }
            else g_wake.next = mono_ms() + 1000;
            return;
        }
        r = luna_json("luna://com.webos.service.tvpower/power/getPowerOnReason", "{}", 2000);
        cJSON *rs = cJSON_GetObjectItemCaseSensitive(r, "reason");
        memset(&g_last_wake, 0, sizeof g_last_wake);
        snprintf(g_last_wake.reason, sizeof g_last_wake.reason, "%s", cJSON_IsString(rs) ? rs->valuestring : "?");
        g_last_wake.at = now_ms();
        cJSON_Delete(r);
        g_wake.active = 0;
        int key = 0, missed = 0;
        for (int i = 0; i < NWAKE && !key; i++)
            if (!strcmp(WAKE_KEYS[i].reason, g_last_wake.reason)) key = WAKE_KEYS[i].key;
        if (g_standby_press_key && mono_ms() - g_standby_press_at < 30000 && key != g_standby_press_key) {
            /* we turned the TV on for this key but tvpower recorded something
             * else, so bootd did not act on it either: do it all here */
            key = g_standby_press_key;
            missed = 1;
        }
        g_standby_press_key = 0;
        const struct mapping *m = key ? g_cfg.by_key[key] : NULL;
        if (m && m->enabled && !m->looped && m->standby) {
            g_last_wake.key = m->key;
            if (m->press.type == ACT_REPLACE || m->press.type == ACT_EXEC || (missed && m->press.type == ACT_LAUNCH)) {
                /* let the app the TV resumes into settle first */
                g_wake.active = 1;
                g_wake.stage = 1;
                g_wake.key = m->key;
                g_wake.next = mono_ms() + 1500;
                g_wake.until = mono_ms() + 10000;
            }
        }
        LOG("woken by '%s'%s", g_last_wake.reason, g_wake.active ? ", running its mapping" : "");
        write_status();
        return;
    }
    const struct mapping *m = g_cfg.by_key[g_wake.key];
    g_wake.active = 0;
    if (!m || !m->enabled || m->looped) return;
    if (m->press.type == ACT_LAUNCH) {
        run_launch(m->press.app, m->press.params, m->key);
        g_last_wake.action = "launch";
    } else if (m->press.type == ACT_EXEC) {
        setenv("LGINPUTMAPPER_WAKE", "1", 1);
        run_exec(m->press.command, m->key, 1);
        unsetenv("LGINPUTMAPPER_WAKE");
        g_last_wake.action = "exec";
    } else if (m->press.type == ACT_REPLACE) {
        struct device *out = NULL;
        for (int i = 0; i < MAX_DEVS && !out; i++) {
            struct device *d = &g_devs[i];
            if (d->fd >= 0 && d->out_fd >= 0 && (d->out_is_clone || TEST_BIT(m->press.to, d->out_keybits))) out = d;
        }
        if (!out) {
            if (mono_ms() < g_wake.until) { g_wake.active = 1; g_wake.next = mono_ms() + 1000; return; }
            WARN("no output device to send key %d after the wake", m->press.to);
            return;
        }
        LOG("key %d after wake: acts as %d", m->key, m->press.to);
        out_tap(out, m->press.to);
        g_last_wake.action = "replace";
    }
    write_status();
}

/* Called for every key press: in active standby (the SoC is up, so remote
 * keys reach the input devices rather than the micom), a hotkey whose wake
 * is ours powers the TV on with that hotkey as the reason. bootd then does
 * what the table says, exactly as after a wake from suspend, and the wake
 * handler runs "acts as" and command mappings. */
static int standby_wake_key_pressed(int code) {
    if (g_dry_run || g_pw.fd < 0 || !g_pw.state[0] || !is_standby_state(g_pw.state)) return 0;
    int i;
    for (i = 0; i < NWAKE && WAKE_KEYS[i].key != code; i++) {}
    if (i == NWAKE || (g_wake_mode[i] != WM_LAUNCH && g_wake_mode[i] != WM_NORMAL && g_wake_mode[i] != WM_OFF)) return 0;
    if (g_wake_mode[i] == WM_OFF) {
        LOG("key %d pressed while the TV is in '%s': it does nothing", code, g_pw.state);
        return 1;
    }
    LOG("key %d pressed while the TV is in '%s': turning it on as a '%s' wake", code, g_pw.state, WAKE_KEYS[i].reason);
    g_standby_press_key = code;
    g_standby_press_at = mono_ms();
    char payload[64];
    snprintf(payload, sizeof payload, "{\"reason\":\"%s\"}", WAKE_KEYS[i].reason);
    char *argv[] = { "luna-send", "-n", "1", "luna://com.webos.service.tvpower/power/powerOn", payload, NULL };
    run_detached(argv, code, 1);
    return 1;
}

/* When the package is uninstalled the daemon keeps running from the deleted
 * binary: give LG's hotkey table back, remove the boot script and quit. */
static void check_installed(void) {
    if (!g_install_dir[0] || mono_ms() < g_install_next_check) return;
    g_install_next_check = mono_ms() + 15000;
    struct stat st;
    if (stat(g_install_dir, &st) == 0) { g_install_missing_since = 0; return; }
    if (!g_install_missing_since) {
        /* an update replaces the directory too: give it time to come back */
        g_install_missing_since = mono_ms();
        LOG("%s is gone, checking whether LG Input Mapper was uninstalled", g_install_dir);
        return;
    }
    if (mono_ms() - g_install_missing_since < 60000) return;
    LOG("LG Input Mapper was uninstalled: giving the TV's hotkeys back, removing our files and exiting");
    if (standby_release() == 0) {
        char *script = read_file(INIT_SCRIPT);
        if (script && strstr(script, g_install_dir)) unlink(INIT_SCRIPT);
        free(script);
        g_uninstalled = 1;
    } else {
        /* the boot script still has the restore requests: it retries at the next boot */
        WARN("could not give the hotkeys back now; the boot script will at the next boot");
    }
    g_quit = 1;
}

/* Empties and removes one of our own directories. Only a directory named
 * like ours is touched, whatever --config or --state-dir pointed at. */
static void remove_own_dir(const char *dir, const char *expected_name) {
    const char *base = strrchr(dir, '/');
    base = base ? base + 1 : dir;
    if (strcmp(base, expected_name)) { WARN("not removing %s: not our directory", dir); return; }
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (unlinkat(dirfd(d), de->d_name, 0) < 0) WARN("cannot remove %s/%s: %s", dir, de->d_name, strerror(errno));
    }
    closedir(d);
    if (rmdir(dir) < 0) WARN("cannot remove %s: %s", dir, strerror(errno));
}

static void find_install_dir(void) {
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    char *del = strstr(exe, " (deleted)");
    if (del) *del = 0;
    if (!strstr(exe, "/usr/palm/services/")) return;  /* a development copy */
    char *slash = strrchr(exe, '/');                  /* .../<service>/bin/lginputmapperd-arm */
    if (slash) *slash = 0;
    slash = strrchr(exe, '/');
    if (slash) *slash = 0;
    snprintf(g_install_dir, sizeof g_install_dir, "%s", exe);
}

/* ---------------------------------------------------------------- main */

static volatile sig_atomic_t g_release = 0;

static void on_signal(int sig) {
    /* SIGUSR2 comes from the service when the remapper is turned off: give the
     * TV's hotkeys back too. A plain SIGTERM (reboot, --replace) keeps them. */
    if (sig == SIGUSR2) g_release = 1;
    g_quit = 1;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "lginputmapperd %s - LG Input Mapper: LG webOS remote button remapper\n\n"
        "usage: %s [options]\n"
        "  -c, --config PATH     mappings file (default %s)\n"
        "  -s, --state-dir DIR   status/events directory (default %s)\n"
        "  -l, --log PATH        append daemon log to PATH instead of stderr\n"
        "  -p, --pidfile PATH    write pid to PATH\n"
        "  -r, --replace         stop an already running instance first\n"
        "  -n, --dry-run         observe and log key events without grabbing devices\n"
        "  -L, --list            print input devices and exit\n"
        "  -v, --verbose         debug logging\n"
        "  -h, --help\n",
        LGINPUTMAPPERD_VERSION, argv0, DEFAULT_CONFIG, DEFAULT_STATE_DIR);
}

/* Whatever spawned us (the Node service, a shell) may leak descriptors that
 * are not close-on-exec, e.g. the service's Luna hub socket. Holding those
 * would make the bus believe the service is still alive after it exits. */
static void close_inherited_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return;
    int dfd = dirfd(d);
    struct dirent *de;
    while ((de = readdir(d))) {
        int fd = atoi(de->d_name);
        if (fd > 2 && fd != dfd) close(fd);
    }
    closedir(d);
}

static void mkdir_p(const char *path, mode_t mode) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, mode);
}

/* The state dir usually sits in world-writable /tmp and holds the pid file, the
 * capture file we obey and the logs: only use a real directory owned by us and
 * move anything else aside. Returns 1 if something was moved. */
static int ensure_state_dir(void) {
    struct stat st;
    int moved = 0;
    if (lstat(g_state_dir, &st) == 0 && (!S_ISDIR(st.st_mode) || st.st_uid != geteuid())) {
        char aside[600];
        snprintf(aside, sizeof aside, "%s.untrusted-%lld", g_state_dir, (long long)now_ms());
        if (rename(g_state_dir, aside) == 0) moved = 1;
    }
    mkdir_p(g_state_dir, 0700);
    chmod(g_state_dir, 0700);
    return moved;
}

static int acquire_lock(int replace) {
    char path[512];
    snprintf(path, sizeof path, "%s/lock", g_state_dir);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { ERR("cannot open lock %s: %s", path, strerror(errno)); return -1; }
    fchmod(fd, 0600); /* older versions created it world-readable */
    for (int attempt = 0; attempt < 50; attempt++) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            char buf[32];
            snprintf(buf, sizeof buf, "%d\n", getpid());
            if (ftruncate(fd, 0) == 0) { lseek(fd, 0, SEEK_SET); write_all(fd, buf, strlen(buf)); }
            return fd;
        }
        if (errno != EWOULDBLOCK) { ERR("flock: %s", strerror(errno)); close(fd); return -1; }
        char buf[32] = "";
        lseek(fd, 0, SEEK_SET);
        ssize_t n = read(fd, buf, sizeof buf - 1);
        pid_t other = (n > 0) ? (pid_t)atoi(buf) : 0;
        if (!replace) {
            ERR("another lginputmapperd is already running (pid %d); use --replace", (int)other);
            close(fd);
            return -1;
        }
        if (attempt == 0 && other > 0) {
            LOG("stopping running instance pid %d", (int)other);
            kill(other, SIGTERM);
        }
        usleep(100 * 1000);
    }
    ERR("could not take over from the running instance");
    close(fd);
    return -1;
}

int main(int argc, char **argv) {
    const char *log_path = NULL, *pid_path = NULL;
    int replace = 0, list = 0;

    close_inherited_fds();

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "-c") || !strcmp(a, "--config")) && i + 1 < argc) g_config_path = argv[++i];
        else if ((!strcmp(a, "-s") || !strcmp(a, "--state-dir")) && i + 1 < argc) g_state_dir = argv[++i];
        else if ((!strcmp(a, "-l") || !strcmp(a, "--log")) && i + 1 < argc) log_path = argv[++i];
        else if ((!strcmp(a, "-p") || !strcmp(a, "--pidfile")) && i + 1 < argc) pid_path = argv[++i];
        else if (!strcmp(a, "-r") || !strcmp(a, "--replace")) replace = 1;
        else if (!strcmp(a, "-n") || !strcmp(a, "--dry-run")) g_dry_run = 1;
        else if (!strcmp(a, "-L") || !strcmp(a, "--list")) list = 1;
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) g_verbose = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else { usage(argv[0]); return 2; }
    }

    if (list) { list_devices(); return 0; }

    int state_moved = ensure_state_dir();

    if (log_path) {
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) { ERR("cannot open log %s: %s", log_path, strerror(errno)); return 1; }
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 1024 * 1024) { ftruncate(fd, 0); }
        fchmod(fd, 0600); /* older versions created it world-readable */
        dup2(fd, 2);
        dup2(fd, 1);
        close(fd);
    }

    if (state_moved) WARN("%s was not ours, moved it aside", g_state_dir);
    {
        snprintf(g_config_dir, sizeof g_config_dir, "%s", g_config_path);
        char *slash = strrchr(g_config_dir, '/');
        if (slash) { *slash = 0; mkdir_p(g_config_dir, 0700); }
        else snprintf(g_config_dir, sizeof g_config_dir, ".");
    }
    find_install_dir();

    int lock_fd = acquire_lock(replace);
    if (lock_fd < 0) return 1;
    if (pid_path) {
        char buf[32];
        snprintf(buf, sizeof buf, "%d\n", getpid());
        write_file_atomic(pid_path, buf);
    }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGUSR2, on_signal);

    g_started_ms = now_ms();
    for (int i = 0; i < MAX_DEVS; i++) { g_devs[i].fd = -1; g_devs[i].out_fd = -1; }
    LOG("lginputmapperd %s starting (pid %d, config %s, state %s%s)", LGINPUTMAPPERD_VERSION, getpid(),
        g_config_path, g_state_dir, g_dry_run ? ", DRY RUN" : "");

    load_config();
    {
        /* Left behind by versions that logged every key press. */
        char old[512];
        snprintf(old, sizeof old, "%s/events.1.jsonl", g_state_dir);
        unlink(old);
    }
    apply_capture();

    int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino < 0) { ERR("inotify_init: %s", strerror(errno)); return 1; }
    int w_dev = inotify_add_watch(ino, DEV_INPUT, IN_CREATE | IN_DELETE | IN_ATTRIB);
    const char *cfg_base = strrchr(g_config_path, '/');
    cfg_base = cfg_base ? cfg_base + 1 : g_config_path;
    int w_cfg = inotify_add_watch(ino, g_config_dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE);
    int w_state = inotify_add_watch(ino, g_state_dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE);
    /* the settings service rewrites this file whenever LG's hotkey table changes */
    int w_prefs = g_dry_run ? -2 : inotify_add_watch(ino, LG_PREFS_DIR, IN_CLOSE_WRITE | IN_MOVED_TO);
    if (w_dev < 0 || w_cfg < 0 || w_state < 0 || w_prefs == -1) WARN("inotify watches incomplete (%s)", strerror(errno));

    scan_devices();
    if (!g_dry_run) {
        standby_load_state();
        standby_sync(0);
        power_watch_update();
        /* Started at boot: a hotkey may have been what turned the TV on. */
        char marker[600];
        snprintf(marker, sizeof marker, "%s/boot-wake-checked", g_state_dir);
        int mfd = boot_ms() < 180000 ? open(marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600) : -1;
        if (mfd >= 0) { close(mfd); standby_start_wake(); }
    }
    write_status();

    struct pollfd pfds[MAX_DEVS + 1];
    int64_t last_scan = mono_ms();
    int reload_cfg = 0, rescan = 0, status_dirty = 0, sync_standby = 0;

    while (!g_quit) {
        int n = 0;
        pfds[n].fd = ino; pfds[n].events = POLLIN; n++;
        struct device *map[MAX_DEVS + 1];
        for (int i = 0; i < MAX_DEVS; i++) {
            if (g_devs[i].fd < 0) continue;
            map[n] = &g_devs[i];
            pfds[n].fd = g_devs[i].fd; pfds[n].events = POLLIN; n++;
        }
        int pw_idx = -1;
        if (g_pw.fd >= 0) { pw_idx = n; pfds[n].fd = g_pw.fd; pfds[n].events = POLLIN; n++; }
        int timeout = next_timeout_ms();
        if (rescan) timeout = 500; /* devices are still settling; poll again soon */
        if (timeout < 0 || timeout > 30000) timeout = 30000;
        if (g_pw.fd < 0 && g_pw.restart_at && standby_needs_watch()) {
            int64_t left = g_pw.restart_at - mono_ms();
            if (left < timeout) timeout = left < 0 ? 0 : (int)left;
        }
        if (g_wake.active) {
            int64_t left = g_wake.next - mono_ms();
            if (left < timeout) timeout = left < 0 ? 0 : (int)left;
        }
        if (g_sb_retry_at) {
            int64_t left = g_sb_retry_at - mono_ms();
            if (left < timeout) timeout = left < 0 ? 0 : (int)left;
        }
        if (g_sb_recheck_at) {
            int64_t left = g_sb_recheck_at - mono_ms();
            if (left < timeout) timeout = left < 0 ? 0 : (int)left;
        }

        int rc = poll(pfds, (nfds_t)n, timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            ERR("poll: %s", strerror(errno));
            break;
        }

        if (pfds[0].revents & POLLIN) {
            char buf[4096] __attribute__((aligned(8)));
            ssize_t len;
            while ((len = read(ino, buf, sizeof buf)) > 0) {
                for (char *p = buf; p < buf + len;) {
                    struct inotify_event *ev = (struct inotify_event *)p;
                    if (ev->wd == w_dev) {
                        if (ev->len && !strncmp(ev->name, "event", 5)) rescan = 1;
                    } else if (ev->wd == w_cfg) {
                        if (ev->len && !strcmp(ev->name, cfg_base)) reload_cfg = 1;
                    } else if (ev->wd == w_state) {
                        if (ev->len && !strcmp(ev->name, "capture.json")) { apply_capture(); status_dirty = 1; }
                    } else if (ev->wd == w_prefs) {
                        if (ev->len && !strcmp(ev->name, LG_PREFS_NAME)) sync_standby = 1;
                    }
                    p += sizeof *ev + ev->len;
                }
            }
        }

        if (pw_idx >= 0 && (pfds[pw_idx].revents & (POLLIN | POLLERR | POLLHUP))) power_watch_read();

        for (int i = 1; i < n; i++) {
            if (i == pw_idx) continue;
            if (pfds[i].revents & (POLLIN | POLLERR | POLLHUP)) {
                if (map[i]->fd < 0) continue;
                if ((pfds[i].revents & (POLLERR | POLLHUP)) && !(pfds[i].revents & POLLIN)) {
                    LOG("%s went away", map[i]->path);
                    close_device(map[i]);
                    status_dirty = 1;
                    continue;
                }
                handle_device_input(map[i]);
            }
        }

        check_hold_timeouts();

        if (g_capture.until_ms > 0 && !capture_active()) {
            LOG("capture mode ended");
            g_capture.until_ms = 0;
            close_events_log();
            status_dirty = 1;
        }

        if (reload_cfg) {
            reload_cfg = 0;
            int had_mode = g_cfg.output_mode;
            char *had_dev = g_cfg.output_device ? xstrdup(g_cfg.output_device) : NULL;
            if (load_config() == 0) {
                int output_changed = had_mode != g_cfg.output_mode ||
                    (had_dev && g_cfg.output_device && strcmp(had_dev, g_cfg.output_device));
                /* Device filters or output mode may have changed: re-evaluate everything. */
                for (int i = 0; i < MAX_DEVS; i++) {
                    if (g_devs[i].fd < 0) continue;
                    if (output_changed || !name_matches(g_devs[i].name)) close_device(&g_devs[i]);
                }
                rescan = 1;
                g_sb_paused = 0;
                sync_standby = 1;
            }
            free(had_dev);
            status_dirty = 1;
        }

        if (!g_dry_run) {
            power_watch_update();
            int recheck_due = g_sb_recheck_at && mono_ms() >= g_sb_recheck_at;
            if (sync_standby || (g_sb_retry_at && mono_ms() >= g_sb_retry_at) || recheck_due) {
                sync_standby = 0;
                if (recheck_due) g_sb_recheck_at = 0;
                standby_sync(0);
                status_dirty = 1;
            }
            standby_poll_wake();
            check_installed();
        }

        if (rescan) {
            /* Wait a moment after the first notification so udev/permissions settle. */
            if (mono_ms() - last_scan >= 300) {
                int before = 0, after = 0;
                for (int i = 0; i < MAX_DEVS; i++) if (g_devs[i].fd >= 0) before++;
                scan_devices();
                for (int i = 0; i < MAX_DEVS; i++) if (g_devs[i].fd >= 0) after++;
                last_scan = mono_ms();
                rescan = 0;
                if (before != after) status_dirty = 1;
            }
        }

        if (status_dirty) { write_status(); status_dirty = 0; }
    }

    LOG("shutting down");
    if (g_release && !g_dry_run) {
        LOG("remapper turned off: giving the TV's hotkeys back");
        if (standby_release() != 0) WARN("could not give the hotkeys back; LG's own sync restores them after the next reboot");
    }
    power_watch_stop();
    for (int i = 0; i < MAX_DEVS; i++) close_device(&g_devs[i]);
    if (pid_path) unlink(pid_path);
    close(lock_fd);
    if (g_uninstalled) {
        /* uninstalled: leave nothing behind, the mappings included */
        remove_own_dir(g_config_dir, "lginputmapper");
        remove_own_dir(g_state_dir, "lginputmapperd");
    }
    return 0;
}
