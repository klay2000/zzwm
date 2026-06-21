/*
 * zzwm-run — minimal app launcher.
 *
 * A plain X11 client -- zzwm manages it like any other window (parked,
 * centred on the viewport, canvas-scaled). Type to filter $PATH executables
 * by prefix; Tab accepts the top suggestion; Enter launches; Escape exits.
 *
 * Build:  cc -O2 -o zzwm-run runner.c -lX11
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "../appearance.h"

#define BUF_MAX  256
#define MAXCAND  8
#define BOX_W    480

typedef struct {
    Display *dpy;
    Window   win;
    GC       gc;
    XFontStruct *font;
    unsigned long col_bg, col_fg, col_dim;
    char  buf[BUF_MAX];
    int   len;
    char  **bins;
    int   nbins;
    char  *cands[MAXCAND];
    int   ncand;
} App;

static int has_bin(App *a, const char *name) {
    for (int i = 0; i < a->nbins; i++)
        if (!strcmp(a->bins[i], name)) return 1;
    return 0;
}

static void build_cache(App *a) {
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    char *p = strdup(path);
    size_t cap = 256;
    a->bins = malloc(cap * sizeof(char *));
    for (char *dir = strtok(p, ":"); dir; dir = strtok(NULL, ":")) {
        DIR *dp = opendir(dir);
        if (!dp) continue;
        struct dirent *de;
        char full[1024];
        while ((de = readdir(dp))) {
            if (de->d_name[0] == '.') continue;
            if (has_bin(a, de->d_name)) continue;
            snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
            if (access(full, X_OK) != 0) continue;
            if (a->nbins == (int)cap) { cap *= 2; a->bins = realloc(a->bins, cap * sizeof(char *)); }
            a->bins[a->nbins++] = strdup(de->d_name);
        }
        closedir(dp);
    }
    free(p);
}

static void filter(App *a) {
    a->ncand = 0;
    if (a->len == 0) return;
    for (int i = 0; i < a->nbins && a->ncand < MAXCAND; i++)
        if (!strncmp(a->bins[i], a->buf, a->len))
            a->cands[a->ncand++] = a->bins[i];
}

static void redraw(App *a) {
    int lh = a->font ? a->font->ascent + a->font->descent + 4 : 18;
    int bh = lh + 12 + (a->ncand ? a->ncand * lh + 8 : 0);
    XResizeWindow(a->dpy, a->win, BOX_W, bh);

    XSetForeground(a->dpy, a->gc, a->col_bg);
    XFillRectangle(a->dpy, a->win, a->gc, 0, 0, BOX_W, bh);

    int tx = 8, ty = lh - 2;
    XSetForeground(a->dpy, a->gc, a->col_fg);
    XDrawString(a->dpy, a->win, a->gc, tx, ty, a->buf, a->len);

    if (a->ncand > 0) {
        int tw = a->font ? XTextWidth(a->font, a->buf, a->len) : 6 * a->len;
        const char *cand = a->cands[0];
        int tail = (int)strlen(cand) - a->len;
        if (tail > 0) {
            XSetForeground(a->dpy, a->gc, a->col_dim);
            XDrawString(a->dpy, a->win, a->gc, tx + tw, ty, cand + a->len, tail);
        }
        for (int i = 0; i < a->ncand; i++) {
            XSetForeground(a->dpy, a->gc, i == 0 ? a->col_fg : a->col_dim);
            XDrawString(a->dpy, a->win, a->gc, tx, ty + lh * (i + 1) + 8,
                        a->cands[i], (int)strlen(a->cands[i]));
        }
    }
    XFlush(a->dpy);
}

int main(void) {
    App a = {0};
    a.dpy = XOpenDisplay(NULL);
    if (!a.dpy) { fputs("zzwm-run: no display\n", stderr); return 1; }
    int scr = DefaultScreen(a.dpy);
    Window root = RootWindow(a.dpy, scr);
    Colormap cmap = DefaultColormap(a.dpy, scr);

    a.col_bg  = alloc_color(a.dpy, cmap, BG_R, BG_G, BG_B);
    a.col_fg  = alloc_color(a.dpy, cmap, FG_R, FG_G, FG_B);
    a.col_dim = alloc_color(a.dpy, cmap, DIM_R, DIM_G, DIM_B);

    XSetWindowAttributes wa = { .background_pixel = a.col_bg,
                                 .event_mask = KeyPressMask | ExposureMask };
    a.win = XCreateWindow(a.dpy, root, 0, 0, BOX_W, 40, 0, DefaultDepth(a.dpy, scr),
                           InputOutput, DefaultVisual(a.dpy, scr),
                           CWBackPixel | CWEventMask, &wa);
    XStoreName(a.dpy, a.win, "zzwm-run");

    a.gc = XCreateGC(a.dpy, a.win, 0, NULL);
    a.font = XLoadQueryFont(a.dpy, FONT_NAME);
    if (a.font) XSetFont(a.dpy, a.gc, a.font->fid);

    build_cache(&a);
    XMapWindow(a.dpy, a.win);
    redraw(&a);

    XEvent ev;
    for (;;) {
        XNextEvent(a.dpy, &ev);
        if (ev.type == Expose) { redraw(&a); continue; }
        if (ev.type != KeyPress) continue;

        char buf[8];
        KeySym ks;
        XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);

        if (ks == XK_Escape) break;
        if (ks == XK_Return) {
            if (a.len > 0) {
                const char *launch = (a.ncand > 0 && !strchr(a.buf, ' ')) ? a.cands[0] : a.buf;
                char cmd[BUF_MAX + 2];
                snprintf(cmd, sizeof cmd, "%s &", launch);
                system(cmd);
            }
            break;
        }
        if (ks == XK_BackSpace) {
            if (a.len > 0) a.buf[--a.len] = 0;
            filter(&a); redraw(&a); continue;
        }
        if (ks == XK_Tab && a.ncand > 0) {
            snprintf(a.buf, sizeof a.buf, "%s", a.cands[0]);
            a.len = (int)strlen(a.buf);
            filter(&a); redraw(&a); continue;
        }
        if (buf[0] >= 32 && buf[0] < 127 && a.len < BUF_MAX - 1) {
            a.buf[a.len++] = buf[0];
            a.buf[a.len] = 0;
            filter(&a); redraw(&a);
        }
    }
    XCloseDisplay(a.dpy);
    return 0;
}
