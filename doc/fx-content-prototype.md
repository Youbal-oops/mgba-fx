# GBA-FX larger-content prototype

This is the first implementation of numbered Step 7, dated 2026-10-12.
It retains a conventional boot ROM and exposes up to 128 MiB of read-only
content through a 16 MiB banked CPU window. It is a development transport;
the public SDK and `.gna` container remain deferred.

## Build and launch

```powershell
./tools/fx-build.ps1 -ContentPrototype
./tools/fx-run.ps1 -ContentPrototype
```

The separate build is `build-fx/content/mGBA.exe`. The default demonstration
is `build-fx/content/diagnostics/content-64.gba`; `content-128.gba` and
`content-tail.gba` are additional fixtures. The previously tested RAM desktop
build stays in `build-fx/`. The content feature is selected by the ROM footer,
not a user enhancement-mode toggle. `-ContentPrototype` selects a build/output
directory and large test generation; it does not force ordinary games into
an enhanced mode. It can be combined with `-TimingExperiment` to test both
features in the separate content build.

The launcher applies the diagnostic audio-synchronization workaround and
disables rewind. A full payload check can take tens of seconds at 60 fps;
the labels appear first and final numbers appear after the check completes.

## Validated file layout

The layout is exactly `boot bytes | content bytes | 64-byte footer`.
Required feature bit 4 (`16`) enables the content transport. Footer offset
`0x1C` holds content length and `0x20` its CRC-32. Content length must be
nonzero and at most 128 MiB when the feature is required, and zero otherwise.
Boot length remains at most 32 MiB and has its existing separate checksum.
The two remaining reserved words stay zero.

There is one contiguous content segment whose start is the end of the boot
segment. No caller-supplied segment offsets or overlapping segments exist in
this version. The parser validates the exact total length using widened
arithmetic, both segment checksums, feature support, and the footer checksum
before mapping. Truncation, trailing bytes before the footer, oversized
content, and descriptor/content length mismatches are rejected.

Cartridge size, cartridge mirrors, and internal boot CRC retain their boot
image meanings. The core ROM-size query reports the full development file.
For content-backed games, the core CRC identity covers the whole file,
including payload and footer; MD5/SHA-1 already use the full file. A content
checksum is separately visible to the guest. Enhanced patches remain
rejected: regenerate the boot/content checksums and footer together.

## Mapping and policy

Windows host policy is required. Reserve the full file mapping length from
budget headroom before calculating any optional RAM grant. Reject launch if
the mapping or required RAM cannot fit. Charging the complete view is
conservative accounting; it does not claim all file pages remain resident.

The loader maps the file through the existing VFile interface and retains a
pointer to the content suffix. The ordinary boot mapping is separate. Regular
files use native mappings, so this path does not allocate and copy a second
payload-sized content buffer. Parsing uses fixed 4 KiB checksum buffers.
Mapping failure frees pending RAM/content ownership; unload frees the content
view before closing the file. Reset retains the mapping and selects bank 0.

Preloading/archive frontends can materialize the entire VFile first; their
existing allocation precedes core admission and appears in process overhead.
They do not provide the same low-copy path as directly mapped files. A shared
cache, streaming disk interface, memory-pressure reclamation, and a process
RAM hard cap are not provided. The active host budget remains advisory.

## Guest registers

| Address | Access | Meaning |
| --- | --- | --- |
| `0x040003A0` | Read/write | Selected 16 MiB bank index, initially 0 |
| `0x040003A4` | Read-only | Total content bytes |
| `0x040003A8` | Read-only | Window capacity, 16 MiB while mapped |
| `0x040003AC` | Read-only | Valid bytes in the selected bank |
| `0x040003B0` | Read-only | Last bank write invalid: 0 or 1 |
| `0x040003B4` | Read-only | Payload CRC-32 |

Byte, halfword, and word bank writes update the corresponding portions of the
register. An out-of-range request leaves the current bank unchanged and sets
the status flag. A valid bank write clears that flag. Bank changes notify core
memory-list consumers. Registers exist only for explicitly enhanced games;
games without mapped content cannot write the bank selection.

