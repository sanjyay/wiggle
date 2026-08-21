# Wiggle

**Shake your mouse to find your cursor.**

Wiggle implements the KDE Plasma / KWin Shake Cursor locator effect for Omarchy Quickshell on Hyprland. When you rapidly oscillate your mouse, the pointer smoothly magnifies so you can instantly locate it on multi-monitor setups, ultrawide displays, or busy workspaces.

---

## Features

* **KDE Kinematic Shake Detection:** Evaluates relative movement over a rolling 1000ms window with vector coalescing and sensitivity thresholding ($sensitivity = 4.0$).
* **Continuous Fluid Magnification:** Normal cursor ($1.0\times$) $\to$ smooth $3.0\times$ on first shake detection $\to$ $+1.0\times$ for each subsequent shake detection ($3\times \to 4\times \to 5\times \to 6\times \to 7\times \dots$).
* **200ms InOutCubic Animation:** Smooth cubic transitions with seamless mid-flight rebasing between target magnifications without visual pops.
* **2000ms Deflate Timer:** After 2 seconds of stillness, smoothly animates back to $1.0\times$ over 200ms and restores the native cursor.
* **Universal Single Proxy Overlay:** Renders identically across applications (terminals, browsers), empty workspaces, and desktop backgrounds on a click-through, focusless overlay (`WlrLayer.Overlay`).
* **Hotspot-Anchored Precision:** Scales precisely around the cursor hotspot with zero pointer tip drift across all magnification levels.
* **Theme-Agnostic Asset Discovery:** Automatically discovers the active theme and extracts the highest-resolution arrow/default cursor asset (XCursor bitmaps up to 96px+ or Hyprcursor SVGs).
* **Robust Fail-Safe:** Kernel parent-death notifications (`PR_SET_PDEATHSIG`) and signal handlers guarantee compositor cursor visibility is restored even if Quickshell or the helper terminates unexpectedly.

---

## Installation

Install directly with the Omarchy plugin manager:

```bash
omarchy plugin add https://github.com/sanjyay/wiggle --enable
```

To reload the shell immediately:

```bash
omarchy restart shell
```

---

## Removal

```bash
omarchy plugin remove io.github.sanjyay.wiggle
```

---

## Architecture & Security

* **Permissions & Device Access:** The bundled `wiggle-monitor` helper opens pointer devices in `/dev/input/event*` using `libevdev`. Stock Omarchy Quattro automatically provisions desktop users into the `input` group during system installation and user creation (`install/hardware/input-group.sh` and `bin/omarchy-provision-owner`), granting required read permissions without requiring `sudo`, custom udev rules, or privileged setup.
* **Input Minimization:** The monitor opens only pointer devices with relative axes (`REL_X`, `REL_Y`). Keyboards, touchscreens, and touchpads are explicitly rejected. All `EV_KEY` and button events are discarded. The monitor never injects input and never uses `EVIOCGRAB`.
* **Privacy & Memory Safety:** Cursor coordinates are queried over local Hyprland IPC sockets only while magnification is active. Coordinates exist solely in volatile memory and are never persisted to disk, logged in production, or sent over any network.
* **No Network:** Wiggle performs zero network operations, contains no HTTP/socket network libraries, and includes no analytics, telemetry, or remote dependencies.
* **Filesystem Safety:** Temporary extracted cursor assets are written exclusively to `$XDG_RUNTIME_DIR/wiggle/` with strict `0700` directory and `0600` file permissions.

---

## Supported Cursor Backends

* **XCursor Themes:** Discovers discrete sizes (e.g. 24, 30, 36, 48, 72, 96px) and extracts the highest-resolution asset for sharp magnification.
* **Hyprcursor Themes:** Supports both scalable vector SVGs and high-resolution discrete raster PNGs.
* **Fallback & Safety:** If a theme is broken or missing, Wiggle fails safely to native cursor mode without stranding or modifying user configuration.

---

## Runtime Dependencies

* **Omarchy** with Quickshell
* **Hyprland** (0.50+)
* **libevdev** (standard on Arch Linux / Omarchy)
* **Python 3** (standard system Python for high-res asset discovery)

---

## Building from Source

The helper binary is pre-built and bundled. To rebuild it locally:

```bash
bash scripts/build.sh
```

---

## License

MIT License. Clean-room implementation mirroring KDE KWin shake cursor behavior.
