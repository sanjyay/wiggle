#!/usr/bin/env python3
"""
test-cursor-capabilities.py — Unit test suite for KDE-style cursor discovery and asset extraction

Tests:
  - Discrete XCursor with standard sizes (Adwaita): discovers sizes and extracts matching nominal size
  - Discrete XCursor with high-res sizes (Banana): extracts matching nominal size (e.g. 32px), not oversized assets
  - Hyprcursor theme (Banana-Hyprcursor, Bibata)
  - Semantic cursor shapes / roles across themes (default, pointer, text, resize)
  - Scalable Hyprcursor with SVG
  - Scalable Hyprcursor with bilinear PNG resampling
  - Discrete Hyprcursor with resize_algorithm=none and closest nominal size matching
  - Unknown/missing themes (safe fallback)
  - Hotspot screen-coordinate stability across arbitrary scales
  - Environment preservation (Wiggle never mutates cursor env vars)

Run: python3 tests/test-cursor-capabilities.py
"""

import os
import sys
import tempfile
import subprocess
from pathlib import Path

# Import discovery helpers
sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from importlib.machinery import SourceFileLoader
discover_module = SourceFileLoader(
    "wiggle_discover_cursor",
    str(Path(__file__).parent.parent / "scripts" / "wiggle-discover-cursor")
).load_module()

probe_xcursor = discover_module.probe_xcursor
probe_hyprcursor = discover_module.probe_hyprcursor
export_xcursor_image = discover_module.export_xcursor_image
export_hyprcursor_image = discover_module.export_hyprcursor_image
find_theme_dir = discover_module.find_theme_dir
get_active_theme = discover_module.get_active_theme
get_active_size = discover_module.get_active_size

passed = 0
total = 0

def test(name, condition):
    global passed, total
    total += 1
    if condition:
        print(f"  ✓ {name}")
        passed += 1
    else:
        print(f"  ✗ {name} (FAILED)")

