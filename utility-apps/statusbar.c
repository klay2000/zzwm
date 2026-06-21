/*
 * zzwm-bar — status app + fixed canvas landmark.
 *
 * Named "zzwm-bar" (ANCHOR_NAME in config.h) -- zzwm anchors it at the
 * canvas origin instead of the viewport, and won't close or move it.
 * Shows date/time, a help hint, and optionally a line of BAR_CMD output
 * (see config.h), centred. Intentionally minimal.
 *
 * Build:  cc -O2 -o zzwm-bar statusbar.c -lX11
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>
#include <X11/Xlib.h>

#include "../appearance.h"
#define BIND(mod, key, act, arg) // pulls in BAR_* defines without the keybindings table
#include "../config.h"
#undef BIND

#define BAR_W 280

static void run_bar_cmd(char *out, size_t outsz) {
    FILE *p = popen(BAR_CMD, "r");
    if (!p) { out[0] = 0; return; }
    if (!fgets(out, (int)outsz, p)) out[0] = 0;
    pclose(p);
    out[strcspn(out, "\n")] = 0;
}

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs("zzwm-bar: no display\n", stderr); return 1; }
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    Colormap cmap = DefaultColormap(dpy, scr);

    unsigned long col_bg  = alloc_color(dpy, cmap, BG_R, BG_G, BG_B);
    unsigned long col_fg  = alloc_color(dpy, cmap, FG_R, FG_G, FG_B);
    unsigned long col_dim = alloc_color(dpy, cmap, DIM_R, DIM_G, DIM_B);

    XFontStruct *font = XLoadQueryFont(dpy, FONT_NAME);
    int th = font ? font->ascent + font->descent : 14;
    int lh = th + 6;
    int nlines = BAR_CMD[0] ? 3 : 2;
    int bar_h = lh * nlines + 16;

    XSetWindowAttributes wa = { .background_pixel = col_bg, .event_mask = ExposureMask };
    Window win = XCreateWindow(dpy, root, 0, 0, BAR_W, bar_h, 0, DefaultDepth(dpy, scr),
                                InputOutput, DefaultVisual(dpy, scr),
                                CWBackPixel | CWEventMask, &wa);
    XStoreName(dpy, win, "zzwm-bar");

    GC gc = XCreateGC(dpy, win, 0, NULL);
    if (font) XSetFont(dpy, gc, font->fid);

    XMapWindow(dpy, win);

    char cmd_out[64] = "";
    time_t cmd_last = 0;
    int xfd = XConnectionNumber(dpy);
    for (;;) {
        time_t now = time(NULL);
        if (BAR_CMD[0] && now - cmd_last >= BAR_CMD_INTERVAL) {
            run_bar_cmd(cmd_out, sizeof cmd_out);
            cmd_last = now;
        }

        char text[64];
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(text, sizeof text, BAR_TIME_FORMAT, &tmv);

        XSetForeground(dpy, gc, col_bg);
        XFillRectangle(dpy, win, gc, 0, 0, BAR_W, bar_h);

        int y = (bar_h - nlines * lh) / 2 + th;
        int len = (int)strlen(text);
        int tw = font ? XTextWidth(font, text, len) : 6 * len;
        XSetForeground(dpy, gc, col_fg);
        XDrawString(dpy, win, gc, (BAR_W - tw) / 2, y, text, len);
        y += lh;

        int hlen = (int)strlen(BAR_HINT);
        int hw = font ? XTextWidth(font, BAR_HINT, hlen) : 6 * hlen;
        XSetForeground(dpy, gc, col_dim);
        XDrawString(dpy, win, gc, (BAR_W - hw) / 2, y, BAR_HINT, hlen);
        y += lh;

        if (BAR_CMD[0]) {
            int clen = (int)strlen(cmd_out);
            int cw = font ? XTextWidth(font, cmd_out, clen) : 6 * clen;
            XSetForeground(dpy, gc, col_fg);
            XDrawString(dpy, win, gc, (BAR_W - cw) / 2, y, cmd_out, clen);
        }
        XFlush(dpy);

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        select(xfd + 1, &fds, NULL, NULL, &tv);
    }
}
