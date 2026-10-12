# GBA-FX arithmetic timing experiment

This is the M3 feasibility proof, corresponding to numbered Step 5 in
`planning.md`. It is a limited arithmetic accelerator, rather than a general
CPU overclock or a stable SDK interface. The default build excludes it.

## Build and run

```powershell
./tools/fx-build.ps1 -TimingExperiment
./tools/fx-run.ps1 -TimingExperiment
./tools/fx-run.ps1 -TimingExperiment -Rom ./build-fx/timing/diagnostics/timing-1x.gba
```

The experiment builds into ignored `build-fx/timing/`, keeping the previous
desktop build intact. The two timing ROMs display the arithmetic profile and
the number of additions completed during the measured portion of each frame.
Their screen rendering and VCOUNT polling also consume time. The launch script
disables rewind for the experiment session; experimental state saving/loading
is rejected before state data is written or applied.

The desktop 2x demo was opened and confirmed running with the window title
`mGBA - FX ALU 2X`. Its stdout/stderr logs are in `build-fx/milestone-3/`.

`BUILD_FX_TIMING_EXPERIMENT` defaults to `OFF`. Enabling it adds the internal
`GBA_FX_TIMING_EXPERIMENT` compile definition. A validated development footer
must also require feature bit 2 (`0x4`) to activate the accelerator. Neither
ordinary games nor detection/resource-only games activate it. An optional bit
does not activate it. A default build rejects a ROM requiring that feature.

The visible 1x fixture requires features `0x3`; the 2x fixture requires `0x7`.
These demonstration files therefore also require the currently Windows-only
host budget capability. Their code and pixels are generated without a public
SDK or an ARM compiler.

## Clock and ownership rules

| Quantity | Unit and handling |
| --- | --- |
| `cpu->cycles` | Integer board ticks, including completed CPU work since the last scheduler tick |
| `cpu->nextEvent` | Deadline in that same relative board-tick domain |
| `mTiming.masterCycles` | Unsigned 32-bit board timestamp; wrap is intentional |
| `mTimingEvent.when` | Absolute timestamp in the same wrapping board domain |
| `mTiming.globalCycles` | Diagnostic 64-bit accumulated board time when debugger support is enabled |
| `fx.aluPhase` | One outstanding half-board-tick of accelerated arithmetic, either 0 or 1 |
| Memory waitstates, prefetch, bus stalls | Original board-tick costs |
| DMA service and its scheduling | Original board-tick costs, outside the arithmetic conversion |
| Video, sound, timers, IRQ delay | Original board-tick schedules |
| Nominal core frequency | Unchanged `GBA_ARM7TDMI_FREQUENCY` |

The scheduler already shares its relative-cycle and deadline pointers with the
ARM core. Scaling those counters wholesale would also scale peripherals and
memory stalls. This proof instead converts only a known one-cycle arithmetic
retirement, leaving the scheduler and instruction implementations intact.

The selected instruction must execute in GBA IWRAM, whose active sequential
fetch wait is zero. Eligible ARM instructions are immediate data processing or
data processing with an immediate register shift, excluding writes to PC and
status-control encodings. Eligible Thumb instructions are immediate
MOV/CMP/ADD/SUB. The decoder checks eligibility before execution and verifies
that the instruction really added exactly one cycle before converting it.
Condition-failed eligible ARM slots follow the same conversion. Other
instructions use their original cost.

Each eligible slot adds half a board tick. The first slot retains a fractional
remainder; the second adds one integer tick and clears that remainder. The
remainder survives ordinary instruction and scheduler boundaries. HALT and DMA
resume at integer hardware deadlines and clear it. Soft reset clears both the
remainder and measurement counter while preserving the selected profile;
unload or a different ROM clears the profile with the rest of its FX state.

Branches, pipeline refills, register-controlled shifts, loads/stores,
multiplication, BIOS services, and IRQ entry remain at their original costs.
I/O operations are never classified as accelerated arithmetic. The existing
instruction-level representation still quantizes side effects and event
delivery to integer board ticks; this experiment does not model an independent
fractional-time bus. It must not be generalized by dividing every instruction
cost or changing the frequency constant.

## Verification

Verified locally on 2026-10-11 with the portable GCC 13.1/Qt 6.8.3 build:

| Check | Result |
| --- | --- |
| Experimental CTest build | 27/27 suites; 25 GBA-FX cases |
| Separate default core build | 17/17 GBA-FX cases, including rejection of required experimental features |
| Ordinary GBA CInema regressions | 26/26 cases |
| Original/resource/timing ROM screens | Seven runs; all 38,400 pixels checked independently in each |
| ARM register-addition loop, 12 frames | 2,990,666 operations at 1x; 5,724,992 at the arithmetic profile |
| Thumb register-addition loop, 12 frames | Same work counts as ARM |
| Board-time difference after 12 frames | 2 ticks, within the original instruction boundary tolerance |
| Sound output over those frames | Both produce 6,114 samples; full trace hashes match |
| Memory operations and register-controlled shift | Same costs and data in both profiles |
| DMA copy | Same completion timestamp and all copied words |
| HALT and timer interrupt | Same wake-up timestamp; phase reset verified |
| Timer counter | Same cadence within one count at the selected /64 divider |
| Wrapping timestamp and equal-deadline events | Correct delivery across 32-bit wrap, preserving event priority |

The tight 64-addition loop gains about 1.9143x because its branch remains at
the standard cost: 67 board ticks versus 35 for each loop. The visible game
reports 101,312 versus 186,816 additions, about 1.8440x, because it also polls
video state and draws its result. These are **guest work per frame** measurements,
not a claim that the emulator itself runs faster on the PC.

Evidence is under ignored `build-fx/milestone-3/`, the experimental build's
CTest logs, and `build-fx/timing/diagnostics/visual-*/verification.json`.
Each visual run captures into a new directory and checks the screen independently;
it never changes committed compatibility baselines.

Five interleaved 600-frame software-renderer runs per variant recorded ordinary
game costs against the preserved M2 binaries:

| Ordinary case | M2 median, ms | Experiment-enabled median, ms | Default core median, ms |
| --- | ---: | ---: | ---: |
| IRQ/key input | 28.305 | 26.083 | 27.464 |
| Object 2D wrapping | 134.887 | 140.576 | 149.286 |
| Disabled-background blend | 378.192 | 403.834 | 397.232 |

Ranges overlap and there are substantial outliers; the default core comparison
also omits Qt, scripting, OpenGL, and SQLite. These short host-dependent samples
do not isolate a reliable percentage cost or establish performance equivalence.
The enabled experiment has a per-event dispatch check and additional checks
for accelerated instructions. Broader profiling remains necessary before
performance claims. Raw measurements are in `ordinary-paired.json`.

## Remaining work

This proves that a bounded selection of internal operations can fit more work
between unchanged peripheral events. It does not prove a general two-clock CPU
model, faster memory or multiplication, execution from expanded RAM, dynamic
host calibration, a maximum reliable profile, or long-run game compatibility.
Full BIOS/bus timing and breakpoint behavior need broader coverage before
extending the eligible instruction set. State serialization and rewind need a
format that preserves the profile and fractional remainder before support is
enabled.

The original resource/timing fixtures still request zero extra guest RAM.
The separate [RAM prototype](fx-ram-prototype.md) now allocates extra RAM
for required feature bit 3; its code retains ordinary instruction timing. Windows hard-cap
measurement remains unavailable inside this managed external job. Those are
separate limitations; the arithmetic proof neither allocates the RAM budget
nor claims enforcement of the host CPU target.
