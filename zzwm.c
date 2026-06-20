/*
 * ZZWM (Zoe's Zooming Window Manager)
 *
 * Main Controls
 * -------------
 *   scroll wheel        zoom in / out (centred on cursor)
 *   middle-click drag   pan the canvas
 *   left/right-click    focus + raise, then forwarded to the window (click
 *                        buttons, links, etc. -- see Architecture)
 *   Super+left-drag     move window on canvas
 *   Super+right-drag    resize window
 *
 * See config.h for customizable keybindings to spawn apps and close windows.
 *
 * Build:  cc -O2 -o zzwm zzwm.c -lX11 -lXrender -lXcomposite -lXdamage -lXfixes -lXi
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
#include <X11/extensions/shape.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrender.h>

#include "appearance.h"

#define ZOOM_SPEED   1.1
#define ZOOM_MIN     0.05
#define ZOOM_MAX     20.0
#define MAX_CLIENTS  64

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

typedef struct {
    Display  *dpy;
    Window    root, overlay;
    Picture   overlay_pic;
    int       sw, sh;
    Viewport  vp;
    XRenderPictFormat *fmt32, *fmt24;
    Client    clients[MAX_CLIENTS];
    int       nclients;
    Window    overrides[MAX_CLIENTS];
    int       noverride;
    Window    focused;
    int       damage_event;
    int       xi_opcode;
    Window    hover_win;   /* client currently parked under the real cursor */
    int    panning, pan_sx, pan_sy;
    double pan_vx, pan_vy;
    int    moving, move_sx, move_sy;
    double move_cx, move_cy;
    Window move_win;
    int    resizing, resize_sx, resize_sy, resize_cw0, resize_ch0;
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

static Client *find(ZWM *z, Window w) {
    for (int i = 0; i < z->nclients; i++)
        if (z->clients[i].window == w) return &z->clients[i];
    return NULL;
}

static Client *hit(ZWM *z, int sx, int sy) {
    for (int i = z->nclients - 1; i >= 0; i--) {
        int x, y, w, h;
        srect(&z->clients[i], &z->vp, &x, &y, &w, &h);
        if (sx >= x && sx < x+w && sy >= y && sy < y+h) return &z->clients[i];
    }
    return NULL;
}

/* Override-redirect windows (menus, tooltips) are drawn -- and already sit,
 * for real, at their real screen position (see redraw()), so hit-testing
 * them is a plain unscaled rectangle test against their current geometry.
 * Used by on_hover() to avoid parking a client on top of a popup. */
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

/* zzwm draws every client window scaled/panned by the canvas transform, but
 * the real window itself sits parked off-screen at its native size (see
 * park()) -- so a screen-space point can't be delivered to it by ordinary
 * X event routing the way it would on a normal (non-compositing) WM. Invert
 * the canvas transform to get a canvas point, then subtract the window's
 * canvas-space origin to get a window-local pixel. */
static void to_client_local(Client *c, Viewport *v, int sx, int sy, int *wx, int *wy) {
    double cx, cy;
    to_canvas(v, sx, sy, &cx, &cy);
    *wx = (int)(cx - c->cx);
    *wy = (int)(cy - c->cy);
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
    to_canvas(&z->vp, a.x, a.y, &c->cx, &c->cy);
    c->cw = a.width; c->ch = a.height; c->depth = a.depth;
    char *name = NULL;
    c->anchor = XFetchName(z->dpy, w, &name) && name && !strcmp(name, ANCHOR_NAME);
    if (name) XFree(name);
    /* The composited window pixmap includes the X border, so a nonzero
     * border_width (many toolkits default to 1px) shows up baked into the
     * rendered window. Strip it -- zzwm draws no decorations of its own. */
    if (a.border_width) XSetWindowBorderWidth(z->dpy, w, 0);
    park(z, c);
    /* Passive, synchronous grab: on_hover() keeps this window parked under
     * the real cursor whenever it's the visual hit-target, so a plain click
     * (no modifier -- Super-drag is grabbed globally on the root instead)
     * lands for real on this exact window. on_button() does its focus+raise
     * bookkeeping and replays the event, letting the X server deliver the
     * genuine press -- and everything that follows it (drag motion,
     * release) -- straight to the client via its own implicit grab. */
    XGrabButton(z->dpy, Button1, 0, w, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);
    XGrabButton(z->dpy, Button3, 0, w, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);
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
    if (z->hover_win == w)  z->hover_win = 0;
    if (z->focused == w) {
        z->focused = z->nclients ? z->clients[z->nclients-1].window : 0;
        if (z->focused) XSetInputFocus(z->dpy, z->focused, RevertToPointerRoot, CurrentTime);
    }
}

