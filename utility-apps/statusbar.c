/*
 * zzwm-bar — status app + fixed canvas landmark.
 *
 * Named "zzwm-bar" (ANCHOR_NAME in config.h) -- zzwm anchors it at the
 * canvas origin instead of the viewport, and won't close or move it.
 * Its entire contents come from running BAR_CMD (see config.h): each
 * line of output becomes one centred line in the bar, re-run every
 * BAR_CMD_INTERVAL seconds. Intentionally minimal.
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
#define BAR_MAX_LINES 8
#define BAR_LINE_LEN 64

static int run_bar_cmd(char lines[][BAR_LINE_LEN], int max_lines) {
    FILE *p = popen(BAR_CMD, "r");
    if (!p) return 0;
    int n = 0;
    while (n < max_lines && fgets(lines[n], BAR_LINE_LEN, p)) {
        lines[n][strcspn(lines[n], "\n")] = 0;
        n++;
    }
    pclose(p);
    return n;
}

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs("zzwm-bar: no display\n", stderr); return 1; }
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    Colormap cmap = DefaultColormap(dpy, scr);

    unsigned long col_bg  = alloc_color(dpy, cmap, BG_R, BG_G, BG_B);
    unsigned long col_fg  = alloc_color(dpy, cmap, FG_R, FG_G, FG_B);

    XFontStruct *font = XLoadQueryFont(dpy, FONT_NAME);
    int th = font ? font->ascent + font->descent : 14;
    int lh = th + 6;
    int nlines = 1;
    int bar_h = lh * nlines + 16;

    XSetWindowAttributes wa = { .background_pixel = col_bg, .event_mask = ExposureMask };
    Window win = XCreateWindow(dpy, root, 0, 0, BAR_W, bar_h, 0, DefaultDepth(dpy, scr),
                                InputOutput, DefaultVisual(dpy, scr),
                                CWBackPixel | CWEventMask, &wa);
    XStoreName(dpy, win, "zzwm-bar");

    GC gc = XCreateGC(dpy, win, 0, NULL);
    if (font) XSetFont(dpy, gc, font->fid);

    XMapWindow(dpy, win);

    char cmd_out[BAR_MAX_LINES][BAR_LINE_LEN] = {{0}};
    int cmd_n = 0;
    time_t cmd_last = 0;
    int xfd = XConnectionNumber(dpy);
    for (;;) {
        time_t now = time(NULL);
        if (now - cmd_last >= BAR_CMD_INTERVAL) {
            cmd_n = run_bar_cmd(cmd_out, BAR_MAX_LINES);
            cmd_last = now;
        }

        nlines = cmd_n > 0 ? cmd_n : 1;
        int new_h = lh * nlines + 16;
        if (new_h != bar_h) {
            bar_h = new_h;
            XResizeWindow(dpy, win, BAR_W, bar_h);
        }

        XSetForeground(dpy, gc, col_bg);
        XFillRectangle(dpy, win, gc, 0, 0, BAR_W, bar_h);

        int y = (bar_h - nlines * lh) / 2 + th;
        XSetForeground(dpy, gc, col_fg);
        for (int i = 0; i < cmd_n; i++) {
            int len = (int)strlen(cmd_out[i]);
            int w = font ? XTextWidth(font, cmd_out[i], len) : 6 * len;
            XDrawString(dpy, win, gc, (BAR_W - w) / 2, y, cmd_out[i], len);
            y += lh;
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
