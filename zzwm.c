/*
 * ZZWM (Zoe's Zooming Window Manager)
 *
 * Main Controls
 * -------------
 *   scroll wheel        zoom in / out (centred on cursor), per-monitor
 *   middle-click drag   pan the canvas on the monitor under the cursor
 *   left/right-click    focus + raise, then forwarded to the window (click
 *                        buttons, links, etc. -- see Architecture)
 *   Super+left-drag     move window on canvas (snaps to other windows'
 *                       edges, see SNAP_* in config.h)
 *   Super+right-drag    resize window (also snaps)
 *
 * Each monitor has its own independent zoom/pan viewport into the shared
 * infinite canvas.  Use RandR at startup to detect monitor geometry; falls
 * back to a single monitor covering the full virtual screen.
 *
 * See config.h for keybindings and the SNAP_* edge-snapping settings.
 *
 * Build:  cc -O2 -o zzwm zzwm.c -lX11 -lXrender -lXcomposite -lXdamage -lXfixes -lXi -lXpresent -lXrandr -lm
 * Test:   Xephyr :1 -screen 1280x800 && DISPLAY=:1 ./zzwm
 */

#define _DEFAULT_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xpresent.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xrandr.h>

#include "appearance.h"

#define ZOOM_SPEED   1.1
#define ZOOM_MIN     0.05
#define ZOOM_MAX     20.0
#define MAX_CLIENTS  64
#define MAX_MONITORS 8

typedef enum { ACT_SPAWN, ACT_CLOSE } Action;
typedef struct {
    unsigned int mod;
    KeySym       keysym;
    Action       action;
    const char  *arg;
    KeyCode      keycode;   /* resolved at startup */
} Binding;
#define BIND(mod, key, act, arg) { mod, key, act, arg, 0 },
static Binding bindings[] = {
#include "config.h"
};
#undef BIND
#define NBINDINGS (int)(sizeof(bindings) / sizeof(bindings[0]))

typedef struct {
    Window window;
    double cx, cy;
    int cw, ch, depth;
    int anchor;   /* base window (ANCHOR_NAME, set in config.h) -- can't be closed or moved */
} Client;

/* Per-monitor viewport into the shared infinite canvas.
 * cx,cy is the canvas point visible at the centre of this monitor. */
typedef struct {
    int    x, y, w, h;      /* position and size in virtual-screen pixels */
    double cx, cy, zoom;    /* canvas point at monitor centre, zoom level */
} Monitor;

typedef struct {
    Display  *dpy;
    Window    root, overlay;
    Pixmap    backbuf[2];
    Picture   backbuf_pic[2];
    int       buf_busy[2];   /* server may still be presenting/reading this buffer */
    int       cur_buf;       /* buffer to draw into next */
    int       dirty;         /* a redraw is pending */
    uint32_t  present_serial;
    int       sw, sh;        /* full virtual-screen dimensions (back-buffer size) */
    Monitor   monitors[MAX_MONITORS];
    int       nmonitors;
    XRenderPictFormat *fmt32, *fmt24;
    Client    clients[MAX_CLIENTS];
    int       nclients;
    Window    overrides[MAX_CLIENTS];
    int       noverride;
    Window    focused;
    int       damage_event;
    int       xi_opcode;
    int       present_opcode;
    Window    hover_win;   /* client currently parked under the real cursor */
    int    panning, pan_sx, pan_sy;
    double pan_vx, pan_vy;
    int    pan_mon;        /* index into monitors[] for the active pan */
    int    moving, move_sx, move_sy;
    double move_cx, move_cy;
    Window move_win;
    int    resizing, resize_sx, resize_sy, resize_cw0, resize_ch0;
    Window resize_win;
    double last_input;   /* CLOCK_MONOTONIC seconds, updated on any input event */
    int    idle_fired;   /* IDLE_LOCK_CMD already run for the current idle period */
} ZWM;

static int g_other_wm;
static int xerr(Display *d, XErrorEvent *e) { (void)d; if (e->error_code == BadAccess) g_other_wm = 1; return 0; }

static double now_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void mark_activity(ZWM *z) {
    z->last_input = now_secs();
    z->idle_fired = 0;
}

/* canvas <-> screen coordinate transforms for a specific monitor */
static void to_screen(Monitor *m, double cx, double cy, double *sx, double *sy) {
    *sx = (cx - m->cx) * m->zoom + m->x + m->w / 2.0;
    *sy = (cy - m->cy) * m->zoom + m->y + m->h / 2.0;
}

