// scrim --snapshot: renders one frame offscreen with headless EGL and writes a PNG,
// for effect authoring and for tests (no display server needed). Mirrors the macOS
// app's --snapshot. Effects with persistent passes are simulated from t = 0 up to
// --time in fixed steps of 1/fps, so the result is the same on every run.
#include "rp.h"

#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// Prefers displays that need no window system: Mesa's surfaceless platform, then
// the first EGL device (e.g. NVIDIA), then whatever the default display is.
static EGLDisplay headless_display(const char **via) {
    const char *ext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (ext && strstr(ext, "EGL_MESA_platform_surfaceless")) {
        EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, (void *)EGL_DEFAULT_DISPLAY, NULL);
        if (d != EGL_NO_DISPLAY) { *via = "surfaceless EGL"; return d; }
    }
    if (ext && strstr(ext, "EGL_EXT_platform_device")) {
        PFNEGLQUERYDEVICESEXTPROC query_devices = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
        EGLDeviceEXT devices[8];
        EGLint n = 0;
        if (query_devices && query_devices(8, devices, &n) && n > 0) {
            EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[0], NULL);
            if (d != EGL_NO_DISPLAY) { *via = "EGL device"; return d; }
        }
    }
    *via = "default EGL display";
    return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

static int compare_doubles(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int fail(const char *msg) {
    fprintf(stderr, "snapshot: %s\n", msg);
    return 1;
}

int rp_snapshot_run(const rp_snapshot_options *o) {
    char err[1024];
    const char *via = "";
    EGLDisplay display = headless_display(&via);
    if (!rp_egl_init(display, false, via, err, sizeof err) || !rp_render_init(err, sizeof err)) return fail(err);

    rp_effect effect;
    if (!rp_effect_find(&effect, o->effect, err, sizeof err) || !rp_effect_compile(&effect, err, sizeof err))
        return fail(err);

    // Effect defaults plus --param overrides (not the config file, so results are reproducible).
    rp_config cfg;
    rp_config_defaults(&cfg);
    for (int i = 0; i < o->nvalues; i++) {
        bool known = false;
        for (int k = 0; k < effect.nparams; k++) known |= !strcmp(effect.params[k].id, o->values[i][0]);
        if (!known) rp_log("effect '%s' has no parameter '%s' (see --list)", effect.id, o->values[i][0]);
        rp_config_set_value(&cfg, effect.id, o->values[i][0], o->values[i][1]);
    }
    float params[RP_MAX_PARAMS] = {0};
    rp_config_params(&cfg, &effect, params);

    char wallpaper_path[PATH_MAX];
    if (o->wallpaper && o->wallpaper[0]) snprintf(wallpaper_path, sizeof wallpaper_path, "%s", o->wallpaper);
    else rp_wallpaper_detect(wallpaper_path, sizeof wallpaper_path, NULL);
    rp_texture wallpaper;
    int w = o->width, h = o->height;
    if (!rp_wallpaper_load(&wallpaper, wallpaper_path, w > h ? w : h))
        printf("note: no readable wallpaper, using the fallback gradient\n");

    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail("cannot create the offscreen image");

    bool persistent = false;
    for (int i = 0; i < effect.npasses; i++) persistent |= effect.passes[i].persistent;
    double dt = 1.0 / o->fps;
    long steps = persistent ? lround(o->time * o->fps) : 0;   // stateless effects: one frame at --time
    rp_targets targets;
    memset(&targets, 0, sizeof targets);

    double start = now_ms();
    long frame = 0;
    for (long k = 0; k <= steps; k++, frame++) {
        double t = persistent ? (double)k * dt : o->time;
        rp_render_frame(&effect, &targets, &wallpaper, params, t, (double)frame + 1, dt, true, fbo, w, h);
    }
    glFinish();
    double first_ms = now_ms() - start;

    uint8_t *pixels = malloc((size_t)w * h * 4);
    if (!pixels) return fail("out of memory");
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (size_t i = 3; i < (size_t)w * h * 4; i += 4) pixels[i] = 255;
    png_image img;
    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    img.width = (png_uint_32)w;
    img.height = (png_uint_32)h;
    img.format = PNG_FORMAT_RGBA;
    // GL rows are bottom-up; a negative stride tells libpng the buffer starts with the bottom row.
    if (!png_image_write_to_file(&img, o->path, 0, pixels, -(png_int_32)(w * 4), NULL)) {
        snprintf(err, sizeof err, "cannot write %s: %s", o->path, img.message);
        free(pixels);
        return fail(err);
    }
    free(pixels);
    if (persistent)
        printf("wrote %s (%dx%d, %ld steps of 1/%d s, %.2f ms incl. setup)\n", o->path, w, h, steps + 1, o->fps, first_ms);
    else
        printf("wrote %s (%dx%d, %.2f ms incl. setup)\n", o->path, w, h, first_ms);

    // Steady-state cost: more frames, median wall time per frame (CPU + GPU, as glFinish waits).
    enum { N = 60 };
    double times[N];
    for (int i = 0; i < N; i++, frame++) {
        double t = persistent ? (double)(steps + 1 + i) * dt : o->time + i * dt;
        double t0 = now_ms();
        rp_render_frame(&effect, &targets, &wallpaper, params, t, (double)frame + 1, dt, true, fbo, w, h);
        glFinish();
        times[i] = now_ms() - t0;
    }
    qsort(times, N, sizeof *times, compare_doubles);
    printf("steady state: median %.2f ms/frame over %d frames, RSS %ld MB\n", times[N / 2], N, rp_rss_kb() / 1024);

    rp_targets_release(&targets);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    rp_effect_release(&effect);
    rp_wallpaper_release(&wallpaper);
    eglMakeCurrent(rp_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(rp_egl_display, rp_egl_context);
    eglTerminate(rp_egl_display);
    return 0;
}
