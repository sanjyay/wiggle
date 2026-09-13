# Security Policy

## Security Architecture & Invariants

Wiggle implements a minimal-privilege architecture for cursor magnification:

1. **Input Minimization & Permissions:**
   - `scripts/wiggle-monitor` opens only devices reporting `EV_REL` with axes `REL_X` and `REL_Y`.
   - Access to `/dev/input/event*` (`0660 root:input`) is provided by standard Omarchy Quattro user provisioning (`input` supplementary group membership created at install time via `install/hardware/input-group.sh` and `bin/omarchy-provision-owner`).
   - Touchpads and touchscreens are explicitly rejected. Composite receiver nodes may also advertise keyboard capabilities, but all `EV_KEY` events are discarded without inspection.
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
   - The filesystem-independent `parse_xcursor(data)` boundary accepts arbitrary bytes directly; `tests/test-xcursor-parser.py` exercises malformed metadata and can be reused by a lightweight byte-input fuzz harness.

6. **Fail-Safe & Resource Lifecycle:**
   - `scripts/wiggle-monitor` registers `PR_SET_PDEATHSIG` with `SIGTERM` and monitors STDIN pipe EOF.
   - Normal termination signals (`SIGTERM`, `SIGINT`, `SIGHUP`, `SIGQUIT`, `SIGPIPE`) stop the event loop so its cleanup path can request `cursor:invisible false` without performing unsafe socket work inside the signal handler.
   - QML handoff failures keep a tracking 1x proxy visible until native-cursor restoration is acknowledged. Abrupt process or system failure can still prevent cleanup from completing.

## Bundled Helper Provenance

Omarchy plugin installation does not execute arbitrary build hooks, so the x86-64 `scripts/wiggle-monitor` ELF is bundled for the keep-loaded service. Its reviewed source is `scripts/wiggle-monitor.c`.

Run `./scripts/verify-wiggle-monitor` from the repository root to rebuild the helper and compare it byte-for-byte with the committed ELF. The build environment is fixed by:

- Debian bookworm-slim base image digest `sha256:abd67ffcfa541b485a3dff59865ab629aa048a6c613e639d36e7456b0b229241`.
- Debian and Debian Security snapshots at `20260820T000000Z`.
- Explicit GCC 12.2, binutils 2.40, glibc 2.36, libevdev 1.13.0, and pkg-config/pkgconf 1.8.1 package versions in `build/wiggle-monitor/Dockerfile`.
- Fixed compiler/linker/strip flags, build paths, `LC_ALL=C`, `TZ=UTC`, and `SOURCE_DATE_EPOCH=1787184000`; the linker build ID is disabled.

The verifier prints the source, toolchain, dependencies, expected binary hash, rebuilt hash, and exact comparison result. CI runs this same verifier and fails if the committed bytes cannot be reproduced from the checked-out source. This establishes a reviewable source-to-binary binding; it does not attest to binaries obtained from other locations.

## Reporting a Vulnerability

If you discover a security issue or vulnerability in Wiggle, please report it privately via GitHub Security Advisories or contact the maintainer directly.
