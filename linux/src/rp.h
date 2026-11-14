// Scrim for Linux — shared declarations.
// Implements spec/effect-format.md (formats 1 and 2) with EGL + OpenGL ES 3.0.
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#define RP_MAX_PASSES 8
#define RP_MAX_PARAMS 16
#define RP_MAX_OUTPUTS 16

// ---- effects (effect.c) ----------------------------------------------------

typedef enum { RP_PARAM_FLOAT, RP_PARAM_BOOL } rp_param_type;

typedef struct {
    char id[64];
    char label[128];
    rp_param_type type;
    float min, max, def;
} rp_param;

typedef struct {
    char stem[128];
    float scale;
    bool persistent;            // keeps its output between frames (format 2)
    bool half_float;            // pixelFormat "rgba16f" (format 2)
    GLuint program;
    GLint loc_wallpaper, loc_previous, loc_feedback, loc_flip;
} rp_pass;

typedef struct {
    char id[64];
    char name[128];
    char license[128];
    char dir[PATH_MAX];
    unsigned generation;        // unique per load; render targets are rebuilt when it changes
    int npasses;
    rp_pass passes[RP_MAX_PASSES];
    int nparams;
    rp_param params[RP_MAX_PARAMS];
    bool compiled;
} rp_effect;

// Effect directories in priority order (highest first). Returns the count.
int rp_effect_dirs(char dirs[][PATH_MAX], int max);
bool rp_effect_load(rp_effect *e, const char *dir, char *err, size_t errlen);
bool rp_effect_find(rp_effect *e, const char *id, char *err, size_t errlen);
// Sorted, unique ids of every loadable effect. Returns the count.
int rp_effect_ids(char ids[][64], int max);
void rp_effect_print_all(void);
// Needs a current GL context. Compiles every pass from build/<stem>.gles.
bool rp_effect_compile(rp_effect *e, char *err, size_t errlen);
void rp_effect_release(rp_effect *e);

// ---- rendering (render.c) --------------------------------------------------

typedef struct {
    GLuint tex;
    int width, height, levels;
} rp_texture;

// Per-output intermediate targets for the passes before the last one. Persistent
// passes have two targets: each step reads [cur] as uFeedback and writes the other.
typedef struct {
    int width, height;              // render size of the last pass
    int npasses;
    unsigned generation;            // rp_effect.generation these were built for
    GLuint fbo[RP_MAX_PASSES][2], tex[RP_MAX_PASSES][2];
    int tw[RP_MAX_PASSES], th[RP_MAX_PASSES];
    int cur;                        // persistent passes: index holding the latest state
    double steps;                   // persistent steps since the state was cleared
    double last_step_time;          // rendered time of the last step on this output
} rp_targets;

bool rp_render_init(char *err, size_t errlen);          // UBO, VAO; needs a context
// True if the GL context can render into RGBA16F textures (pixelFormat "rgba16f").
bool rp_render_half_float(void);
// `step`: advance persistent passes. False when redrawing without new time (paused);
// the last state is then reused. The first frame after a reset always steps.
// `target`: framebuffer for the last pass (0 = the current window surface).
void rp_render_frame(const rp_effect *e, rp_targets *t, const rp_texture *wallpaper,
                     const float *params, double time, double frame, double delta,
                     bool step, GLuint target, int width, int height);
void rp_targets_release(rp_targets *t);
GLuint rp_compile_program(const char *fragment_source, char *err, size_t errlen);

// ---- wallpaper (wallpaper.c) -----------------------------------------------

// Decodes a JPEG/PNG so its longest side is at most max_side, uploads it with a
// full mip chain. Falls back to a dark gradient (returns false) when unreadable.
bool rp_wallpaper_load(rp_texture *out, const char *path, int max_side);
void rp_wallpaper_release(rp_texture *t);
// Best-effort detection of the desktop's own wallpaper (GNOME gsettings). Empty if none.
void rp_wallpaper_detect(char *out, size_t len);