static void to_canvas(Monitor *m, double sx, double sy, double *cx, double *cy) {
    *cx = (sx - m->x - m->w / 2.0) / m->zoom + m->cx;
    *cy = (sy - m->y - m->h / 2.0) / m->zoom + m->cy;
}

static void zoom_at(Monitor *m, double sx, double sy, double f) {
    double cx, cy, zoom = m->zoom * f;
    to_canvas(m, sx, sy, &cx, &cy);
    m->zoom = zoom < ZOOM_MIN ? ZOOM_MIN : zoom > ZOOM_MAX ? ZOOM_MAX : zoom;
    m->cx = cx - (sx - m->x - m->w / 2.0) / m->zoom;
    m->cy = cy - (sy - m->y - m->h / 2.0) / m->zoom;
}

/* return the monitor containing screen point (sx,sy); falls back to monitors[0] */
static Monitor *monitor_at(ZWM *z, int sx, int sy) {
    for (int i = 0; i < z->nmonitors; i++) {
        Monitor *m = &z->monitors[i];
        if (sx >= m->x && sx < m->x + m->w && sy >= m->y && sy < m->y + m->h)
            return m;
    }
    return z->nmonitors ? &z->monitors[0] : NULL;
}

/* screen-space rect for a client as seen through monitor m; minimum 1px so
 * zoomed-out windows stay hit-testable */
static void srect(Client *c, Monitor *m, int *x, int *y, int *w, int *h) {
    double sx, sy;
    to_screen(m, c->cx, c->cy, &sx, &sy);
    *x = (int)sx; *y = (int)sy;
    *w = (int)(c->cw * m->zoom); if (*w < 1) *w = 1;
    *h = (int)(c->ch * m->zoom); if (*h < 1) *h = 1;
}

static Client *find(ZWM *z, Window w) {
    for (int i = 0; i < z->nclients; i++)
        if (z->clients[i].window == w) return &z->clients[i];
    return NULL;
}

/* hit-test using the viewport of monitor m (cursor is on that monitor) */
static Client *hit(ZWM *z, Monitor *m, int sx, int sy) {
    for (int i = z->nclients - 1; i >= 0; i--) {
        int x, y, w, h;
        srect(&z->clients[i], m, &x, &y, &w, &h);
        if (sx >= x && sx < x+w && sy >= y && sy < y+h) return &z->clients[i];
    }
    return NULL;
}

/* override-redirect windows sit at real screen coords, so this is an unscaled rect test */
static Window hit_override(ZWM *z, int sx, int sy, XWindowAttributes *out) {
    for (int i = z->noverride - 1; i >= 0; i--) {
        Window w = z->overrides[i];
        XWindowAttributes a;
        if (!XGetWindowAttributes(z->dpy, w, &a) || a.map_state != IsViewable) continue;
        if (sx >= a.x && sx < a.x + a.width && sy >= a.y && sy < a.y + a.height) {
            *out = a;
            return w;
        }
    }
    return None;
}

/* invert the canvas transform for monitor m -> window-local pixel */
static void to_client_local(Client *c, Monitor *m, int sx, int sy, int *wx, int *wy) {
    double cx, cy;
    to_canvas(m, sx, sy, &cx, &cy);
    *wx = (int)(cx - c->cx);
    *wy = (int)(cy - c->cy);
}

/* draw/hit-test order follows the array; moving c to the end raises it */
static void raise_client(ZWM *z, Client *c) {
    int idx = (int)(c - z->clients);
    if (idx < 0 || idx >= z->nclients - 1) return;
    Client tmp = *c;
    for (int i = idx; i < z->nclients - 1; i++) z->clients[i] = z->clients[i + 1];
    z->clients[z->nclients - 1] = tmp;
}

/* moves the real window off-screen at native size; on_hover() repositions it under the cursor */
static void park(ZWM *z, Client *c) {
    XMoveResizeWindow(z->dpy, c->window, -(c->cw+32), -(c->ch+32), c->cw, c->ch);
}

static Client *manage(ZWM *z, Window w) {
    if (z->nclients == MAX_CLIENTS) return NULL;
    XWindowAttributes a;
    if (!XGetWindowAttributes(z->dpy, w, &a)) return NULL;
    Client *c = &z->clients[z->nclients++];
    c->window = w;
    /* convert existing screen position to canvas coords using the monitor it's on */
    Monitor *m = monitor_at(z, a.x + a.width/2, a.y + a.height/2);
    if (!m) m = &z->monitors[0];
    to_canvas(m, a.x, a.y, &c->cx, &c->cy);
    c->cw = a.width; c->ch = a.height; c->depth = a.depth;
    char *name = NULL;
    c->anchor = XFetchName(z->dpy, w, &name) && name && !strcmp(name, ANCHOR_NAME);
    if (name) XFree(name);
    if (a.border_width) XSetWindowBorderWidth(z->dpy, w, 0);
    park(z, c);
    XGrabButton(z->dpy, Button1, 0, w, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);
    XGrabButton(z->dpy, Button3, 0, w, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);
    XDamageCreate(z->dpy, w, XDamageReportNonEmpty);
    return c;
}

