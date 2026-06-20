/*
 * ZZWM (Zoe's Zooming Window Manager)
 *
 * Controls
 * --------
 *   scroll wheel        zoom in / out (centred on cursor)
 *   middle-click drag   pan the canvas
 *   left-click window   focus
 *   Super+left-drag     move window on canvas
 *   Super+right-drag    resize window
 *   Super+Return        spawn xterm
 *   Super+Space         spawn zzwm-run (app launcher)
 *   Super+H             spawn zzwm-help (keybinding reference)
 *   Super+Q             close focused window
 *
 * zzwm-run, zzwm-bar, zzwm-help are separate binaries (utility-apps/).
 *
 * Build:  cc -O2 -o zzwm zzwm.c -lX11 -lXrender -lXcomposite -lXrandr
 * Test:   Xephyr :1 -screen 1280x800 && DISPLAY=:1 ./zzwm
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xrandr.h>

#include "appearance.h"

#define ZOOM_SPEED    1.1
#define ZOOM_MIN      0.05
#define ZOOM_MAX      20.0
#define MAX_CLIENTS   64
#define MAX_MONITORS  16

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

typedef struct {
    int sw, sh;
    double cx, cy, zoom;
} Viewport;

/* Each physical monitor gets its own independent Viewport (pan + zoom) onto
 * the single shared canvas that holds all Clients. mx/my/mw/mh are the
 * monitor's position and size within the root window (as reported by
 * XRandR); Viewport.sw/sh mirror mw/mh so to_screen()/to_canvas() treat the
 * monitor's own center as the origin for pan/zoom math. */
typedef struct {
    int      mx, my, mw, mh;
    Viewport vp;
    char     name[64];   /* RandR output name, used to keep a monitor's
                             viewport stable across reconfiguration */
} Monitor;

typedef struct {
    Display  *dpy;
    Window    root, overlay;
    Picture   overlay_pic;
    int       sw, sh;        /* full root bounding size, all monitors */
    Monitor   monitors[MAX_MONITORS];
    int       nmonitors;
    int       randr_event;
    XRenderPictFormat *fmt32, *fmt24;
    Client    clients[MAX_CLIENTS];
    int       nclients;
    Window    overrides[MAX_CLIENTS];
    int       noverride;
    Window    focused;
    int       damage_event;
    int    panning, pan_sx, pan_sy, pan_mon;
    double pan_vx, pan_vy;
    int    moving, move_sx, move_sy, move_mon;
    double move_cx, move_cy;
    Window move_win;
    int    resizing, resize_sx, resize_sy, resize_cw0, resize_ch0, resize_mon;
    Window resize_win;
} ZWM;

static int g_other_wm;
static int xerr(Display *d, XErrorEvent *e) { (void)d; if (e->error_code == BadAccess) g_other_wm = 1; return 0; }

static void to_screen(Viewport *v, double cx, double cy, double *sx, double *sy) {
    *sx = (cx - v->cx) * v->zoom + v->sw / 2.0;
    *sy = (cy - v->cy) * v->zoom + v->sh / 2.0;
}

static void to_canvas(Viewport *v, double sx, double sy, double *cx, double *cy) {
    *cx = (sx - v->sw / 2.0) / v->zoom + v->cx;
    *cy = (sy - v->sh / 2.0) / v->zoom + v->cy;
}

static void zoom_at(Viewport *v, double sx, double sy, double f) {
    double cx, cy, zoom = v->zoom * f;
    to_canvas(v, sx, sy, &cx, &cy);
    v->zoom = zoom < ZOOM_MIN ? ZOOM_MIN : zoom > ZOOM_MAX ? ZOOM_MAX : zoom;
    v->cx = cx - (sx - v->sw / 2.0) / v->zoom;
    v->cy = cy - (sy - v->sh / 2.0) / v->zoom;
}

static void srect(Client *c, Viewport *v, int *x, int *y, int *w, int *h) {
    double sx, sy;
    to_screen(v, c->cx, c->cy, &sx, &sy);
    *x = (int)sx; *y = (int)sy;
    *w = (int)(c->cw * v->zoom); if (*w < 1) *w = 1;
    *h = (int)(c->ch * v->zoom); if (*h < 1) *h = 1;
}

