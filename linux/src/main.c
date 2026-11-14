// Scrim for Linux — entry point, settings, EGL and the frame loop.
#include "rp.h"

#include <EGL/eglext.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

EGLDisplay rp_egl_display = EGL_NO_DISPLAY;
EGLConfig rp_egl_config;
EGLContext rp_egl_context = EGL_NO_CONTEXT;

void rp_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "scrim: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

EGLSurface rp_egl_create_surface(void *native_window) {
    EGLSurface s = eglCreatePlatformWindowSurface(rp_egl_display, rp_egl_config, native_window, NULL);
    if (s == EGL_NO_SURFACE) {
        rp_log("eglCreatePlatformWindowSurface failed (0x%x)", eglGetError());
        return s;
    }
    eglMakeCurrent(rp_egl_display, s, s, rp_egl_context);
    // Never block in eglSwapBuffers: pacing is done by our timer + frame callbacks.
    eglSwapInterval(rp_egl_display, 0);
    return s;
}

// ---- command line ----------------------------------------------------------

typedef struct {
    char effect[64];
    int fps, hidpi, pause_on_battery;   // -1 = not given
    char wallpaper[PATH_MAX];
    int nvalues;
    char values[32][2][64];             // "id", "value" for the selected effect
    char config_path[PATH_MAX];
    const char *backend;
    bool stats;
    double duration;
    const char *snapshot;               // --snapshot PATH: render offscreen and exit
    double time;                        // --time (snapshot)
    int width, height;                  // --size (snapshot)
} cli_options;

static void usage(FILE *f) {
    fprintf(f,
        "usage: scrim [options]\n"
        "  -e, --effect ID            effect to run (default: rain)\n"
        "  -p, --param ID=VALUE       set an effect parameter (repeatable)\n"
        "  -f, --fps N                frame rate (default 30)\n"
        "  -w, --wallpaper PATH       background image (JPEG or PNG)\n"
        "      --hidpi                render at the output's full HiDPI resolution (Wayland)\n"
        "      --no-pause-on-battery  keep animating on battery power\n"
        "  -c, --config PATH          config file (default ~/.config/scrim/scrim.conf)\n"
        "      --backend wayland|x11  force a display backend\n"
        "  -l, --list                 list effects and their parameters\n"
        "      --stats                print frame rate and memory every 5 s\n"
        "      --duration SECONDS     quit after this long (for testing)\n"
        "      --snapshot PATH.png    render one frame offscreen, without a display, and exit\n"
        "      --time SECONDS         (snapshot) time to render; persistent effects are\n"
        "                             simulated up to it in steps of 1/fps (default 10)\n"
        "      --size WxH             (snapshot) image size (default 1280x800)\n"
        "A snapshot uses the effect's defaults plus --param, not the config file.\n"
        "\n"
        "       scrim msg COMMAND         control the running instance: pause, resume, toggle,\n"
        "                                 effect ID, next, param ID=VALUE, reload, status\n"
        "kill -USR1 $(pidof scrim) also toggles pause.\n"
        "The config file is reloaded whenever it is saved. Command-line options win over it.\n");
}

static bool parse_cli(int argc, char **argv, cli_options *o) {
    memset(o, 0, sizeof *o);
    o->fps = o->hidpi = o->pause_on_battery = -1;
    o->time = 10;
    o->width = 1280;
    o->height = 800;
    static const struct option longopts[] = {
        {"effect", required_argument, 0, 'e'}, {"param", required_argument, 0, 'p'},
        {"fps", required_argument, 0, 'f'}, {"wallpaper", required_argument, 0, 'w'},
        {"hidpi", no_argument, 0, 'H'}, {"no-pause-on-battery", no_argument, 0, 'B'},
        {"config", required_argument, 0, 'c'}, {"backend", required_argument, 0, 'b'},
        {"list", no_argument, 0, 'l'}, {"stats", no_argument, 0, 's'},
        {"duration", required_argument, 0, 'd'}, {"help", no_argument, 0, 'h'},
        {"snapshot", required_argument, 0, 'S'}, {"time", required_argument, 0, 'T'},
        {"size", required_argument, 0, 'Z'}, {0, 0, 0, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "e:p:f:w:c:lh", longopts, NULL)) != -1) {
        switch (c) {
        case 'e': snprintf(o->effect, sizeof o->effect, "%s", optarg); break;
        case 'p': {
            char *eq = strchr(optarg, '=');
            if (!eq || o->nvalues >= 32) { rp_log("--param expects ID=VALUE"); return false; }
            *eq = 0;
            snprintf(o->values[o->nvalues][0], 64, "%s", optarg);
            snprintf(o->values[o->nvalues][1], 64, "%s", eq + 1);
            o->nvalues++;
            break;
        }
        case 'f': o->fps = atoi(optarg); break;
        case 'w': snprintf(o->wallpaper, sizeof o->wallpaper, "%s", optarg); break;
        case 'H': o->hidpi = 1; break;
        case 'B': o->pause_on_battery = 0; break;
        case 'c': snprintf(o->config_path, sizeof o->config_path, "%s", optarg); break;
        case 'b': o->backend = optarg; break;
        case 'l': rp_effect_print_all(); exit(0);
        case 's': o->stats = true; break;
        case 'd': o->duration = atof(optarg); break;
        case 'S': o->snapshot = optarg; break;
        case 'T': o->time = atof(optarg); break;
        case 'Z':
            if (sscanf(optarg, "%dx%d", &o->width, &o->height) != 2 || o->width < 1 || o->height < 1 ||
                o->width > 16384 || o->height > 16384) {
                rp_log("--size expects WIDTHxHEIGHT, e.g. 1728x1117");
                return false;
            }
            break;
        case 'h': usage(stdout); exit(0);
        default: usage(stderr); return false;
        }
    }
    return true;
}

