# GBA-FX release candidate checks

Step 11 produces an internal Windows desktop candidate, not a frozen SDK or
public compatibility promise. The candidate is built separately so a running
prototype can keep its DLL loaded. Original diagnostic sources and fixture
generators remain in `tools/`; generated ROMs and evidence are ignored.

## Build and launch

```powershell
./tools/fx-build.ps1 -ReleasePrototype -AdaptivePrototype -TimingExperiment -ContentPrototype
./tools/fx-run.ps1 -ReleasePrototype -AdaptivePrototype -TimingExperiment -Rewind -RewindEntries 3
```

The second command opens `build-fx/release/diagnostics/adaptive-fast.gba`.
Rewind captures every six frames by default; `-RewindInterval 1` requests every
frame. Requested entries are limited by the actual memory policy. Omit
`-Rewind` to disable history. The launcher waits for the GUI process and checks
its exit code, avoiding the previous empty/stale PowerShell exit-code error.

## Recorded checks, 2026-10-12

Evidence is under `build-fx/milestone-6-release/` and
`build-fx/release/Testing/Temporary/LastTest.log`.

| Check | Result |
| --- | --- |
| Desktop CTest | 29/29 suites passed; FX suite has 47 cases, with the opt-in active soak skipped in the routine run |
| Default arithmetic build | FX and patch suites passed; experimental arithmetic remains off by default |
| Complete diagnostic frames | 19 runs verified all 38,400 pixels, including 1/4/16 MiB RAM and 64/128 MiB/tail content |
| Ordinary CInema | 184 passes, 88 expected failures, zero unexpected failures |
| Descriptor mutation sweep | 1,024 seeded mutations, including recomputed-checksum inputs and accepted-input bounds checks |
| State mutation sweep | 256 corrupt states rejected without changing the running state; reference CRC acceptance also checked |
| Lifecycle | 32 low/high grant cycles covering reset, state restore, failed enhanced loads, ordinary replacement, and cleanup |
| Active execution soak | 3,000 frames, 16 MiB RAM, 1,545,612,916 accelerated instructions, 1,656,372 audio samples, ten continuation checkpoints |
| Idle stability soak | 36,000 frames; the adaptive ROM finishes its active workload after 120 updates |
| CPU cap | Status 4: external Windows job prevents enforcement measurement |

The active soak runs a continuous arithmetic loop with timer/audio activity,
checks RAM retention, and compares execution after save/load checkpoints.
Its sample hash was `65D48000`. This verifies generated audio and execution;
it does not measure playback through a desktop audio device.

## Measured costs and changes

`tools/fx-release-check.py` records three alternating control/candidate runs,
commands, DLL/ROM hashes, and raw logs in `performance/report.json`. It uses
preserved pre-optimization builds as controls, rather than rebuilding the
controls with the candidate sources. The ordinary control is the earlier
default build; it is not the original upstream baseline revision.

```powershell
python tools/fx-release-check.py
```

The default paths expect the preserved `build-fx/adaptive` rewind control and
`build-fx` ordinary control. Explicit `--rewind-control`, `--ordinary-control`,
and `--build` paths can select other preserved builds inside this repository.

For the 16 MiB adaptive ROM, median candidate/control elapsed-time ratios were
0.981 without rewind, 0.913 with capture every frame, and 1.028 with capture
every six frames. Every-frame capture improved about 9%, but still achieved
only about 38 frames/second in this headless test. Six-frame capture measured
about 126 frames/second; its comparison showed no clear improvement. These
are host-dependent software-renderer measurements, not desktop pacing guarantees.
Three ordinary-game samples had ratios 0.948, 0.989, and 0.916; no slowdown
was observed in those samples.

Profiling separated raw state saving, copying, reference CRC, and unchanged
diffing. The changes remove redundant clearing of the RAM portion of a state
and use a local slicing-by-eight IEEE CRC calculation. Saved bytes and CRCs
match the reference implementation; checksum validation and the state format
are unchanged. Hardware cache misses and frontend overhead were not measured.

Correctness checks also exposed and fixed two shared rewind problems:

- A capture or restore could reuse buffers while a worker still had pending
  diff work. Both now wait for completion; rapid capture/restore is tested.
- Fast patches stepped through byte tails as words and rounded application
  offsets. Byte tails and unaligned extents now apply at their exact offsets,
  with extent bounds checks and forward/reverse guarded round trips.

## Support boundaries and remaining gate

The validated enhanced frontend is Windows Qt desktop. Headless performance
and CInema tools provide test evidence. Other desktop platforms, mobile and
console frontends have not passed this enhanced release matrix. Ordinary
games remain on the standard machine path.

Libretro explicitly rejects marked enhanced ROMs pending frontend validation.
Enhanced video-log recording and playback also reject the request; their
separate format does not yet carry a validated enhanced machine. Ordinary
video logging is covered by a positive recording test. Raw/PNG enhanced
saves and budgeted desktop rewind are supported.

ASan/UBSan runtimes and a Clang toolchain were unavailable in this portable
Windows toolchain. Bounded deterministic mutation and guarded patch tests
passed, but do not replace sanitizer coverage or broader fuzzing.

Step 11 remains open for user-visible frame pacing and audio-device stability
with CPU limiting actually enforced on a host outside the managed Windows job.
Every-frame 16 MiB rewind is not promised to maintain real time. The current
limits remain 16 MiB guest RAM and 128 MiB content; a world-scale area-loading
and cache demonstration should validate actual asset needs before freezing
the later SDK or `.gna` format.