// ---- config (config.c) -----------------------------------------------------

typedef struct {
    char section[64], key[64];
    char value[64];
} rp_param_value;

typedef struct {
    char effect[64];
    int fps;
    bool hidpi;
    bool pause_on_battery;
    bool paused;                // paused by the user (config `paused`)
    char wallpaper[PATH_MAX];
    int nvalues;
    rp_param_value values[128];
} rp_config;

void rp_config_defaults(rp_config *c);
void rp_config_path(char *out, size_t len);
// Merges the file into c. Missing file is not an error.
bool rp_config_load(rp_config *c, const char *path, char *err, size_t errlen);
void rp_config_set_value(rp_config *c, const char *section, const char *key, const char *value);
// Fills params[] (manifest order) from defaults + config, clamped to each range.
void rp_config_params(const rp_config *c, const rp_effect *e, float *params);

// ---- control socket (control.c) -------------------------------------------

#define RP_CONTROL_RUNNING (-2)   // rp_control_listen: another instance owns the socket
typedef void (*rp_control_handler)(const char *command, char *reply, size_t len);
void rp_control_path(char *out, size_t len);
// Listening socket for `scrim msg` (non-blocking), -1 if unavailable, or RP_CONTROL_RUNNING.
int rp_control_listen(void);
// Serves every pending client: one command line each, answered by `handler`.
void rp_control_accept(int listen_fd, rp_control_handler handler);
void rp_control_close(int listen_fd);
// `scrim msg ARGS...`: sends a command to the running instance. Returns an exit code.
int rp_control_client(int argc, char **argv);

// ---- power (power.c) -------------------------------------------------------

bool rp_on_battery(void);

// ---- display backends (wayland.c, x11.c) -----------------------------------

typedef struct rp_output {
    int width, height;          // size in surface coordinates
    int scale;                  // buffer scale (Wayland HiDPI); 1 on X11
    int render_w, render_h;     // pixels we render
    EGLSurface egl_surface;
    bool configured;            // has a size and an EGL surface
    bool can_draw;              // compositor wants a frame (Wayland) / not obscured (X11)
    bool needs_redraw;          // must draw even while paused (expose, resize)
    rp_targets targets;
    void *native;
} rp_output;

typedef struct {
    const char *name;
    bool (*init)(bool hidpi, char *err, size_t errlen);
    EGLDisplay (*egl_display)(void);
    void (*create_outputs)(EGLConfig config);   // after EGL is ready
    int (*fd)(void);
    void (*prepare_poll)(void);
    void (*after_poll)(bool readable);
    int (*count)(void);
    rp_output *(*output)(int i);
    void (*before_swap)(rp_output *o);
    void (*shutdown)(void);     // destroys surfaces/windows (EGL still alive)
    void (*disconnect)(void);   // closes the display connection (after eglTerminate)
} rp_backend;

extern const rp_backend rp_wayland_backend;
extern const rp_backend rp_x11_backend;

// Shared EGL state (main.c); backends use it to create surfaces.
extern EGLDisplay rp_egl_display;
extern EGLConfig rp_egl_config;
extern EGLContext rp_egl_context;
EGLSurface rp_egl_create_surface(void *native_window);
// Initialises EGL on `display` and makes an OpenGL ES 3 context current without a
// surface. `windows`: the config must support window surfaces.
bool rp_egl_init(EGLDisplay display, bool windows, const char *via, char *err, size_t errlen);

// ---- snapshot (snapshot.c) -------------------------------------------------

typedef struct {
    const char *path, *effect, *wallpaper;
    double time;
    int fps, width, height;
    int nvalues;
    char (*values)[2][64];      // "id", "value" overrides
} rp_snapshot_options;

// Renders one frame offscreen with headless EGL and writes it as PNG. Returns an exit code.
int rp_snapshot_run(const rp_snapshot_options *o);
void rp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
long rp_rss_kb(void);   // resident memory in kB, -1 if unknown