// ---- state -------------------------------------------------------------------

static cli_options cli;
static rp_config cfg;
static rp_effect effect;
static float params[RP_MAX_PARAMS];
static rp_texture wallpaper;
static char wallpaper_loaded[PATH_MAX] = "\x01";   // forces the first load
static int wallpaper_side;
static const rp_backend *backend;
static int timer_fd = -1;
static bool outputs_ready;
static bool on_battery;
static bool pause_dirty;            // pause state may have changed outside the timer tick

// Changes made with `scrim msg` (newest of all sources) until `reload` or a restart.
static struct {
    int paused;                     // -1: follow the config file
    char effect[64];
    int nvalues;
    rp_param_value values[64];
} runtime = { .paused = -1 };

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static double timer_interval;

static void set_timer(double interval) {
    timer_interval = interval;
    struct itimerspec its = {0};
    its.it_interval.tv_sec = (time_t)interval;
    its.it_interval.tv_nsec = (long)((interval - (double)(time_t)interval) * 1e9);
    its.it_value = its.it_interval;
    timerfd_settime(timer_fd, 0, &its, NULL);
}

// Re-arms the frame timer only when the interval changes (re-arming resets its phase).
static void set_timer_if_changed(double interval) {
    if (interval == timer_interval) return;
    timer_interval = interval;
    set_timer(interval);
}

static void make_current_any(void) {
    eglMakeCurrent(rp_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, rp_egl_context);
}

static void write_template(const char *path) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *d = dirname(dir);
    char partial[PATH_MAX] = "";
    for (char *tok = strtok(d, "/"); tok; tok = strtok(NULL, "/")) {   // mkdir -p
        strncat(partial, "/", sizeof partial - strlen(partial) - 1);
        strncat(partial, tok, sizeof partial - strlen(partial) - 1);
        mkdir(partial, 0755);
    }
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "# Scrim settings. Changes apply as soon as you save this file.\n"
        "# Run `scrim --list` to see every effect and its parameters.\n\n"
        "# effect = rain\n# fps = 30\n# hidpi = false\n# pause_on_battery = true\n"
        "# paused = false   (also: `scrim msg pause|resume|toggle`, or kill -USR1)\n"
        "# wallpaper = ~/Pictures/your-image.jpg   (default: GNOME's setting, if any)\n\n");
    if (effect.nparams) {
        fprintf(f, "# [%s]\n", effect.id);
        for (int i = 0; i < effect.nparams; i++) {
            const rp_param *p = &effect.params[i];
            if (p->type == RP_PARAM_BOOL)
                fprintf(f, "# %s = %s\n", p->id, p->def > 0.5f ? "true" : "false");
            else
                fprintf(f, "# %-12s = %-6g # %s, %g..%g\n", p->id, p->def, p->label, p->min, p->max);
        }
    }
    fclose(f);
    rp_log("wrote a settings template to %s", path);
}

static int max_output_side(void) {
    int side = 0;
    for (int i = 0; i < backend->count(); i++) {
        rp_output *o = backend->output(i);
        if (!o) continue;
        if (o->render_w > side) side = o->render_w;
        if (o->render_h > side) side = o->render_h;
    }
    return side > 0 ? side : 2048;
}

