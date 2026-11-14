// X11 backend: one click-through window per monitor (XRandR), typed as the
// desktop (_NET_WM_WINDOW_TYPE_DESKTOP) so window managers keep it below
// everything. Best with WMs that don't run their own desktop-icon window
// (i3, bspwm, Openbox, ...); a file manager drawing the desktop may cover it.
//
// An output stops drawing when it is fully obscured (VisibilityNotify; compositing
// WMs never report that), when the active window is fullscreen on it (EWMH, works
// under compositors too), or when DPMS has blanked the screens.
#include "rp.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/shape.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    rp_output base;
    Window window;
    int x, y;                   // position on the root window
    bool visible;               // not fully obscured (VisibilityNotify)
    bool fullscreen_covered;    // the active window is fullscreen over this output
    bool in_use;
} x_out;

static Display *dpy;
static Window root;
static int randr_event_base = -1;
static x_out outputs[RP_MAX_OUTPUTS];
static EGLConfig egl_config;
static Atom net_active_window, net_wm_state, net_wm_state_fullscreen;
static Window active_window = None;

// The active window can vanish between an event and our query of it; Xlib's
// default handler would exit the process on the resulting BadWindow.
static int ignore_x_errors(Display *d, XErrorEvent *e) {
    (void)d;
    if (e->error_code != BadWindow && e->error_code != BadDrawable && e->error_code != BadMatch)
        rp_log("x11: X error %d (request %d)", e->error_code, e->request_code);
    return 0;
}

static void update_can_draw(x_out *o) {
    bool before = o->base.can_draw;
    o->base.can_draw = o->visible && !o->fullscreen_covered;
    if (o->base.can_draw && !before) o->base.needs_redraw = true;
}

static bool window_is_fullscreen(Window w) {
    Atom type;
    int format;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, net_wm_state, 0, 64, False, AnyPropertyType, &type, &format, &n, &after,
                           &data) != Success || !data)
        return false;
    bool fullscreen = false;
    if (format == 32)
        for (unsigned long i = 0; i < n; i++) fullscreen |= ((Atom *)data)[i] == net_wm_state_fullscreen;
    XFree(data);
    return fullscreen;
}

// Pauses the outputs that the active window covers while it is fullscreen.
static void update_fullscreen(void) {
    int wx = 0, wy = 0;
    unsigned ww = 0, wh = 0;
    bool fullscreen = active_window != None && window_is_fullscreen(active_window);
    if (fullscreen) {
        XWindowAttributes a;
        Window child;
        if (XGetWindowAttributes(dpy, active_window, &a) &&
            XTranslateCoordinates(dpy, active_window, root, 0, 0, &wx, &wy, &child)) {
            ww = (unsigned)a.width;
            wh = (unsigned)a.height;
        } else {
            fullscreen = false;
        }
    }
    for (int i = 0; i < RP_MAX_OUTPUTS; i++) {
        x_out *o = &outputs[i];
        if (!o->in_use) continue;
        bool covered = false;
        if (fullscreen) {
            long ix = (long)(wx > o->x ? wx : o->x), iy = (long)(wy > o->y ? wy : o->y);
            long ex = (long)wx + (long)ww, ey = (long)wy + (long)wh;
            long ox = (long)o->x + o->base.width, oy = (long)o->y + o->base.height;
            long iw = (ex < ox ? ex : ox) - ix, ih = (ey < oy ? ey : oy) - iy;
            covered = iw > 0 && ih > 0 && iw * ih >= (long)o->base.width * o->base.height * 9 / 10;
        }
        if (covered != o->fullscreen_covered) {
            rp_log(covered ? "x11: output %d is covered by a fullscreen window: pausing it"
                           : "x11: output %d is no longer covered by a fullscreen window", i);
            o->fullscreen_covered = covered;
            update_can_draw(o);
        }
    }
}

// Follows _NET_ACTIVE_WINDOW, watching the active window for state/geometry changes.
static void track_active_window(void) {
    Atom type;
    int format;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    Window w = None;
    if (XGetWindowProperty(dpy, root, net_active_window, 0, 1, False, AnyPropertyType, &type, &format, &n,
                           &after, &data) == Success && data) {
        if (n == 1 && format == 32) w = (Window)((unsigned long *)data)[0];
        XFree(data);
    }
    if (w != active_window) {
        if (active_window != None) XSelectInput(dpy, active_window, NoEventMask);
        active_window = w;
        if (w != None) XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask);
    }
    update_fullscreen();
}

