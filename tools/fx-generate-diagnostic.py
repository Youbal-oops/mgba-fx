#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""Generate original ordinary/enhanced GBA diagnostics without an ARM SDK."""

import argparse
from array import array
import hashlib
import json
from pathlib import Path
import struct
import sys
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

FONT.update({
    "H": (17, 17, 17, 31, 17, 17, 17),
    "K": (17, 18, 20, 24, 20, 18, 17),
    "P": (30, 17, 17, 30, 16, 16, 16),
    "S": (15, 16, 16, 14, 1, 1, 30),
    "U": (17, 17, 17, 17, 17, 17, 14),
    "V": (17, 17, 17, 17, 17, 10, 4),
    "W": (17, 17, 17, 21, 21, 27, 17),
    "0": (14, 17, 19, 21, 25, 17, 14),
    "1": (4, 12, 4, 4, 4, 4, 14),
    "2": (14, 17, 1, 2, 4, 8, 31),
    "3": (30, 1, 1, 14, 1, 1, 30),
    "4": (2, 6, 10, 18, 31, 2, 2),
    "5": (31, 16, 16, 30, 1, 1, 30),
    "6": (14, 16, 16, 30, 17, 17, 14),
    "7": (31, 1, 2, 4, 8, 8, 8),
    "8": (14, 17, 17, 14, 17, 17, 14),
    "9": (14, 17, 17, 15, 1, 1, 14),
    "=": (0, 0, 31, 0, 31, 0, 0),
})

POLICY_FIELDS = (
    ("RAM BUDGET MIB", 0x3C0),
    ("AVAILABLE MIB", 0x3C4),
    ("GUEST RAM MIB", 0x3EC),
    ("CPU TARGET PCT", 0x3D8),
    ("CPU CAP STATUS", 0x3DC),
    ("LOGICAL CORES", 0x3F0),
    ("CPU PROFILE X", 0x3F4),
)

TIMING_FIELDS = (("ALU PROFILE X", 0x3F4), ("ADDS THIS FRAME", 0), ("GUEST RAM MIB", 0x3EC))
RAM_FIELDS = (("GUEST RAM MIB", 0), ("WORDS VERIFIED", 0), ("DATA ERRORS", 0),
              ("ARM RESULT", 0), ("THUMB RESULT", 0))
CONTENT_FIELDS = (("CONTENT BYTES", 0), ("WORDS VERIFIED", 0), ("DATA ERRORS", 0),
                  ("BANKS CHECKED", 0), ("LAST BYTE", 0), ("RAM COPY OK", 0))


def draw_text(pixels, text, x, y, scale=1):
    for character in text:
        for row, bits in enumerate(FONT[character]):
            for column in range(5):
                if bits & (1 << (4 - column)):
                    for dy in range(scale):
                        for dx in range(scale):
                            pixels[(y + row * scale + dy) * 240 + x + column * scale + dx] = 0x7FFF
        x += 6 * scale


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

    def branch(self, name, condition=0xE, link=False):
        self.branches.append((len(self.words), name, condition, link))
        self.emit(0)

    def literal(self, register, value, condition=0xE):
        self.literals.append((len(self.words), register, value, condition))
        self.emit(0)

    def finish(self):
        for index, name, condition, link in self.branches:
            displacement = self.labels[name] - index - 2
            if not -(1 << 23) <= displacement < (1 << 23):
                raise ValueError("ARM branch out of range")
            self.words[index] = (condition << 28) | (0x0B000000 if link else 0x0A000000) | (displacement & 0xFFFFFF)
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


def enhanced_image(boot, features=1, minimum_ram=0):
    fields = (1, FOOTER_SIZE, features, 0, len(boot), zlib.crc32(boot), minimum_ram, 0, 0, 0, 0)
    body = struct.pack("<11I", *fields)
    footer = body + struct.pack("<I", zlib.crc32(body)) + FOOTER_MAGIC
    if len(footer) != FOOTER_SIZE:
        raise ValueError("Invalid diagnostic footer size")
    return boot + footer


