#!/usr/bin/env python3
"""
zui — zooming window manager prototype

All windows live on an infinite 2-D canvas.  Navigate by zooming and panning.
Windows render to off-screen pixmaps (XComposite) and are composited at the
current scale via XRender — so content never reflows regardless of zoom level.

Controls
--------
  scroll wheel          zoom in / out (centred on cursor)
  middle-click drag     pan the canvas
  left-click window     focus + raise
  Super+left-drag       move window on canvas
  Super+Return          spawn xterm
  Super+Q               close focused window

Testing
-------
  Xephyr :1 -screen 1280x800 &
  DISPLAY=:1 python3 zwm.py
"""

import os
import sys
import ctypes
from Xlib import X, XK, error
from Xlib import display as xdisplay
from Xlib.ext import composite

# ── tunables ────────────────────────────────────────────────────────────────

ZOOM_SPEED = 1.1
ZOOM_MIN   = 0.05
ZOOM_MAX   = 20.0
BG_R, BG_G, BG_B = 0x1818, 0x1818, 0x2222   # 16-bit per channel


# ── ctypes XRender structures ────────────────────────────────────────────────

class XRenderColor(ctypes.Structure):
    _fields_ = [('red',   ctypes.c_uint16),
                ('green', ctypes.c_uint16),
                ('blue',  ctypes.c_uint16),
                ('alpha', ctypes.c_uint16)]

class XRectangle(ctypes.Structure):
    _fields_ = [('x',      ctypes.c_int16),
                ('y',      ctypes.c_int16),
                ('width',  ctypes.c_uint16),
                ('height', ctypes.c_uint16)]

class XTransform(ctypes.Structure):
    _fields_ = [('matrix', (ctypes.c_int32 * 3) * 3)]


# ── Renderer ─────────────────────────────────────────────────────────────────

class Renderer:
    """
    Thin ctypes wrapper around libXrender + libX11.
    Opens its own X connection (sharing the server with the python-xlib WM
    connection) so we can call XRender directly.
    """
    PictOpSrc          = 1
    PictStandardARGB32 = 0
    PictStandardRGB24  = 1

    def __init__(self, display_name: str):
        lX  = ctypes.CDLL("libX11.so")
        lXr = ctypes.CDLL("libXrender.so")

        lX.XOpenDisplay.restype          = ctypes.c_void_p
        lX.XDefaultVisual.restype        = ctypes.c_void_p
        lXr.XRenderFindStandardFormat.restype = ctypes.c_void_p
        lXr.XRenderFindVisualFormat.restype   = ctypes.c_void_p
        lXr.XRenderCreatePicture.restype      = ctypes.c_ulong

        self._lX  = lX
        self._lXr = lXr

        self._dpy = lX.XOpenDisplay(display_name.encode())
        if not self._dpy:
            raise RuntimeError(f"Renderer: cannot open display {display_name!r}")

        # Cache picture formats
        self._fmt32 = lXr.XRenderFindStandardFormat(self._dpy, self.PictStandardARGB32)
        self._fmt24 = lXr.XRenderFindStandardFormat(self._dpy, self.PictStandardRGB24)
        vis         = lX.XDefaultVisual(self._dpy, 0)
        self._fmt_vis = lXr.XRenderFindVisualFormat(self._dpy, vis)

        self._overlay_pic = None

    def setup_overlay(self, overlay_xid: int, sw: int, sh: int):
        self._sw = sw
        self._sh = sh
        self._overlay_pic = self._lXr.XRenderCreatePicture(
            self._dpy, ctypes.c_ulong(overlay_xid), self._fmt_vis,
            ctypes.c_ulong(0), None,
        )

    def fill_background(self):
        color = XRenderColor(red=BG_R, green=BG_G, blue=BG_B, alpha=0xffff)
        rect  = XRectangle(x=0, y=0, width=self._sw, height=self._sh)
        self._lXr.XRenderFillRectangles(
            self._dpy, self.PictOpSrc, self._overlay_pic,
            ctypes.byref(color), ctypes.byref(rect), 1,
        )

    def composite_pixmap(self, pix_id: int, depth: int,
                         dst_x: int, dst_y: int,
                         dst_w: int, dst_h: int, scale: float):
        """Scale-composite a pixmap onto the overlay."""
        fmt = self._fmt32 if depth == 32 else self._fmt24
        src = self._lXr.XRenderCreatePicture(
            self._dpy, ctypes.c_ulong(pix_id), fmt, ctypes.c_ulong(0), None,
        )

        if scale != 1.0:
            self._lXr.XRenderSetPictureFilter(
                self._dpy, src, b"bilinear", None, ctypes.c_int(0),
            )
            inv = int(65536 / scale)
            one = 65536
            t = XTransform()
            t.matrix[0][0] = inv; t.matrix[0][1] = 0;   t.matrix[0][2] = 0
            t.matrix[1][0] = 0;   t.matrix[1][1] = inv; t.matrix[1][2] = 0
            t.matrix[2][0] = 0;   t.matrix[2][1] = 0;   t.matrix[2][2] = one
            self._lXr.XRenderSetPictureTransform(self._dpy, src, ctypes.byref(t))

        self._lXr.XRenderComposite(
            self._dpy, ctypes.c_int(self.PictOpSrc),
            src, ctypes.c_ulong(0), self._overlay_pic,
            ctypes.c_int(0), ctypes.c_int(0),
            ctypes.c_int(0), ctypes.c_int(0),
            ctypes.c_int(dst_x), ctypes.c_int(dst_y),
            ctypes.c_uint(dst_w), ctypes.c_uint(dst_h),
        )
        self._lXr.XRenderFreePicture(self._dpy, src)

    def flush(self):
        self._lX.XFlush(self._dpy)


