#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""Execute diagnostics with CInema and independently check every screen pixel."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib

_spec = importlib.util.spec_from_file_location("fx_diagnostic", Path(__file__).with_name("fx-generate-diagnostic.py"))
_diagnostic = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_diagnostic)
FONT = _diagnostic.FONT
PIXELS = _diagnostic.PIXELS
POLICY_FIELDS = _diagnostic.POLICY_FIELDS
TIMING_FIELDS = _diagnostic.TIMING_FIELDS
RAM_FIELDS = _diagnostic.RAM_FIELDS


def read_png(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Invalid PNG signature")
    compressed = bytearray()
    offset = 8
    channels = 0
    while offset < len(data):
        length, kind = struct.unpack_from(">I4s", data, offset)
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", body)
            if (width, height, depth, compression, filtering, interlace) != (240, 160, 8, 0, 0, 0):
                raise ValueError("Unsupported diagnostic PNG layout")
            if color not in (2, 6):
                raise ValueError("Expected RGB or RGBA screenshot")
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            compressed.extend(body)
        offset += length + 12
    if not channels:
        raise ValueError("Missing PNG header")
    raw = zlib.decompress(compressed)
    stride = 240 * channels
    if len(raw) != 160 * (stride + 1):
        raise ValueError("Unexpected screenshot data length")
    previous = bytearray(stride)
    pixels = []
    for y in range(160):
        method = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            if method == 0:
                predictor = 0
            elif method == 1:
                predictor = left
            elif method == 2:
                predictor = up
            elif method == 3:
                predictor = (left + up) // 2
            elif method == 4:
                prediction = left + up - upper_left
                distances = (abs(prediction - left), abs(prediction - up), abs(prediction - upper_left))
                predictor = (left, up, upper_left)[distances.index(min(distances))]
            else:
                raise ValueError("Unknown PNG filter")
            row[x] = (row[x] + predictor) & 255
        pixels.extend(tuple(row[x:x + 3]) for x in range(0, stride, channels))
        previous = row
    return pixels


def rgb555(value):
    components = [(value >> shift) & 31 for shift in (0, 5, 10)]
    return tuple((component << 3) | (component >> 2) for component in components)


def verify(path, rom, mode, memory, cpu):
    actual = read_png(path)
    data = rom.read_bytes()
    asset = 0x1000 if mode in ("policy", "timing", "ram") else 0x200 + int(mode) * PIXELS * 2
    expected = [rgb555(value) for value in struct.unpack_from(f"<{PIXELS}H", data, asset)]
    values = []
    if mode in ("policy", "timing", "ram"):
        fields = RAM_FIELDS if mode == "ram" else TIMING_FIELDS if mode == "timing" else POLICY_FIELDS
        for field, (label, _) in enumerate(fields):
            text = ""
            for digit in range(10):
                glyph = []
                for row in range(7):
                    bits = 0
                    for column in range(5):
                        color = actual[(34 + field * 14 + row) * 240 + 164 + digit * 6 + column]
                        if color == (255, 255, 255):
                            bits |= 1 << (4 - column)
                        elif color != rgb555(0x0200):
                            raise ValueError(f"Unexpected glyph pixel in {label}")
                    glyph.append(bits)
                matches = [character for character in "0123456789" if FONT[character] == tuple(glyph)]
                if len(matches) != 1:
                    raise ValueError(f"Guest drew an invalid digit in {label}")
                text += matches[0]
                for row in range(7):
                    for column in range(5):
                        expected[(34 + field * 14 + row) * 240 + 164 + digit * 6 + column] = (
                            (255, 255, 255) if glyph[row] & (1 << (4 - column)) else rgb555(0x0200)
                        )
            values.append(int(text))
    if mode == "policy":
        budget, available, guest_ram, target, status, cores, clock = values
        if memory and budget != memory:
            raise ValueError(f"RAM override: expected {memory} MiB, guest displayed {budget}")
        if not memory and budget not in (available * 80 // 100, available * 80 // 100 + 1):
            raise ValueError("Default RAM budget is not 80% of the launch snapshot")
        if available < budget or guest_ram != 0 or target != cpu or status not in (1, 2, 4) or cores < 1 or clock != 1:
            raise ValueError(f"Incorrect guest resource report: {values}")
    if mode == "timing" and (values[0] != memory or values[1] <= 1000 or values[2] != 0):
        raise ValueError(f"Incorrect arithmetic diagnostic report: {values}")
    if mode == "ram" and (values[0] not in (1, 2, 4, 8, 16) or values[1] != values[0] * 262144 or values[2:] != [0, 42, 43]):
        raise ValueError(f"RAM pattern/execution check failed: {values}")
    if mode == "ram" and values[0] != {0: 16, 97: 1, 100: 4}[memory]:
        raise ValueError(f"Unexpected RAM grant for the test budget: {values}")
    if actual != expected:
        index = next(index for index, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1])
        raise ValueError(f"Screen mismatch at pixel {index}: {actual[index]} != {expected[index]}")
    return {"pixels": PIXELS, "mode": mode, "guest_values": values}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cinema", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--timing", action="store_true")
    args = parser.parse_args()
    fixtures = args.fixtures.resolve()
    run_root = Path(tempfile.mkdtemp(prefix="visual-", dir=fixtures))
    cases = [("ordinary", "0", 0, 80), ("enhanced", "1", 0, 80)]
    if os.name == "nt":
        cases += [("resources", "policy", 0, 80), ("resources-256-25", "policy", 256, 25),
                  ("resources-512-60", "policy", 512, 60), ("ram", "ram", 0, 80),
                  ("ram-97", "ram", 97, 80), ("ram-100", "ram", 100, 80)]
    if args.timing:
        cases += [("timing-1x", "timing", 1, 80), ("timing-2x", "timing", 2, 80)]
    for name, mode, memory, cpu in cases:
        case = run_root / name
        case.mkdir()
        shutil.copyfile(fixtures / ("resources.gba" if mode == "policy" else "ram.gba" if mode == "ram" else f"{name}.gba"), case / "test.gba")
        (case / "config.ini").write_text(
            f"[ports.cinema]\nskipBios=1\nuseBios=0\ngba.fxMemoryMiB={memory if mode in ('policy', 'ram') else 0}\ngba.fxCPUPercent={cpu}\n"
            f"[testinfo]\nskip={2048 if mode == 'ram' else 8}\nframes=1\n", encoding="utf-8")
    # Missing references are captured only in a new, ignored directory. They are
    # checked independently below; these images never become compatibility baselines.
    result = subprocess.run([str(args.cinema.resolve()), "-b", str(run_root), "-R", "-v"],
                            capture_output=True, text=True)
    (run_root / "cinema.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode not in (0, 1):
        raise RuntimeError(f"CInema failed: {result.stderr}")
    report = {}
    for name, mode, memory, cpu in cases:
        case = run_root / name
        screenshot = case / "baseline_0000.png"
        report[name] = verify(screenshot, case / "test.gba", mode, memory, cpu)
        shutil.copyfile(screenshot, fixtures / f"{name}.png")
        print(f"{name}: verified {PIXELS} pixels; guest_values={report[name]['guest_values']}")
    if args.timing:
        ratio = report["timing-2x"]["guest_values"][1] / report["timing-1x"]["guest_values"][1]
        if not 1.7 < ratio < 2.0:
            raise ValueError(f"Unexpected visible arithmetic gain: {ratio}")
        print(f"Visible arithmetic work ratio: {ratio:.4f}")
    (run_root / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"ROM execution evidence: {run_root}")


if __name__ == "__main__":
    main()
