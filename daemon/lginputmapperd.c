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
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <linux/uinput.h>

#include "vendor/cJSON.h"

#ifndef LGINPUTMAPPERD_VERSION
#define LGINPUTMAPPERD_VERSION "1.0.1"
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
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
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

/* ---------------------------------------------------------------- events log */

static void open_events_log(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/events.jsonl", g_state_dir);
    if (g_events) fclose(g_events);
    g_events = fopen(path, "ae"); /* e: close-on-exec, keep it away from exec'd commands */
    if (!g_events) { WARN("cannot open %s: %s", path, strerror(errno)); return; }
    g_events_bytes = ftell(g_events);
}

static void rotate_events_log_if_needed(void) {
    if (g_events_bytes < 512 * 1024) return;
    char a[512], b[512];
    snprintf(a, sizeof a, "%s/events.jsonl", g_state_dir);
    snprintf(b, sizeof b, "%s/events.1.jsonl", g_state_dir);
    fclose(g_events);
    g_events = NULL;
    rename(a, b);
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

    cJSON *cap = cJSON_AddObjectToObject(root, "capture");
    cJSON_AddBoolToObject(cap, "active", capture_active());
    cJSON_AddNumberToObject(cap, "until", (double)g_capture.until_ms);

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

/* ---------------------------------------------------------------- main */

static void on_signal(int sig) {
    (void)sig;
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

static void mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static int acquire_lock(int replace) {
    char path[512];
    snprintf(path, sizeof path, "%s/lock", g_state_dir);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) { ERR("cannot open lock %s: %s", path, strerror(errno)); return -1; }
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

    if (log_path) {
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) { ERR("cannot open log %s: %s", log_path, strerror(errno)); return 1; }
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 1024 * 1024) { ftruncate(fd, 0); }
        dup2(fd, 2);
        dup2(fd, 1);
        close(fd);
    }

    mkdir_p(g_state_dir);
    {
        char cdir[512];
        snprintf(cdir, sizeof cdir, "%s", g_config_path);
        char *slash = strrchr(cdir, '/');
        if (slash) { *slash = 0; mkdir_p(cdir); }
    }

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

    g_started_ms = now_ms();
    for (int i = 0; i < MAX_DEVS; i++) { g_devs[i].fd = -1; g_devs[i].out_fd = -1; }
    LOG("lginputmapperd %s starting (pid %d, config %s, state %s%s)", LGINPUTMAPPERD_VERSION, getpid(),
        g_config_path, g_state_dir, g_dry_run ? ", DRY RUN" : "");

    open_events_log();
    load_config();
    load_capture();

    int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino < 0) { ERR("inotify_init: %s", strerror(errno)); return 1; }
    int w_dev = inotify_add_watch(ino, DEV_INPUT, IN_CREATE | IN_DELETE | IN_ATTRIB);
    char cdir[512];
    snprintf(cdir, sizeof cdir, "%s", g_config_path);
    { char *slash = strrchr(cdir, '/'); if (slash) *slash = 0; }
    const char *cfg_base = strrchr(g_config_path, '/');
    cfg_base = cfg_base ? cfg_base + 1 : g_config_path;
    int w_cfg = inotify_add_watch(ino, cdir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE);
    int w_state = inotify_add_watch(ino, g_state_dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE);
    if (w_dev < 0 || w_cfg < 0 || w_state < 0) WARN("inotify watches incomplete (%s)", strerror(errno));

    scan_devices();
    write_status();

    struct pollfd pfds[MAX_DEVS + 1];
    int64_t last_scan = mono_ms();
    int reload_cfg = 0, rescan = 0, status_dirty = 0;

    while (!g_quit) {
        int n = 0;
        pfds[n].fd = ino; pfds[n].events = POLLIN; n++;
        struct device *map[MAX_DEVS + 1];
        for (int i = 0; i < MAX_DEVS; i++) {
            if (g_devs[i].fd < 0) continue;
            map[n] = &g_devs[i];
            pfds[n].fd = g_devs[i].fd; pfds[n].events = POLLIN; n++;
        }
        int timeout = next_timeout_ms();
        if (rescan) timeout = 500; /* devices are still settling; poll again soon */
        if (timeout < 0 || timeout > 30000) timeout = 30000;

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
                        if (ev->len && !strcmp(ev->name, "capture.json")) { load_capture(); status_dirty = 1; }
                    }
                    p += sizeof *ev + ev->len;
                }
            }
        }

        for (int i = 1; i < n; i++) {
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
            }
            free(had_dev);
            status_dirty = 1;
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
    for (int i = 0; i < MAX_DEVS; i++) close_device(&g_devs[i]);
    if (pid_path) unlink(pid_path);
    close(lock_fd);
    return 0;
}