# ── Viewport ─────────────────────────────────────────────────────────────────

class Viewport:
    def __init__(self, sw: int, sh: int):
        self.sw, self.sh = sw, sh
        self.cx = 0.0
        self.cy = 0.0
        self.zoom = 1.0

    def to_screen(self, cx: float, cy: float) -> tuple[float, float]:
        return (
            (cx - self.cx) * self.zoom + self.sw / 2,
            (cy - self.cy) * self.zoom + self.sh / 2,
        )

    def to_canvas(self, sx: float, sy: float) -> tuple[float, float]:
        return (
            (sx - self.sw / 2) / self.zoom + self.cx,
            (sy - self.sh / 2) / self.zoom + self.cy,
        )

    def zoom_at(self, sx: float, sy: float, factor: float):
        pcx, pcy = self.to_canvas(sx, sy)
        self.zoom = max(ZOOM_MIN, min(ZOOM_MAX, self.zoom * factor))
        self.cx = pcx - (sx - self.sw / 2) / self.zoom
        self.cy = pcy - (sy - self.sh / 2) / self.zoom


# ── Client ───────────────────────────────────────────────────────────────────

class Client:
    def __init__(self, window, cx: float, cy: float, cw: int, ch: int, depth: int):
        self.window = window
        self.cx, self.cy = cx, cy
        self.cw, self.ch = cw, ch
        self.depth = depth          # needed to pick correct PictFormat

    def screen_rect(self, vp: Viewport) -> tuple[int, int, int, int]:
        sx, sy = vp.to_screen(self.cx, self.cy)
        return (
            int(sx), int(sy),
            max(1, int(self.cw * vp.zoom)),
            max(1, int(self.ch * vp.zoom)),
        )

    def visible(self, vp: Viewport) -> bool:
        sx, sy, sw, sh = self.screen_rect(vp)
        return sx < vp.sw and sy < vp.sh and sx + sw > 0 and sy + sh > 0


# ── ZWM ──────────────────────────────────────────────────────────────────────

