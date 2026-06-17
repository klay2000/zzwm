# zui — zooming window manager prototype

A proof-of-concept X11 window manager where all windows live on an infinite
2-D canvas. Navigation is spatial: scroll to zoom, drag to pan — no virtual
desktops.

## Controls

| Input | Action |
|---|---|
| Scroll wheel | Zoom in / out, centred on cursor |
| Middle-click drag | Pan the canvas |
| Left-click window | Focus |
| Super + left-drag | Move window on canvas |
| Super + Return | Spawn xterm |
| Super + Q | Close focused window |

## Architecture

**Non-reparenting WM** — managed windows remain direct children of the root;
no frame windows are inserted.

**XComposite redirect** — every managed window is redirected to an off-screen
pixmap (`RedirectAutomatic`). Windows are parked out of sight at their natural
pixel dimensions, so terminal content never reflows regardless of zoom level.

**XRender compositing** — on each viewport change the WM composites each
window's pixmap onto the Composite overlay window at the current scale,
using a bilinear filter for smooth downscaling. XRender is accessed via
`ctypes` against `libXrender.so` because python-xlib 0.33 ships no XRender
extension; all other X operations use python-xlib.

**Two X connections** — python-xlib handles the WM protocol (events,
`SubstructureRedirect`, window management); a separate ctypes/libX11
connection handles XRender rendering. Window XIDs are global to the X server
so pixmap IDs flow freely between the two.

**Input** — the full-screen overlay window receives all pointer and keyboard
events. Keyboard input reaches windows via `XSetInputFocus`. Mouse clicks
within windows are not forwarded (known limitation; requires input coordinate
transformation to map overlay positions back to parked window positions).

```
scroll / pan → Viewport(cx, cy, zoom) → _redraw()
                                              │
                         ┌────────────────────┘
                         ▼
               for each Client:
                 composite_name_window_pixmap()   ← off-screen pixmap
                 XRenderSetPictureTransform()      ← scale matrix 1/zoom
                 XRenderComposite() → overlay      ← bilinear scaled blit
```

## Dependencies

- Python 3.11+
- [python-xlib](https://github.com/python-xlib/python-xlib) 0.33+
- `libX11.so`, `libXrender.so`, `libXcomposite.so` (standard on any X desktop)
- `xserver-xephyr` for testing

```
pip install python-xlib
# Debian/Ubuntu:
apt install xserver-xephyr
```

## Running

Always test inside a nested X server to avoid disrupting your real desktop:

```sh
Xephyr :1 -screen 1280x800 &
DISPLAY=:1 python3 zwm.py
```

Then open windows on `:1`:

```sh
DISPLAY=:1 xterm &
```

## Known limitations

- Mouse clicks within windows land at incorrect coordinates (overlay input is
  not transformed back to parked window coordinates).
- No XDamage integration — the overlay only repaints on viewport changes, not
  on window content updates. Type in a terminal, zoom out and back to refresh.
- Single monitor only.
- No window resize handle; only move (Super+drag).
- Windows with non-standard visuals (depth ≠ 24/32) are skipped.
