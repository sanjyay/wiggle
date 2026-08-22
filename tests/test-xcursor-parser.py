#!/usr/bin/env python3
"""Adversarial regression tests for the bounded Xcursor decoder."""

import os
import struct
import sys
import tempfile
import time
import unittest
from importlib.machinery import SourceFileLoader
from pathlib import Path


SCRIPT = Path(__file__).parent.parent / "scripts" / "wiggle-discover-cursor"
discover = SourceFileLoader("wiggle_discover_cursor_security", str(SCRIPT)).load_module()

MAGIC = discover.XCURSOR_MAGIC
VERSION = discover.XCURSOR_FILE_VERSION
IMAGE_TYPE = discover.XCURSOR_IMAGE_TYPE


def image_chunk(nominal=32, width=32, height=32, hot_x=1, hot_y=1,
                chunk_header_size=36, chunk_type=IMAGE_TYPE, chunk_version=1,
                pixels=None):
    header = struct.pack(
        "<IIIIIIIII", chunk_header_size, chunk_type, nominal, chunk_version,
        width, height, hot_x, hot_y, 0
    )
    if chunk_header_size > len(header):
        header += bytes(chunk_header_size - len(header))
    if pixels is None and 0 < width <= 512 and 0 < height <= 512:
        pixels = struct.pack("<I", 0xff336699) * (width * height)
    return header + (pixels or b"")


def cursor_file(chunks, header_size=16, version=VERSION, toc_overrides=None):
    toc_size = len(chunks) * 12
    position = header_size + toc_size
    entries = []
    payload = bytearray()
    for index, (nominal, chunk) in enumerate(chunks):
        entry = (IMAGE_TYPE, nominal, position)
        if toc_overrides and index in toc_overrides:
            entry = toc_overrides[index]
        entries.append(struct.pack("<III", *entry))
        payload.extend(chunk)
        position += len(chunk)
    header = struct.pack("<IIII", MAGIC, header_size, version, len(chunks))
    header += bytes(header_size - len(header))
    return header + b"".join(entries) + bytes(payload)


class XcursorParserSecurityTests(unittest.TestCase):
    def assert_rejected(self, data):
        started = time.monotonic()
        self.assertEqual(discover.parse_xcursor(data), [])
        self.assertLess(time.monotonic() - started, 0.5)

    def test_truncated_fixed_header(self):
        self.assert_rejected(b"Xcur" + bytes(11))

    def test_absurd_ntoc(self):
        data = struct.pack("<IIII", MAGIC, 16, VERSION,
                           discover.MAX_XCURSOR_TOC_ENTRIES + 1)
        self.assert_rejected(data)

    def test_ntoc_multiplication_overflow_metadata(self):
        data = struct.pack("<IIII", MAGIC, 16, VERSION, 0xffffffff)
        self.assert_rejected(data)

    def test_truncated_toc(self):
        data = struct.pack("<IIII", MAGIC, 16, VERSION, 1) + bytes(11)
        self.assert_rejected(data)

    def test_toc_offset_beyond_eof(self):
        chunk = image_chunk(width=1, height=1)
        data = cursor_file(
            [(1, chunk)], toc_overrides={0: (IMAGE_TYPE, 1, 0xffffffff)}
        )
        self.assert_rejected(data)

    def test_chunk_header_crosses_eof(self):
        header = struct.pack("<IIII", MAGIC, 16, VERSION, 1)
        toc = struct.pack("<III", IMAGE_TYPE, 1, 29)
        self.assert_rejected(header + toc + bytes(9))

    def test_zero_width(self):
        self.assert_rejected(cursor_file([(32, image_chunk(width=0, height=32))]))

    def test_zero_height(self):
        self.assert_rejected(cursor_file([(32, image_chunk(width=32, height=0))]))

    def test_absurd_width(self):
        width = discover.MAX_CURSOR_DIMENSION + 1
        self.assert_rejected(cursor_file([(32, image_chunk(width=width, pixels=b""))]))

    def test_absurd_height(self):
        height = discover.MAX_CURSOR_DIMENSION + 1
        self.assert_rejected(cursor_file([(32, image_chunk(height=height, pixels=b""))]))

    def test_oversized_pixel_count(self):
        side = 513
        self.assert_rejected(cursor_file([
            (side, image_chunk(nominal=side, width=side, height=side, pixels=b""))
        ]))

    def test_truncated_pixel_payload(self):
        chunk = image_chunk(width=32, height=32, pixels=bytes(32 * 32 * 4 - 1))
        self.assert_rejected(cursor_file([(32, chunk)]))

    def test_malformed_chunk_header_size(self):
        chunk = image_chunk(width=1, height=1, chunk_header_size=35, pixels=bytes(4))
        self.assert_rejected(cursor_file([(1, chunk)]))

    def test_declared_chunk_header_beyond_eof(self):
        chunk = image_chunk(width=1, height=1, chunk_header_size=4096, pixels=b"")[:36]
        self.assert_rejected(cursor_file([(1, chunk)]))

    def test_malformed_entries_before_valid_entry(self):
        malformed = image_chunk(width=0, height=32)
        valid = image_chunk(nominal=64, width=64, height=64)
        images = discover.parse_xcursor(cursor_file([(32, malformed), (64, valid)]))
        self.assertEqual([(image["width"], image["height"]) for image in images], [(64, 64)])

    def test_valid_normal_cursor(self):
        images = discover.parse_xcursor(cursor_file([(32, image_chunk())]))
        self.assertEqual((images[0]["nominal"], images[0]["width"], images[0]["height"]),
                         (32, 32, 32))

    def test_valid_high_dpi_cursor(self):
        chunk = image_chunk(nominal=256, width=256, height=256, hot_x=8, hot_y=9)
        images = discover.parse_xcursor(cursor_file([(256, chunk)]))
        self.assertEqual((images[0]["width"], images[0]["height"], images[0]["hot_x"]),
                         (256, 256, 8))

    def test_bad_candidate_falls_through_to_valid_candidate(self):
        with tempfile.TemporaryDirectory() as tmp:
            theme = Path(tmp) / "theme"
            cursors = theme / "cursors"
            runtime = Path(tmp) / "runtime"
            cursors.mkdir(parents=True)
            runtime.mkdir()
            (cursors / "default").write_bytes(struct.pack("<IIII", MAGIC, 16, VERSION, 0xffffffff))
            (cursors / "left_ptr").write_bytes(cursor_file([
                (64, image_chunk(nominal=64, width=64, height=64, hot_x=2, hot_y=3))
            ]))
            previous = os.environ.get("XDG_RUNTIME_DIR")
            os.environ["XDG_RUNTIME_DIR"] = str(runtime)
            try:
                self.assertEqual(discover.probe_xcursor(str(theme)), [64])
                path, hot_x, hot_y, width, height = discover.export_xcursor_image(
                    str(theme), 64
                )
            finally:
                if previous is None:
                    os.environ.pop("XDG_RUNTIME_DIR", None)
                else:
                    os.environ["XDG_RUNTIME_DIR"] = previous
            self.assertTrue(Path(path).is_file())
            self.assertEqual((hot_x, hot_y, width, height), (2, 3, 64, 64))


if __name__ == "__main__":
    unittest.main(verbosity=2)
