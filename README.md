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

## Experimental native Hyprland backend

The `experiment/hyprland-native-wiggle` branch adds a second backend. It keeps Wiggle as an Omarchy/Quickshell plugin for identity, settings, backend status, and error reporting, while an in-process Hyprland plugin owns pointer sampling, shake detection, interpolation, damage, and cursor rendering. The original `Wiggle.qml` LayerShell implementation remains unchanged for A/B testing.

This backend is experimental, version-bound, and currently targets Hyprland 0.56.2 (`efb50993780079460b0cbed1363e2166a2de1d9f`). Hyprland plugins use an unstable C++ ABI. Rebuild the plugin after every Hyprland update; the backend refuses to load when its build-time ABI does not match the running compositor.

### Prerequisites

Install Hyprland development headers, a C++23 compiler, CMake, pkg-config, and the development packages for Hyprland's dependencies. Arch/Omarchy's `hyprland` and `base-devel` packages provide the expected environment. Review native plugin source before loading it: a Hyprland plugin runs inside the compositor process.

### Install this branch

Omarchy's plugin manager clones the default branch and has no branch flag. Clone the experiment explicitly, then add that checked-out clone:

```bash
git clone --branch experiment/hyprland-native-wiggle \
  https://github.com/sanjyay/wiggle.git wiggle-native-experiment
omarchy plugin add "$(pwd)/wiggle-native-experiment" --enable
```

Install and enable the native backend from the same branch through `hyprpm`:

```bash
hyprpm add https://github.com/sanjyay/wiggle.git experiment/hyprland-native-wiggle
hyprpm enable wiggle-native
hyprpm reload
```

`wiggle-native` is the plugin identifier defined by `hyprpm.toml`. `hyprpm` builds `native/build/wiggle-native.so` with CMake; no precompiled native library is distributed.

### Backend selection and settings

The experiment defaults to `"backend": "native"` in `manifest.json`. Set it to `"overlay"` to run the preserved Quickshell implementation, or launch Omarchy shell with `WIGGLE_BACKEND=overlay` for a temporary comparison. Only the selected backend is instantiated.

Native settings are also under `manifest.json`:

```json
"native": {
  "enabled": true,
  "sensitivity": 4.0,
  "maxScale": 4.0
}
```

Quickshell detects the loaded backend once with `hyprctl plugin list`, then writes these values only when they change to `$XDG_RUNTIME_DIR/wiggle-native.conf`. The native plugin watches that per-session file with inotify on Hyprland's event loop. There is no cursor polling, command loop, daemon, or per-frame file I/O.

Backend states are:

* `available`: `wiggle-native` is loaded; its version is reported by Hyprland.
* `unavailable`: it is neither loaded nor present in `hyprpm` state.
* `error`: it appears installed/enabled but did not load, or status/configuration failed.

Backend absence never starts the overlay implicitly and never crashes Quickshell. Choose `overlay` explicitly when native mode is unavailable.

### Rendering limitation

The 0.56.2 proof scales Hyprland's compositor-owned cursor buffer and forces Hyprland's existing software-cursor render path only while active. It creates no `PanelWindow`, LayerShell surface, transparent window, or input-capturing proxy. Client-supplied cursor surfaces cannot be safely rescaled through the available API; the effect skips those cursors and leaves them untouched.

The detector and interaction model were informed by KDE KWin's `src/plugins/shakecursor` implementation (rolling movement history, path-length-to-bounds score, direction coalescing, smooth cubic growth/decay). KWin's implementation is GPL-2.0-or-later. Wiggle's detector is an original MIT implementation and does not incorporate KDE source code.

### Troubleshooting

```bash
hyprctl plugin list
hyprpm list
hyprctl configerrors
```

If the repository is installed but the backend is absent, run `hyprpm update` after a Hyprland update, then `hyprpm reload`. An ABI mismatch is intentional protection against compositor crashes. Do not load an old `.so` manually.

### Disable or remove safely

The native backend and Omarchy frontend are separate:

```bash
# Disable/remove only the native Hyprland backend.
hyprpm disable wiggle-native
hyprpm reload
hyprpm remove Wiggle

# Separately remove the Omarchy/Quickshell frontend if desired.
omarchy plugin remove io.github.sanjyay.wiggle
```

The runtime configuration is session-scoped and disappears on logout. Removing the backend does not edit or delete unrelated Hyprland configuration.

---

## License

MIT License. Clean-room implementation mirroring KDE KWin shake cursor behavior.
