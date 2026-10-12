#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""Generate original ordinary/enhanced GBA diagnostics without an ARM SDK."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


FOOTER_MAGIC = b"mGBA-FX-DRAFT01\0"
FOOTER_SIZE = 64
IO_MAGIC = 0x040003E0
MAGIC_VALUE = 0x58464247
PIXELS = 240 * 160

# Original five-by-seven glyphs for the diagnostic's labels.
FONT = {
    "A": (14, 17, 17, 31, 17, 17, 17),
    "B": (30, 17, 17, 30, 17, 17, 30),
    "C": (14, 17, 16, 16, 16, 17, 14),
    "D": (30, 17, 17, 17, 17, 17, 30),
    "E": (31, 16, 16, 30, 16, 16, 31),
    "F": (31, 16, 16, 30, 16, 16, 16),
    "G": (14, 17, 16, 23, 17, 17, 14),
    "I": (31, 4, 4, 4, 4, 4, 31),
    "L": (16, 16, 16, 16, 16, 16, 31),
    "M": (17, 27, 21, 21, 17, 17, 17),
    "N": (17, 25, 21, 19, 17, 17, 17),
    "O": (14, 17, 17, 17, 17, 17, 14),
    "R": (30, 17, 17, 30, 20, 18, 17),
    "T": (31, 4, 4, 4, 4, 4, 4),
    "X": (17, 17, 10, 4, 10, 17, 17),
    "Y": (17, 17, 10, 4, 4, 4, 4),
    " ": (0,) * 7,
}

def screen(label, background):
    pixels = [background] * PIXELS
    for text, y in (("GBA FX", 36), (label, 76), ("DETECTION ONLY", 116)):
        scale = 2
        x = (240 - len(text) * 6 * scale) // 2
        for character in text:
            for row, bits in enumerate(FONT[character]):
                for column in range(5):
                    if bits & (1 << (4 - column)):
                        for dy in range(scale):
                            for dx in range(scale):
                                pixels[(y + row * scale + dy) * 240 + x + column * scale + dx] = 0x7FFF
            x += 6 * scale
    return struct.pack(f"<{PIXELS}H", *pixels)


class ArmProgram:
    """Minimal ARMv4T instruction/literal encoder for this one diagnostic."""

    def __init__(self, origin):
        self.origin = origin
        self.words = []
        self.labels = {}
        self.branches = []
        self.literals = []

    def emit(self, opcode):
        self.words.append(opcode)

    def label(self, name):
        self.labels[name] = len(self.words)

    def branch(self, name, condition=0xE):
        self.branches.append((len(self.words), name, condition))
        self.emit(0)

    def literal(self, register, value, condition=0xE):
        self.literals.append((len(self.words), register, value, condition))
        self.emit(0)

    def finish(self):
        for index, name, condition in self.branches:
            displacement = self.labels[name] - index - 2
            if not -(1 << 23) <= displacement < (1 << 23):
                raise ValueError("ARM branch out of range")
            self.words[index] = (condition << 28) | 0x0A000000 | (displacement & 0xFFFFFF)
        for index, register, value, condition in self.literals:
            displacement = (len(self.words) - index - 2) * 4
            if not 0 <= displacement <= 0xFFF:
                raise ValueError("ARM literal out of range")
            self.words[index] = (condition << 28) | 0x059F0000 | (register << 12) | displacement
            self.words.append(value)
        return struct.pack(f"<{len(self.words)}I", *self.words)


def boot_image(repo_root):
    image = bytearray(256 * 1024)
    struct.pack_into("<I", image, 0, 0xEA00002E)  # Branch from header to 0xC0.
    # Reuse only the standardized cartridge-header logo, not fixture game code.
    reference = (repo_root / "cinema/gba/irq/keyirq/test.gba").read_bytes()
    image[4:0xA0] = reference[4:0xA0]
    image[0xA0:0xAC] = b"FX DETECT   "
    image[0xAC:0xB0] = b"FX01"
    image[0xB0:0xB2] = b"00"
    image[0xB2] = 0x96
    image[0xBD] = (-sum(image[0xA0:0xBD]) - 0x19) & 0xFF

    normal_offset = 0x200
    enhanced_offset = normal_offset + PIXELS * 2
    program = ArmProgram(0xC0)
    program.emit(0xE3A00000)  # mov r0, #0 (ordinary by default)
    program.literal(1, IO_MAGIC)
    program.emit(0xE5912000)  # ldr r2, [r1]
    program.literal(3, MAGIC_VALUE)
    program.emit(0xE1520003)  # cmp r2, r3
    program.branch("draw", condition=1)  # bne
    program.emit(0xE5912004)  # ldr r2, [r1, #4] (interface version)
    program.emit(0xE3520001)  # cmp r2, #1
    program.emit(0x03A00001)  # moveq r0, #1
    program.label("draw")
    program.literal(1, 0x02000000)
    program.emit(0xE5810000)  # str r0, [r1] (machine-readable result)
    program.literal(1, 0x04000000)
    program.literal(2, 0x0403)  # Mode 3, BG2 enabled
    program.emit(0xE1C120B0)  # strh r2, [r1]
    program.emit(0xE3500000)  # cmp r0, #0
    program.literal(4, 0x08000000 + normal_offset, condition=0)
    program.literal(4, 0x08000000 + enhanced_offset, condition=1)
    program.literal(5, 0x06000000)
    program.literal(3, PIXELS)
    program.label("copy")
    program.emit(0xE0D460B2)  # ldrh r6, [r4], #2
    program.emit(0xE0C560B2)  # strh r6, [r5], #2
    program.emit(0xE2533001)  # subs r3, r3, #1
    program.branch("copy", condition=1)
    program.label("idle")
    program.branch("idle")
    code = program.finish()
    if 0xC0 + len(code) > normal_offset:
        raise ValueError("Diagnostic code overlaps screen data")
    image[0xC0:0xC0 + len(code)] = code
    image[normal_offset:normal_offset + PIXELS * 2] = screen("NORMAL GBA", 0x4000)
    image[enhanced_offset:enhanced_offset + PIXELS * 2] = screen("FX DETECTED", 0x0200)
    return bytes(image)


def enhanced_image(boot):
    fields = (1, FOOTER_SIZE, 1, 0, len(boot), zlib.crc32(boot), 0, 0, 0, 0, 0)
    body = struct.pack("<11I", *fields)
    footer = body + struct.pack("<I", zlib.crc32(body)) + FOOTER_MAGIC
    if len(footer) != FOOTER_SIZE:
        raise ValueError("Invalid diagnostic footer size")
    return boot + footer


def main():
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=repo_root / "build-fx/diagnostics")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    boot = boot_image(repo_root)
    fixtures = {"ordinary.gba": boot, "enhanced.gba": enhanced_image(boot)}
    metadata = {}
    for name, data in fixtures.items():
        output = args.output_dir / name
        output.write_bytes(data)
        metadata[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        print(output)
    (args.output_dir / "fixtures.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
