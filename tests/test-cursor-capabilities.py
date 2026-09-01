#!/usr/bin/env python3
"""
test-cursor-capabilities.py — Unit test suite for KDE-style cursor discovery and asset extraction

Tests:
  - Discrete XCursor with standard sizes (Adwaita): discovers sizes and extracts highest available asset (96px)
  - Discrete XCursor with high-res sizes (Banana)
  - Scalable Hyprcursor with SVG
  - Scalable Hyprcursor with bilinear PNG resampling
  - Discrete Hyprcursor with resize_algorithm=none
  - Unknown/missing themes (safe fallback)

Run: python3 tests/test-cursor-capabilities.py
"""

import os
import sys
import tempfile
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

    # 1. Real Installed Themes
    adwaita_dir = "/usr/share/icons/Adwaita"
    if os.path.isdir(adwaita_dir):
        sizes = probe_xcursor(adwaita_dir)
        path, hx, hy, w, h = export_xcursor_image(adwaita_dir, 96)
        test("Adwaita (XCursor): discovers sizes [24..96] and extracts high-res 96px asset",
             sizes == [24, 30, 36, 48, 72, 96] and w == 96 and h == 96 and hx > 0 and hy > 0 and os.path.isfile(path))

    bibata_dir = "/usr/share/icons/Bibata-Catppuccin-Mocha"
    if os.path.isdir(bibata_dir):
        sizes = probe_xcursor(bibata_dir)
        path, hx, hy, w, h = export_xcursor_image(bibata_dir, max(sizes) if sizes else 96)
        test("Bibata-Catppuccin-Mocha (XCursor): discovers sizes and extracts high-res asset",
             len(sizes) > 0 and w > 0 and h > 0 and hx >= 0 and hy >= 0 and os.path.isfile(path))

    yaru_dir = "/usr/share/icons/Yaru"
    if os.path.isdir(yaru_dir):
        sizes = probe_xcursor(yaru_dir)
        path, hx, hy, w, h = export_xcursor_image(yaru_dir, max(sizes) if sizes else 96)
        test("Yaru (XCursor): discovers sizes and extracts high-res asset",
             len(sizes) > 0 and w > 0 and h > 0 and hx >= 0 and hy >= 0 and os.path.isfile(path))

    banana_dir = os.path.expanduser("~/.local/share/icons/Banana")
    if os.path.isdir(banana_dir):
        sizes = probe_xcursor(banana_dir)
        path, hx, hy, w, h = export_xcursor_image(banana_dir, max(sizes) if sizes else 96)
        test("Banana (XCursor): extracts highest available discrete asset (256px)",
             256 in sizes and w == 256 and h == 256 and os.path.isfile(path))

    # 2. Hyprcursor SVG Fixture
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
        path, hx, hy, w, h = export_hyprcursor_image(t_svg)
        test("Hyprcursor SVG: detected as scalable, extracts SVG asset and hotspot",
             backend == "hyprcursor_scalable" and cap == "scalable" and path.endswith("default.svg") and hx == 4 and hy == 2)

    # 3. Hyprcursor Discrete PNG Fixture
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
        path, hx, hy, w, h = export_hyprcursor_image(t_disc)
        test("Hyprcursor Discrete PNG: extracts highest defined size asset (96px)",
             backend == "hyprcursor_discrete" and cap == "discrete" and w == 96 and hx == 12 and hy == 4)

    # 4. Hyprcursor Discrete .hlc Archive Fixture
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
        path, hx, hy, w, h = export_hyprcursor_image(t_hlc)
        test("Hyprcursor .hlc Archive: extracts highest discrete asset (128px) and computes hotspot",
             backend == "hyprcursor_discrete" and cap == "discrete" and sizes == [24, 128] and w == 128 and h == 128 and hx == 32 and hy == 32 and os.path.isfile(path))

    # 5. Hyprcursor SVG .hlc Archive Fixture
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
        path, hx, hy, w, h = export_hyprcursor_image(t_hlc_svg)
        test("Hyprcursor SVG .hlc Archive: detected as scalable, extracts SVG asset and computes hotspot",
             backend == "hyprcursor_scalable" and cap == "scalable" and path.endswith(".svg") and w == 256 and h == 256 and hx == 26 and hy == 51 and os.path.isfile(path))

    # 6. Unknown / Corrupted Theme
    with tempfile.TemporaryDirectory() as tmp:
        t_broken = os.path.join(tmp, "BrokenTheme")
        os.makedirs(t_broken)
        backend, cap, sizes = probe_hyprcursor(t_broken)
        test("Broken/Missing Theme: fails safely to capability=none",
             cap == "none" and backend is None)

    # 7. Stale Environment Theme Fallback
    get_active_theme = discover_module.get_active_theme
    old_hypr_theme = os.environ.get("HYPRCURSOR_THEME")
    try:
        os.environ["HYPRCURSOR_THEME"] = "NonExistentThemeXYZ12345"
        discovered_name, discovered_src = get_active_theme()
        test("Stale HYPRCURSOR_THEME: skips missing candidate and falls back to valid theme",
             discovered_name != "NonExistentThemeXYZ12345" and discover_module.find_theme_dir(discovered_name) is not None)
    finally:
        if old_hypr_theme is None:
            os.environ.pop("HYPRCURSOR_THEME", None)
        else:
            os.environ["HYPRCURSOR_THEME"] = old_hypr_theme

    print("\n==========================================================")
    print(f"Results: {passed}/{total} tests passed")
    return 0 if passed == total else 1

if __name__ == "__main__":
    sys.exit(main())