/* Resolve a root-relative point to the Monitor it falls within. Always
 * returns non-NULL once monitors have been detected (falls back to the
 * first monitor for points outside every monitor's bounds, e.g. while a
 * drag's pointer briefly crosses a gap between monitors). */
static Monitor *find_monitor(ZWM *z, int x_root, int y_root) {
    for (int i = 0; i < z->nmonitors; i++) {
        Monitor *m = &z->monitors[i];
        if (x_root >= m->mx && x_root < m->mx + m->mw &&
            y_root >= m->my && y_root < m->my + m->mh) return m;
    }
    return &z->monitors[0];
}

static Client *find(ZWM *z, Window w) {
    for (int i = 0; i < z->nclients; i++)
        if (z->clients[i].window == w) return &z->clients[i];
    return NULL;
}

/* sx/sy are monitor-local (relative to m->mx/m->my). */
static Client *hit(ZWM *z, Monitor *m, int sx, int sy) {
    for (int i = z->nclients - 1; i >= 0; i--) {
        int x, y, w, h;
        srect(&z->clients[i], &m->vp, &x, &y, &w, &h);
        if (sx >= x && sx < x+w && sy >= y && sy < y+h) return &z->clients[i];
    }
    return NULL;
}

/* Clients are drawn (and hit-tested) in array order, last = topmost. Moving
 * a client to the end of the array raises it above everything else. */
static void raise_client(ZWM *z, Client *c) {
    int idx = (int)(c - z->clients);
    if (idx < 0 || idx >= z->nclients - 1) return;
    Client tmp = *c;
    for (int i = idx; i < z->nclients - 1; i++) z->clients[i] = z->clients[i + 1];
    z->clients[z->nclients - 1] = tmp;
}

static void park(ZWM *z, Client *c) {
    XMoveResizeWindow(z->dpy, c->window, -(c->cw+32), -(c->ch+32), c->cw, c->ch);
}

static Client *manage(ZWM *z, Window w) {
    if (z->nclients == MAX_CLIENTS) return NULL;
    XWindowAttributes a;
    if (!XGetWindowAttributes(z->dpy, w, &a)) return NULL;
    Client *c = &z->clients[z->nclients++];
    c->window = w;
    Monitor *m = find_monitor(z, a.x, a.y);
    to_canvas(&m->vp, a.x - m->mx, a.y - m->my, &c->cx, &c->cy);
    c->cw = a.width; c->ch = a.height; c->depth = a.depth;
    char *name = NULL;
    c->anchor = XFetchName(z->dpy, w, &name) && name && !strcmp(name, ANCHOR_NAME);
    if (name) XFree(name);
    /* The composited window pixmap includes the X border, so a nonzero
     * border_width (many toolkits default to 1px) shows up baked into the
     * rendered window. Strip it -- zzwm draws no decorations of its own. */
    if (a.border_width) XSetWindowBorderWidth(z->dpy, w, 0);
    park(z, c);
    XDamageCreate(z->dpy, w, XDamageReportNonEmpty);
    return c;
}

/* Override-redirect windows (menus, tooltips, dropdowns) bypass WM
 * negotiation but are still caught by CompositeRedirectAutomatic, so they
 * must be composited too -- at their real screen position, never scaled. */
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
    if (z->focused == w) {
        z->focused = z->nclients ? z->clients[z->nclients-1].window : 0;
        if (z->focused) XSetInputFocus(z->dpy, z->focused, RevertToPointerRoot, CurrentTime);
    }
}

