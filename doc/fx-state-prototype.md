# GBA-FX state persistence prototype

Enhanced raw and PNG save states now contain the complete extra RAM allocation,
the RAM grant, required feature mask, arithmetic profile and fractional remainder,
accelerated instruction counter, content bank, and invalid-bank flag. Ordinary
GBA states retain their existing size and format. This remains a private draft
interface, not an SDK or `.gna` container.

## Using the desktop build

```powershell
./tools/fx-build.ps1 -AdaptivePrototype -TimingExperiment
./tools/fx-run.ps1 -AdaptivePrototype -TimingExperiment
# Optional rewind; actual capacity is reduced to fit remaining memory.
./tools/fx-run.ps1 -AdaptivePrototype -TimingExperiment -Rewind -RewindEntries 30
```

Use the emulator's ordinary save/load state commands with the adaptive, RAM,
content, or arithmetic diagnostic. Reopen with the same ROM, granted RAM size,
and arithmetic profile to load the state. A different grant is rejected, even
when it is larger: changing RAM size changes address mirrors and guest behavior.
Pending resource settings and the current process CPU limit are not restored
from the state. Host quotas and measurements always belong to the current run.

Cartridge SRAM/EEPROM/Flash saves use the existing mechanisms; extra RAM is
volatile and is not automatically written into a cartridge save. A larger
persistent storage device is future work.

## Draft binary format

The ordinary `0x61000`-byte state prefix is retained, with its version marker
replaced by `0x02000001`. This marker makes older ordinary GBA loaders reject
the enhanced state before applying it. The prefix is followed by a 64-byte
little-endian header and exactly the current RAM grant in bytes. Content is
identified rather than duplicated in each state.

| Header offset | Value |
| --- | --- |
| `00` | Extension marker `0x02000001` |
| `04` | Header size, 64 |
| `08` | RAM grant in bytes |
| `0C` | Required ROM features |
| `10`, `14` | Boot length and CRC32 |
| `18`, `1C`, `20` | Content length, content CRC32, complete ROM-file CRC32 |
| `24` | Arithmetic profile, 1 or 2 |
| `28` | Fractional phase, 0 or 1; profile 1 requires 0 |
| `2C`, `30` | Selected content bank and invalid-bank flag |
| `34` | CRC32 of prefix, header, and RAM, excluding these four bytes |
| `38` | 64-bit accelerated instruction counter |

No host pointers, content mappings, CPU enforcement handles, or host resource
budgets are serialized. The CRC detects accidental corruption; it is not an
authentication mechanism. Identity and allocation checks finish before reading
RAM bytes, rebuilding the instruction pipeline, or changing the bank.

Core loading prevalidates the extension and ordinary state, including CPU mode,
PC alignment, and executable region. Named file loading also retains a rollback
snapshot, including cartridge data, while renderer/link-driver extension data is applied. Failure restores
that snapshot before returning false. Third-party subsystem callbacks must be
able to restore their own valid snapshots; a rollback failure is logged as an
error. Disk write failures are reported, but saving is not an atomic file rename.

## Rewind and memory

Desktop rewind starts with a conservative limit derived from remaining launch
budget and current available physical/address space. It reserves eight state
sizes plus 1 MiB for growing buffers and scratch storage. Each history entry is
charged 64 state sizes plus 16 KiB because sparse changes can produce many
large diff extents and vector growth can nearly double their allocation.
Actual typical deltas are much smaller. Optional history is disabled if no
entry fits. Before captures, lower available memory can shrink or discard
history; the policy is logged. Required guest RAM never shrinks mid-run.

This is allocation planning, not an operating-system RAM hard cap. The fixed
subsystem reserve covers the supported desktop configuration, not arbitrary
third-party drivers. Direct rewind API clients should call
`mCoreRewindCapacity` before constructing history. The launch script keeps
rewind off unless `-Rewind` is supplied.

## Verification and limits

Tests cover full 1/4/16 MiB RAM restoration after reset, raw/PNG saves, rewind,
bank and error-flag restoration, arithmetic continuation, corrupt CRCs,
valid-CRC malformed metadata/CPU states, mismatched grants, missing/truncated
data, subsystem rollback, and unchanged ordinary raw/PNG formats.

Qt state loading and scripting use the shared named-state path. The Qt save
converter recognizes enhanced states and locates raw savedata after the dynamic
RAM suffix. Libretro uses the dynamic named-state size and propagates save
failure, but this milestone validates the Windows desktop build. Video-log
recordings have their own format and are not covered by enhanced persistence.
Manual desktop save/load/rewind testing remains useful before wider release.
