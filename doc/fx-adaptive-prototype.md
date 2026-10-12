# GBA-FX adaptive-resource proof

This implements the controlled resource-adaptation portion of numbered
Step 8, dated 2026-10-12. It adds an internal capability/allocator helper and
an original ROM that changes optional cache/work allocations while retaining
one simulation update per display frame. Automatic host CPU calibration and
sustained slow-host warnings remain unfinished.

## Build and test

```powershell
./tools/fx-build.ps1 -AdaptivePrototype -TimingExperiment
./tools/fx-run.ps1 -AdaptivePrototype
```

The separate build is `build-fx/adaptive/mGBA.exe`. `adaptive.gba` uses normal
instruction timing; `adaptive-fast.gba` additionally requires the existing
experimental arithmetic feature. The latter is generated only with the
timing build option. The default launcher chooses the normal-timing ROM;
adding `-TimingExperiment` to the launcher chooses the marked fast ROM.

Try different host budgets with the same ordinary-profile adaptive ROM:

```powershell
./tools/fx-run.ps1 -AdaptivePrototype -MemoryMiB 128
./tools/fx-run.ps1 -AdaptivePrototype -MemoryMiB 256
```

Budget includes process overhead and the content mapping, so these numbers
are not direct guest-RAM allocations. Desktop overhead can differ from the
headless runner. An insufficient budget rejects launch instead of shrinking
required execution memory. Use the displayed actual grant when comparing
runs. The launcher retains the diagnostic audio-sync workaround and keeps
rewind off by default. See [fx-state-prototype.md](fx-state-prototype.md) for
save/load support and optional bounded rewind.

## Internal guest helper

`include/mgba/internal/gba/fx-test.h` is an internal diagnostic header, not a
public SDK contract. `GBAFXTestQuery` takes a read callback so the same helper
can be exercised against real core bus reads in host tests. A guest adapter
can provide volatile 32-bit device reads. The helper checks interface magic
and version, then snapshots active feature bits, actual guest RAM in bytes,
content length, and the selected arithmetic profile. Unsupported versions,
impossible grants, and unsupported profiles fail instead of producing a
usable allocation.

The arena supplies aligned, bounded bump allocations within the canonical
RAM grant. Invalid alignment, size/rounding overflow, and exhaustion fail
without advancing its cursor. A game reserves essential storage first and
allocates its optional cache from the rest. Cache sizing is bounded by guest
RAM, mapped content, and a 1 MiB diagnostic ceiling. No live memory resizing
is implied; pending host settings do not alter an active grant.

The hand-assembled diagnostic reads the same versioned registers and imports
the helper's memory/cache constants during generation. It uses the same
required-first bounds and cache placement. It does not compile the C header
into ARM code; a compiled guest example remains part of later toolchain/SDK
work. Host unit tests exercise the C query and allocations against real FX
RAM mappings at 1/4/16 MiB, including failures and reset/config stability.

## Adaptive game behavior

The file contains the same 256 KiB boot program and 1 MiB original content
payload for every RAM-budget run. It requests the existing policy, RAM, and
content features, with a minimum 1 MiB RAM grant. The alternate fast file
differs only in its required arithmetic bit/footer checksum.

The guest reserves 64 KiB for essential game storage, then chooses a cache
equal to one sixteenth of its actual RAM grant, capped at 1 MiB. It copies
content words into that cache using the CPU. Optional work per frame is one
cache lookup/pattern check per cache KiB, multiplied by the explicitly
selected arithmetic profile (1 or 2). The test tracks a rolling offset and
counts any mismatch against the original content's address-dependent pattern.

The game waits for each transition into VBlank and advances its simulation
counter once. It runs 120 updates, then draws the results. The optional work
is deliberately bounded to fit VBlank for all supported grants/profiles.
It cannot increase the simulation counter merely by completing work faster.

Timers 0/1 form a cascaded 32-bit counter with 64 board ticks per count.
Capture begins at the first update and ends at the 120th, before optional
work. There are 119 frame intervals, giving
`119 * 280896 / 64 = 522291` counts. Independent verification allows two
counts of polling/read latency. The timer remains the GBA board clock even
when selected IWRAM arithmetic retires faster.

## Screen values and controlled expectations

| Value | Meaning |
| --- | --- |
| GUEST RAM MIB | Actual stable grant, not the host budget |
| CACHE KIB | Optional content cache allocated inside the grant |
| WORK PER FRAME | Bounded optional cache checks per simulation update |
| SIMULATION TICKS | Required simulation updates, always 120 |
| OPTIONAL WORK | Total completed optional checks |
| DATA ERRORS | Cache/pattern mismatches, expected 0 |
| CPU PROFILE X | Existing limited arithmetic profile, 1 or 2 |
| FRAME TIMER | Board-timer counts across the 119 measured intervals |

For profile 1, the expected controlled reports are:

| RAM MiB | Cache KiB | Work/frame | Updates | Optional checks | Errors | Timer |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 64 | 64 | 120 | 7,680 | 0 | 522,291 |
| 4 | 256 | 256 | 120 | 30,720 | 0 | 522,291 |
| 16 | 1,024 | 1,024 | 120 | 122,880 | 0 | 522,291 |

Profile 2 doubles optional work/frame and total optional checks; required
updates and the board-timer interval stay the same. This is a tested guest
work policy using the limited arithmetic profile, not a general CPU clock
or a measured entitlement to twice the host performance.

The headless runner uses host budgets of 99, 102, and 128 MiB to obtain the
small/middle/large grants when its baseline process fits the 64 MiB accounting
floor. Requested CPU targets are 25%, 60%, and 80%; these targets do not choose
the guest profile. The managed Windows job still prevents claiming measured
hard-cap enforcement. Each profile uses a byte-identical ROM across its
three budget runs. Complete-frame checks decode all eight values and verify
every screen pixel after 256 emulated frames.

Final verification passed 27/27 desktop CTest suites, including 35 FX cases
in the experiment-enabled build. A separate default core passed 27 FX cases.
Ordinary GBA visual regression passed 26/26. Sixteen complete diagnostic
screens passed independent checks, including all six adaptive runs. Every
adaptive run measured exactly 522,291 timer counts and zero errors; profile 2
completed 15,360, 61,440, and 245,760 optional checks for the three grants.
The original timing diagnostic retained its 1.8440x visible workload result.
Evidence is under ignored `build-fx/milestone-4-adaptive/` and screenshots
under `build-fx/adaptive/diagnostics/`.

## Remaining Step 8 work

No automatic host calibration or runtime CPU-profile selection is added.
Current profiles remain explicit ROM requirements; the arithmetic experiment
has not become a general enhanced CPU model. Active grants are stable, and
soft reset retains them. A restart can obtain a new grant. The demo retains
all required simulation updates rather than silently changing virtual timing
or dropping state under a slower wall-clock host.

Long-running wall-clock pacing, CPU-cap measurements outside an external
job, automatic profile calibration, sustained slow-host reporting, and
runtime capability-change negotiation remain future validation/design work.
Enhanced persistence remains rejected until Step 9 captures the complete
machine. This proof does not complete the full Step 8 release criteria.
