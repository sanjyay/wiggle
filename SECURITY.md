# Security Policy

## Security Architecture & Invariants

Wiggle implements a minimal-privilege architecture for cursor magnification:

1. **Input Minimization & Permissions:**
   - `scripts/wiggle-monitor` opens only devices reporting `EV_REL` with axes `REL_X` and `REL_Y`.
   - Access to `/dev/input/event*` (`0660 root:input`) is provided by standard Omarchy Quattro user provisioning (`input` supplementary group membership created at install time via `install/hardware/input-group.sh` and `bin/omarchy-provision-owner`).
   - Keyboard devices (devices reporting alphanumeric text keys `KEY_A`, `KEY_SPACE`, `KEY_B`, `KEY_Z`), touchpads, and touchscreens are explicitly rejected.
   - All `EV_KEY` (mouse buttons and keys), `EV_ABS`, and `EV_MSC` events are discarded immediately upon reception.
   - Input grabbing (`EVIOCGRAB`) and virtual device creation (`uinput`) are never used by the monitor.

2. **Pointer Position Privacy:**
   - Absolute cursor coordinates are queried from local Hyprland IPC when a shake is detected and while the locator proxy may need to track the pointer.
   - Coordinates reside strictly in volatile memory and are never written to disk, logged in production, or transmitted over network.

3. **No Network Access:**
   - Wiggle performs zero network calls. There are no telemetry endpoints, analytics trackers, or external HTTP requests.

4. **Filesystem Safety:**
   - Temporary cursor assets are stored exclusively in `$XDG_RUNTIME_DIR/wiggle/` with directory mode `0700` and file mode `0600` using secure atomic creation.
   - Never falls back to shared `/tmp`.

5. **Untrusted Cursor Themes:**
   - Xcursor files are treated as untrusted binary input. The parser validates the fixed header, version, bounded TOC count, complete TOC extent, absolute chunk offsets, chunk headers, hotspots, and complete pixel payloads before decoding.
   - Files are limited to 32 MiB, TOCs to 4096 entries, dimensions to 1024 pixels per axis, and decoded images to 262,144 pixels (1 MiB of encoded ARGB). This permits normal and high-DPI cursor assets, including 256px themes, without allowing metadata-driven unbounded allocations or loops.
   - Malformed image entries and candidate files are rejected; discovery continues to later valid entries or cursor-name candidates when available.

6. **Fail-Safe & Resource Lifecycle:**
   - `scripts/wiggle-monitor` registers `PR_SET_PDEATHSIG` with `SIGTERM` and monitors STDIN pipe EOF.
   - Normal termination signals (`SIGTERM`, `SIGINT`, `SIGHUP`, `SIGQUIT`, `SIGPIPE`) stop the event loop so its cleanup path can request `cursor:invisible false` without performing unsafe socket work inside the signal handler.
   - QML handoff failures keep a tracking 1x proxy visible until native-cursor restoration is acknowledged. Abrupt process or system failure can still prevent cleanup from completing.

## Reporting a Vulnerability

If you discover a security issue or vulnerability in Wiggle, please report it privately via GitHub Security Advisories or contact the maintainer directly.