static void destroy_windows(void) {
    eglMakeCurrent(rp_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, rp_egl_context);
    for (int i = 0; i < RP_MAX_OUTPUTS; i++) {
        x_out *o = &outputs[i];
        if (!o->in_use) continue;
        rp_targets_release(&o->base.targets);
        if (o->base.egl_surface) eglDestroySurface(rp_egl_display, o->base.egl_surface);
        XDestroyWindow(dpy, o->window);
        memset(o, 0, sizeof *o);
    }
}

static void create_window(int idx, int x, int y, int w, int h) {
    x_out *o = &outputs[idx];
    memset(o, 0, sizeof *o);
    o->x = x;
    o->y = y;
    o->visible = true;

    EGLint visual_id = 0;
    eglGetConfigAttrib(rp_egl_display, egl_config, EGL_NATIVE_VISUAL_ID, &visual_id);
    XVisualInfo tmpl = { .visualid = (VisualID)visual_id }, *vi;
    int nvis = 0;
    vi = XGetVisualInfo(dpy, VisualIDMask, &tmpl, &nvis);
    Visual *visual = vi ? vi->visual : DefaultVisual(dpy, DefaultScreen(dpy));
    int depth = vi ? vi->depth : DefaultDepth(dpy, DefaultScreen(dpy));

    XSetWindowAttributes attrs = {0};
    attrs.colormap = XCreateColormap(dpy, root, visual, AllocNone);
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.event_mask = ExposureMask | StructureNotifyMask | VisibilityChangeMask;
    o->window = XCreateWindow(dpy, root, x, y, (unsigned)w, (unsigned)h, 0, depth, InputOutput, visual,
                              CWColormap | CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
    if (vi) XFree(vi);

    // Desktop type + below/sticky/skip hints, so WMs treat it as the wallpaper layer.
    Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(dpy, o->window, type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&desktop, 1);
    Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom states[] = {
        XInternAtom(dpy, "_NET_WM_STATE_BELOW", False),
        XInternAtom(dpy, "_NET_WM_STATE_STICKY", False),
        XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False),
        XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False),
    };
    XChangeProperty(dpy, o->window, state, XA_ATOM, 32, PropModeReplace, (unsigned char *)states, 4);
    long all_desktops = 0xFFFFFFFF;
    XChangeProperty(dpy, o->window, XInternAtom(dpy, "_NET_WM_DESKTOP", False), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&all_desktops, 1);
    XSizeHints hints = { .flags = USPosition | USSize, .x = x, .y = y, .width = w, .height = h };
    XSetWMNormalHints(dpy, o->window, &hints);
    XStoreName(dpy, o->window, "scrim");
    XClassHint cls = { .res_name = "scrim", .res_class = "Scrim" };
    XSetClassHint(dpy, o->window, &cls);

    // Empty input shape: clicks go straight through to whatever is below.
    XserverRegion empty = XFixesCreateRegion(dpy, NULL, 0);
    XFixesSetWindowShapeRegion(dpy, o->window, ShapeInput, 0, 0, empty);
    XFixesDestroyRegion(dpy, empty);

    XMapWindow(dpy, o->window);
    XLowerWindow(dpy, o->window);

    o->in_use = true;
    o->base.native = o;
    o->base.scale = 1;
    o->base.width = o->base.render_w = w;
    o->base.height = o->base.render_h = h;
    o->base.egl_surface = rp_egl_create_surface(&o->window);
    o->base.configured = o->base.egl_surface != EGL_NO_SURFACE;
    o->base.can_draw = true;
    o->base.needs_redraw = true;
}

static void create_windows(void) {
    int n = 0;
    XRRMonitorInfo *mons = randr_event_base >= 0 ? XRRGetMonitors(dpy, root, True, &n) : NULL;
    if (mons && n > 0) {
        for (int i = 0; i < n && i < RP_MAX_OUTPUTS; i++)
            create_window(i, mons[i].x, mons[i].y, mons[i].width, mons[i].height);
    } else {
        XWindowAttributes ra;
        XGetWindowAttributes(dpy, root, &ra);
        create_window(0, 0, 0, ra.width, ra.height);
    }
    if (mons) XRRFreeMonitors(mons);
    track_active_window();
    XFlush(dpy);
}

