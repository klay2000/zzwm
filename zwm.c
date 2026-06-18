/*
 * zwm — zooming window manager
 *
 * Controls
 * --------
 *   scroll wheel        zoom in / out (centred on cursor)
 *   middle-click drag   pan the canvas
 *   left-click window   focus
 *   Super+left-drag     move window on canvas
 *   Super+Return        spawn xterm
 *   Super+Q             close focused window
 *
 * Build:  cc -O2 -o zwm zwm.c -lX11 -lXrender -lXcomposite
 * Test:   Xephyr :1 -screen 1280x800 &
 *         DISPLAY=:1 ./zwm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>

#define ZOOM_SPEED  1.1
#define ZOOM_MIN    0.05
#define ZOOM_MAX    20.0
#define BG_R        0x1818
#define BG_G        0x1818
#define BG_B        0x2222
#define MAX_CLIENTS 64

typedef struct {
    \Window window;
    double cx, cy;
    int cw, ch, depth; 
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
    KeyCode   kc_enter, kc_q;
    Client    clients[MAX_CLIENTS];
    int       nclients;
    Window    focused;
    int       damage_event;
    int    panning, pan_sx, pan_sy;
    double pan_vx, pan_vy;
    int    moving, move_sx, move_sy;
    double move_cx, move_cy;
    Window move_win;
} ZWM;

/* ── error handler ────────────────────────────────────────────────────────── */

static int g_other_wm;
static int xerr(Display *d, XErrorEvent *e) { (void)d; if (e->error_code == BadAccess) g_other_wm = 1; return 0; }

/* ── viewport ─────────────────────────────────────────────────────────────── */

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

/* ── client helpers ───────────────────────────────────────────────────────── */

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
    park(z, c);
    XDamageCreate(z->dpy, w, XDamageReportNonEmpty);
    return c;
}

static void unmanage(ZWM *z, Window w) {
    int i;
    for (i = 0; i < z->nclients; i++) if (z->clients[i].window == w) break;
    if (i == z->nclients) return;
    z->clients[i] = z->clients[--z->nclients];
    if (z->move_win == w) { z->moving = 0; z->move_win = 0; }
    if (z->focused == w) {
        z->focused = z->nclients ? z->clients[z->nclients-1].window : 0;
        if (z->focused) XSetInputFocus(z->dpy, z->focused, RevertToPointerRoot, CurrentTime);
    }
}

/* ── render ───────────────────────────────────────────────────────────────── */

static void redraw(ZWM *z) {
    XRenderColor bg = { BG_R, BG_G, BG_B, 0xffff };
    XRectangle rect = { 0, 0, (unsigned short)z->sw, (unsigned short)z->sh };
    XRenderFillRectangles(z->dpy, PictOpSrc, z->overlay_pic, &bg, &rect, 1);

    for (int i = 0; i < z->nclients; i++) {
        Client *c = &z->clients[i];
        int sx, sy, sw, sh;
        srect(c, &z->vp, &sx, &sy, &sw, &sh);
        if (sx >= z->sw || sy >= z->sh || sx+sw <= 0 || sy+sh <= 0) continue;

        Pixmap pix = XCompositeNameWindowPixmap(z->dpy, c->window);
        if (!pix) continue;
        Picture src = XRenderCreatePicture(z->dpy, pix,
            c->depth == 32 ? z->fmt32 : z->fmt24, 0, NULL);

        if (z->vp.zoom != 1.0) {
            XFixed inv = XDoubleToFixed(1.0 / z->vp.zoom);
            XTransform xf = {{{ inv, 0, 0 }, { 0, inv, 0 }, { 0, 0, XDoubleToFixed(1.0) }}};
            XRenderSetPictureFilter(z->dpy, src, FilterBilinear, NULL, 0);
            XRenderSetPictureTransform(z->dpy, src, &xf);
        }
        XRenderComposite(z->dpy, PictOpSrc, src, None, z->overlay_pic,
                         0, 0, 0, 0, sx, sy, sw, sh);
        XRenderFreePicture(z->dpy, src);
        XFreePixmap(z->dpy, pix);
    }
    XFlush(z->dpy);
}

/* ── event handlers ───────────────────────────────────────────────────────── */

static void on_map(ZWM *z, XMapRequestEvent *ev) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(z->dpy, ev->window, &a)) return;
    if (a.override_redirect) { XMapWindow(z->dpy, ev->window); return; }
    Client *c = find(z, ev->window); if (!c) c = manage(z, ev->window);
    XMapWindow(z->dpy, ev->window);
    if (c) { z->focused = c->window; XSetInputFocus(z->dpy, c->window, RevertToPointerRoot, CurrentTime); }
    redraw(z);
}

