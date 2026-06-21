/* ZZWM configuration. #included into an array initializer (for bindings),
 * not compiled standalone -- but #define lines work fine anywhere in it.
 */

/* Window named this (via XStoreName/WM_NAME) is anchored at the canvas
 * origin instead of the viewport, and can't be closed or moved. Default
 * matches zzwm-bar (utility-apps/statusbar.c). */
#define ANCHOR_NAME "zzwm-bar"

/* Keybindings: BIND(modifier, keysym, action, arg). modifier is an X11 mask
 * (Mod4Mask = Super, Mod1Mask = Alt, ShiftMask, ControlMask, ...); OR several
 * together (e.g. Mod4Mask|Mod1Mask) to require holding multiple keys at
 * once. keysym from <X11/keysym.h>; action is ACT_SPAWN (run arg as a shell
 * command) or ACT_CLOSE (close the focused window, arg ignored). Below are
 * some example bindings -- edit, add, or remove as you like.
 *   Super+Return        spawn xterm
 *   Super+Space         spawn zzwm-run (app launcher, included in utility-apps/)
 *   Super+H             spawn zzwm-help (keybinding reference, included in utility-apps/)
 *   Super+Q             close focused window
 *   Super+Alt+L         logout (multi-key bind example)
*/
BIND(Mod4Mask, XK_Return, ACT_SPAWN, "xterm &")
BIND(Mod4Mask, XK_space,  ACT_SPAWN, "zzwm-run &")
BIND(Mod4Mask, XK_h,      ACT_SPAWN, "zzwm-help &")
BIND(Mod4Mask, XK_q,      ACT_CLOSE, NULL)
BIND(Mod4Mask|Mod1Mask, XK_l, ACT_SPAWN, "pkill -x zzwm &")

/* Edge snapping during Super-drag move/resize: an edge within SNAP_DIST
 * canvas units (zoom-independent, like window width/height) of another
 * window's edge locks onto it, leaving SNAP_GAP units between them when
 * snapped side-by-side. SNAP_ENABLED 0 disables it. */
#define SNAP_ENABLED 1
#define SNAP_DIST    12
#define SNAP_GAP     10

/* Idle locking: after IDLE_LOCK_TIMEOUT seconds with no keyboard/mouse input,
 * IDLE_LOCK_CMD is run once. IDLE_LOCK_TIMEOUT 0 disables idle locking.
 * IDLE_LOCK_CMD is a shell command -- like the spawn bindings above, end it
 * with & so it backgrounds instead of blocking the WM (e.g. slock, i3lock,
 * xtrlock, or a custom script). */
#define IDLE_LOCK_TIMEOUT 300
#define IDLE_LOCK_CMD     "dm-tool lock"

/* zzwm-bar (utility-apps/statusbar.c). BAR_CMD is a shell command; each
 * line of its output becomes one centred line in the bar, re-run every
 * BAR_CMD_INTERVAL seconds. */
#define BAR_CMD          "date '+%Y-%m-%d  %H:%M:%S'; echo 'Super+H for help'"
#define BAR_CMD_INTERVAL 1