static bool x_init(bool hidpi, char *err, size_t errlen) {
    (void)hidpi;
    dpy = XOpenDisplay(NULL);
    if (!dpy) { snprintf(err, errlen, "cannot open X display"); return false; }
    root = DefaultRootWindow(dpy);
    XSetErrorHandler(ignore_x_errors);
    net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    net_wm_state_fullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XSelectInput(dpy, root, PropertyChangeMask);
    int event_base, error_base;
    if (XRRQueryExtension(dpy, &event_base, &error_base)) {
        randr_event_base = event_base;
        XRRSelectInput(dpy, root, RRScreenChangeNotifyMask);
    }
    if (!XFixesQueryExtension(dpy, &event_base, &error_base)) {
        snprintf(err, errlen, "X server lacks the XFixes extension (needed for click-through)");
        return false;
    }
    return true;
}

static EGLDisplay x_egl_display(void) {
    return eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, dpy, NULL);
}

static void x_create_outputs(EGLConfig config) {
    egl_config = config;
    create_windows();
}

static int x_fd(void) { return ConnectionNumber(dpy); }

static void x_prepare_poll(void) { XFlush(dpy); }

static void x_after_poll(bool readable) {
    (void)readable;
    bool rebuild = false;
    while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (randr_event_base >= 0 && ev.type == randr_event_base + RRScreenChangeNotify) {
            XRRUpdateConfiguration(&ev);
            rebuild = true;
            continue;
        }
        if (ev.type == PropertyNotify) {
            if (ev.xproperty.window == root && ev.xproperty.atom == net_active_window) track_active_window();
            else if (ev.xproperty.window == active_window && ev.xproperty.atom == net_wm_state) update_fullscreen();
            continue;
        }
        if (ev.xany.window == active_window && active_window != None) {
            if (ev.type == DestroyNotify) active_window = None;
            if (ev.type == ConfigureNotify || ev.type == DestroyNotify) update_fullscreen();
            continue;
        }
        for (int i = 0; i < RP_MAX_OUTPUTS; i++) {
            x_out *o = &outputs[i];
            if (!o->in_use || o->window != ev.xany.window) continue;
            switch (ev.type) {
            case Expose:
                o->base.needs_redraw = true;
                break;
            case VisibilityNotify:
                // Fully covered (e.g. a fullscreen window): stop drawing. Compositing WMs
                // may always report "unobscured"; then we just keep drawing.
                o->visible = ev.xvisibility.state != VisibilityFullyObscured;
                update_can_draw(o);
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width != o->base.width || ev.xconfigure.height != o->base.height) {
                    o->base.width = o->base.render_w = ev.xconfigure.width;
                    o->base.height = o->base.render_h = ev.xconfigure.height;
                    o->base.needs_redraw = true;
                }
                break;
            }
        }
    }
    if (rebuild) {
        destroy_windows();
        create_windows();
    }
}

static int x_count(void) { return RP_MAX_OUTPUTS; }
static rp_output *x_output_at(int i) { return outputs[i].in_use ? &outputs[i].base : NULL; }
static void x_before_swap(rp_output *o) { (void)o; }

static bool x_screens_off(void) {
    int event_base, error_base;
    if (!DPMSQueryExtension(dpy, &event_base, &error_base) || !DPMSCapable(dpy)) return false;
    CARD16 level;
    BOOL enabled;
    return DPMSInfo(dpy, &level, &enabled) && enabled && level != DPMSModeOn;
}

static void x_shutdown(void) { destroy_windows(); }

static void x_disconnect(void) {
    if (dpy) XCloseDisplay(dpy);
    dpy = NULL;
}

const rp_backend rp_x11_backend = {
    .name = "x11",
    .init = x_init,
    .egl_display = x_egl_display,
    .create_outputs = x_create_outputs,
    .fd = x_fd,
    .prepare_poll = x_prepare_poll,
    .after_poll = x_after_poll,
    .count = x_count,
    .output = x_output_at,
    .before_swap = x_before_swap,
    .screens_off = x_screens_off,
    .shutdown = x_shutdown,
    .disconnect = x_disconnect,
};