static void config_file_path(char *path, size_t len) {
    if (cli.config_path[0]) snprintf(path, len, "%s", cli.config_path);
    else rp_config_path(path, len);
}

// Defaults, then the config file, then the command line, then `scrim msg` changes.
static void merge_settings(rp_config *next) {
    static char file_effect[64];
    static bool file_paused, file_seen;
    char path[PATH_MAX], err[1024];
    config_file_path(path, sizeof path);
    rp_config_defaults(next);
    if (!rp_config_load(next, path, err, sizeof err)) rp_log("%s", err);
    // Changing a setting in the file wins over an earlier `scrim msg` change to it.
    if (file_seen && strcmp(next->effect, file_effect) != 0) runtime.effect[0] = 0;
    if (file_seen && next->paused != file_paused) runtime.paused = -1;
    snprintf(file_effect, sizeof file_effect, "%s", next->effect);
    file_paused = next->paused;
    file_seen = true;
    if (cli.effect[0]) snprintf(next->effect, sizeof next->effect, "%s", cli.effect);
    if (cli.fps > 0) next->fps = cli.fps < 120 ? cli.fps : 120;
    if (cli.hidpi >= 0) next->hidpi = cli.hidpi;
    if (cli.pause_on_battery >= 0) next->pause_on_battery = cli.pause_on_battery;
    if (cli.wallpaper[0]) snprintf(next->wallpaper, sizeof next->wallpaper, "%s", cli.wallpaper);
    for (int i = 0; i < cli.nvalues; i++) rp_config_set_value(next, next->effect, cli.values[i][0], cli.values[i][1]);
    if (runtime.effect[0]) snprintf(next->effect, sizeof next->effect, "%s", runtime.effect);
    for (int i = 0; i < runtime.nvalues; i++)
        rp_config_set_value(next, runtime.values[i].section, runtime.values[i].key, runtime.values[i].value);
}

// Applies merged settings: effect, params, wallpaper, frame rate.
static bool apply_settings(bool first) {
    char path[PATH_MAX], err[1024];
    config_file_path(path, sizeof path);
    rp_config next;
    merge_settings(&next);
    if (!first && next.hidpi != cfg.hidpi) rp_log("hidpi changes take effect after a restart");

    make_current_any();
    if (first || strcmp(next.effect, effect.id) != 0) {
        rp_effect e;
        if (!rp_effect_find(&e, next.effect, err, sizeof err) || !rp_effect_compile(&e, err, sizeof err)) {
            rp_log("%s", err);
            if (first) return false;
            rp_log("keeping effect '%s'", effect.id);
            snprintf(next.effect, sizeof next.effect, "%s", effect.id);
        } else {
            if (effect.compiled) rp_effect_release(&effect);
            effect = e;
            rp_log("effect: %s%s%s%s", effect.name, *effect.license ? " [" : "", effect.license,
                   *effect.license ? "]" : "");
        }
    }
    cfg = next;
    rp_config_params(&cfg, &effect, params);
    if (first && access(path, F_OK) != 0 && !cli.config_path[0]) write_template(path);

    if (!outputs_ready) return true;   // the wallpaper is decoded at the outputs' size
    char wp[PATH_MAX];
    if (cfg.wallpaper[0]) snprintf(wp, sizeof wp, "%s", cfg.wallpaper);
    else rp_wallpaper_detect(wp, sizeof wp);
    int side = max_output_side();
    if (strcmp(wp, wallpaper_loaded) != 0 || side != wallpaper_side) {
        rp_wallpaper_release(&wallpaper);
        if (!rp_wallpaper_load(&wallpaper, wp, side) && !wp[0])
            rp_log("no wallpaper set: add `wallpaper = /path/to/image.jpg` to %s", path);
        snprintf(wallpaper_loaded, sizeof wallpaper_loaded, "%s", wp);
        wallpaper_side = side;
    }

    for (int i = 0; i < backend->count(); i++) {
        rp_output *o = backend->output(i);
        if (o) o->needs_redraw = true;
    }
    if (timer_fd >= 0) set_timer(1.0 / cfg.fps);
    return true;
}

// ---- scrim msg -----------------------------------------------------------------

static bool user_paused(void) { return runtime.paused >= 0 ? runtime.paused : cfg.paused; }

static void set_user_paused(bool paused) {
    runtime.paused = paused;
    pause_dirty = true;
}