static void manage_override(ZWM *z, Window w) {
    for (int i = 0; i < z->noverride; i++) if (z->overrides[i] == w) return;
    if (z->noverride == MAX_CLIENTS) return;
    z->overrides[z->noverride++] = w;
    XWindowAttributes a;
    if (XGetWindowAttributes(z->dpy, w, &a) && a.border_width)
        XSetWindowBorderWidth(z->dpy, w, 0);
    XDamageCreate(z->dpy, w, XDamageReportNonEmpty);
}

static void unmanage_override(ZWM *z, Window w) {
    for (int i = 0; i < z->noverride; i++)
        if (z->overrides[i] == w) { z->overrides[i] = z->overrides[--z->noverride]; return; }
}

static void unmanage(ZWM *z, Window w) {
    int i;
    for (i = 0; i < z->nclients; i++) if (z->clients[i].window == w) break;
    if (i == z->nclients) return;
    z->clients[i] = z->clients[--z->nclients];
    if (z->move_win == w)   { z->moving = 0;   z->move_win = 0; }
    if (z->resize_win == w) { z->resizing = 0; z->resize_win = 0; }
    if (z->hover_win == w)  z->hover_win = 0;
    if (z->focused == w) {
        z->focused = z->nclients ? z->clients[z->nclients-1].window : 0;
        if (z->focused) XSetInputFocus(z->dpy, z->focused, RevertToPointerRoot, CurrentTime);
    }
}

/* shared tail of both redraw() loops; zoom <= 0 skips the transform (override-redirect popups
 * always draw at native scale) */
static void composite_window(ZWM *z, Window w, int depth, double zoom, int op,
                              Picture dst, int sx, int sy, int sw, int sh) {
    Pixmap pix = XCompositeNameWindowPixmap(z->dpy, w);
    if (!pix) return;
    Picture src = XRenderCreatePicture(z->dpy, pix,
        depth == 32 ? z->fmt32 : z->fmt24, 0, NULL);
    if (zoom > 0 && zoom != 1.0) {
        XFixed inv = XDoubleToFixed(1.0 / zoom);
        XTransform xf = {{{ inv, 0, 0 }, { 0, inv, 0 }, { 0, 0, XDoubleToFixed(1.0) }}};
        const char *filt = zoom > 1.0 ? FilterNearest : FilterBilinear;
        XRenderSetPictureFilter(z->dpy, src, filt, NULL, 0);
        XRenderSetPictureTransform(z->dpy, src, &xf);
    }
    XRenderComposite(z->dpy, op, src, None, dst, 0, 0, 0, 0, sx, sy, sw, sh);
    XRenderFreePicture(z->dpy, src);
    XFreePixmap(z->dpy, pix);
}

/* builds one full frame into the back-buffer.
 * Each monitor is composited independently with its own viewport, clipped to
 * its screen rect so adjacent monitors don't bleed into each other. */
static void composite_frame(ZWM *z) {
    Picture dst = z->backbuf_pic[z->cur_buf];
    XRenderColor bg = { CANVAS_BG_R * 257, CANVAS_BG_G * 257, CANVAS_BG_B * 257, 0xffff };
    XRenderColor border_col = { BORDER_R * 257, BORDER_G * 257, BORDER_B * 257, 0xffff };

    /* fill the entire back-buffer with the canvas background */
    XRectangle full = { 0, 0, (unsigned short)z->sw, (unsigned short)z->sh };
    XRenderFillRectangles(z->dpy, PictOpSrc, dst, &bg, &full, 1);

    for (int mi = 0; mi < z->nmonitors; mi++) {
        Monitor *m = &z->monitors[mi];
        /* clip rendering to this monitor's screen region */
        XRectangle mrect = { (short)m->x, (short)m->y,
                             (unsigned short)m->w, (unsigned short)m->h };
        XRenderSetPictureClipRectangles(z->dpy, dst, 0, 0, &mrect, 1);

        for (int i = 0; i < z->nclients; i++) {
            Client *c = &z->clients[i];
            int sx, sy, sw, sh;
            srect(c, m, &sx, &sy, &sw, &sh);
            int bw = (int)(BORDER_THICKNESS * m->zoom + 0.5);
            int bx = sx - bw, by = sy - bw, bsw = sw + 2*bw, bsh = sh + 2*bw;
            /* skip windows entirely outside this monitor */
            if (bx >= m->x + m->w || by >= m->y + m->h ||
                bx + bsw <= m->x   || by + bsh <= m->y) continue;
            if (bw > 0) {
                XRectangle brect = { (short)bx, (short)by,
                                     (unsigned short)bsw, (unsigned short)bsh };
                XRenderFillRectangles(z->dpy, PictOpSrc, dst, &border_col, &brect, 1);
            }
            composite_window(z, c->window, c->depth, m->zoom, PictOpSrc, dst, sx, sy, sw, sh);
        }
    }

    /* remove per-monitor clip before drawing override-redirect windows, which
     * live at native screen coordinates and may span monitor boundaries */
    XRenderSetPictureClipRectangles(z->dpy, dst, 0, 0, &full, 1);

    for (int i = 0; i < z->noverride; i++) {
        Window w = z->overrides[i];
        XWindowAttributes a;
        if (!XGetWindowAttributes(z->dpy, w, &a) || a.map_state != IsViewable) continue;
        if (a.x >= z->sw || a.y >= z->sh || a.x+a.width <= 0 || a.y+a.height <= 0) continue;
        composite_window(z, w, a.depth, 0, PictOpOver, dst, a.x, a.y, a.width, a.height);
    }
}

