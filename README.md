# ZZWM (Zoe's Zooming Window Manager)

A minimal (>1000 lines for the base wm) X11 window manager where all windows live on an infinite
2D canvas. Navigation is spatial, you can scroll to zoom and drag to pan.

Four binaries: `zzwm` (the main window manager) plus three small utility apps in
`utility-apps/`:

- `zzwm-run` — a small app launcher (Super+Space). A plain client window, so
  it's managed like any other window: it zooms and pans with the canvas.
- `zzwm-bar` — a minimal status app that doubles as the "base window" (see
  below): zzwm anchors it at the canvas origin (0,0) and won't let it be
  closed or moved, so it acts as a "you are here" reference point as you
  zoom/pan. Shows a clock and a permanent "Super+H for help" hint.
- `zzwm-help` — a keybinding reference (Super+H). Reads `config.h`'s
  bindings table directly, so it always matches the real configuration; any
  keypress closes it (not clicks -- see Known limitations).

## Controls
(mostly reconfigurable)
| Input | Action |
|---|---|
| Scroll wheel | Zoom in / out, centred on cursor |
| Middle-click drag | Pan the canvas |
| Left-click window | Focus (and raise to top) |
| Super + left-drag | Move window on canvas (no-op on the base window; snaps to other windows' edges, see Configuration) |
| Super + right-drag | Resize window (also snaps) |
| Super + Return | Spawn xterm |
| Super + Space | Spawn zzwm-run (type a command, Enter to launch) |
| Super + H | Spawn zzwm-help (keybinding reference) |
| Super + Q | Close focused window (no-op on the base window) |

## Configuration

Edit `config.h` and rebuild to change functionality:

- `ANCHOR_NAME` — the X window name (`WM_NAME`/`XStoreName`) of the "base
  window". Whatever window has this name is anchored at the canvas origin
  instead of the viewport, and can't be closed or moved. Defaults to
  `"zzwm-bar"`, matching `utility-apps/statusbar.c`.
- Keybindings — each line is `BIND(modifier, keysym, action, arg)`, e.g.
  `BIND(Mod4Mask, XK_Return, ACT_SPAWN, "xterm &")`. Available actions:
  `ACT_SPAWN` (run `arg` as a shell command), `ACT_CLOSE` (close the focused
  window).
- `SNAP_ENABLED` — set to `0` to disable edge snapping entirely. Default `1`.
- `SNAP_DIST` — how close (in canvas/native pixels, independent of zoom) an
  edge has to get to another window's edge before it snaps. Default `12`.
- `SNAP_GAP` — space (same units) left between two windows when they snap
  side-by-side instead of sitting flush. Default `0`.

No changes to `zzwm.c` are needed for either.

Edit `appearance.h` and rebuild to change aesthetics:

- `CANVAS_BG_*` — zzwm's canvas background, behind all windows. Dark navy
  by default.
- `BG_*`/`FG_*`/`DIM_*` — background, foreground, and dim text for
  `zzwm-run`, `zzwm-bar`, and `zzwm-help`. Background is white by default.
- `BORDER_R`/`BORDER_G`/`BORDER_B` — color of the border drawn around every
  managed window.
- `BORDER_THICKNESS` — border thickness in canvas pixels at zoom 1.0 (it
  scales with the window as you zoom). Set to `0` to disable borders.

No changes to any `.c` files are needed.

## Building and running

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
DISPLAY=:1 ./zzwm &
DISPLAY=:1 ./zzwm-bar &
```

Then open windows on `:1`:

```sh
DISPLAY=:1 xterm &
```

## Installation

```sh
make
sudo make install         # installs to /usr/local/bin
# or, without root:
make install PREFIX=~/.local
```

`zzwm` launches `zzwm-run`/`zzwm-bar`/`zzwm-help` via `system()` (e.g.
`"zzwm-run &"`), so they must be on `$PATH` for keybindings like Super+Space
to work — `make install` puts all four binaries in the same directory for
that reason. `make uninstall` removes them again (respects the same
`PREFIX`/`DESTDIR`).

To run zzwm as your actual X session window manager, add to `~/.xinitrc`:

```sh
zzwm-bar &
exec zzwm
```

then start X with `startx`. (Test in Xephyr first, per above — zzwm replaces
whatever WM is currently running on the display, so a bad keybinding config
could leave you without a way to spawn a terminal.)

## License

MIT — see [LICENSE](LICENSE).

## Architecture

**Non-reparenting WM** — managed windows remain direct children of the root;
no frame windows are inserted.

**XComposite redirect** — every managed window is redirected to an off-screen
pixmap (`RedirectAutomatic`). Windows are parked out of sight at their natural
pixel dimensions, so terminal content never reflows regardless of zoom level.

**XRender compositing** — on each viewport change the WM composites each
window's pixmap onto the Composite overlay window at the current scale:
bilinear filtering when shrinking (smooth), nearest-neighbor when
magnifying (crisp, avoids blur).

**XDamage** — the WM subscribes to `XDamageNotify` on each managed window
(including override-redirect popups like menus) and redraws the overlay
whenever window content changes.

**zzwm-run, zzwm-bar, and zzwm-help are separate processes**, not built into
`zzwm`. All are plain client windows with no special X properties; zzwm
treats `zzwm-run` and `zzwm-help` like any other window (centred on the
viewport, canvas-scaled). `zzwm-bar` names itself `ANCHOR_NAME`
(`"zzwm-bar"` by default, configurable in `config.h`) via `XStoreName`;
zzwm recognizes that name, anchors the window at the canvas origin instead
of the viewport, and ignores `Super+Q` and `Super+left-drag` on it (the
`anchor` flag on `Client` in `zzwm.c`).

`zzwm-help` includes `config.h` directly with the same `BIND`-macro trick
`zzwm.c` uses, so its listing always reflects the real configuration rather
than a separately maintained copy.

**Click passthrough** (the same technique [InfiniteGlass](https://github.com/redhog/InfiniteGlass)
uses): clicks are never synthesized. The Composite overlay's input shape is
set to empty (`XFixesSetWindowShapeRegion`), so it's fully click-through;
`XI_RawMotion`, selected globally on the root, drives `on_hover()`, which
continuously parks whichever client is visually under the real cursor at the
matching real screen position (inverting the canvas transform on every
event, so drags stay pixel-correct at any zoom level) and raises it. A
plain click is then a genuine `ButtonPress` landing on that real window via
ordinary X routing — caught by a synchronous passive grab in `manage()`
just long enough to do focus+raise bookkeeping, then replayed
(`XAllowEvents(ReplayPointer)`) so the X server delivers it for real. Its
own implicit grab then carries the rest of the gesture (drag motion,
release) straight to the client with no further WM involvement, which is
also why it works for XInput2-only clients, not just core-protocol ones.
Super-modified gestures (move/resize) and pan/zoom are grabbed globally on
the root instead, so they never compete with this.

```
scroll / pan → Viewport(cx, cy, zoom) → redraw()
                                              │
                         ┌────────────────────┘
                         ▼
               for each Client:
                 XCompositeNameWindowPixmap()    ← off-screen pixmap
                 XRenderSetPictureTransform()    ← scale matrix 1/zoom
                 XRenderComposite() → overlay    ← scaled blit
```

## Known limitations

- `zzwm-help` only closes on a keypress, not a click.
- Single monitor only.
- Windows with non-standard visuals (depth ≠ 24/32) are skipped.
- Max 64 managed windows (`MAX_CLIENTS`).
