// Wayland backend: one wlr-layer-shell surface per output on the BACKGROUND layer
// (KDE Plasma, Sway, Hyprland, river, labwc, Wayfire, niri, ...). GNOME does not
// implement layer-shell, so it is unsupported there.
//
// Pacing: we draw only after the compositor's frame callback, so when an output
// is hidden, off or locked the compositor stops asking and rendering stops by itself.
#include "rp.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>

typedef struct {
    rp_output base;
    uint32_t global_name;
    struct wl_output *wl_output;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wl_egl_window *egl_window;
    struct wl_callback *frame_cb;
    bool in_use;
} wl_out;

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct zwlr_layer_shell_v1 *layer_shell;
static wl_out outputs[RP_MAX_OUTPUTS];
static bool hidpi;
static bool egl_ready;

static void update_render_size(wl_out *o) {
    int s = hidpi ? o->base.scale : 1;
    o->base.render_w = o->base.width * s;
    o->base.render_h = o->base.height * s;
    if (o->surface) wl_surface_set_buffer_scale(o->surface, s);
    if (o->egl_window) wl_egl_window_resize(o->egl_window, o->base.render_w, o->base.render_h, 0, 0);
}

// ---- frame callbacks ----

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)time;
    wl_out *o = data;
    wl_callback_destroy(cb);
    o->frame_cb = NULL;
    o->base.can_draw = true;
}
static const struct wl_callback_listener frame_listener = { .done = frame_done };

// ---- layer surface ----

static void destroy_surface(wl_out *o) {
    if (o->frame_cb) { wl_callback_destroy(o->frame_cb); o->frame_cb = NULL; }
    if (o->base.egl_surface) {
        eglMakeCurrent(rp_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, rp_egl_context);
        rp_targets_release(&o->base.targets);
        eglDestroySurface(rp_egl_display, o->base.egl_surface);
        o->base.egl_surface = EGL_NO_SURFACE;
    }
    if (o->egl_window) { wl_egl_window_destroy(o->egl_window); o->egl_window = NULL; }
    if (o->layer) { zwlr_layer_surface_v1_destroy(o->layer); o->layer = NULL; }
    if (o->surface) { wl_surface_destroy(o->surface); o->surface = NULL; }
    o->base.configured = false;
    o->base.can_draw = false;
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
                            uint32_t width, uint32_t height) {
    wl_out *o = data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    bool changed = (int)width != o->base.width || (int)height != o->base.height;
    o->base.width = (int)width;
    o->base.height = (int)height;
    if (!o->egl_window) {
        int s = hidpi ? o->base.scale : 1;
        o->egl_window = wl_egl_window_create(o->surface, (int)width * s, (int)height * s);
        o->base.egl_surface = rp_egl_create_surface(o->egl_window);
        if (o->base.egl_surface == EGL_NO_SURFACE) { rp_log("wayland: cannot create EGL surface"); return; }
    }
    update_render_size(o);
    o->base.configured = true;
    o->base.can_draw = true;
    if (changed) o->base.needs_redraw = true;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
    (void)layer;
    destroy_surface(data);   // output went away or the compositor closed us
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void create_surface(wl_out *o) {
    if (o->surface || !layer_shell || !compositor) return;
    o->surface = wl_compositor_create_surface(compositor);
    // An empty input region makes the surface click-through.
    struct wl_region *empty = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(o->surface, empty);
    wl_region_destroy(empty);

    o->layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, o->surface, o->wl_output,
                                                     ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "scrim");
    zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
    zwlr_layer_surface_v1_set_size(o->layer, 0, 0);
    zwlr_layer_surface_v1_set_anchor(o->layer,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);   // cover panels' reserved space too
    zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer, 0);
    wl_surface_commit(o->surface);
}

// ---- wl_output ----

static void output_geometry(void *d, struct wl_output *w, int32_t x, int32_t y, int32_t pw, int32_t ph,
                            int32_t sub, const char *make, const char *model, int32_t tr) {
    (void)d; (void)w; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)tr;
}
static void output_mode(void *d, struct wl_output *w, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)d; (void)w; (void)flags; (void)width; (void)height; (void)refresh;
}
static void output_done(void *d, struct wl_output *w) { (void)d; (void)w; }
static void output_scale(void *data, struct wl_output *w, int32_t factor) {
    (void)w;
    wl_out *o = data;
    if (o->base.scale != factor) {
        o->base.scale = factor;
        if (o->base.configured) { update_render_size(o); o->base.needs_redraw = true; }
    }
}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