static void redraw(ZWM *z) {
    XRenderColor bg = { CANVAS_BG_R * 257, CANVAS_BG_G * 257, CANVAS_BG_B * 257, 0xffff };
    XRectangle full = { 0, 0, (unsigned short)z->sw, (unsigned short)z->sh };
    XRenderFillRectangles(z->dpy, PictOpSrc, z->overlay_pic, &bg, &full, 1);

    /* Each monitor renders the same canvas through its own Viewport. Clip to
     * the monitor's rect on the shared overlay so a window that's clipped
     * out of one monitor's view doesn't bleed into a neighboring monitor
     * that happens to be panned/zoomed to show it at the same overlay
     * coordinates. */
    for (int mi = 0; mi < z->nmonitors; mi++) {
        Monitor *m = &z->monitors[mi];
        XRectangle clip = { (short)m->mx, (short)m->my,
                             (unsigned short)m->mw, (unsigned short)m->mh };
        XRenderSetPictureClipRectangles(z->dpy, z->overlay_pic, 0, 0, &clip, 1);

        for (int i = 0; i < z->nclients; i++) {
            Client *c = &z->clients[i];
            int sx, sy, sw, sh;
            srect(c, &m->vp, &sx, &sy, &sw, &sh);
            if (sx >= m->mw || sy >= m->mh || sx+sw <= 0 || sy+sh <= 0) continue;
            sx += m->mx; sy += m->my;

            Pixmap pix = XCompositeNameWindowPixmap(z->dpy, c->window);
            if (!pix) continue;
            Picture src = XRenderCreatePicture(z->dpy, pix,
                c->depth == 32 ? z->fmt32 : z->fmt24, 0, NULL);

            if (m->vp.zoom != 1.0) {
                XFixed inv = XDoubleToFixed(1.0 / m->vp.zoom);
                XTransform xf = {{{ inv, 0, 0 }, { 0, inv, 0 }, { 0, 0, XDoubleToFixed(1.0) }}};
                /* Bilinear smooths nicely when shrinking, but blurs when
                 * magnifying -- nearest-neighbor keeps zoomed-in content crisp. */
                const char *filt = m->vp.zoom > 1.0 ? FilterNearest : FilterBilinear;
                XRenderSetPictureFilter(z->dpy, src, filt, NULL, 0);
                XRenderSetPictureTransform(z->dpy, src, &xf);
            }
            XRenderComposite(z->dpy, PictOpSrc, src, None, z->overlay_pic,
                             0, 0, 0, 0, sx, sy, sw, sh);
            XRenderFreePicture(z->dpy, src);
            XFreePixmap(z->dpy, pix);
        }
    }
    XRenderSetPictureClipRectangles(z->dpy, z->overlay_pic, 0, 0, &full, 1);

    /* Override-redirect popups sit at a fixed real screen position (never
     * canvas-scaled), so they're drawn once, independent of any monitor. */
    for (int i = 0; i < z->noverride; i++) {
        Window w = z->overrides[i];
        XWindowAttributes a;
        if (!XGetWindowAttributes(z->dpy, w, &a) || a.map_state != IsViewable) continue;
        if (a.x >= z->sw || a.y >= z->sh || a.x+a.width <= 0 || a.y+a.height <= 0) continue;

        Pixmap pix = XCompositeNameWindowPixmap(z->dpy, w);
        if (!pix) continue;
        Picture src = XRenderCreatePicture(z->dpy, pix,
            a.depth == 32 ? z->fmt32 : z->fmt24, 0, NULL);
        XRenderComposite(z->dpy, PictOpOver, src, None, z->overlay_pic,
                         0, 0, 0, 0, a.x, a.y, a.width, a.height);
        XRenderFreePicture(z->dpy, src);
        XFreePixmap(z->dpy, pix);
    }

    XFlush(z->dpy);
}