// Switches to effect `id` for this session. False (and a reply) if it can't load.
static bool switch_effect(const char *id, char *reply, size_t len) {
    rp_effect probe;
    char err[1024];
    if (!rp_effect_find(&probe, id, err, sizeof err)) { snprintf(reply, len, "error: %s\n", err); return false; }
    snprintf(runtime.effect, sizeof runtime.effect, "%s", probe.id);
    apply_settings(false);
    if (strcmp(effect.id, probe.id) != 0) {
        runtime.effect[0] = 0;
        snprintf(reply, len, "error: effect '%s' failed to load (see Scrim's log)\n", id);
        return false;
    }
    snprintf(reply, len, "effect %s\n", effect.id);
    return true;
}

static void handle_command(const char *line, char *reply, size_t len) {
    char cmd[32] = "", arg[256] = "";
    sscanf(line, "%31s %255[^\n]", cmd, arg);
    if (!strcmp(cmd, "pause") || !strcmp(cmd, "resume") || !strcmp(cmd, "toggle")) {
        bool p = !strcmp(cmd, "pause") ? true : !strcmp(cmd, "resume") ? false : !user_paused();
        set_user_paused(p);
        snprintf(reply, len, "%s\n", p ? "paused" : cfg.pause_on_battery && on_battery ? "running (but paused on battery)" : "running");
    } else if (!strcmp(cmd, "effect") && arg[0]) {
        switch_effect(arg, reply, len);
    } else if (!strcmp(cmd, "next")) {
        char ids[64][64];
        int n = rp_effect_ids(ids, 64), at = 0;
        for (int i = 0; i < n; i++) if (!strcmp(ids[i], effect.id)) at = i;
        // Skip effects that fail to load rather than getting stuck on them.
        for (int k = 1; k <= n; k++)
            if (n && switch_effect(ids[(at + k) % n], reply, len)) return;
        if (!reply[0]) snprintf(reply, len, "error: no other effect could be loaded\n");
    } else if (!strcmp(cmd, "param") && strchr(arg, '=')) {
        char *eq = strchr(arg, '=');
        *eq = 0;
        char key[64], value[64];
        snprintf(key, sizeof key, "%.63s", arg);
        snprintf(value, sizeof value, "%.63s", eq + 1);
        int index = -1;
        for (int i = 0; i < effect.nparams; i++) if (!strcmp(effect.params[i].id, key)) index = i;
        if (index < 0) { snprintf(reply, len, "error: effect '%s' has no parameter '%s'\n", effect.id, key); return; }
        int slot = runtime.nvalues;
        for (int i = 0; i < runtime.nvalues; i++)
            if (!strcmp(runtime.values[i].section, effect.id) && !strcmp(runtime.values[i].key, key)) slot = i;
        if (slot == (int)(sizeof runtime.values / sizeof *runtime.values)) { snprintf(reply, len, "error: too many changes, use reload\n"); return; }
        if (slot == runtime.nvalues) runtime.nvalues++;
        snprintf(runtime.values[slot].section, sizeof runtime.values[slot].section, "%s", effect.id);
        snprintf(runtime.values[slot].key, sizeof runtime.values[slot].key, "%s", key);
        snprintf(runtime.values[slot].value, sizeof runtime.values[slot].value, "%s", value);
        apply_settings(false);
        snprintf(reply, len, "%s = %g\n", key, params[index]);
    } else if (!strcmp(cmd, "reload")) {
        runtime.paused = -1;
        runtime.effect[0] = 0;
        runtime.nvalues = 0;
        pause_dirty = true;
        apply_settings(false);
        snprintf(reply, len, "reloaded\n");
    } else if (!strcmp(cmd, "status")) {
        size_t n = 0;
        const char *state = user_paused() ? "paused" : cfg.pause_on_battery && on_battery ? "paused (on battery)" : "running";
        n += (size_t)snprintf(reply + n, len - n, "effect: %s (%s)\nstate: %s\nfps: %d\nwallpaper: %s\n",
                              effect.id, effect.name, state, cfg.fps, wallpaper_loaded[0] ? wallpaper_loaded : "(none)");
        for (int i = 0; i < effect.nparams && n < len; i++) {
            const rp_param *q = &effect.params[i];
            if (q->type == RP_PARAM_BOOL) n += (size_t)snprintf(reply + n, len - n, "%s = %s\n", q->id, params[i] > 0.5f ? "true" : "false");
            else n += (size_t)snprintf(reply + n, len - n, "%s = %g\n", q->id, params[i]);
        }
    } else {
        snprintf(reply, len, "error: unknown command '%s' (run `scrim msg` for the list)\n", line);
    }
}

