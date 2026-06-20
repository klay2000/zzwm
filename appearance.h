/* Shared appearance settings for zzwm and the zzwm-run/bar/help utility
 * apps. Edit the RGB values (0-255 each) and rebuild (`make`) to restyle.
 */
#include <X11/Xlib.h>

/* zzwm canvas background, behind all windows. */
#define CANVAS_BG_R 0x6d
#define CANVAS_BG_G 0xc1
#define CANVAS_BG_B 0xb4

/* Utility app window background/foreground/dim text. */
#define BG_R 0xff
#define BG_G 0xff
#define BG_B 0xff

#define FG_R 0x11
#define FG_G 0x11
#define FG_B 0x11

#define DIM_R 0x99
#define DIM_G 0x99
#define DIM_B 0x99

/* Border drawn around every managed client window (canvas pixels at
 * zoom 1.0 -- it scales with the window as you zoom, like everything else
 * on the canvas). Set BORDER_THICKNESS to 0 to disable borders entirely. */
#define BORDER_R 0x33
#define BORDER_G 0x33
#define BORDER_B 0x33
#define BORDER_THICKNESS 2

static inline unsigned long alloc_color(Display *dpy, Colormap cmap, int r, int g, int b) {
    XColor c = { .red = (unsigned short)(r * 257), .green = (unsigned short)(g * 257),
                 .blue = (unsigned short)(b * 257), .flags = DoRed | DoGreen | DoBlue };
    XAllocColor(dpy, cmap, &c);
    return c.pixel;
}
