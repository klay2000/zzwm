/* ZZWM configuration. #included into an array initializer (for bindings),
 * not compiled standalone -- but #define lines work fine anywhere in it.
 */

/* Window named this (via XStoreName/WM_NAME) is anchored at the canvas
 * origin instead of the viewport, and can't be closed or moved. Default
 * matches zzwm-bar (utility-apps/statusbar.c). */
#define ANCHOR_NAME "zzwm-bar"

/* Keybindings: BIND(modifier, keysym, action, arg). modifier is an X11 mask
 * (Mod4Mask = Super, Mod1Mask = Alt, ...); keysym from <X11/keysym.h>;
 * action is ACT_SPAWN (run arg as a shell command) or ACT_CLOSE (close the
 * focused window, arg ignored). Edit and rebuild (`make`) -- no need to
 * touch zzwm.c. */
BIND(Mod4Mask, XK_Return, ACT_SPAWN, "xterm &")
BIND(Mod4Mask, XK_space,  ACT_SPAWN, "zzwm-run &")
BIND(Mod4Mask, XK_h,      ACT_SPAWN, "zzwm-help &")
BIND(Mod4Mask, XK_q,      ACT_CLOSE, NULL)