def emit_decimal(p):
    p.label("decimal")  # r0 = value, r1 = top-left VRAM address.
    p.literal(2, 0x08015000)
    p.emit(0xE3A0300A)  # mov r3, #10
    p.label("digit")
    p.emit(0xE4924004)  # ldr r4, [r2], #4 (next decimal divisor)
    p.emit(0xE3A05000)  # mov r5, #0
    p.label("subtract")
    p.emit(0xE1500004)  # cmp r0, r4
    p.emit(0x20400004)  # subhs r0, r0, r4
    p.emit(0x22855001)  # addhs r5, r5, #1
    p.branch("subtract", condition=2)
    p.emit(0xE3A04046)  # mov r4, #70 (glyph bytes)
    p.emit(0xE0060495)  # mul r6, r5, r4
    p.literal(12, 0x08014000)
    p.emit(0xE08C6006)  # add r6, r12, r6
    p.emit(0xE1A07001)  # mov r7, r1
    p.emit(0xE3A08007)  # mov r8, #7 (rows)
    p.literal(10, 470)  # 480-byte scanline minus five pixels.
    p.label("row")
    p.emit(0xE3A09005)
    p.label("pixel")
    p.emit(0xE0D6B0B2)  # ldrh r11, [r6], #2
    p.emit(0xE0C7B0B2)  # strh r11, [r7], #2
    p.emit(0xE2599001)
    p.branch("pixel", condition=1)
    p.emit(0xE087700A)  # add r7, r7, r10
    p.emit(0xE2588001)
    p.branch("row", condition=1)
    p.emit(0xE281100C)  # add r1, r1, #12 (six pixels)
    p.emit(0xE2533001)
    p.branch("digit", condition=1)
    p.emit(0xE12FFF1E)  # bx lr


def policy_image(repo_root):
    """Guest code reads real registers and draws decimal values, without an SDK."""
    image = bytearray(boot_image(repo_root))
    image[0xA0:0xAC] = b"FX POLICY   "
    image[0xAC:0xB0] = b"FX02"
    image[0xBD] = (-sum(image[0xA0:0xBD]) - 0x19) & 0xFF
    background = [0x0200] * PIXELS
    draw_text(background, "GBA FX RESOURCES", 30, 8, 2)
    for index, (label, _) in enumerate(POLICY_FIELDS):
        draw_text(background, label, 8, 34 + index * 14)
    draw_text(background, "CAP 1=ON  OTHER=TARGET ONLY", 8, 142)
    draw_text(background, "RAM EXPANSION IS NEXT", 8, 152)
    image[0x1000:0x1000 + PIXELS * 2] = struct.pack(f"<{PIXELS}H", *background)
    glyphs = []
    for digit in "0123456789":
        for row in FONT[digit]:
            glyphs.extend(0x7FFF if row & (1 << (4 - x)) else 0x0200 for x in range(5))
    image[0x14000:0x14000 + len(glyphs) * 2] = struct.pack(f"<{len(glyphs)}H", *glyphs)
    struct.pack_into("<10I", image, 0x15000, *(10 ** power for power in range(9, -1, -1)))
    p = ArmProgram(0xC0)
    p.literal(1, IO_MAGIC)
    p.emit(0xE5912000)  # ldr r2, [r1]
    p.literal(3, MAGIC_VALUE)
    p.emit(0xE1520003)  # cmp r2, r3
    p.emit(0x03A00001)  # moveq r0, #1
    p.emit(0x13A00000)  # movne r0, #0
    p.literal(1, 0x02000000)
    p.emit(0xE5810000)  # str r0, [r1]
    p.emit(0xE3500000)  # cmp r0, #0
    p.branch("idle", condition=0)  # Unsupported emulator: never claim detection.
    p.literal(1, 0x04000000)
    p.literal(2, 0x0403)
    p.emit(0xE1C120B0)
    p.literal(4, 0x08001000)
    p.literal(5, 0x06000000)
    p.literal(3, PIXELS)
    p.label("copy")
    p.emit(0xE0D460B2)
    p.emit(0xE0C560B2)
    p.emit(0xE2533001)
    p.branch("copy", condition=1)
    for index, (_, register) in enumerate(POLICY_FIELDS):
        p.literal(4, 0x04000000 + register)
        p.emit(0xE5940000)  # ldr r0, [r4]
        p.literal(4, 0x02000004 + index * 4)
        p.emit(0xE5840000)  # Store a machine-readable copy of the guest's value.
        p.literal(1, 0x06000000 + ((34 + index * 14) * 240 + 164) * 2)
        p.branch("decimal", link=True)
    p.literal(4, 0x02000020)
    p.emit(0xE3A00001)
    p.emit(0xE5840000)  # Done flag, after every digit was rendered.
    p.label("idle")
    p.branch("idle")
    emit_decimal(p)
    code = p.finish()
    if 0xC0 + len(code) > 0x1000:
        raise ValueError("Policy code overlaps screen data")
    image[0xC0:0xC0 + len(code)] = code
    return enhanced_image(bytes(image), features=3)


