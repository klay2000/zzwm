# zui — zooming window manager prototype

A proof-of-concept X11 window manager where all windows live on an infinite
2-D canvas. Navigation is spatial: scroll to zoom, drag to pan — no virtual
desktops.

Two implementations: a C version (`zwm.c`) and a Python version (`zwm.py`).
The C version is the more capable one — it has XDamage integration so windows
repaint live as their content changes.

## Controls

| Input | Action |
|---|---|
| Scroll wheel | Zoom in / out, centred on cursor |
| Middle-click drag | Pan the canvas |
| Left-click window | Focus |
| Super + left-drag | Move window on canvas |
| Super + Return | Spawn xterm |
| Super + Q | Close focused window |

## Building and running

### C version (recommended)

```sh
make
```

Requires: `libX11`, `libXrender`, `libXcomposite`, `libXdamage` (standard on any X desktop).

```sh
# Debian/Ubuntu:
apt install libx11-dev libxrender-dev libxcomposite-dev libxdamage-dev xserver-xephyr
```

Always test inside a nested X server:

```sh
Xephyr :1 -screen 1280x800 &
DISPLAY=:1 ./zwm
```

### Python version

Requires Python 3.11+ and python-xlib:

```sh
pip install python-xlib
```

```sh
Xephyr :1 -screen 1280x800 &
DISPLAY=:1 python3 zwm.py
```

Then open windows on `:1`:

```sh
DISPLAY=:1 xterm &
```

## Architecture

**Non-reparenting WM** — managed windows remain direct children of the root;
no frame windows are inserted.

**XComposite redirect** — every managed window is redirected to an off-screen
pixmap (`RedirectAutomatic`). Windows are parked out of sight at their natural
pixel dimensions, so terminal content never reflows regardless of zoom level.

**XRender compositing** — on each viewport change the WM composites each
window's pixmap onto the Composite overlay window at the current scale,
using a bilinear filter for smooth downscaling.

**XDamage** *(C version only)* — the WM subscribes to `XDamageNotify` on each
managed window and redraws the overlay whenever window content changes. The
Python version lacks this and only repaints on viewport changes.

**Two X connections** *(Python version only)* — python-xlib handles the WM
protocol (events, `SubstructureRedirect`, window management); a separate
ctypes/libX11 connection handles XRender rendering. Window XIDs are global to
the X server so pixmap IDs flow freely between the two. The C version uses a
single connection for everything.

```
scroll / pan → Viewport(cx, cy, zoom) → redraw()
                                              │
                         ┌────────────────────┘
                         ▼
               for each Client:
                 XCompositeNameWindowPixmap()    ← off-screen pixmap
                 XRenderSetPictureTransform()    ← scale matrix 1/zoom
                 XRenderComposite() → overlay    ← bilinear scaled blit
```

## Known limitations

- Mouse clicks within windows land at incorrect coordinates — overlay input
  is not transformed back to parked window coordinates, so clicks miss.
- Single monitor only.
- No window resize handle; only move (Super+drag).
- Windows with non-standard visuals (depth ≠ 24/32) are skipped.
- C version: max 64 managed windows (`MAX_CLIENTS`).
- Python version: no XDamage — the overlay only repaints on viewport changes,
  not on window content updates. Zoom out and back to refresh after typing.
