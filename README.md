# Wiggle

**Shake your mouse to find your cursor.**

Wiggle implements the KDE Plasma / KWin Shake Cursor locator effect for Omarchy Quickshell on Hyprland. When you rapidly oscillate your mouse, the pointer smoothly magnifies so you can instantly locate it on multi-monitor setups, ultrawide displays, or busy workspaces.


https://github.com/user-attachments/assets/d1133515-9c44-48a9-8b22-29bc225317ae




---

## Features

* **KDE Kinematic Shake Detection:** Evaluates relative movement over a rolling 1000ms window with vector coalescing and sensitivity thresholding ($sensitivity = 4.0$).
* **Continuous Fluid Magnification:** Normal cursor ($1.0\times$) $\to$ smooth $3.0\times$ on first shake detection $\to$ $+1.0\times$ for each subsequent shake detection ($3\times \to 4\times \to 5\times \to 6\times \to 7\times \dots$).
* **200ms InOutCubic Animation:** Smooth cubic transitions with seamless mid-flight rebasing between target magnifications without visual pops.
* **900ms Deflate Timer:** After 0.9 seconds of stillness, smoothly animates back to $1.0\times$ over 200ms and restores the native cursor.
* **Universal Single Proxy Overlay:** Renders identically across applications (terminals, browsers), empty workspaces, and desktop backgrounds on a click-through, focusless overlay (`WlrLayer.Overlay`).
* **Hotspot-Anchored Precision:** Scales precisely around the cursor hotspot with zero pointer tip drift across all magnification levels.
* **Theme-Agnostic Asset Discovery:** Automatically discovers the active theme and extracts the highest-resolution arrow/default cursor asset (XCursor bitmaps up to 96px+ or Hyprcursor SVGs).
* **Defensive Cursor Handoff:** The proxy is presented at 1x before the native cursor is hidden, and HIDE/SHOW operations require compositor acknowledgements. Normal termination and handled failure paths request native-cursor restoration; an unconfirmed SHOW keeps the proxy available as a fallback.

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

Wiggle has no user-facing settings. Its shake sensitivity, magnification, and timing follow the plugin's built-in behavior.

## Enable or Disable

```bash
omarchy plugin enable io.github.sanjyay.wiggle
omarchy plugin disable io.github.sanjyay.wiggle
```

Disabling Wiggle stops its helper and restores the native cursor through the normal shutdown path.

---

## Removal

```bash
omarchy plugin remove io.github.sanjyay.wiggle
```

---

## Architecture & Security

* **Permissions & Device Access:** The bundled `wiggle-monitor` helper opens pointer devices in `/dev/input/event*` using `libevdev`. Stock Omarchy Quattro automatically provisions desktop users into the `input` group during system installation and user creation (`install/hardware/input-group.sh` and `bin/omarchy-provision-owner`), granting required read permissions without requiring `sudo`, custom udev rules, or privileged setup.
* **Input Minimization:** The monitor opens only pointer devices with relative axes (`REL_X`, `REL_Y`). Keyboards, touchscreens, and touchpads are explicitly rejected. All `EV_KEY` and button events are discarded. The monitor never injects input and never uses `EVIOCGRAB`.
* **Privacy & Memory Safety:** Cursor coordinates are queried over local Hyprland IPC when a shake is detected and while the proxy may need to track the pointer. Coordinates exist solely in volatile memory and are never persisted to disk, logged in production, or sent over any network.
* **No Network:** Wiggle performs zero network operations, contains no HTTP/socket network libraries, and includes no analytics, telemetry, or remote dependencies.
* **Filesystem Safety:** Temporary extracted cursor assets are written exclusively to `$XDG_RUNTIME_DIR/wiggle/` with strict `0700` directory and `0600` file permissions.
* **Untrusted Theme Files:** Xcursor headers, TOCs, offsets, dimensions, hotspots, and pixel payloads are bounds-checked before decoding. Malformed candidates are skipped in favor of a valid fallback when available.
* **Bundled Helper:** Omarchy installs plugins as plain Git checkouts and does not run build hooks. The x86-64 `scripts/wiggle-monitor` executable is therefore included alongside its complete C source. A digest-pinned Debian builder with a timestamped package snapshot reproduces it exactly, and CI rejects any source/binary mismatch.

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
* Read access to relative pointer devices under `/dev/input` (stock Omarchy user provisioning supplies this through the `input` group)

---

## Helper Executable and Source

The bundled helper is built from `scripts/wiggle-monitor.c` because Omarchy's plugin installer intentionally does not execute build hooks. To independently rebuild it in the pinned toolchain and compare it byte-for-byte with the committed ELF, install Docker and run:

```bash
./scripts/verify-wiggle-monitor
```

The verifier reports the reviewed source hash, exact compiler/linker/strip and dependency versions, expected and rebuilt binary hashes, and `MATCH` or `MISMATCH`. The builder pins the base image by immutable digest and obtains explicitly versioned packages from Debian Snapshot `20260820T000000Z`. The build uses fixed paths and locale/time settings, omits the linker build ID, strips deterministically, and runs without network access after the builder image is created. `.github/workflows/verify-wiggle-monitor.yml` runs the same comparison for pushes and pull requests.

---

## Troubleshooting

* Confirm discovery and enabled state with `omarchy plugin list`.
* If no shake is detected, confirm your user can read the relevant `/dev/input/event*` pointer device and log out/in after any group-membership change.
* If the cursor theme cannot be resolved, Wiggle remains in native-cursor mode; check `HYPRCURSOR_THEME`, `XCURSOR_THEME`, and the corresponding size variables.
* Reload after configuration or plugin updates with `omarchy restart shell`.
* Omarchy plugins share Quickshell's GUI thread. A plugin performing long synchronous QML or JavaScript work can temporarily stall shell animations, including Wiggle.

---

## License

MIT License. Clean-room implementation mirroring KDE KWin shake cursor behavior.