def timing_image(repo_root, fast):
    image = bytearray(policy_image(repo_root)[:-FOOTER_SIZE])
    image[0xA0:0xAC] = b"FX ALU 2X   " if fast else b"FX ALU 1X   "
    image[0xAC:0xB0] = b"FX04" if fast else b"FX03"
    image[0xBD] = (-sum(image[0xA0:0xBD]) - 0x19) & 0xFF
    image[0xC0:0x1000] = bytes(0x1000 - 0xC0)
    background = [0x0200] * PIXELS
    draw_text(background, "GBA FX ALU TEST", 30, 8, 2)
    for index, (label, _) in enumerate(TIMING_FIELDS):
        draw_text(background, label, 8, 34 + index * 14)
    draw_text(background, "ARITHMETIC ONLY", 8, 112)
    draw_text(background, "VIDEO AUDIO DMA STAY NORMAL", 8, 134)
    image[0x1000:0x1000 + PIXELS * 2] = struct.pack(f"<{PIXELS}H", *background)
    boot = ArmProgram(0xC0)
    boot.literal(1, 0x04000000)
    boot.literal(2, 0x0403)
    boot.emit(0xE1C120B0)
    boot.literal(4, 0x08001000)
    boot.literal(5, 0x06000000)
    boot.literal(3, PIXELS)
    boot.label("background")
    boot.emit(0xE0D460B2)
    boot.emit(0xE0C560B2)
    boot.emit(0xE2533001)
    boot.branch("background", condition=1)
    boot.literal(4, 0x08016000)
    boot.literal(5, 0x03000000)
    size_literal = len(boot.words)
    boot.literal(3, 0)  # Patched with the IWRAM program's final word count.
    boot.label("code")
    boot.emit(0xE4946004)
    boot.emit(0xE4856004)
    boot.emit(0xE2533001)
    boot.branch("code", condition=1)
    boot.emit(0xE3A0D000)  # r13 counts arithmetic slots; this test uses no stack/IRQ.
    boot.literal(12, 0x03000000)
    boot.emit(0xE12FFF1C)
    emit_decimal(boot)
    decimal_address = 0x08000000 + boot.origin + boot.labels["decimal"] * 4

    work = ArmProgram(0x03000000)
    work.label("start")
    work.literal(11, 0x04000006)
    work.label("count")
    for _ in range(64):
        work.emit(0xE28DD001)  # add r13, r13, #1
    work.emit(0xE1DB20B0)  # ldrh r2, [r11] (VCOUNT)
    work.emit(0xE35200A0)
    work.branch("count", condition=3)  # bcc: count until VBlank.
    for index, (_, register) in enumerate(TIMING_FIELDS):
        if register:
            work.literal(4, 0x04000000 + register)
            work.emit(0xE5940000)
        else:
            work.emit(0xE1A0000D)
            work.emit(0xE3A0D000)
        work.literal(4, 0x02000004 + index * 4)
        work.emit(0xE5840000)
        work.literal(1, 0x06000000 + ((34 + index * 14) * 240 + 164) * 2)
        work.literal(12, decimal_address)
        work.emit(0xE1A0E00F)  # mov lr, pc; return after the following BX.
        work.emit(0xE12FFF1C)
    work.literal(11, 0x04000006)
    work.label("wait")
    work.emit(0xE1DB20B0)
    work.emit(0xE35200A0)
    work.branch("wait", condition=2)
    work.branch("start")
    work_code = work.finish()
    boot.literals = [(index, register, len(work_code) // 4 if index == size_literal else value, condition)
                     for index, register, value, condition in boot.literals]
    boot_code = boot.finish()
    if 0xC0 + len(boot_code) > 0x1000 or len(work_code) > 0x8000:
        raise ValueError("Timing diagnostic exceeds its code region")
    image[0xC0:0xC0 + len(boot_code)] = boot_code
    image[0x16000:0x16000 + len(work_code)] = work_code
    return enhanced_image(bytes(image), features=7 if fast else 3)


def ram_image(repo_root):
    """Verify every granted word, then execute original ARM and Thumb code in FX RAM."""
    image = bytearray(policy_image(repo_root)[:-FOOTER_SIZE])
    image[0xA0:0xAC] = b"FX RAM TEST "
    image[0xAC:0xB0] = b"FX05"
    image[0xBD] = (-sum(image[0xA0:0xBD]) - 0x19) & 0xFF
    background = [0x0200] * PIXELS
    draw_text(background, "GBA FX RAM TEST", 30, 8, 2)
    for index, (label, _) in enumerate(RAM_FIELDS):
        draw_text(background, label, 8, 34 + index * 14)
    draw_text(background, "FULL GRANT PATTERN CHECK", 8, 120)
    draw_text(background, "CPU COPY FOR LEGACY DMA", 8, 142)
    image[0x1000:0x1000 + PIXELS * 2] = struct.pack(f"<{PIXELS}H", *background)
    boot = ArmProgram(0xC0)
    boot.literal(1, 0x04000000)
    boot.literal(2, 0x0403)
    boot.emit(0xE1C120B0)
    boot.literal(4, 0x08001000)
    boot.literal(5, 0x06000000)
    boot.literal(3, PIXELS)
    boot.label("background")
    boot.emit(0xE0D460B2)
    boot.emit(0xE0C560B2)
    boot.emit(0xE2533001)
    boot.branch("background", condition=1)
    boot.literal(4, 0x08016000)
    boot.literal(5, 0x03000000)
    size_literal = len(boot.words)
    boot.literal(3, 0)
    boot.label("code")
    boot.emit(0xE4946004)
    boot.emit(0xE4856004)
    boot.emit(0xE2533001)
    boot.branch("code", condition=1)
    boot.literal(12, 0x03000000)
    boot.emit(0xE12FFF1C)
    emit_decimal(boot)
    decimal_address = 0x08000000 + boot.origin + boot.labels["decimal"] * 4

    work = ArmProgram(0x03000000)
    work.literal(4, 0x040003EC)
    work.emit(0xE5940000)
    work.literal(4, 0x02000004)
    work.emit(0xE5840000)
    work.emit(0xE1A0A900)  # r10 = MiB << 18: word count.
    work.literal(4, 0x02000008)
    work.emit(0xE584A000)
    work.literal(7, 0xA5C39E71)
    work.literal(4, 0x10000000)
    work.emit(0xE3A01000)
    work.emit(0xE1A0300A)
    work.label("write")
    work.emit(0xE0212007)  # EOR r2, r1, r7
    work.emit(0xE4842004)
    work.emit(0xE2811001)
    work.emit(0xE2533001)
    work.branch("write", condition=1)
    work.literal(4, 0x10000000)
    work.emit(0xE3A01000)
    work.emit(0xE1A0300A)
    work.emit(0xE3A09000)
    work.label("read")
    work.emit(0xE4942004)
    work.emit(0xE0215007)
    work.emit(0xE1520005)
    work.emit(0x12899001)  # ADDNE r9, r9, #1
    work.emit(0xE2811001)
    work.emit(0xE2533001)
    work.branch("read", condition=1)
    work.literal(4, 0x0200000C)
    work.emit(0xE5849000)
    work.literal(4, 0x10000000)
    work.literal(0, 0xE3A0002A)  # MOV r0, #42
    work.emit(0xE5840000)
    work.literal(0, 0xE12FFF1E)  # BX lr
    work.emit(0xE5840004)
    work.emit(0xE1A0E00F)
    work.emit(0xE12FFF14)
    work.literal(4, 0x02000010)
    work.emit(0xE5840000)
    work.literal(4, 0x10000100)
    work.literal(0, 0x4770202B)  # Thumb MOV r0, #43; BX lr.
    work.emit(0xE5840000)
    work.emit(0xE2844001)
    work.emit(0xE1A0E00F)
    work.emit(0xE12FFF14)
    work.literal(4, 0x02000014)
    work.emit(0xE5840000)
    for index, _ in enumerate(RAM_FIELDS):
        work.literal(4, 0x02000004 + index * 4)
        work.emit(0xE5940000)
        work.literal(1, 0x06000000 + ((34 + index * 14) * 240 + 164) * 2)
        work.literal(12, decimal_address)
        work.emit(0xE1A0E00F)
        work.emit(0xE12FFF1C)
    work.literal(4, 0x02000000)
    work.emit(0xE3A00001)
    work.emit(0xE5840000)
    work.label("idle")
    work.branch("idle")
    code = work.finish()
    boot.literals = [(index, register, len(code) // 4 if index == size_literal else value, condition)
                     for index, register, value, condition in boot.literals]
    boot_code = boot.finish()
    if 0xC0 + len(boot_code) > 0x1000 or len(code) > 0x8000:
        raise ValueError("RAM diagnostic exceeds its code region")
    image[0xC0:0x1000] = bytes(0x1000 - 0xC0)
    image[0xC0:0xC0 + len(boot_code)] = boot_code
    image[0x16000:0x16000 + len(code)] = code
    return enhanced_image(bytes(image), features=11, minimum_ram=1048576)


def content_boot(repo_root):
    image = bytearray(policy_image(repo_root)[:-FOOTER_SIZE])
    image[0xA0:0xAC] = b"FX CONTENT  "
    image[0xAC:0xB0] = b"FX06"
    image[0xBD] = (-sum(image[0xA0:0xBD]) - 0x19) & 0xFF
    background = [0x0200] * PIXELS
    draw_text(background, "GBA FX CONTENT", 30, 8, 2)
    for index, (label, _) in enumerate(CONTENT_FIELDS):
        draw_text(background, label, 8, 34 + index * 14)
    draw_text(background, "FULL PAYLOAD PATTERN CHECK", 8, 130)
    draw_text(background, "READ ONLY BANKED CONTENT", 8, 148)
    image[0x1000:0x1000 + PIXELS * 2] = struct.pack(f"<{PIXELS}H", *background)
    boot = ArmProgram(0xC0)
    boot.literal(1, 0x04000000)
    boot.literal(2, 0x0403)
    boot.emit(0xE1C120B0)
    boot.literal(4, 0x08001000)
    boot.literal(5, 0x06000000)
    boot.literal(3, PIXELS)
    boot.label("background")
    boot.emit(0xE0D460B2)
    boot.emit(0xE0C560B2)
    boot.emit(0xE2533001)
    boot.branch("background", condition=1)
    boot.literal(4, 0x08016000)
    boot.literal(5, 0x03000000)
    size_literal = len(boot.words)
    boot.literal(3, 0)
    boot.label("code")
    boot.emit(0xE4946004)
    boot.emit(0xE4856004)
    boot.emit(0xE2533001)
    boot.branch("code", condition=1)
    boot.literal(12, 0x03000000)
    boot.emit(0xE12FFF1C)
    emit_decimal(boot)
    decimal_address = 0x08000000 + boot.origin + boot.labels["decimal"] * 4
    work = ArmProgram(0x03000000)
    work.literal(4, 0x040003A4)
    work.emit(0xE5940000)
    work.literal(4, 0x02000004)
    work.emit(0xE5840000)
    work.emit(0xE1A0A120)  # r10 = byte length >> 2 (total whole words).
    work.literal(4, 0x02000008)
    work.emit(0xE584A000)
    work.literal(7, 0xA5C39E71)
    work.emit(0xE3A01000)  # Global word index.
    work.emit(0xE3A08000)  # Bank index.
    work.emit(0xE3A09000)  # Mismatch count.
    work.label("bank")
    work.literal(4, 0x040003A0)
    work.emit(0xE5848000)
    work.literal(4, 0x040003AC)
    work.emit(0xE5943000)
    work.emit(0xE1A03123)  # Current bank length >> 2.
    work.literal(4, 0x11000000)
    work.label("read")
    work.emit(0xE4942004)
    work.emit(0xE0215007)
    work.emit(0xE1520005)
    work.emit(0x12899001)
    work.emit(0xE2811001)
    work.emit(0xE2533001)
    work.branch("read", condition=1)
    work.emit(0xE2888001)
    work.emit(0xE151000A)
    work.branch("bank", condition=3)
    work.literal(5, 0x02000010)
    work.emit(0xE5858000)
    work.literal(5, 0x02000004)
    work.emit(0xE5953000)
    work.emit(0xE2033003)  # Trailing bytes after the final whole word.
    work.emit(0xE3530000)
    work.branch("tail_done", condition=0)
    work.emit(0xE0212007)
    work.label("tail")
    work.emit(0xE4D45001)  # LDRB r5, [r4], #1.
    work.emit(0xE20260FF)  # AND r6, r2, #255.
    work.emit(0xE1550006)
    work.emit(0x12899001)
    work.emit(0xE1A02422)  # MOV r2, r2, LSR #8.
    work.emit(0xE2533001)
    work.branch("tail", condition=1)
    work.label("tail_done")
    work.literal(5, 0x0200000C)
    work.emit(0xE5859000)
    work.literal(5, 0x040003AC)
    work.emit(0xE5953000)
    work.literal(4, 0x11000000)
    work.emit(0xE0844003)
    work.emit(0xE5540001)  # LDRB r0, [r4, #-1].
    work.literal(5, 0x02000014)
    work.emit(0xE5850000)
    work.literal(4, 0x10000000)
    work.emit(0xE5840000)
    work.emit(0xE5942000)
    work.emit(0xE1520000)
    work.emit(0x03A00001)
    work.emit(0x13A00000)
    work.literal(5, 0x02000018)
    work.emit(0xE5850000)
    for index, _ in enumerate(CONTENT_FIELDS):
        work.literal(4, 0x02000004 + index * 4)
        work.emit(0xE5940000)
        work.literal(1, 0x06000000 + ((34 + index * 14) * 240 + 164) * 2)
        work.literal(12, decimal_address)
        work.emit(0xE1A0E00F)
        work.emit(0xE12FFF1C)
    work.literal(4, 0x02000000)
    work.emit(0xE3A00001)
    work.emit(0xE5840000)
    work.label("idle")
    work.branch("idle")
    code = work.finish()
    boot.literals = [(index, register, len(code) // 4 if index == size_literal else value, condition)
                     for index, register, value, condition in boot.literals]
    boot_code = boot.finish()
    if 0xC0 + len(boot_code) > 0x1000 or len(code) > 0x8000:
        raise ValueError("Content diagnostic exceeds its code region")
    image[0xC0:0x1000] = bytes(0x1000 - 0xC0)
    image[0xC0:0xC0 + len(boot_code)] = boot_code
    image[0x16000:0x16000 + len(code)] = code
    return bytes(image)


def write_content_fixture(path, boot, size):
    checksum = 0
    digest = hashlib.sha256()
    with path.open("wb") as output:
        output.write(boot)
        digest.update(boot)
        offset = 0
        while offset < size:
            count = min(1048576, size - offset)
            words = array("I", (0xA5C39E71 ^ index for index in range(offset // 4, (offset + count + 3) // 4)))
            if words.itemsize != 4:
                raise ValueError("Expected four-byte unsigned integers")
            if sys.byteorder != "little":
                words.byteswap()
            block = words.tobytes()[:count]
            output.write(block)
            digest.update(block)
            checksum = zlib.crc32(block, checksum)
            offset += count
        body = struct.pack("<11I", 1, FOOTER_SIZE, 27, 0, len(boot), zlib.crc32(boot),
                           1048576, size, checksum, 0, 0)
        footer = body + struct.pack("<I", zlib.crc32(body)) + FOOTER_MAGIC
        output.write(footer)
        digest.update(footer)
    return {"bytes": len(boot) + size + FOOTER_SIZE, "content_bytes": size, "sha256": digest.hexdigest()}


def main():
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=repo_root / "build-fx/diagnostics")
    parser.add_argument("--timing", action="store_true", help="Generate opt-in 1x/2x arithmetic diagnostics")
    parser.add_argument("--content", action="store_true", help="Generate 64/128 MiB and non-power-of-two content diagnostics")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    boot = boot_image(repo_root)
    fixtures = {"ordinary.gba": boot, "enhanced.gba": enhanced_image(boot),
                "resources.gba": policy_image(repo_root), "ram.gba": ram_image(repo_root)}
    if args.timing:
        fixtures.update({"timing-1x.gba": timing_image(repo_root, False),
                         "timing-2x.gba": timing_image(repo_root, True)})
    metadata = {}
    for name, data in fixtures.items():
        output = args.output_dir / name
        output.write_bytes(data)
        metadata[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        print(output)
    if args.content:
        content_program = content_boot(repo_root)
        for name, size in (("content-64", 64 * 1048576), ("content-128", 128 * 1048576),
                           ("content-tail", 64 * 1048576 + 7)):
            output = args.output_dir / f"{name}.gba"
            metadata[output.name] = write_content_fixture(output, content_program, size)
            print(output)
    (args.output_dir / "fixtures.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