// ---- EGL ---------------------------------------------------------------------

bool rp_egl_init(EGLDisplay display, bool windows, const char *via, char *err, size_t errlen) {
    rp_egl_display = display;
    EGLint major, minor;
    if (rp_egl_display == EGL_NO_DISPLAY || !eglInitialize(rp_egl_display, &major, &minor)) {
        snprintf(err, errlen, "EGL initialisation failed (0x%x)", eglGetError());
        return false;
    }
    const char *ext = eglQueryString(rp_egl_display, EGL_EXTENSIONS);
    if (!ext || !strstr(ext, "EGL_KHR_surfaceless_context")) {
        snprintf(err, errlen, "EGL_KHR_surfaceless_context is required");
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {
        EGL_SURFACE_TYPE, windows ? EGL_WINDOW_BIT : 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(rp_egl_display, attrs, &rp_egl_config, 1, &n) || n < 1) {
        snprintf(err, errlen, "no EGL config with OpenGL ES 3");
        return false;
    }
    const EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    rp_egl_context = eglCreateContext(rp_egl_display, rp_egl_config, EGL_NO_CONTEXT, ctx_attrs);
    if (rp_egl_context == EGL_NO_CONTEXT) {
        snprintf(err, errlen, "cannot create an OpenGL ES 3 context (0x%x)", eglGetError());
        return false;
    }
    make_current_any();
    rp_log("%s via %s, EGL %d.%d", (const char *)glGetString(GL_RENDERER), via, major, minor);
    return true;
}

long rp_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "VmRSS: %ld", &kb) == 1) break;
    fclose(f);
    return kb;
}

