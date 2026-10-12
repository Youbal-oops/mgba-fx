# GBA-FX development prototype

This experimental interface supports explicit detection, host resource budgets, bounded extra RAM, and a read-only banked content prototype. A separate build also supports the limited arithmetic timing experiment. Future `.gna` packaging and SDK interfaces are separate milestones.

Implemented and tested on 2026-10-11. Build and demonstration commands are in [fx-development.md](fx-development.md). This interface is provisional and does not establish a stable SDK or library ABI.

## Development ROM footer

An enhanced development ROM consists of a conventional GBA boot image followed by an optional contiguous content payload and a 64-byte footer. All integer fields are unsigned, little-endian 32-bit values.

| Offset | Field | Prototype requirement |
| --- | --- | --- |
| `0x00` | Descriptor version | `1` |
| `0x04` | Footer size | `64` |
| `0x08` | Required features | Bit 0: detection; bit 1: host budget queries; bit 2: optional-build arithmetic experiment; bit 3: bounded extra RAM; bit 4: read-only banked content |
| `0x0C` | Optional features | Unknown bits may be ignored |
| `0x10` | Boot image length | At least `0xC0`, at most 32 MiB; exactly file length minus content length and 64 |
| `0x14` | Boot image CRC-32 | CRC-32 of the complete boot image |
| `0x18` | Minimum enhanced RAM | Minimum grant in bytes, at most 16 MiB; nonzero requires bit 3 |
| `0x1C` | Additional content length | Content byte length, at most 128 MiB; nonzero requires bit 4 |
| `0x20` | Content CRC-32 | Payload checksum, or `0` without content |
| `0x24` | Reserved | `0` |
| `0x28` | Reserved | `0` |
| `0x2C` | Footer CRC-32 | CRC-32 of bytes `0x00` through `0x2B` |
| `0x30` | Identification, 16 bytes | ASCII `mGBA-FX-DRAFT01` followed by a NUL byte |

The marker is anchored to the end of the file. Filename, ROM title, ROM size, and arbitrary strings inside the boot image do not activate the extension. A recognized footer with invalid fields, unsupported requirements, or bad CRC is rejected before ROM mapping. The footer is excluded from the cartridge's mapped boot image and internal boot checksum. Content-backed game identity covers the complete development file.

A file whose identifying bytes have been removed or damaged is indistinguishable from an ordinary file; it follows normal loading rules. Truncated descriptor bodies that retain the end marker remain recognizable and are rejected. CRC checks detect accidental corruption and are not authentication signatures.

## Guest registers

Only a successfully validated enhanced ROM exposes these experimental registers. Ordinary games retain existing behavior at these addresses. Content control registers at `0x040003A0`?`0x040003B4`, including the writable bank selection, are specified in [fx-content-prototype.md](fx-content-prototype.md). The identification and budget registers below are read-only.

| Address | Width | Value |
| --- | --- | --- |
| `0x040003C0` | 32 bits | Host RAM budget, MiB |
| `0x040003C4` | 32 bits | Available physical RAM at launch, MiB |
| `0x040003C8` | 32 bits | Estimated reserved host overhead plus extra RAM allocation, MiB |
| `0x040003CC` | 32 bits | Budget minus reserved overhead, MiB |
| `0x040003D0` | 32 bits | Active RAM percentage, or `0` when using an explicit MiB override |
| `0x040003D4` | 32 bits | Active requested RAM override in MiB; `0` selects percentage |
| `0x040003D8` | 32 bits | Requested host CPU percentage |
| `0x040003DC` | 32 bits | Current CPU enforcement status, below |
| `0x040003E0` | 32 bits | `0x58464247`, little-endian `GBFX` |
| `0x040003E4` | 32 bits | Interface version `1` |
| `0x040003E8` | 32 bits | Active features: bit 0; bit 1 when a host budget is available; bit 2 when experimental arithmetic is active; bit 3 while extra RAM is allocated; bit 4 while content is mapped |
| `0x040003EC` | 32 bits | Actual enhanced RAM grant, MiB; zero without required bit 3 |
| `0x040003F0` | 32 bits | Allowed logical CPU count at launch |
| `0x040003F4` | 32 bits | Arithmetic profile multiplier: normally `1`, experimental `2` for selected IWRAM arithmetic |
| `0x040003F8` | 32 bits | Confirmed effective process CPU limit in percent; `0` if unconfirmed |
| `0x040003FC` | 32 bits | Host RAM hard-cap enforcement: `0` (budget only) |

