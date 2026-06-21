/*
 * zzwm-help — lists fixed mouse/scroll basics plus whatever's configured
 * in config.h. Reuses config.h's BIND list directly (same X-macro trick as
 * zzwm.c), so this never drifts from the real configuration. Any keypress
 * closes the window -- not clicks, since zzwm grabs all pointer buttons on
 * its overlay and they never reach client windows.
 *
 * Build:  cc -O2 -o zzwm-help help.c -lX11
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include "../appearance.h"

typedef enum { ACT_SPAWN, ACT_CLOSE } Action;

typedef struct {
    unsigned int mod;
    KeySym       keysym;
    Action       action;
    const char  *arg;
} Binding;

#define BIND(mod, key, act, arg) { mod, key, act, arg },
static Binding bindings[] = {
#include "../config.h"
};
#undef BIND
#define NBINDINGS (int)(sizeof(bindings) / sizeof(bindings[0]))

static const char *BASIC[] = {
    "Scroll wheel        Zoom in / out, centred on cursor",
    "Middle-click drag   Pan the canvas",
    "Left-click window    Focus (and raise to top)",
    "Super+left-drag      Move window on canvas",
    "Super+right-drag     Resize window",
};
#define NBASIC (int)(sizeof(BASIC) / sizeof(BASIC[0]))

static const char *mod_name(unsigned int mod, char *buf, size_t bufsz) {
    buf[0] = '\0';
    if (mod & Mod4Mask)    strncat(buf, "Super+", bufsz - strlen(buf) - 1);
    if (mod & Mod1Mask)    strncat(buf, "Alt+",   bufsz - strlen(buf) - 1);
    if (mod & ControlMask) strncat(buf, "Ctrl+",  bufsz - strlen(buf) - 1);
    if (mod & ShiftMask)   strncat(buf, "Shift+", bufsz - strlen(buf) - 1);
    size_t n = strlen(buf);
    if (n > 0) buf[n - 1] = '\0'; /* drop trailing '+' */
    else strncpy(buf, "?", bufsz);
    return buf;
}

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fputs("zzwm-help: no display\n", stderr); return 1; }
    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);
    Colormap cmap = DefaultColormap(dpy, scr);

    unsigned long col_bg  = alloc_color(dpy, cmap, BG_R, BG_G, BG_B);
    unsigned long col_fg  = alloc_color(dpy, cmap, FG_R, FG_G, FG_B);
    unsigned long col_dim = alloc_color(dpy, cmap, DIM_R, DIM_G, DIM_B);

    XFontStruct *font = XLoadQueryFont(dpy, "fixed");
    int lh = font ? font->ascent + font->descent + 4 : 18;

    int nlines = 1 + NBASIC + 1 + 1 + NBINDINGS;
    int w = 520;
    int h = lh * nlines + 16;

    XSetWindowAttributes wa = { .background_pixel = col_bg,
                                 .event_mask = KeyPressMask | ExposureMask };
    Window win = XCreateWindow(dpy, root, 0, 0, w, h, 0, DefaultDepth(dpy, scr),
                                InputOutput, DefaultVisual(dpy, scr),
                                CWBackPixel | CWEventMask, &wa);
    XStoreName(dpy, win, "zzwm-help");

    GC gc = XCreateGC(dpy, win, 0, NULL);
    if (font) XSetFont(dpy, gc, font->fid);

    XMapWindow(dpy, win);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == KeyPress) break;
        if (ev.type != Expose) continue;

        XSetForeground(dpy, gc, col_bg);
        XFillRectangle(dpy, win, gc, 0, 0, w, h);

        int y = lh;
        XSetForeground(dpy, gc, col_fg);
        static const char title[] = "zzwm bindings (press any key to close)";
        XDrawString(dpy, win, gc, 8, y, title, (int)strlen(title));
        y += lh + 4;

        for (int i = 0; i < NBASIC; i++) {
            XDrawString(dpy, win, gc, 8, y, BASIC[i], (int)strlen(BASIC[i]));
            y += lh;
        }

        y += lh / 2;
        XSetForeground(dpy, gc, col_dim);
        static const char cfg[] = "configured (config.h):";
        XDrawString(dpy, win, gc, 8, y, cfg, (int)strlen(cfg));
        y += lh;
        XSetForeground(dpy, gc, col_fg);

        for (int i = 0; i < NBINDINGS; i++) {
            Binding *b = &bindings[i];
            const char *keyname = XKeysymToString(b->keysym);
            char mods[32];
            mod_name(b->mod, mods, sizeof mods);
            char line[256];
            if (b->action == ACT_SPAWN)
                snprintf(line, sizeof line, "%s+%-10s spawn: %s",
                         mods, keyname ? keyname : "?", b->arg ? b->arg : "");
            else
                snprintf(line, sizeof line, "%s+%-10s close focused window",
                         mods, keyname ? keyname : "?");
            XDrawString(dpy, win, gc, 8, y, line, (int)strlen(line));
            y += lh;
        }
        XFlush(dpy);
    }
    XCloseDisplay(dpy);
    return 0;
}