static void on_map(ZWM *z, XMapRequestEvent *ev) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(z->dpy, ev->window, &a)) return;
    if (a.override_redirect) { XMapWindow(z->dpy, ev->window); return; }
    Client *c = find(z, ev->window);
    if (!c && (c = manage(z, ev->window))) {
        if (c->anchor) {
            /* Fixed landmark at the canvas origin -- not wherever the
             * viewport happens to be when it's launched. */
            c->cx = -c->cw / 2.0;
            c->cy = -c->ch / 2.0;
        } else {
            /* New windows appear centered on whichever monitor's viewport
             * the pointer is currently over, so they land where the user
             * is looking rather than always on a fixed monitor. */
            Window dr, dc; int rx, ry, wx, wy; unsigned int mask;
            XQueryPointer(z->dpy, z->root, &dr, &dc, &rx, &ry, &wx, &wy, &mask);
            Monitor *m = find_monitor(z, rx, ry);
            c->cx = m->vp.cx - c->cw / 2.0;
            c->cy = m->vp.cy - c->ch / 2.0;
        }
    }
    XMapWindow(z->dpy, ev->window);
    if (c) { z->focused = c->window; XSetInputFocus(z->dpy, c->window, RevertToPointerRoot, CurrentTime); }
    redraw(z);
}

static void on_unmap(ZWM *z, XUnmapEvent *ev) {
    if (!ev->send_event) {
        unmanage(z, ev->window); unmanage_override(z, ev->window); redraw(z);
    }
}

static void on_destroy(ZWM *z, XDestroyWindowEvent *ev) {
    unmanage(z, ev->window); unmanage_override(z, ev->window); redraw(z);
}

/* Override-redirect windows map themselves directly (no MapRequest), so
 * they're only visible to us via MapNotify. */
