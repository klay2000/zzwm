/*
 * zzwm-bar — status app + fixed canvas landmark.
 *
 * Named "zzwm-bar" (ANCHOR_NAME in config.h) -- zzwm anchors it at the
 * canvas origin instead of the viewport, and won't close or move it.
 * Shows date/time + a help hint, centred. Intentionally minimal.
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

#define BAR_W 280
#define BAR_H 56

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs("zzwm-bar: no display\n", stderr); return 1; }
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    Colormap cmap = DefaultColormap(dpy, scr);

    unsigned long col_bg  = alloc_color(dpy, cmap, BG_R, BG_G, BG_B);
    unsigned long col_fg  = alloc_color(dpy, cmap, FG_R, FG_G, FG_B);
    unsigned long col_dim = alloc_color(dpy, cmap, DIM_R, DIM_G, DIM_B);

    XSetWindowAttributes wa = { .background_pixel = col_bg, .event_mask = ExposureMask };
    Window win = XCreateWindow(dpy, root, 0, 0, BAR_W, BAR_H, 0, DefaultDepth(dpy, scr),
                                InputOutput, DefaultVisual(dpy, scr),
                                CWBackPixel | CWEventMask, &wa);
    XStoreName(dpy, win, "zzwm-bar");

    GC gc = XCreateGC(dpy, win, 0, NULL);
    XFontStruct *font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, gc, font->fid);

    XMapWindow(dpy, win);

    int xfd = XConnectionNumber(dpy);
    for (;;) {
        char text[64];
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(text, sizeof text, "%Y-%m-%d  %H:%M:%S", &tmv);

        XSetForeground(dpy, gc, col_bg);
        XFillRectangle(dpy, win, gc, 0, 0, BAR_W, BAR_H);

        int len = (int)strlen(text);
        int tw = font ? XTextWidth(font, text, len) : 6 * len;
        int th = font ? font->ascent + font->descent : 14;
        int lh = th + 6;
        XSetForeground(dpy, gc, col_fg);
        XDrawString(dpy, win, gc, (BAR_W - tw) / 2, (BAR_H - lh) / 2 + th, text, len);

        static const char hint[] = "Super+H for help";
        int hlen = (int)strlen(hint);
        int hw = font ? XTextWidth(font, hint, hlen) : 6 * hlen;
        XSetForeground(dpy, gc, col_dim);
        XDrawString(dpy, win, gc, (BAR_W - hw) / 2, (BAR_H - lh) / 2 + th + lh, hint, hlen);
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
