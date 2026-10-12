# GBA-FX expanded RAM prototype

Implemented on 2026-10-12 as the first part of numbered Step 6. This is a
provisional emulator interface, with a 16 MiB maximum rather than a finished
large-memory architecture. Larger content, general CPU acceleration, extended
save states, and the public SDK remain later work.

## Build and run

```powershell
./tools/fx-build.ps1
./tools/fx-run.ps1 -RAMDiagnostic
```

The desktop executable is `build-fx/mGBA.exe`; the original diagnostic is
`build-fx/diagnostics/ram.gba`. The launcher supplies the portable DLL/plugin
paths and disables rewind. The RAM feature is available in the default build;
the separate arithmetic experiment still requires its own build flag.

The RAM diagnostic launcher also disables host audio synchronization, enables
video synchronization, and selects software emulation rendering. This avoids
a locally observed desktop stall during drawing. The user confirmed the full
screen and expected results at 60 fps with these session overrides. They do
not change the emulated audio timing or persistently modify user settings;
the underlying audio-backend issue still needs separate investigation.

The same diagnostic adapts to different budgets:

```powershell
./tools/fx-run.ps1 -RAMDiagnostic -MemoryMiB 256
```

The budget covers estimated process overhead and the extra allocation. It
does not instruct the emulator to commit the entire host allowance. Desktop
overhead may differ from the headless test runner's overhead.

## Admission and grant

Required footer bit 3 (`8`) requests expanded RAM. Optional bits do not
activate it. Footer offset `0x18` declares the minimum acceptable grant in
bytes; it must be zero for games without required bit 3. Values above 16 MiB
are rejected. Existing detection/resource/timing fixtures need no changes.

After validating the descriptor and host policy, select the largest of
1, 2, 4, 8, or 16 MiB that fits the remaining host budget. Reject admission if
none fits or the result is smaller than the minimum. These are bounded
prototype allocations, not fixed requirements for every game.

Allocate only the selected region. The anonymous mapping starts at zero;
reset clears it and retains its size and active policy. Deduct its size from
budget headroom and add it to reserved bytes. A failed ROM mapping releases
the new allocation. Unload, ordinary loading, multiboot loading, null loading,
and destruction release it. Ownership is per GBA instance.

Windows host measurement is currently necessary for admission. Unsupported
hosts reject games requiring the RAM feature. Allocation failure reports an
error instead of advertising a grant. Physical out-of-memory injection and
shared accounting under many simultaneous instances remain validation work.

Register `0x040003EC` reports the actual grant in MiB. Active feature register
`0x040003E8` reports bit 3 only while the allocation exists. Normal games and
enhanced games without this requirement receive no extra RAM.

## Memory and execution contract

The CPU window is `0x10000000` through `0x10FFFFFF`. It does not replace a
standard GBA region. Within this window, the offset is masked by grant size
minus one: a 1 MiB grant has sixteen mirrors; a 16 MiB grant has one.
Programs should use the canonical range from the base through grant size.
Addresses outside this window retain the ordinary invalid-access behavior.

Byte stores preserve neighboring bytes. Halfword and word stores align down;
unaligned loads follow the existing ARM7 rotation behavior. ARM/Thumb
instruction fetch uses the same allocation and mirror mask. Block transfers
use the existing ARM register order, writeback, and aligned access rules, with
wrapping at a mirrored grant boundary. Do not let sequential execution cross
the outer 16 MiB window; branch explicitly when transitioning to another
region, as the cached fetch machinery does not check each sequential address.

The memory uses a provisional zero-wait 32-bit CPU bus: data loads have the
existing two-tick load accounting, stores one tick, and sequential fetch has
zero extra wait ticks. Block transfers use the existing internal/base costs.
All costs remain board ticks; host memory latency does not change virtual
timing. Game Pak wait settings do not change this region. Cartridge prefetch
may overlap accesses while executing cartridge code, but is never applied as
cartridge prefetch while executing FX RAM code.

The arithmetic accelerator remains limited to IWRAM. Executing from FX RAM
does not implicitly activate or broaden it.

The core memory list includes `fxram` with its canonical grant bounds. Raw
views, byte/halfword/word patches, and the existing debugger patch paths use
the allocation and matching masks. The direct memory-block API reports the
actual pointer and size, and removes the block after ordinary loading.

## DMA, BIOS, and persistence

Legacy DMA address masks exclude bit 28. Supplying an FX address to DMA
therefore targets its masked legacy address, not FX RAM. Copy with CPU
loads/stores into EWRAM/IWRAM first, then use normal DMA to video/audio memory.
This prototype adds no transfer device and does not change the DMA masks.

The bundled HLE BIOS `CpuSet` word-copy path has been tested from FX RAM to
EWRAM. Other BIOS calls and external BIOS images are not claimed compatible
with this extension; use explicit guest routines until their assumptions are
audited. RAM-reset SWIs retain their original hardware contract; emulator
reset clears FX RAM.

Core save/load operations reject the RAM prototype because ordinary GBA
states omit this allocation. Shared raw and PNG saving now propagate core
rejection; raw named saving obtains a successful snapshot before modifying
its destination. Failed core loading stops before restoring extension data.
Rewind discards failed captures rather than appending invalid history. The
launcher disables it for the demonstration. These changes do not provide
enhanced state serialization; normal save states remain supported.

## Verification

Five new FX test cases cover grant calculations including low budgets,
descriptor limits, 1/4/16 MiB accesses, aliases, last-word alignment, raw
inspection/patching, memory listing, ARM-to-Thumb-to-IWRAM transitions,
instruction fetch across a small grant's mirror boundary, block transfers,
DMA masks, HLE BIOS copying, reset, failed mapping, admission failure,
enhanced-to-normal cleanup, raw/PNG save rejection, rewind rejection, and
ordinary raw/PNG state round trips.

`ram.gba` writes an address-dependent pattern to every granted word, reads
every word back, counts mismatches, then executes small original ARM and
Thumb routines from the new region. It draws the grant, verified word count,
error count, and routine results. Independent PNG decoding checks every
screen pixel and the expected values:

| Grant | Words verified | Errors | ARM result | Thumb result |
| --- | --- | --- | --- | --- |
| 1 MiB | 262,144 | 0 | 42 | 43 |
| 4 MiB | 1,048,576 | 0 | 42 | 43 |
| 16 MiB | 4,194,304 | 0 | 42 | 43 |

The headless runner uses explicit 97 and 100 MiB host budgets for small grants
when its baseline process consumption fits the 64 MiB floor; desktop grants
can differ. The diagnostic capture waits 2,048 emulated frames for the full
pattern check. Fixture generation and verification are part of the default
portable build. Captures remain in ignored `build-fx/diagnostics/visual-*/`.

Final default and timing desktop builds each passed 27/27 CTest suites, with
22 and 30 FX cases respectively. Ordinary GBA visual regression passed 26/26.
The default build verified eight complete diagnostic screens; the timing
build verified ten, retaining its 1.8440x visible arithmetic result. Final
logs are under ignored `build-fx/milestone-4-ram/`.

After compilation finished, three 600-frame ordinary-game smoke runs had
median guest execution times of 24.168 ms (keyirq), 130.389 ms (2d-wrap), and
379.816 ms (disabled-bg-semitrans-blend). Earlier measurements taken while
the other build was compiling were substantially noisier. These are smoke
measurements, not a guarantee of performance equivalence or improved speed.

This completes a bounded RAM proof. External BIOS coverage, interactive
debugger breakpoint validation, physical allocation-failure injection,
long-running pressure tests, a larger address-space design, and enhanced
persistence remain before treating it as a complete Step 6 release gate.