static void on_mapnotify(ZWM *z, XMapEvent *ev) {
    if (ev->window == z->overlay) return;
    XWindowAttributes a;
    if (!XGetWindowAttributes(z->dpy, ev->window, &a)) return;
    if (a.override_redirect) { manage_override(z, ev->window); redraw(z); }
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

static void on_button(ZWM *z, XButtonEvent *ev) {
    Monitor *m = find_monitor(z, ev->x_root, ev->y_root);
    int lx = ev->x_root - m->mx, ly = ev->y_root - m->my;

    if (ev->button == 4) { zoom_at(&m->vp, lx, ly, ZOOM_SPEED);     redraw(z); return; }
    if (ev->button == 5) { zoom_at(&m->vp, lx, ly, 1.0/ZOOM_SPEED); redraw(z); return; }
    if (ev->button == 2) {
        z->panning = 1; z->pan_sx = ev->x_root; z->pan_sy = ev->y_root;
        z->pan_mon = (int)(m - z->monitors);
        z->pan_vx = m->vp.cx; z->pan_vy = m->vp.cy;
        XGrabPointer(z->dpy, z->overlay, True, PointerMotionMask|ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        return;
    }
    if (ev->button == 1) {
        Client *c = hit(z, m, lx, ly);
        if (c) {
            raise_client(z, c);
            c = &z->clients[z->nclients - 1];
            z->focused = c->window; XSetInputFocus(z->dpy, c->window, RevertToPointerRoot, CurrentTime);
            redraw(z);
        }
        if ((ev->state & Mod4Mask) && c && !c->anchor) {
            z->moving = 1; z->move_win = c->window;
            z->move_mon = (int)(m - z->monitors);
            z->move_sx = ev->x_root; z->move_sy = ev->y_root;
            z->move_cx = c->cx;      z->move_cy = c->cy;
            XGrabPointer(z->dpy, z->overlay, True, PointerMotionMask|ButtonReleaseMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        }
    }
    if (ev->button == 3) {
        Client *c = hit(z, m, lx, ly);
        if ((ev->state & Mod4Mask) && c) {
            raise_client(z, c);
            c = &z->clients[z->nclients - 1];
            z->focused = c->window; XSetInputFocus(z->dpy, c->window, RevertToPointerRoot, CurrentTime);
            redraw(z);
            z->resizing = 1; z->resize_win = c->window;
            z->resize_mon = (int)(m - z->monitors);
            z->resize_sx = ev->x_root; z->resize_sy = ev->y_root;
            z->resize_cw0 = c->cw;     z->resize_ch0 = c->ch;
            XGrabPointer(z->dpy, z->overlay, True, PointerMotionMask|ButtonReleaseMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        }
    }
}

static void on_release(ZWM *z, XButtonEvent *ev) {
    if ((ev->button == 2 && z->panning) || (ev->button == 1 && z->moving) ||
        (ev->button == 3 && z->resizing)) {
        z->panning = z->moving = z->resizing = 0;
        z->move_win = z->resize_win = 0;
        XUngrabPointer(z->dpy, CurrentTime);
    }
}

static void on_motion(ZWM *z, XMotionEvent *ev) {
    XEvent next;
    while (XCheckTypedEvent(z->dpy, MotionNotify, &next)) *ev = next.xmotion;
    if (z->moving) {
        Client *c = find(z, z->move_win);
        Monitor *m = &z->monitors[z->move_mon];
        if (c) {
            c->cx = z->move_cx + (ev->x_root - z->move_sx) / m->vp.zoom;
            c->cy = z->move_cy + (ev->y_root - z->move_sy) / m->vp.zoom;
            redraw(z);
        }
    } else if (z->resizing) {
        Client *c = find(z, z->resize_win);
        Monitor *m = &z->monitors[z->resize_mon];
        if (c) {
            int nw = z->resize_cw0 + (int)((ev->x_root - z->resize_sx) / m->vp.zoom);
            int nh = z->resize_ch0 + (int)((ev->y_root - z->resize_sy) / m->vp.zoom);
            c->cw = nw < 20 ? 20 : nw;
            c->ch = nh < 20 ? 20 : nh;
            park(z, c);
            redraw(z);
        }
    } else if (z->panning) {
        Monitor *m = &z->monitors[z->pan_mon];
        m->vp.cx = z->pan_vx - (ev->x_root - z->pan_sx) / m->vp.zoom;
        m->vp.cy = z->pan_vy - (ev->y_root - z->pan_sy) / m->vp.zoom;
        redraw(z);
    }
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

/* (Re)builds the monitor list from XRandR. A monitor that matches an
 * existing one by output name keeps its current pan/zoom; a newly
 * connected monitor starts centered at the canvas origin, unzoomed. Falls
 * back to a single monitor spanning the whole root if XRandR reports none
 * (e.g. a plain Xephyr session with no monitors configured). */
static void detect_monitors(ZWM *z) {
    XWindowAttributes ra;
    if (XGetWindowAttributes(z->dpy, z->root, &ra)) { z->sw = ra.width; z->sh = ra.height; }

    Monitor old[MAX_MONITORS];
    int nold = z->nmonitors;
    memcpy(old, z->monitors, sizeof(old));

    XRRMonitorInfo *info = NULL;
    int n = 0;
    {
        int got = 0;
        XRRMonitorInfo *r = XRRGetMonitors(z->dpy, z->root, True, &got);
        if (r && got > 0) { info = r; n = got; }
        else if (r) XRRFreeMonitors(r);
    }
    if (n <= 0) n = 1;
    if (n > MAX_MONITORS) n = MAX_MONITORS;
    z->nmonitors = n;

    for (int i = 0; i < n; i++) {
        Monitor *m = &z->monitors[i];
        if (info) {
            m->mx = info[i].x; m->my = info[i].y;
            m->mw = info[i].width; m->mh = info[i].height;
            char *aname = XGetAtomName(z->dpy, info[i].name);
            snprintf(m->name, sizeof(m->name), "%s", aname ? aname : "");
            if (aname) XFree(aname);
        } else {
            m->mx = 0; m->my = 0; m->mw = z->sw; m->mh = z->sh;
            snprintf(m->name, sizeof(m->name), "default");
        }
        m->vp.sw = m->mw; m->vp.sh = m->mh;

        int matched = 0;
        for (int j = 0; j < nold; j++) {
            if (!strcmp(old[j].name, m->name)) {
                m->vp.cx = old[j].vp.cx; m->vp.cy = old[j].vp.cy; m->vp.zoom = old[j].vp.zoom;
                matched = 1; break;
            }
        }
        if (!matched) { m->vp.cx = 0; m->vp.cy = 0; m->vp.zoom = 1.0; }
    }
    if (info) XRRFreeMonitors(info);
}

int main(void) {
    ZWM z = {0};

    if (!(z.dpy = XOpenDisplay(NULL))) { fputs("zzwm: no display\n", stderr); return 1; }
    int scr = DefaultScreen(z.dpy);
    z.root = RootWindow(z.dpy, scr);

    XSetErrorHandler(xerr);
    XSelectInput(z.dpy, z.root, SubstructureRedirectMask|SubstructureNotifyMask);
    XSync(z.dpy, False);
    if (g_other_wm) { fputs("zzwm: another WM running\n", stderr); return 1; }

    int eb, ee;
    if (!XCompositeQueryExtension(z.dpy, &eb, &ee)) { fputs("zzwm: no XComposite\n", stderr); return 1; }
    if (!XDamageQueryExtension(z.dpy, &z.damage_event, &ee)) { fputs("zzwm: no XDamage\n", stderr); return 1; }
    if (!XRenderQueryExtension(z.dpy, &eb, &ee))    { fputs("zzwm: no XRender\n",    stderr); return 1; }
    if (!XRRQueryExtension(z.dpy, &z.randr_event, &ee)) { fputs("zzwm: no XRandR\n", stderr); return 1; }
    XRRSelectInput(z.dpy, z.root, RRScreenChangeNotifyMask);
    detect_monitors(&z);

    for (int i = 0; i < NBINDINGS; i++) {
        Binding *b = &bindings[i];
        b->keycode = XKeysymToKeycode(z.dpy, b->keysym);
        XGrabKey(z.dpy, b->keycode, b->mod, z.root, True, GrabModeAsync, GrabModeAsync);
    }

    XCompositeRedirectSubwindows(z.dpy, z.root, CompositeRedirectAutomatic);
    z.overlay = XCompositeGetOverlayWindow(z.dpy, z.root);
    XSelectInput(z.dpy, z.overlay, ButtonPressMask|ButtonReleaseMask|PointerMotionMask|KeyPressMask);
    Cursor cur = XCreateFontCursor(z.dpy, XC_left_ptr);
    XDefineCursor(z.dpy, z.overlay, cur); XFreeCursor(z.dpy, cur);
    int btns[] = { 1, 2, 3, 4, 5 };
    for (int i = 0; i < 5; i++)
        XGrabButton(z.dpy, btns[i], AnyModifier, z.overlay, True,
                    ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);

    z.fmt32 = XRenderFindStandardFormat(z.dpy, PictStandardARGB32);
    z.fmt24 = XRenderFindStandardFormat(z.dpy, PictStandardRGB24);
    XWindowAttributes oa;
    XGetWindowAttributes(z.dpy, z.overlay, &oa);
    z.overlay_pic = XRenderCreatePicture(z.dpy, z.overlay,
                                         XRenderFindVisualFormat(z.dpy, oa.visual), 0, NULL);

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
    redraw(&z);

    XEvent ev;
    for (;;) {
        XNextEvent(z.dpy, &ev);
        if (ev.type == z.damage_event + XDamageNotify) {
            XDamageSubtract(z.dpy, ((XDamageNotifyEvent *)&ev)->damage, None, None);
            redraw(&z);
            continue;
        }
        if (ev.type == z.randr_event + RRScreenChangeNotify) {
            XRRUpdateConfiguration(&ev);
            detect_monitors(&z);
            redraw(&z);
            continue;
        }
        switch (ev.type) {
        case MapRequest:       on_map(&z, &ev.xmaprequest);             break;
        case MapNotify:        on_mapnotify(&z, &ev.xmap);              break;
        case UnmapNotify:      on_unmap(&z, &ev.xunmap);                break;
        case DestroyNotify:    on_destroy(&z, &ev.xdestroywindow);      break;
        case ConfigureRequest: on_configure(&z, &ev.xconfigurerequest); break;
        case ButtonPress:      on_button(&z, &ev.xbutton);              break;
        case ButtonRelease:    on_release(&z, &ev.xbutton);             break;
        case MotionNotify:     on_motion(&z, &ev.xmotion);              break;
        case KeyPress:         on_key(&z, &ev.xkey); redraw(&z);        break;
        }
    }
}
