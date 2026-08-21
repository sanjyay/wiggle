# Security Policy

## Security Architecture & Invariants

Wiggle implements a minimal-privilege architecture for cursor magnification:

1. **Input Minimization:**
   - `scripts/wiggle-monitor` opens only devices reporting `EV_REL` with axes `REL_X` and `REL_Y`.
   - Keyboard devices (devices reporting `KEY_A`, `KEY_SPACE`, `KEY_ENTER`), touchpads, and touchscreens are explicitly rejected.
   - All `EV_KEY` (mouse buttons and keys), `EV_ABS`, and `EV_MSC` events are discarded immediately upon reception.
   - Input grabbing (`EVIOCGRAB`) and virtual device creation (`uinput`) are never used by the monitor.

2. **Pointer Position Privacy:**
   - Absolute cursor coordinates are queried from Hyprland IPC exclusively while the locator proxy is active ($magnification > 1.0\times$).
   - Coordinates reside strictly in volatile memory and are never written to disk, logged in production, or transmitted over network.

3. **No Network Access:**
   - Wiggle performs zero network calls. There are no telemetry endpoints, analytics trackers, or external HTTP requests.

4. **Filesystem Safety:**
   - Temporary cursor assets are stored exclusively in `$XDG_RUNTIME_DIR/wiggle/` with directory mode `0700` and file mode `0600` (`O_NOFOLLOW` / secure atomic creation).
   - Never falls back to shared `/tmp`.

5. **Fail-Safe & Resource Lifecycle:**
   - `scripts/wiggle-monitor` registers `PR_SET_PDEATHSIG` with `SIGTERM` and monitors STDIN pipe EOF.
   - Signal handlers (`SIGTERM`, `SIGINT`, `SIGHUP`, `SIGQUIT`, `SIGPIPE`, `SIGSEGV`, `SIGABRT`) and `atexit` handlers immediately restore `cursor:invisible false` on compositor socket if termination occurs.

## Reporting a Vulnerability

If you discover a security issue or vulnerability in Wiggle, please report it privately via GitHub Security Advisories or contact the maintainer directly.