static void on_unmap(ZWM *z, XUnmapEvent *ev) {
    if (!ev->send_event) { unmanage(z, ev->window); redraw(z); }
}

static void on_destroy(ZWM *z, XDestroyWindowEvent *ev) {
    unmanage(z, ev->window); redraw(z);
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
    if (ev->button == 1) {
        Client *c = hit(z, ev->x_root, ev->y_root);
        if (c) { z->focused = c->window; XSetInputFocus(z->dpy, c->window, RevertToPointerRoot, CurrentTime); }
        if ((ev->state & Mod4Mask) && c) {
            z->moving = 1; z->move_win = c->window;
            z->move_sx = ev->x_root; z->move_sy = ev->y_root;
            z->move_cx = c->cx;      z->move_cy = c->cy;
            XGrabPointer(z->dpy, z->overlay, True, PointerMotionMask|ButtonReleaseMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        }
    }
}

static void on_release(ZWM *z, XButtonEvent *ev) {
    if ((ev->button == 2 && z->panning) || (ev->button == 1 && z->moving)) {
        z->panning = z->moving = 0; z->move_win = 0;
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
    } else if (z->panning) {
        z->vp.cx = z->pan_vx - (ev->x_root - z->pan_sx) / z->vp.zoom;
        z->vp.cy = z->pan_vy - (ev->y_root - z->pan_sy) / z->vp.zoom;
        redraw(z);
    }
}

static void on_key(ZWM *z, XKeyEvent *ev) {
    if (!(ev->state & Mod4Mask)) return;
    if (ev->keycode == z->kc_enter) system("xterm &");
    else if (ev->keycode == z->kc_q && z->focused) XDestroyWindow(z->dpy, z->focused);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    ZWM z = { .vp = { .zoom = 1.0 } };

    if (!(z.dpy = XOpenDisplay(NULL))) { fputs("zwm: no display\n", stderr); return 1; }
    int scr = DefaultScreen(z.dpy);
    z.root = RootWindow(z.dpy, scr);
    z.sw = z.vp.sw = DisplayWidth(z.dpy, scr);
    z.sh = z.vp.sh = DisplayHeight(z.dpy, scr);

    XSetErrorHandler(xerr);
    XSelectInput(z.dpy, z.root, SubstructureRedirectMask|SubstructureNotifyMask);
    XSync(z.dpy, False);
    if (g_other_wm) { fputs("zwm: another WM running\n", stderr); return 1; }

    int eb, ee;
    if (!XCompositeQueryExtension(z.dpy, &eb, &ee)) { fputs("zwm: no XComposite\n", stderr); return 1; }
    if (!XDamageQueryExtension(z.dpy, &z.damage_event, &ee)) { fputs("zwm: no XDamage\n", stderr); return 1; }
    if (!XRenderQueryExtension(z.dpy, &eb, &ee))    { fputs("zwm: no XRender\n",    stderr); return 1; }

    z.kc_enter = XKeysymToKeycode(z.dpy, XK_Return);
    z.kc_q     = XKeysymToKeycode(z.dpy, XK_q);
    XGrabKey(z.dpy, z.kc_enter, Mod4Mask, z.root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(z.dpy, z.kc_q,     Mod4Mask, z.root, True, GrabModeAsync, GrabModeAsync);

    XCompositeRedirectSubwindows(z.dpy, z.root, CompositeRedirectAutomatic);
    z.overlay = XCompositeGetOverlayWindow(z.dpy, z.root);
    XSelectInput(z.dpy, z.overlay, ButtonPressMask|ButtonReleaseMask|PointerMotionMask|KeyPressMask);
    Cursor cur = XCreateFontCursor(z.dpy, XC_left_ptr);
    XDefineCursor(z.dpy, z.overlay, cur); XFreeCursor(z.dpy, cur);
    int btns[] = { 1, 2, 4, 5 };
    for (int i = 0; i < 4; i++)
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
                && a.map_state == IsViewable && !a.override_redirect)
            manage(&z, ch[i]);
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
        case UnmapNotify:      on_unmap(&z, &ev.xunmap);                break;
        case DestroyNotify:    on_destroy(&z, &ev.xdestroywindow);      break;
        case ConfigureRequest: on_configure(&z, &ev.xconfigurerequest); break;
        case ButtonPress:      on_button(&z, &ev.xbutton);              break;
        case ButtonRelease:    on_release(&z, &ev.xbutton);             break;
        case MotionNotify:     on_motion(&z, &ev.xmotion);              break;
        case KeyPress:         on_key(&z, &ev.xkey);                    break;
        }
    }
}