// ---- main loop -----------------------------------------------------------------

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "msg")) return rp_control_client(argc - 2, argv + 2);
    if (!parse_cli(argc, argv, &cli)) return 2;
    if (cli.snapshot) {
        rp_snapshot_options so = {
            .path = cli.snapshot, .effect = cli.effect[0] ? cli.effect : "rain",
            .wallpaper = cli.wallpaper, .time = cli.time, .fps = cli.fps > 0 ? cli.fps : 30,
            .width = cli.width, .height = cli.height, .nvalues = cli.nvalues, .values = cli.values,
        };
        return rp_snapshot_run(&so);
    }

    int control_fd = rp_control_listen();
    if (control_fd == RP_CONTROL_RUNNING) {
        // Exit cleanly, so a systemd unit with Restart=on-failure doesn't keep retrying.
        rp_log("Scrim is already running; control it with `scrim msg`");
        return 0;
    }

    char err[1024] = "";
    rp_config early;
    merge_settings(&early);   // the display connection needs the HiDPI choice up front

    const char *want = cli.backend ? cli.backend : getenv("WAYLAND_DISPLAY") ? "wayland" : "x11";
    backend = !strcmp(want, "wayland") ? &rp_wayland_backend : &rp_x11_backend;
    if (!backend->init(early.hidpi, err, sizeof err)) {
        rp_log("%s: %s", backend->name, err);
        // Fall back to X11 (e.g. XWayland on GNOME) unless the user forced a backend.
        if (cli.backend || backend == &rp_x11_backend || !getenv("DISPLAY")) return 1;
        backend = &rp_x11_backend;
        if (!backend->init(early.hidpi, err, sizeof err)) { rp_log("x11: %s", err); return 1; }
        rp_log("falling back to X11 — the effect may sit above the desktop's own wallpaper window");
    }
    if (!rp_egl_init(backend->egl_display(), true, backend->name, err, sizeof err) ||
        !rp_render_init(err, sizeof err)) {
        rp_log("%s", err);
        return 1;
    }

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (!apply_settings(true)) return 1;   // compiles the effect
    backend->create_outputs(rp_egl_config);
    outputs_ready = true;
    apply_settings(false);                 // loads the wallpaper at the outputs' size

    // Reload settings when the config file (or its directory entry) changes.
    char cfg_path[PATH_MAX], cfg_dir[PATH_MAX];
    if (cli.config_path[0]) snprintf(cfg_path, sizeof cfg_path, "%s", cli.config_path);
    else rp_config_path(cfg_path, sizeof cfg_path);
    snprintf(cfg_dir, sizeof cfg_dir, "%s", cfg_path);
    char *cfg_name = strrchr(cfg_path, '/') ? strrchr(cfg_path, '/') + 1 : cfg_path;
    int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino >= 0 && inotify_add_watch(ino, dirname(cfg_dir), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
        close(ino);
        ino = -1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

    double start = now_seconds(), last = start, stats_at = start + 5, battery_at = 0;
    double time = 0, frame = 0;
    bool was_paused = false;
    const char *pause_reason = "";
    long frames_drawn = 0;
    set_timer(1.0 / cfg.fps);

    for (bool running = true; running;) {
        backend->prepare_poll();
        struct pollfd fds[5] = {
            { .fd = backend->fd(), .events = POLLIN }, { .fd = timer_fd, .events = POLLIN },
            { .fd = ino, .events = POLLIN }, { .fd = sig_fd, .events = POLLIN },
            { .fd = control_fd, .events = POLLIN },
        };
        int r = poll(fds, 5, -1);
        backend->after_poll(r > 0 && (fds[0].revents & POLLIN));
        if (r < 0 && errno != EINTR) break;

        if (fds[3].revents & POLLIN) {
            struct signalfd_siginfo si;
            if (read(sig_fd, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo == SIGHUP) apply_settings(false);
                else if (si.ssi_signo == SIGUSR1) set_user_paused(!user_paused());
                else running = false;
            }
        }
        if (ino >= 0 && (fds[2].revents & POLLIN)) {
            char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t len;
            bool hit = false;
            while ((len = read(ino, buf, sizeof buf)) > 0)
                for (char *p = buf; p < buf + len;) {
                    struct inotify_event *ev = (struct inotify_event *)p;
                    if (ev->len && !strcmp(ev->name, cfg_name)) hit = true;
                    p += sizeof *ev + ev->len;
                }
            if (hit) { rp_log("settings changed, reloading"); apply_settings(false); pause_dirty = true; }
        }
        if (control_fd >= 0 && (fds[4].revents & POLLIN)) rp_control_accept(control_fd, handle_command);
        if (pause_dirty) {   // react now instead of on the next (possibly 1 s) tick
            pause_dirty = false;
            set_timer(0.001);
        }
        if (!(fds[1].revents & POLLIN)) continue;
        uint64_t expirations;
        if (read(timer_fd, &expirations, sizeof expirations) < 0) continue;

        double now = now_seconds();
        if (now >= battery_at) { on_battery = rp_on_battery(); battery_at = now + 5; }
        const char *reason = user_paused() ? "paused" : cfg.pause_on_battery && on_battery ? "on battery: paused" : "";
        bool paused = reason[0] != 0;
        if (strcmp(reason, pause_reason) != 0) {
            rp_log("%s", paused ? reason : "resumed");
            pause_reason = reason;
            if (paused != was_paused) last = now;   // don't count paused time as one long frame
            was_paused = paused;
        }
        // Paused: wake once a second to re-check. Also restores the rate after set_timer(0.001).
        set_timer_if_changed(paused ? 1.0 : 1.0 / cfg.fps);
        double delta = fmin(fmax(now - last, 0), 0.1);
        last = now;

        bool advanced = false;
        for (int i = 0; i < backend->count(); i++) {
            rp_output *o = backend->output(i);
            if (!o || !o->configured) continue;
            if (!((o->can_draw && !paused) || o->needs_redraw)) continue;
            eglMakeCurrent(rp_egl_display, o->egl_surface, o->egl_surface, rp_egl_context);
            if (!advanced && !paused) {
                time += delta;
                if (time > 20000) time = 0;   // keep float precision (spec allows resets)
                frame += 1;
                advanced = true;
            }
            rp_render_frame(&effect, &o->targets, &wallpaper, params, time, frame, delta,
                            !paused, 0, o->render_w, o->render_h);
            backend->before_swap(o);
            eglSwapBuffers(rp_egl_display, o->egl_surface);
            o->needs_redraw = false;
            frames_drawn++;
        }

        if (cli.stats && now >= stats_at) {
            rp_log("stats: %.1f frames/s (all outputs)%s, RSS %ld MB",
                   frames_drawn / (now - stats_at + 5), paused ? ", paused" : "", rp_rss_kb() / 1024);
            frames_drawn = 0;
            stats_at = now + 5;
        }
        if (cli.duration > 0 && now - start >= cli.duration) running = false;
    }

    rp_control_close(control_fd);
    make_current_any();
    rp_effect_release(&effect);
    rp_wallpaper_release(&wallpaper);
    backend->shutdown();
    eglMakeCurrent(rp_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(rp_egl_display, rp_egl_context);
    eglTerminate(rp_egl_display);   // must happen before the display connection closes
    backend->disconnect();
    return 0;
}