MiB values are rounded down. RAM snapshots and requested settings stay fixed for the loaded game. CPU enforcement status and the confirmed effective limit reflect current process-wide ownership and can change when other cores open or close. No CPU clock change is implied by a host utilization limit. The arithmetic profile is a limited accelerator and does not change the nominal board frequency.

CPU status values are `0` inactive, `1` enforced, `2` unavailable, `3` suspended because an ordinary initialized core shares the process, `4` an existing Windows job prevents safe quota interpretation, and `5` an operating-system error leaves enforcement unconfirmed. Only status `1` claims an enforced cap.

Byte and halfword reads return the corresponding portions through normal I/O access paths. Writes do not change these read-only values. Addresses are provisional and must be reviewed before SDK compatibility is frozen.

Debugger memory views expose the same identification values for enhanced instances.

The diagnostic program checks the magic and interface version, stores its detected status at `0x02000000`, and displays a labeled result. The ordinary and enhanced fixtures share the same boot program; only the enhanced fixture carries the validated footer.

## Lifecycle and unsupported work

Detection state belongs to the loaded GBA instance. It survives a soft reset of that game and is cleared on unload, normal-ROM load, failed load, or a switch to another supported loading mode. Two emulator instances must not share detection state.

Detection, host budget queries, bounded extra RAM, and a read-only banked content prototype are implemented. General enhanced CPU capacity and extension save-state support remain future work. See [fx-content-prototype.md](fx-content-prototype.md) for the content transport. See [fx-ram-prototype.md](fx-ram-prototype.md) for the memory contract and demonstration. Required unknown features are rejected; optional unknown features do not appear in the supported feature mask.

An explicitly enabled experimental build also accepts required feature bit 2
and reports it for games that activate the limited arithmetic profile. Default
builds reject it. See [fx-timing-experiment.md](fx-timing-experiment.md) for its
exact instruction scope, timing rules, tests, and unsupported persistence.

Patching enhanced fixtures is initially unsupported: the boot CRC and descriptor must be regenerated together. Ordinary patches keep their existing workflow. ELF and multiboot extension transports are deferred; recognized development footers must be routed through the cartridge loader for validation.

## Host budget settings

The desktop settings appear under **Enhancements → GBA-FX development resources**. Configuration keys are `gba.fxMemoryPercent` (1–100, default 80), `gba.fxMemoryMiB` (0–1048576, default 0), and `gba.fxCPUPercent` (1–100, default 80). The explicit RAM setting overrides the percentage. Settings apply when reopening a game; a soft reset preserves the active snapshot.

The RAM budget is bounded by available physical RAM and host virtual address space. Estimated overhead is the larger of current process private committed bytes and 64 MiB, plus 32 MiB of reserve. Admission fails if that estimate exceeds the budget. This budget neither preallocates the entire allowance nor sets a process memory cap. RAM-marked games receive a bounded allocation from remaining headroom, subject to their declared minimum.

Windows CPU enforcement uses a [Job Object hard cap](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information), covering the process's threads. The percentage is normalized to its allowed affinity mask on a single processor group. Existing external jobs and multiple processor groups are reported as unavailable rather than interpreted as an independently enforceable quota. When several enhanced cores share the process, the smallest requested limit wins. Any initialized ordinary GBA or GB core suspends the cap. A bounded 64-client registry conservatively disables enforcement after overflow. The job handle remains for the process lifetime, with its CPU limit disabled when unused.

Other hosts currently retain detection-only support. A game requiring host policy is rejected if that capability is unavailable. The host settings do not automatically parallelize guest code or increase the original GBA clock.