// ---- registry ----

static void registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
    (void)data;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        for (int i = 0; i < RP_MAX_OUTPUTS; i++) {
            wl_out *o = &outputs[i];
            if (o->in_use) continue;
            memset(o, 0, sizeof *o);
            o->in_use = true;
            o->global_name = name;
            o->base.scale = 1;
            o->base.native = o;
            o->wl_output = wl_registry_bind(reg, name, &wl_output_interface, version < 2 ? version : 2);
            wl_output_add_listener(o->wl_output, &output_listener, o);
            if (egl_ready) create_surface(o);   // hot-plugged after startup
            return;
        }
        rp_log("wayland: too many outputs, ignoring one");
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg;
    for (int i = 0; i < RP_MAX_OUTPUTS; i++) {
        wl_out *o = &outputs[i];
        if (!o->in_use || o->global_name != name) continue;
        destroy_surface(o);
        wl_output_destroy(o->wl_output);
        memset(o, 0, sizeof *o);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// ---- backend interface ----

static bool wl_init(bool want_hidpi, char *err, size_t errlen) {
    hidpi = want_hidpi;
    display = wl_display_connect(NULL);
    if (!display) { snprintf(err, errlen, "cannot connect to a Wayland display"); return false; }
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);   // globals
    wl_display_roundtrip(display);   // output scale events
    if (!compositor) { snprintf(err, errlen, "Wayland compositor has no wl_compositor"); return false; }
    if (!layer_shell) {
        snprintf(err, errlen, "this compositor doesn't support wlr-layer-shell "
                              "(GNOME doesn't; KDE, Sway, Hyprland and others do)");
        return false;
    }
    return true;
}

static EGLDisplay wl_egl_display(void) {
    return eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, NULL);
}

static void wl_create_outputs(EGLConfig config) {
    (void)config;
    egl_ready = true;
    for (int i = 0; i < RP_MAX_OUTPUTS; i++)
        if (outputs[i].in_use) create_surface(&outputs[i]);
    wl_display_roundtrip(display);   // first configure events
}

static int wl_fd(void) { return wl_display_get_fd(display); }

static void wl_prepare_poll(void) {
    while (wl_display_prepare_read(display) != 0) wl_display_dispatch_pending(display);
    wl_display_flush(display);
}

static void wl_after_poll(bool readable) {
    if (readable) wl_display_read_events(display);
    else wl_display_cancel_read(display);
    wl_display_dispatch_pending(display);
}

static int wl_count(void) { return RP_MAX_OUTPUTS; }

static rp_output *wl_output_at(int i) {
    return outputs[i].in_use && outputs[i].surface ? &outputs[i].base : NULL;
}

static void wl_before_swap(rp_output *base) {
    wl_out *o = base->native;
    // The callback fires when the compositor wants the next frame from us. It is
    // attached to the commit made by eglSwapBuffers.
    if (!o->frame_cb) {
        o->frame_cb = wl_surface_frame(o->surface);
        wl_callback_add_listener(o->frame_cb, &frame_listener, o);
    }
    base->can_draw = false;
}

static void wl_shutdown(void) {
    for (int i = 0; i < RP_MAX_OUTPUTS; i++) {
        if (!outputs[i].in_use) continue;
        destroy_surface(&outputs[i]);
        wl_output_destroy(outputs[i].wl_output);
    }
    if (layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if (compositor) wl_compositor_destroy(compositor);
    if (registry) wl_registry_destroy(registry);
    layer_shell = NULL;
    compositor = NULL;
    registry = NULL;
}

static void wl_disconnect(void) {
    if (display) wl_display_disconnect(display);
    display = NULL;
}

const rp_backend rp_wayland_backend = {
    .name = "wayland",
    .init = wl_init,
    .egl_display = wl_egl_display,
    .create_outputs = wl_create_outputs,
    .fd = wl_fd,
    .prepare_poll = wl_prepare_poll,
    .after_poll = wl_after_poll,
    .count = wl_count,
    .output = wl_output_at,
    .before_swap = wl_before_swap,
    .shutdown = wl_shutdown,
    .disconnect = wl_disconnect,
};