static void redraw(ZWM *z) {
    XRenderColor bg = { CANVAS_BG_R * 257, CANVAS_BG_G * 257, CANVAS_BG_B * 257, 0xffff };
    XRectangle rect = { 0, 0, (unsigned short)z->sw, (unsigned short)z->sh };
    XRenderFillRectangles(z->dpy, PictOpSrc, z->overlay_pic, &bg, &rect, 1);

    XRenderColor border_col = { BORDER_R * 257, BORDER_G * 257, BORDER_B * 257, 0xffff };

    for (int i = 0; i < z->nclients; i++) {
        Client *c = &z->clients[i];
        int sx, sy, sw, sh;
        srect(c, &z->vp, &sx, &sy, &sw, &sh);

        /* Border scales with zoom like the window itself, so it stays
         * proportional whether you're zoomed in or out. */
        int bw = (int)(BORDER_THICKNESS * z->vp.zoom + 0.5);
        int bx = sx - bw, by = sy - bw, bsw = sw + 2*bw, bsh = sh + 2*bw;
        if (bx >= z->sw || by >= z->sh || bx+bsw <= 0 || by+bsh <= 0) continue;
        if (bw > 0) {
            XRectangle brect = { (short)bx, (short)by,
                                  (unsigned short)bsw, (unsigned short)bsh };
            XRenderFillRectangles(z->dpy, PictOpSrc, z->overlay_pic, &border_col, &brect, 1);
        }

        Pixmap pix = XCompositeNameWindowPixmap(z->dpy, c->window);
        if (!pix) continue;
        Picture src = XRenderCreatePicture(z->dpy, pix,
            c->depth == 32 ? z->fmt32 : z->fmt24, 0, NULL);

        if (z->vp.zoom != 1.0) {
            XFixed inv = XDoubleToFixed(1.0 / z->vp.zoom);
            XTransform xf = {{{ inv, 0, 0 }, { 0, inv, 0 }, { 0, 0, XDoubleToFixed(1.0) }}};
            /* Bilinear smooths nicely when shrinking, but blurs when
             * magnifying -- nearest-neighbor keeps zoomed-in content crisp. */
            const char *filt = z->vp.zoom > 1.0 ? FilterNearest : FilterBilinear;
            XRenderSetPictureFilter(z->dpy, src, filt, NULL, 0);
            XRenderSetPictureTransform(z->dpy, src, &xf);
        }
        XRenderComposite(z->dpy, PictOpSrc, src, None, z->overlay_pic,
                         0, 0, 0, 0, sx, sy, sw, sh);
        XRenderFreePicture(z->dpy, src);
        XFreePixmap(z->dpy, pix);
    }

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
            c->cx = z->vp.cx - c->cw / 2.0;
            c->cy = z->vp.cy - c->ch / 2.0;
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
    if (ev->button == 4) { zoom_at(&z->vp, ev->x_root, ev->y_root, ZOOM_SPEED);     redraw(z); return; }
    if (ev->button == 5) { zoom_at(&z->vp, ev->x_root, ev->y_root, 1.0/ZOOM_SPEED); redraw(z); return; }
    if (ev->button == 2) {
        z->panning = 1; z->pan_sx = ev->x_root; z->pan_sy = ev->y_root;
        z->pan_vx = z->vp.cx; z->pan_vy = z->vp.cy;
        XGrabPointer(z->dpy, z->overlay, True, PointerMotionMask|ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        return;
    }
    if (ev->button != 1 && ev->button != 3) return;

    /* Super+drag: grabbed globally on the root (see main()), so this is a
     * WM gesture regardless of which real window the pointer is over. */
    if (ev->state & Mod4Mask) {
        Client *c = hit(z, ev->x_root, ev->y_root);
        if (!c) return;
        raise_client(z, c);
        c = &z->clients[z->nclients - 1];
        z->focused = c->window; XSetInputFocus(z->dpy, c->window, RevertToPointerRoot, CurrentTime);
        redraw(z);
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

    /* Plain click: this is the per-client synchronous grab from manage()
     * firing, with ev->window the real client (on_hover() keeps whatever's
     * under the cursor parked there). Do the focus+raise bookkeeping, then
     * replay the event -- the X server re-does its own hit-test, finds the
     * same window still sitting right there, and delivers the genuine
     * press for real. Its own implicit grab then carries the rest of the
     * gesture (drag motion, release) straight to the client, with zero
     * further involvement from us, visible to XInput2 listeners and all. */
    Client *c = find(z, ev->window);
    if (c) {
        raise_client(z, c);
        c = &z->clients[z->nclients - 1];
        z->focused = c->window; XSetInputFocus(z->dpy, c->window, RevertToPointerRoot, CurrentTime);
        redraw(z);
    }
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

static void on_motion(ZWM *z, XMotionEvent *ev) {
    XEvent next;
    while (XCheckTypedEvent(z->dpy, MotionNotify, &next)) *ev = next.xmotion;
    if (z->moving) {
        Client *c = find(z, z->move_win);
        if (c) {
            c->cx = z->move_cx + (ev->x_root - z->move_sx) / z->vp.zoom;
            c->cy = z->move_cy + (ev->y_root - z->move_sy) / z->vp.zoom;
            redraw(z);
        }
    } else if (z->resizing) {
        Client *c = find(z, z->resize_win);
        if (c) {
            int nw = z->resize_cw0 + (int)((ev->x_root - z->resize_sx) / z->vp.zoom);
            int nh = z->resize_ch0 + (int)((ev->y_root - z->resize_sy) / z->vp.zoom);
            c->cw = nw < 20 ? 20 : nw;
            c->ch = nh < 20 ? 20 : nh;
            park(z, c);
            redraw(z);
        }
    } else if (z->panning) {
        z->vp.cx = z->pan_vx - (ev->x_root - z->pan_sx) / z->vp.zoom;
        z->vp.cy = z->pan_vy - (ev->y_root - z->pan_sy) / z->vp.zoom;
        redraw(z);
    }
}

/* Driven by XI_RawMotion (selected globally on the root in main()), which
 * fires on every real pointer move regardless of which window currently
 * owns any grab -- exactly the property that lets this run continuously
 * during a drag without disturbing whatever real, implicit grab is
 * carrying that drag's events to a client (see on_button()).
 *
 * Keeps the client visually under the cursor's real window parked at the
 * matching real screen position (and raised), so plain X event routing --
 * not synthetic events -- delivers input to the right native pixel. This
 * also keeps drags zoom-correct: re-deriving the window-local point from
 * the live inverse canvas transform on every motion means a screen-pixel
 * mouse delta still becomes the right native-pixel delta at any zoom. */
static void on_hover(ZWM *z) {
    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    if (!XQueryPointer(z->dpy, z->root, &root_ret, &child_ret,
                        &root_x, &root_y, &win_x, &win_y, &mask))
        return;

    XWindowAttributes a;
    Client *c = hit_override(z, root_x, root_y, &a) ? NULL : hit(z, root_x, root_y);
    if (c && z->hover_win && c->window != z->hover_win) {
        Client *prev = find(z, z->hover_win);
        if (prev) park(z, prev);
    }
    if (!c) {
        if (z->hover_win) {
            Client *prev = find(z, z->hover_win);
            if (prev) park(z, prev);
            z->hover_win = 0;
        }
        return;
    }

    int wx, wy;
    to_client_local(c, &z->vp, root_x, root_y, &wx, &wy);
    XMoveWindow(z->dpy, c->window, root_x - wx, root_y - wy);
    XRaiseWindow(z->dpy, c->window);
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
    ZWM z = { .vp = { .zoom = 1.0 } };

    if (!(z.dpy = XOpenDisplay(NULL))) { fputs("zzwm: no display\n", stderr); return 1; }
    int scr = DefaultScreen(z.dpy);
    z.root = RootWindow(z.dpy, scr);
    z.sw = z.vp.sw = DisplayWidth(z.dpy, scr);
    z.sh = z.vp.sh = DisplayHeight(z.dpy, scr);

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

    for (int i = 0; i < NBINDINGS; i++) {
        Binding *b = &bindings[i];
        b->keycode = XKeysymToKeycode(z.dpy, b->keysym);
        XGrabKey(z.dpy, b->keycode, b->mod, z.root, True, GrabModeAsync, GrabModeAsync);
    }

    /* Super+move, Super+resize, pan and zoom are pure WM gestures -- grabbed
     * globally on the root (fires regardless of which real window the
     * pointer is over) rather than on the overlay. Plain clicks (no
     * modifier) are deliberately left ungrabbed here: they're caught by the
     * per-client passive grab in manage() instead, so they can be replayed
     * straight through to the client (see on_button()). */
    XGrabButton(z.dpy, Button1, Mod4Mask, z.root, True, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(z.dpy, Button3, Mod4Mask, z.root, True, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);
    int wheel_btns[] = { 2, 4, 5 };
    for (int i = 0; i < 3; i++)
        XGrabButton(z.dpy, wheel_btns[i], AnyModifier, z.root, True, ButtonPressMask,
                    GrabModeAsync, GrabModeAsync, None, None);

    unsigned char ximask[(XI_LASTEVENT + 7) / 8] = {0};
    XISetMask(ximask, XI_RawMotion);
    XIEventMask evmask = { XIAllMasterDevices, sizeof(ximask), ximask };
    XISelectEvents(z.dpy, z.root, &evmask, 1);

    XCompositeRedirectSubwindows(z.dpy, z.root, CompositeRedirectAutomatic);
    z.overlay = XCompositeGetOverlayWindow(z.dpy, z.root);
    XSelectInput(z.dpy, z.overlay, ButtonPressMask|ButtonReleaseMask|PointerMotionMask|KeyPressMask);
    Cursor cur = XCreateFontCursor(z.dpy, XC_left_ptr);
    XDefineCursor(z.dpy, z.overlay, cur); XFreeCursor(z.dpy, cur);
    /* Make the overlay (and so the visual, zoomed canvas it draws) fully
     * input-transparent: with no shape, every plain click/drag/release
     * passes straight through to whichever real client window on_hover()
     * has parked under the cursor, just like InfiniteGlass's
     * overlay_set_input(False). The WM gestures above don't need the
     * overlay at all, since they're grabbed on the root. */
    XserverRegion empty = XFixesCreateRegion(z.dpy, NULL, 0);
    XFixesSetWindowShapeRegion(z.dpy, z.overlay, ShapeInput, 0, 0, empty);
    XFixesDestroyRegion(z.dpy, empty);

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
        case GenericEvent:
            if (ev.xcookie.extension == z.xi_opcode && XGetEventData(z.dpy, &ev.xcookie)) {
                if (ev.xcookie.evtype == XI_RawMotion) on_hover(&z);
                XFreeEventData(z.dpy, &ev.xcookie);
            }
            break;
        }
    }
}