class ZWM:
    def __init__(self):
        self.dpy  = xdisplay.Display()
        self.scr  = self.dpy.screen()
        self.root = self.scr.root
        self.sw   = self.scr.width_in_pixels
        self.sh   = self.scr.height_in_pixels
        self.vp   = Viewport(self.sw, self.sh)
        self.clients: list[Client] = []
        self.focused: Client | None = None

        self._pan    = False
        self._pan_sx = 0; self._pan_sy = 0
        self._pan_vx = 0.0; self._pan_vy = 0.0

        self._move          = False
        self._move_client: Client | None = None
        self._move_start_sx = 0; self._move_start_sy = 0
        self._move_start_cx = 0.0; self._move_start_cy = 0.0

        self._setup_wm()
        self._setup_composite()
        self._adopt_existing()
        self._redraw()

    # ── X / WM setup ─────────────────────────────────────────────────────────

    def _setup_wm(self):
        self.root.change_attributes(
            event_mask=(
                X.SubstructureRedirectMask |
                X.SubstructureNotifyMask
            )
        )
        self.dpy.sync()

        self._kc_enter = self.dpy.keysym_to_keycode(XK.XK_Return)
        self._kc_q     = self.dpy.keysym_to_keycode(XK.XK_q)
        for kc in (self._kc_enter, self._kc_q):
            self.root.grab_key(kc, X.Mod4Mask, True,
                               X.GrabModeAsync, X.GrabModeAsync)

    def _setup_composite(self):
        # Redirect all child windows to off-screen pixmaps.
        self.root.composite_redirect_subwindows(composite.RedirectAutomatic)

        # Get the full-screen overlay window we draw on.
        reply = self.root.composite_get_overlay_window()
        self._overlay_win = reply.overlay_window   # python-xlib Window object
        self._overlay_xid = self._overlay_win.id   # raw integer XID for ctypes

        # Select input events on the overlay — this is where mouse/kb lands.
        self._overlay_win.change_attributes(
            event_mask=(
                X.ButtonPressMask   |
                X.ButtonReleaseMask |
                X.PointerMotionMask |
                X.KeyPressMask
            )
        )

        # Scroll: zoom
        for btn in (4, 5):
            self._overlay_win.grab_button(
                btn, X.AnyModifier, True,
                X.ButtonPressMask,
                X.GrabModeAsync, X.GrabModeAsync,
                X.NONE, X.NONE,
            )
        # Middle: pan
        self._overlay_win.grab_button(
            2, X.AnyModifier, True,
            X.ButtonPressMask,
            X.GrabModeAsync, X.GrabModeAsync,
            X.NONE, X.NONE,
        )
        # Left: focus / Super+drag
        self._overlay_win.grab_button(
            1, X.AnyModifier, True,
            X.ButtonPressMask,
            X.GrabModeAsync, X.GrabModeAsync,
            X.NONE, X.NONE,
        )

        self.dpy.sync()

        # Open the XRender rendering connection.
        display_name = os.environ.get('DISPLAY', ':0')
        self._renderer = Renderer(display_name)
        self._renderer.setup_overlay(self._overlay_xid, self.sw, self.sh)

    def _adopt_existing(self):
        for w in self.root.query_tree().children:
            if w.id == self._overlay_xid:
                continue
            try:
                a = w.get_attributes()
            except error.BadWindow:
                continue
            if a.map_state == X.IsViewable and not a.override_redirect:
                self._manage(w)

    # ── client management ─────────────────────────────────────────────────────

    def _manage(self, window) -> 'Client | None':
        try:
            g = window.get_geometry()
        except error.BadWindow:
            return None
        cx, cy = self.vp.to_canvas(g.x, g.y)
        c = Client(window, cx, cy, g.width, g.height, g.depth)
        self.clients.append(c)
        # Move off-screen so the window renders to its pixmap undisturbed.
        self._park(c)
        return c

    def _park(self, c: Client):
        """Push window off-screen at its natural size (content never reflows)."""
        try:
            c.window.configure(
                x=-(c.cw + 32), y=-(c.ch + 32),
                width=c.cw, height=c.ch, border_width=0,
            )
        except error.BadWindow:
            pass

    def _unmanage(self, window):
        c = self._find(window)
        if not c:
            return
        self.clients.remove(c)
        if self.focused is c:
            self.focused = self.clients[-1] if self.clients else None
            if self.focused:
                self._do_focus(self.focused)
        self._redraw()

    def _find(self, window) -> 'Client | None':
        for c in self.clients:
            if c.window == window:
                return c
        return None

    def _hit(self, sx: int, sy: int) -> 'Client | None':
        """Return topmost client whose scaled screen rect contains (sx, sy)."""
        for c in reversed(self.clients):
            x, y, w, h = c.screen_rect(self.vp)
            if x <= sx < x + w and y <= sy < y + h:
                return c
        return None

    # ── focus ────────────────────────────────────────────────────────────────

    def _do_focus(self, c: Client):
        self.focused = c
        try:
            c.window.set_input_focus(X.RevertToPointerRoot, X.CurrentTime)
        except error.BadWindow:
            pass
        self.dpy.flush()

    # ── rendering ────────────────────────────────────────────────────────────

    def _redraw(self):
        self.dpy.sync()                   # flush pending python-xlib requests first
        self._renderer.fill_background()

        for c in self.clients:
            if not c.visible(self.vp):
                continue
            sx, sy, sw, sh = c.screen_rect(self.vp)

            # composite_name_window_pixmap allocates the XID internally.
            try:
                pix = c.window.composite_name_window_pixmap()
            except error.BadWindow:
                continue

            self.dpy.sync()               # pixmap must exist before XRender uses it

            self._renderer.composite_pixmap(
                pix.id, c.depth, sx, sy, sw, sh, self.vp.zoom
            )
            # pix freed automatically when it goes out of scope (owner=1)

        self._renderer.flush()

    # ── event loop ────────────────────────────────────────────────────────────

    def run(self):
        handlers = {
            X.MapRequest:       self._on_map_request,
            X.UnmapNotify:      self._on_unmap,
            X.DestroyNotify:    self._on_destroy,
            X.ConfigureRequest: self._on_configure_request,
            X.ButtonPress:      self._on_button_press,
            X.ButtonRelease:    self._on_button_release,
            X.MotionNotify:     self._on_motion,
            X.KeyPress:         self._on_key_press,
        }
        while True:
            ev = self.dpy.next_event()
            h = handlers.get(ev.type)
            if h:
                try:
                    h(ev)
                except error.BadWindow:
                    pass

    # ── event handlers ────────────────────────────────────────────────────────

    def _on_map_request(self, ev):
        w = ev.window
        try:
            a = w.get_attributes()
        except error.BadWindow:
            return
        if a.override_redirect:
            w.map()
            return
        c = self._find(w) or self._manage(w)
        w.map()
        if c:
            self._do_focus(c)
        self._redraw()

    def _on_unmap(self, ev):
        if ev.send_event:
            return
        self._unmanage(ev.window)

    def _on_destroy(self, ev):
        self._unmanage(ev.window)

    def _on_configure_request(self, ev):
        c = self._find(ev.window)
        if c:
            # Honour resize; re-park at new natural size.
            if ev.value_mask & X.CWWidth:  c.cw = ev.width
            if ev.value_mask & X.CWHeight: c.ch = ev.height
            self._park(c)
        else:
            kw: dict = {}
            vm = ev.value_mask
            if vm & X.CWX:           kw['x']            = ev.x
            if vm & X.CWY:           kw['y']            = ev.y
            if vm & X.CWWidth:       kw['width']        = ev.width
            if vm & X.CWHeight:      kw['height']       = ev.height
            if vm & X.CWBorderWidth: kw['border_width'] = ev.border_width
            if vm & X.CWSibling:     kw['sibling']      = ev.above
            if vm & X.CWStackMode:   kw['stack_mode']   = ev.stack_mode
            ev.window.configure(**kw)

    def _on_button_press(self, ev):
        btn = ev.detail

        if btn == 4:
            self.vp.zoom_at(ev.root_x, ev.root_y, ZOOM_SPEED)
            self._redraw()
        elif btn == 5:
            self.vp.zoom_at(ev.root_x, ev.root_y, 1.0 / ZOOM_SPEED)
            self._redraw()

        elif btn == 2:
            self._pan    = True
            self._pan_sx = ev.root_x; self._pan_sy = ev.root_y
            self._pan_vx = self.vp.cx; self._pan_vy = self.vp.cy
            self._overlay_win.grab_pointer(
                True,
                X.PointerMotionMask | X.ButtonReleaseMask,
                X.GrabModeAsync, X.GrabModeAsync,
                X.NONE, X.NONE, X.CurrentTime,
            )

        elif btn == 1:
            c = self._hit(ev.root_x, ev.root_y)
            if (ev.state & X.Mod4Mask) and c:
                self._do_focus(c)
                self._move          = True
                self._move_client   = c
                self._move_start_sx = ev.root_x
                self._move_start_sy = ev.root_y
                self._move_start_cx = c.cx
                self._move_start_cy = c.cy
                self._overlay_win.grab_pointer(
                    True,
                    X.PointerMotionMask | X.ButtonReleaseMask,
                    X.GrabModeAsync, X.GrabModeAsync,
                    X.NONE, X.NONE, X.CurrentTime,
                )
            elif c:
                self._do_focus(c)

    def _on_button_release(self, ev):
        if ev.detail == 2 and self._pan:
            self._pan = False
            self.dpy.ungrab_pointer(X.CurrentTime)
        elif ev.detail == 1 and self._move:
            self._move = False
            self._move_client = None
            self.dpy.ungrab_pointer(X.CurrentTime)

    def _on_motion(self, ev):
        if self._move and self._move_client:
            dx = (ev.root_x - self._move_start_sx) / self.vp.zoom
            dy = (ev.root_y - self._move_start_sy) / self.vp.zoom
            self._move_client.cx = self._move_start_cx + dx
            self._move_client.cy = self._move_start_cy + dy
            self._redraw()
        elif self._pan:
            dx = (ev.root_x - self._pan_sx) / self.vp.zoom
            dy = (ev.root_y - self._pan_sy) / self.vp.zoom
            self.vp.cx = self._pan_vx - dx
            self.vp.cy = self._pan_vy - dy
            self._redraw()

    def _on_key_press(self, ev):
        if not (ev.state & X.Mod4Mask):
            return
        if ev.detail == self._kc_enter:
            os.system('xterm &')
        elif ev.detail == self._kc_q and self.focused:
            self.focused.window.destroy()


# ── entry point ───────────────────────────────────────────────────────────────

if __name__ == '__main__':
    try:
        ZWM().run()
    except error.DisplayConnectionError as e:
        sys.exit(f'Cannot connect to display: {e}')
    except error.BadAccess:
        sys.exit('Another WM is already running on this display.')
