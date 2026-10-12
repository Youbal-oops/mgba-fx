# GBA-FX detection prototype

This is an experimental development interface for the first detection milestone. It does not grant extra RAM or change CPU timing. Future `.gna` packaging and SDK interfaces are separate milestones.

Implemented and tested on 2026-10-11. Build and demonstration commands are in [fx-development.md](fx-development.md). This interface is provisional and does not establish a stable SDK or library ABI.

## Development ROM footer

An enhanced development ROM consists of a conventional GBA boot image followed by a 64-byte footer. All integer fields are unsigned, little-endian 32-bit values.

| Offset | Field | Prototype requirement |
| --- | --- | --- |
| `0x00` | Descriptor version | `1` |
| `0x04` | Footer size | `64` |
| `0x08` | Required features | Only bit 0, detection, is supported |
| `0x0C` | Optional features | Unknown bits may be ignored |
| `0x10` | Boot image length | At least `0xC0`, at most 32 MiB; exactly file length minus 64 |
| `0x14` | Boot image CRC-32 | CRC-32 of the complete boot image |
| `0x18` | Minimum enhanced RAM | `0` until memory grants exist |
| `0x1C` | Additional content length | `0` until larger-content support exists |
| `0x20` | Reserved | `0` |
| `0x24` | Reserved | `0` |
| `0x28` | Reserved | `0` |
| `0x2C` | Footer CRC-32 | CRC-32 of bytes `0x00` through `0x2B` |
| `0x30` | Identification, 16 bytes | ASCII `mGBA-FX-DRAFT01` followed by a NUL byte |

The marker is anchored to the end of the file. Filename, ROM title, ROM size, and arbitrary strings inside the boot image do not activate the extension. A recognized footer with invalid fields, unsupported requirements, or bad CRC is rejected before ROM mapping. The footer is excluded from the cartridge's mapped boot image and ROM checksum.

A file whose identifying bytes have been removed or damaged is indistinguishable from an ordinary file; it follows normal loading rules. Truncated descriptor bodies that retain the end marker remain recognizable and are rejected. CRC checks detect accidental corruption and are not authentication signatures.

## Read-only guest identification registers

Only a successfully validated enhanced ROM exposes these experimental registers. Ordinary games retain existing behavior at these addresses.

| Address | Width | Value |
| --- | --- | --- |
| `0x040003E0` | 32 bits | `0x58464247`, little-endian `GBFX` |
| `0x040003E4` | 32 bits | Interface version `1` |
| `0x040003E8` | 32 bits | Supported feature mask: bit 0 only |
| `0x040003EC` | 32 bits | Enhanced RAM grant: `0` in this milestone |

Byte and halfword reads return the corresponding portions through normal I/O access paths. Writes do not change these read-only values. Addresses are provisional and must be reviewed before SDK compatibility is frozen.

Debugger memory views expose the same identification values for enhanced instances.

The diagnostic program checks the magic and interface version, stores its detected status at `0x02000000`, and displays a labeled result. The ordinary and enhanced fixtures share the same boot program; only the enhanced fixture carries the validated footer.

## Lifecycle and unsupported work

Detection state belongs to the loaded GBA instance. It survives a soft reset of that game and is cleared on unload, normal-ROM load, failed load, or a switch to another supported loading mode. Two emulator instances must not share detection state.

Only identification is advertised. Extra RAM, CPU capacity, host budget settings, large content, and extension save-state support are not advertised until their milestones pass. Required unknown features are rejected; optional unknown features do not appear in the supported feature mask.

Patching enhanced fixtures is initially unsupported: the boot CRC and descriptor must be regenerated together. Ordinary patches keep their existing workflow. ELF and multiboot extension transports are deferred; recognized development footers must be routed through the cartridge loader for validation.