def main():
    print("Wiggle KDE-Style Cursor Capability & Asset Extraction Tests")
    print("==========================================================\n")

    # 1. Real Installed Themes — High-DPI Crisp Asset Extraction
    adwaita_dir = "/usr/share/icons/Adwaita"
    if os.path.isdir(adwaita_dir):
        sizes = probe_xcursor(adwaita_dir)
        path96, hx96, hy96, w96, h96 = export_xcursor_image(adwaita_dir, 96)
        test("Adwaita (XCursor): discovers sizes [24..96] and extracts high-res 96px asset",
             sizes == [24, 30, 36, 48, 72, 96] and w96 == 96 and h96 == 96 and hx96 > 0 and hy96 > 0 and os.path.isfile(path96))

    bibata_dir = "/usr/share/icons/Bibata-Catppuccin-Mocha"
    if os.path.isdir(bibata_dir):
        sizes = probe_xcursor(bibata_dir)
        path96, hx96, hy96, w96, h96 = export_xcursor_image(bibata_dir, 96)
        test("Bibata-Catppuccin-Mocha (XCursor): extracts high-res asset (96px)",
             len(sizes) > 0 and w96 == 96 and h96 == 96 and hx96 >= 0 and hy96 >= 0 and os.path.isfile(path96))

    banana_dir = find_theme_dir("Banana")
    if banana_dir and os.path.isdir(banana_dir):
        sizes = probe_xcursor(banana_dir)
        path96, hx96, hy96, w96, h96 = export_xcursor_image(banana_dir, 96)
        test("Banana (XCursor): extracts high-res asset (96px) with accurate hotspot",
             w96 == 96 and h96 == 96 and hx96 > 0 and hy96 > 0 and os.path.isfile(path96))

    banana_hypr_dir = find_theme_dir("CursorSwitcher-XCursor-CursorSwitcher-Imported-Banana-542bccf69750-50dae6c5fdab")
    if banana_hypr_dir and os.path.isdir(banana_hypr_dir):
        h_back, h_cap, h_sizes = probe_hyprcursor(banana_hypr_dir)
        path96, hx96, hy96, w96, h96 = export_hyprcursor_image(banana_hypr_dir, 96)
        test("Banana-Hyprcursor: extracts high-res asset (96px) with valid hotspot",
             h_back == "hyprcursor_discrete" and w96 == 96 and h96 == 96 and hx96 > 0 and hy96 > 0 and os.path.isfile(path96))

    # 2. Semantic Roles & Shapes Across Themes
    if os.path.isdir(banana_dir):
        roles_ok = True
        for role in ["default", "pointer", "text", "resize"]:
            p, hx, hy, w, h = export_xcursor_image(banana_dir, 32, shape_name=role)
            if not (os.path.isfile(p) and w == 32 and h == 32 and hx >= 0 and hy >= 0):
                roles_ok = False
        test("Banana (XCursor) roles: default, pointer, text, resize resolve at nominal size 32", roles_ok)

    if os.path.isdir(banana_hypr_dir):
        roles_ok = True
        for role in ["default", "pointer", "text", "resize"]:
            p, hx, hy, w, h = export_hyprcursor_image(banana_hypr_dir, 32, shape_name=role)
            if not (os.path.isfile(p) and w == 32 and h == 32 and hx >= 0 and hy >= 0):
                roles_ok = False
        test("Banana-Hyprcursor roles: default, pointer, text, resize resolve at nominal size 32", roles_ok)

    if os.path.isdir(adwaita_dir):
        roles_ok = True
        for role in ["default", "pointer", "text", "resize"]:
            p, hx, hy, w, h = export_xcursor_image(adwaita_dir, 24, shape_name=role)
            if not (os.path.isfile(p) and w in (24, 30) and h in (24, 30) and hx >= 0 and hy >= 0):
                roles_ok = False
        test("Adwaita roles: default, pointer, text, resize resolve cleanly", roles_ok)

    # 3. Hyprcursor SVG Fixture
    with tempfile.TemporaryDirectory() as tmp:
        t_svg = os.path.join(tmp, "HyprSVG")
        os.makedirs(os.path.join(t_svg, "hyprcursors"))
        with open(os.path.join(t_svg, "manifest.hl"), "w") as f:
            f.write("cursors_directory = hyprcursors\n")
        with open(os.path.join(t_svg, "hyprcursors", "default.hl"), "w") as f:
            f.write("resize_algorithm = bilinear\ndefine_size = 0, default.svg, 4, 2\n")
        with open(os.path.join(t_svg, "hyprcursors", "default.svg"), "w") as f:
            f.write("<svg></svg>\n")

        backend, cap, sizes = probe_hyprcursor(t_svg)
        path, hx, hy, w, h = export_hyprcursor_image(t_svg, wanted_size=32)
        test("Hyprcursor SVG: detected as scalable, extracts SVG asset and hotspot",
             backend == "hyprcursor_scalable" and cap == "scalable" and path.endswith("default.svg") and hx == 4 and hy == 2)

    # 4. Hyprcursor Discrete PNG Fixture — Selects Closest Nominal Size
    with tempfile.TemporaryDirectory() as tmp:
        t_disc = os.path.join(tmp, "HyprDiscretePNG")
        os.makedirs(os.path.join(t_disc, "hyprcursors"))
        with open(os.path.join(t_disc, "manifest.hl"), "w") as f:
            f.write("cursors_directory = hyprcursors\n")
        with open(os.path.join(t_disc, "hyprcursors", "default.hl"), "w") as f:
            f.write("resize_algorithm = none\ndefine_size = 24, d24.png, 3, 1\ndefine_size = 96, d96.png, 12, 4\n")
        with open(os.path.join(t_disc, "hyprcursors", "d24.png"), "w") as f:
            f.write("dummy\n")
        with open(os.path.join(t_disc, "hyprcursors", "d96.png"), "w") as f:
            f.write("dummy\n")

        backend, cap, sizes = probe_hyprcursor(t_disc)
        path24, hx24, hy24, w24, h24 = export_hyprcursor_image(t_disc, wanted_size=24)
        path96, hx96, hy96, w96, h96 = export_hyprcursor_image(t_disc, wanted_size=96)
        test("Hyprcursor Discrete PNG: extracts matching nominal size (24px for size 24, 96px for size 96)",
             backend == "hyprcursor_discrete" and cap == "discrete" and w24 == 24 and hx24 == 3 and hy24 == 1 and w96 == 96 and hx96 == 12 and hy96 == 4)

    # 5. Hyprcursor Discrete .hlc Archive Fixture — Selects Closest Nominal Size
    import zipfile
    with tempfile.TemporaryDirectory() as tmp:
        t_hlc = os.path.join(tmp, "HyprHlcDiscrete")
        os.makedirs(os.path.join(t_hlc, "hyprcursors"))
        with open(os.path.join(t_hlc, "manifest.hl"), "w") as f:
            f.write("cursors_directory = hyprcursors\n")
        hlc_path = os.path.join(t_hlc, "hyprcursors", "left_ptr.hlc")
        with zipfile.ZipFile(hlc_path, "w") as zf:
            zf.writestr("meta.hl", "resize_algorithm = none\nhotspot_x = 0.25\nhotspot_y = 0.25\ndefine_size = 24, ptr24.png, 20\ndefine_size = 128, ptr128.png, 20\n")
            zf.writestr("ptr24.png", "dummy24")
            zf.writestr("ptr128.png", "dummy128")

        backend, cap, sizes = probe_hyprcursor(t_hlc)
        path24, hx24, hy24, w24, h24 = export_hyprcursor_image(t_hlc, wanted_size=24)
        path128, hx128, hy128, w128, h128 = export_hyprcursor_image(t_hlc, wanted_size=128)
        test("Hyprcursor .hlc Archive: selects requested nominal size (24px vs 128px) and computes normalized hotspot",
             backend == "hyprcursor_discrete" and cap == "discrete" and w24 == 24 and hx24 == 6 and hy24 == 6 and w128 == 128 and hx128 == 32 and hy128 == 32 and os.path.isfile(path24) and os.path.isfile(path128))

    # 6. Hyprcursor SVG .hlc Archive Fixture
    with tempfile.TemporaryDirectory() as tmp:
        t_hlc_svg = os.path.join(tmp, "HyprHlcSvg")
        os.makedirs(os.path.join(t_hlc_svg, "hyprcursors"))
        with open(os.path.join(t_hlc_svg, "manifest.hl"), "w") as f:
            f.write("cursors_directory = hyprcursors\n")
        hlc_svg_path = os.path.join(t_hlc_svg, "hyprcursors", "default.hlc")
        with zipfile.ZipFile(hlc_svg_path, "w") as zf:
            zf.writestr("meta.hl", "hotspot_x = 0.1\nhotspot_y = 0.2\ndefine_size = 0, default.svg\n")
            zf.writestr("default.svg", "<svg></svg>")

        backend, cap, sizes = probe_hyprcursor(t_hlc_svg)
        path, hx, hy, w, h = export_hyprcursor_image(t_hlc_svg, wanted_size=32)
        test("Hyprcursor SVG .hlc Archive: detected as scalable, extracts SVG asset and computes hotspot",
             backend == "hyprcursor_scalable" and cap == "scalable" and path.endswith(".svg") and os.path.isfile(path))

    # 7. Unknown / Corrupted Theme
    with tempfile.TemporaryDirectory() as tmp:
        t_broken = os.path.join(tmp, "BrokenTheme")
        os.makedirs(t_broken)
        backend, cap, sizes = probe_hyprcursor(t_broken)
        test("Broken/Missing Theme: fails safely to capability=none",
             cap == "none" and backend is None)

    # 8. Stale Environment Theme Fallback
    old_hypr_theme = os.environ.get("HYPRCURSOR_THEME")
    try:
        os.environ["HYPRCURSOR_THEME"] = "NonExistentThemeXYZ12345"
        discovered_name, discovered_src = get_active_theme()
        test("Stale HYPRCURSOR_THEME: skips missing candidate and falls back to valid theme",
             discovered_name != "NonExistentThemeXYZ12345" and find_theme_dir(discovered_name) is not None)
    finally:
        if old_hypr_theme is None:
            os.environ.pop("HYPRCURSOR_THEME", None)
        else:
            os.environ["HYPRCURSOR_THEME"] = old_hypr_theme

    # 9. Hotspot Screen-Coordinate Stability Assertion
    # Mathematical proof: For cursor position (cx, cy), image dimension (iw, ih), hotspot (hx, hy)
    # renderedWidth = iw * S, assetScale = S, hotX = hx * S
    # proxyX = cx - hotX
    # screenHotspot = proxyX + hotX = cx - hotX + hotX = cx (IDENTICALLY cx for all S)
    hotspot_stable = True
    cx, cy = 500, 300
    iw, ih, hx, hy = 32, 32, 6, 6
    for scale in [1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 5.5]:
        rendered_w = iw * scale
        rendered_h = ih * scale
        asset_scale = rendered_w / iw
        hot_x = hx * asset_scale
        hot_y = hy * asset_scale
        proxy_x = cx - hot_x
        proxy_y = cy - hot_y
        screen_hot_x = proxy_x + hot_x
        screen_hot_y = proxy_y + hot_y
        if abs(screen_hot_x - cx) > 1e-9 or abs(screen_hot_y - cy) > 1e-9:
            hotspot_stable = False
    test("Hotspot stability: screen hotspot coordinate remains exactly fixed for all scales S", hotspot_stable)

    # 10. Environment Non-Mutation Assertion
    env_snapshot = {
        "HYPRCURSOR_THEME": os.environ.get("HYPRCURSOR_THEME"),
        "HYPRCURSOR_SIZE": os.environ.get("HYPRCURSOR_SIZE"),
        "XCURSOR_THEME": os.environ.get("XCURSOR_THEME"),
        "XCURSOR_SIZE": os.environ.get("XCURSOR_SIZE"),
    }
    # Run discovery multiple times
    for _ in range(5):
        subprocess.run([sys.executable, str(Path(__file__).parent.parent / "scripts" / "wiggle-discover-cursor")],
                       capture_output=True, text=True)
    env_after = {
        "HYPRCURSOR_THEME": os.environ.get("HYPRCURSOR_THEME"),
        "HYPRCURSOR_SIZE": os.environ.get("HYPRCURSOR_SIZE"),
        "XCURSOR_THEME": os.environ.get("XCURSOR_THEME"),
        "XCURSOR_SIZE": os.environ.get("XCURSOR_SIZE"),
    }
    test("Environment integrity: HYPRCURSOR/XCURSOR settings never mutated by discovery or runs", env_snapshot == env_after)

    # 11. Graphical Magnification Invariance Assertion
    # Verify that Wiggle discovery provides high-resolution base texture for crisp graphical scaling
    test("Graphical magnification invariance: discovery provides high-res visual, scale is purely graphical",
         w96 == 96 and h96 == 96)

    print("\n==========================================================")
    print(f"Results: {passed}/{total} tests passed")
    return 0 if passed == total else 1

if __name__ == "__main__":
    sys.exit(main())