## CPU memory contract

`0x11000000` through `0x11FFFFFF` reads the selected bank. There are no smaller
payload mirrors within a bank. Bytes beyond the final bank's content length
return zero, including partial halfwords/words at the payload tail. CPU loads
retain ARM7 alignment/rotation rules. Block loads use the existing register
order and writeback; crossing the window wraps its offset within the selected
bank, rather than switching banks automatically. Games should split copies
at bank boundaries and change the bank register explicitly.

Provisional access costs are eight board ticks for word loads and four for
halfword/byte loads, before the existing CPU instruction/fetch accounting.
Block loads retain their existing base/internal costs plus six additional
ticks per transferred word. Host paging latency affects wall-clock speed,
not the specified virtual cost. This is a data interface; instruction fetch
from this window is rejected. Copy code into FX RAM or ordinary RAM first.

CPU stores and raw debugger patch operations cannot change content. Memory
listing/raw views expose the current bank with its actual valid size. The
generic direct memory-block pointer is for readers honoring the read-only
flags; internal clients must not write through it. Legacy DMA masks cannot
reach this window, so stage content through CPU copies into ordinary RAM.

The [state prototype](fx-state-prototype.md) preserves bank selection and the
invalid-bank flag and verifies the complete content identity. Payload bytes
remain in the ROM file; states do not duplicate immutable content.

## Original diagnostics

The generator writes payloads in bounded 1 MiB chunks. Each four-byte word is
`0xA5C39E71 XOR globalWordIndex` in little-endian order. The guest executes
from IWRAM, switches banks, checks every whole word and any trailing bytes,
reads the final byte, and verifies a CPU copy into FX RAM. It draws six values:
content bytes, words verified, data errors, banks checked, last byte, and
RAM-copy success. All errors should be zero and RAM-copy success should be 1.

Fixtures contain 64 MiB, 128 MiB, and 64 MiB plus seven content bytes. Their
complete file sizes additionally include the 256 KiB boot image and footer.
The last fixture exercises a partial bank, an unaligned payload end, and a
non-power-of-two size. Independent screen verification decodes the guest's
numbers, validates expected values, and checks all 38,400 pixels. It reads only
the boot image and footer rather than copying a large payload into Python.

Unit tests cover descriptor limits/integrity, data beyond 32 MiB, each access
width, invalid/partial bank writes, tail padding, read-only accesses, raw
views/listing, block loads, access costs, file identity, reset, unsupported
states/fetch, low-budget admission, mapping failure, and ordinary reload.
Real archive loading, forced paging/pressure, additional content segments,
interactive debugger coverage, and extended persistence remain release work.

## Verified results

The desktop build passed 27/27 CTest suites, including 25 FX cases. Ordinary
GBA visual regression passed 26/26. All eleven complete diagnostic screens
passed independent verification, including these guest-rendered reports:

| Content bytes | Words verified | Errors | Banks checked | Last byte | RAM copy |
| --- | --- | --- | --- | --- | --- |
| 67,108,864 | 16,777,216 | 0 | 4 | 165 | 1 |
| 134,217,728 | 33,554,432 | 0 | 8 | 164 | 1 |
| 67,108,871 | 16,777,217 | 0 | 5 | 195 | 1 |

Logs, checksums, and screen-verification reports are recorded under ignored
`build-fx/milestone-4-content/`; original screenshots are under
`build-fx/content/diagnostics/`. These are controlled prototype results,
not a claim of a complete SDK, streaming architecture, or enhanced persistence.

Ordinary-game smoke timings varied with concurrent host load. Five alternating
600-frame object-test runs had medians of 138.463 ms for the preserved policy
build, 139.925 ms for the RAM build, and 134.151 ms for this content build, with
overlapping ranges. The report is `obj-paired.json` beside the other logs;
it supports a smoke comparison, not a performance-equivalence guarantee.