static void present(ZWM *z) {
    int i = z->cur_buf;
    XPresentPixmap(z->dpy, z->overlay, z->backbuf[i], ++z->present_serial,
                   None, None, 0, 0, None, None, None,
                   PresentOptionNone, 0, 0, 0, NULL, 0);
    XFlush(z->dpy);
    z->buf_busy[i] = 1;
    z->cur_buf = 1 - i;
    z->dirty = 0;
}

static void pump_redraw(ZWM *z) {
    if (!z->dirty || z->buf_busy[z->cur_buf]) return;
    composite_frame(z);
    present(z);
}

static void request_redraw(ZWM *z) {
    z->dirty = 1;
    pump_redraw(z);
}

static void on_present_idle(ZWM *z, XPresentIdleNotifyEvent *e) {
    for (int i = 0; i < 2; i++)
        if (z->backbuf[i] == e->pixmap) z->buf_busy[i] = 0;
    pump_redraw(z);
}

static void on_map(ZWM *z, XMapRequestEvent *ev) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(z->dpy, ev->window, &a)) return;
    if (a.override_redirect) { XMapWindow(z->dpy, ev->window); return; }
    Client *c = find(z, ev->window);
    if (!c && (c = manage(z, ev->window))) {
        if (c->anchor) {
            c->cx = -c->cw / 2.0;
            c->cy = -c->ch / 2.0;
        } else {
            /* center the window on the monitor containing the cursor */
            Window dr, dc; int rx, ry, wx, wy; unsigned int mask;
            XQueryPointer(z->dpy, z->root, &dr, &dc, &rx, &ry, &wx, &wy, &mask);
            Monitor *m = monitor_at(z, rx, ry);
            if (!m) m = &z->monitors[0];
            c->cx = m->cx - c->cw / 2.0;
            c->cy = m->cy - c->ch / 2.0;
        }
    }
    XMapWindow(z->dpy, ev->window);
    if (c) { z->focused = c->window; XSetInputFocus(z->dpy, c->window, RevertToPointerRoot, CurrentTime); }
    request_redraw(z);
}

static void on_unmap(ZWM *z, XUnmapEvent *ev) {
    if (!ev->send_event) {
        unmanage(z, ev->window); unmanage_override(z, ev->window); request_redraw(z);
    }
}

static void on_destroy(ZWM *z, XDestroyWindowEvent *ev) {
    unmanage(z, ev->window); unmanage_override(z, ev->window); request_redraw(z);
}

static void on_mapnotify(ZWM *z, XMapEvent *ev) {
    if (ev->window == z->overlay) return;
    XWindowAttributes a;
    if (!XGetWindowAttributes(z->dpy, ev->window, &a)) return;
    if (a.override_redirect) { manage_override(z, ev->window); request_redraw(z); }
}

static void on_configure(ZWM *z, XConfigureRequestEvent *ev) {
    Client *c = find(z, ev->window);
    if (c) {
        if (ev->value_mask & CWWidth)  c->cw = ev->width;
        if (ev->value_mask & CWHeight) c->ch = ev->height;
        park(z, c);
    } else {
        XWindowChanges wc = { .x = ev->x, .y = ev->y,
            .width = ev->width, .height = ev->height,
            .border_width = ev->border_width, .sibling = ev->above, .stack_mode = ev->detail };
        XConfigureWindow(z->dpy, ev->window, ev->value_mask, &wc);
    }
}

static Client *focus_raise(ZWM *z, Client *c) {
    raise_client(z, c);
    c = &z->clients[z->nclients - 1];
    z->focused = c->window;
    XRaiseWindow(z->dpy, c->window);
    XSetInputFocus(z->dpy, c->window, RevertToPointerRoot, CurrentTime);
    request_redraw(z);
    return c;
}

static void on_button(ZWM *z, XButtonEvent *ev) {
    Monitor *m = monitor_at(z, ev->x_root, ev->y_root);
    if (!m) m = &z->monitors[0];

    if (ev->button == 4) { zoom_at(m, ev->x_root, ev->y_root, ZOOM_SPEED);     request_redraw(z); return; }
    if (ev->button == 5) { zoom_at(m, ev->x_root, ev->y_root, 1.0/ZOOM_SPEED); request_redraw(z); return; }
    if (ev->button == 2) {
        z->panning = 1; z->pan_sx = ev->x_root; z->pan_sy = ev->y_root;
        z->pan_vx = m->cx; z->pan_vy = m->cy;
        z->pan_mon = (int)(m - z->monitors);
        XGrabPointer(z->dpy, z->overlay, True, PointerMotionMask|ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        return;
    }
    if (ev->button != 1 && ev->button != 3) return;
    if (ev->state & Mod4Mask) {
        Client *c = hit(z, m, ev->x_root, ev->y_root);
        if (!c) return;
        c = focus_raise(z, c);
        if (ev->button == 1 && !c->anchor) {
            z->moving = 1; z->move_win = c->window;
            z->move_sx = ev->x_root; z->move_sy = ev->y_root;
            z->move_cx = c->cx;      z->move_cy = c->cy;
        } else if (ev->button == 3) {
            z->resizing = 1; z->resize_win = c->window;
            z->resize_sx = ev->x_root; z->resize_sy = ev->y_root;
            z->resize_cw0 = c->cw;     z->resize_ch0 = c->ch;
        } else {
            return;
        }
        XGrabPointer(z->dpy, z->overlay, True, PointerMotionMask|ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        return;
    }

    Client *c = find(z, ev->window);
    if (c) focus_raise(z, c);
    XAllowEvents(z->dpy, ReplayPointer, ev->time);
}

static void on_release(ZWM *z, XButtonEvent *ev) {
    if ((ev->button == 2 && z->panning) || (ev->button == 1 && z->moving) ||
        (ev->button == 3 && z->resizing)) {
        z->panning = z->moving = z->resizing = 0;
        z->move_win = z->resize_win = 0;
        XUngrabPointer(z->dpy, CurrentTime);
    }
}

static void snap_translate(ZWM *z, Client *c, double basex, double basey, double *dx, double *dy) {
    if (!SNAP_ENABLED) return;
    double l = basex + *dx, r = l + c->cw, t = basey + *dy, b = t + c->ch;
    double bestx = SNAP_DIST, besty = SNAP_DIST;
    Client *ox = NULL, *oy = NULL;
    for (int i = 0; i < z->nclients; i++) {
        Client *o = &z->clients[i];
        if (o == c) continue;
        double ol = o->cx, orr = o->cx + o->cw, ot = o->cy, ob = o->cy + o->ch;
        if (t < ob && b > ot) {
            double xc[4] = { ol - l, (orr + SNAP_GAP) - l, orr - r, (ol - SNAP_GAP) - r };
            for (int k = 0; k < 4; k++) if (fabs(xc[k]) < fabs(bestx)) { bestx = xc[k]; ox = o; }
        }
        if (l < orr && r > ol) {
            double yc[4] = { ot - t, (ob + SNAP_GAP) - t, ob - b, (ot - SNAP_GAP) - b };
            for (int k = 0; k < 4; k++) if (fabs(yc[k]) < fabs(besty)) { besty = yc[k]; oy = o; }
        }
    }
    if (ox && !oy) {
        double ot = ox->cy, ob = ox->cy + ox->ch;
        double yc[4] = { ot - t, (ob + SNAP_GAP) - t, ob - b, (ot - SNAP_GAP) - b };
        for (int k = 0; k < 4; k++) if (fabs(yc[k]) < fabs(besty)) { besty = yc[k]; oy = ox; }
    } else if (oy && !ox) {
        double ol = oy->cx, orr = oy->cx + oy->cw;
        double xc[4] = { ol - l, (orr + SNAP_GAP) - l, orr - r, (ol - SNAP_GAP) - r };
        for (int k = 0; k < 4; k++) if (fabs(xc[k]) < fabs(bestx)) { bestx = xc[k]; ox = oy; }
    }
    if (ox) *dx += bestx;
    if (oy) *dy += besty;
}

static void snap_resize(ZWM *z, Client *c, double *dw, double *dh) {
    if (!SNAP_ENABLED) return;
    double l = c->cx, r = l + c->cw + *dw, t = c->cy, b = t + c->ch + *dh;
    double bestw = SNAP_DIST, besth = SNAP_DIST;
    Client *ow = NULL, *oh = NULL;
    for (int i = 0; i < z->nclients; i++) {
        Client *o = &z->clients[i];
        if (o == c) continue;
        double ol = o->cx, orr = o->cx + o->cw, ot = o->cy, ob = o->cy + o->ch;
        if (t < ob && b > ot) {
            double wc[2] = { orr - r, (ol - SNAP_GAP) - r };
            for (int k = 0; k < 2; k++) if (fabs(wc[k]) < fabs(bestw)) { bestw = wc[k]; ow = o; }
        }
        if (l < orr && r > ol) {
            double hc[2] = { ob - b, (ot - SNAP_GAP) - b };
            for (int k = 0; k < 2; k++) if (fabs(hc[k]) < fabs(besth)) { besth = hc[k]; oh = o; }
        }
    }
    if (ow && !oh) {
        double ot = ow->cy, ob = ow->cy + ow->ch;
        double hc[2] = { ob - b, (ot - SNAP_GAP) - b };
        for (int k = 0; k < 2; k++) if (fabs(hc[k]) < fabs(besth)) { besth = hc[k]; oh = ow; }
    } else if (oh && !ow) {
        double ol = oh->cx, orr = oh->cx + oh->cw;
        double wc[2] = { orr - r, (ol - SNAP_GAP) - r };
        for (int k = 0; k < 2; k++) if (fabs(wc[k]) < fabs(bestw)) { bestw = wc[k]; ow = oh; }
    }
    if (ow) *dw += bestw;
    if (oh) *dh += besth;
}

static void on_motion(ZWM *z, XMotionEvent *ev) {
    XEvent next;
    while (XCheckTypedEvent(z->dpy, MotionNotify, &next)) *ev = next.xmotion;

    Monitor *m = monitor_at(z, ev->x_root, ev->y_root);
    if (!m) m = &z->monitors[0];

    if (z->moving) {
        Client *c = find(z, z->move_win);
        if (c) {
            double dx = (ev->x_root - z->move_sx) / m->zoom;
            double dy = (ev->y_root - z->move_sy) / m->zoom;
            snap_translate(z, c, z->move_cx, z->move_cy, &dx, &dy);
            c->cx = z->move_cx + dx;
            c->cy = z->move_cy + dy;
            request_redraw(z);
        }
    } else if (z->resizing) {
        Client *c = find(z, z->resize_win);
        if (c) {
            double dw = (ev->x_root - z->resize_sx) / m->zoom;
            double dh = (ev->y_root - z->resize_sy) / m->zoom;
            snap_resize(z, c, &dw, &dh);
            int nw = z->resize_cw0 + (int)dw;
            int nh = z->resize_ch0 + (int)dh;
            c->cw = nw < 20 ? 20 : nw;
            c->ch = nh < 20 ? 20 : nh;
            park(z, c);
            request_redraw(z);
        }
    } else if (z->panning) {
        Monitor *pm = &z->monitors[z->pan_mon];
        pm->cx = z->pan_vx - (ev->x_root - z->pan_sx) / pm->zoom;
        pm->cy = z->pan_vy - (ev->y_root - z->pan_sy) / pm->zoom;
        request_redraw(z);
    }
}

/* driven by XI_RawMotion; parks whichever client is under the cursor at the
 * matching real screen position so plain X routing delivers input to the
 * right native pixel */
static void on_hover(ZWM *z) {
    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    if (!XQueryPointer(z->dpy, z->root, &root_ret, &child_ret,
                        &root_x, &root_y, &win_x, &win_y, &mask))
        return;

    XWindowAttributes a;
    Window ov = hit_override(z, root_x, root_y, &a);
    Monitor *m = monitor_at(z, root_x, root_y);
    if (!m) m = &z->monitors[0];
    Client *c = ov ? NULL : hit(z, m, root_x, root_y);

    if (c && z->hover_win && c->window != z->hover_win) {
        Client *prev = find(z, z->hover_win);
        if (prev) park(z, prev);
    }
    if (!c) {
        if (!ov && z->hover_win) {
            Client *prev = find(z, z->hover_win);
            if (prev) park(z, prev);
            z->hover_win = 0;
        }
        return;
    }

    int wx, wy;
    to_client_local(c, m, root_x, root_y, &wx, &wy);
    XMoveWindow(z->dpy, c->window, root_x - wx, root_y - wy);
    z->hover_win = c->window;
}

static void on_key(ZWM *z, XKeyEvent *ev) {
    for (int i = 0; i < NBINDINGS; i++) {
        Binding *b = &bindings[i];
        if (ev->keycode != b->keycode || (ev->state & b->mod) != b->mod) continue;
        switch (b->action) {
        case ACT_SPAWN: system(b->arg); break;
        case ACT_CLOSE: {
            Client *c = find(z, z->focused);
            if (z->focused && (!c || !c->anchor)) XDestroyWindow(z->dpy, z->focused);
            break;
        }
        }
        return;
    }
}

int main(void) {
    ZWM z = {0};

    if (!(z.dpy = XOpenDisplay(NULL))) { fputs("zzwm: no display\n", stderr); return 1; }
    int scr = DefaultScreen(z.dpy);
    z.root = RootWindow(z.dpy, scr);
    z.sw = DisplayWidth(z.dpy, scr);
    z.sh = DisplayHeight(z.dpy, scr);

    /* query monitor layout via RandR; fall back to a single full-screen monitor */
    int rr_ev, rr_err, rr_base;
    if (XQueryExtension(z.dpy, "RANDR", &rr_base, &rr_ev, &rr_err)) {
        int nmon = 0;
        XRRMonitorInfo *mons = XRRGetMonitors(z.dpy, z.root, True, &nmon);
        for (int i = 0; i < nmon && z.nmonitors < MAX_MONITORS; i++) {
            Monitor *m = &z.monitors[z.nmonitors++];
            m->x = mons[i].x; m->y = mons[i].y;
            m->w = mons[i].width; m->h = mons[i].height;
            m->cx = 0; m->cy = 0; m->zoom = 1.0;
        }
        if (mons) XRRFreeMonitors(mons);
    }
    if (!z.nmonitors) {
        z.monitors[0].w = z.sw; z.monitors[0].h = z.sh;
        z.monitors[0].zoom = 1.0;
        z.nmonitors = 1;
    }

    XSetErrorHandler(xerr);
    XSelectInput(z.dpy, z.root, SubstructureRedirectMask|SubstructureNotifyMask);
    XSync(z.dpy, False);
    if (g_other_wm) { fputs("zzwm: another WM running\n", stderr); return 1; }

    int eb, ee;
    if (!XCompositeQueryExtension(z.dpy, &eb, &ee)) { fputs("zzwm: no XComposite\n", stderr); return 1; }
    if (!XDamageQueryExtension(z.dpy, &z.damage_event, &ee)) { fputs("zzwm: no XDamage\n", stderr); return 1; }
    if (!XRenderQueryExtension(z.dpy, &eb, &ee))    { fputs("zzwm: no XRender\n",    stderr); return 1; }
    if (!XFixesQueryExtension(z.dpy, &eb, &ee))     { fputs("zzwm: no XFixes\n",     stderr); return 1; }
    if (!XQueryExtension(z.dpy, "XInputExtension", &z.xi_opcode, &eb, &ee)) {
        fputs("zzwm: no XInput2\n", stderr); return 1;
    }
    { int maj = 2, min = 2; XIQueryVersion(z.dpy, &maj, &min); }
    if (!XPresentQueryExtension(z.dpy, &z.present_opcode, &eb, &ee)) {
        fputs("zzwm: no XPresent\n", stderr); return 1;
    }

    for (int i = 0; i < NBINDINGS; i++) {
        Binding *b = &bindings[i];
        b->keycode = XKeysymToKeycode(z.dpy, b->keysym);
        XGrabKey(z.dpy, b->keycode, b->mod, z.root, True, GrabModeAsync, GrabModeAsync);
    }

    int wheel_btns[] = { Button1, Button3, Button2, Button4, Button5 };
    for (int i = 0; i < 5; i++)
        XGrabButton(z.dpy, wheel_btns[i], Mod4Mask, z.root, True, ButtonPressMask,
                    GrabModeAsync, GrabModeAsync, None, None);

    unsigned char ximask[(XI_LASTEVENT + 7) / 8] = {0};
    XISetMask(ximask, XI_RawMotion);
    XIEventMask evmask = { XIAllMasterDevices, sizeof(ximask), ximask };
    XISelectEvents(z.dpy, z.root, &evmask, 1);

    XCompositeRedirectSubwindows(z.dpy, z.root, CompositeRedirectAutomatic);
    z.overlay = XCompositeGetOverlayWindow(z.dpy, z.root);
    XSelectInput(z.dpy, z.overlay, ButtonPressMask|ButtonReleaseMask|PointerMotionMask|KeyPressMask);
    Cursor cur = XCreateFontCursor(z.dpy, XC_left_ptr);
    XDefineCursor(z.dpy, z.root, cur); XFreeCursor(z.dpy, cur);
    XserverRegion empty = XFixesCreateRegion(z.dpy, NULL, 0);
    XFixesSetWindowShapeRegion(z.dpy, z.overlay, ShapeInput, 0, 0, empty);
    XFixesDestroyRegion(z.dpy, empty);

    z.fmt32 = XRenderFindStandardFormat(z.dpy, PictStandardARGB32);
    z.fmt24 = XRenderFindStandardFormat(z.dpy, PictStandardRGB24);
    XWindowAttributes oa;
    XGetWindowAttributes(z.dpy, z.overlay, &oa);
    XRenderPictFormat *ofmt = XRenderFindVisualFormat(z.dpy, oa.visual);
    for (int i = 0; i < 2; i++) {
        z.backbuf[i] = XCreatePixmap(z.dpy, z.root, z.sw, z.sh, oa.depth);
        z.backbuf_pic[i] = XRenderCreatePicture(z.dpy, z.backbuf[i], ofmt, 0, NULL);
    }
    XPresentSelectInput(z.dpy, z.overlay, PresentIdleNotifyMask);

    Window dummy, *ch; unsigned int n;
    XQueryTree(z.dpy, z.root, &dummy, &dummy, &ch, &n);
    for (unsigned i = 0; i < n; i++) {
        XWindowAttributes a;
        if (ch[i] != z.overlay && XGetWindowAttributes(z.dpy, ch[i], &a)
                && a.map_state == IsViewable) {
            if (a.override_redirect) manage_override(&z, ch[i]);
            else                      manage(&z, ch[i]);
        }
    }
    if (ch) XFree(ch);
    request_redraw(&z);
    z.last_input = now_secs();

    XEvent ev;
    for (;;) {
        if (IDLE_LOCK_TIMEOUT > 0) {
            int fd = ConnectionNumber(z.dpy);
            while (!XPending(z.dpy)) {
                double idle = now_secs() - z.last_input;
                if (!z.idle_fired && idle >= IDLE_LOCK_TIMEOUT) {
                    system(IDLE_LOCK_CMD);
                    z.idle_fired = 1;
                }
                double wait = z.idle_fired ? 60.0 : IDLE_LOCK_TIMEOUT - idle;
                if (wait < 0) wait = 0;
                struct timeval tv = { (long)wait, (long)((wait - (long)wait) * 1e6) };
                fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
                select(fd + 1, &fds, NULL, NULL, &tv);
            }
        }
        XNextEvent(z.dpy, &ev);
        if (ev.type == z.damage_event + XDamageNotify) {
            XDamageSubtract(z.dpy, ((XDamageNotifyEvent *)&ev)->damage, None, None);
            request_redraw(&z);
            continue;
        }
        switch (ev.type) {
        case MapRequest:       on_map(&z, &ev.xmaprequest);             break;
        case MapNotify:        on_mapnotify(&z, &ev.xmap);              break;
        case UnmapNotify:      on_unmap(&z, &ev.xunmap);                break;
        case DestroyNotify:    on_destroy(&z, &ev.xdestroywindow);      break;
        case ConfigureRequest: on_configure(&z, &ev.xconfigurerequest); break;
        case ButtonPress:      mark_activity(&z); on_button(&z, &ev.xbutton);   break;
        case ButtonRelease:    mark_activity(&z); on_release(&z, &ev.xbutton);  break;
        case MotionNotify:     mark_activity(&z); on_motion(&z, &ev.xmotion);   break;
        case KeyPress:         mark_activity(&z); on_key(&z, &ev.xkey); request_redraw(&z); break;
        case GenericEvent:
            if (ev.xcookie.extension == z.xi_opcode && XGetEventData(z.dpy, &ev.xcookie)) {
                if (ev.xcookie.evtype == XI_RawMotion) { mark_activity(&z); on_hover(&z); }
                XFreeEventData(z.dpy, &ev.xcookie);
            } else if (ev.xcookie.extension == z.present_opcode && XGetEventData(z.dpy, &ev.xcookie)) {
                if (ev.xcookie.evtype == PresentIdleNotify)
                    on_present_idle(&z, (XPresentIdleNotifyEvent *)ev.xcookie.data);
                XFreeEventData(z.dpy, &ev.xcookie);
            }
            break;
        }
    }
}
