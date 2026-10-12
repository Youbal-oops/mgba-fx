/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/fx.h>

#include <mgba/internal/gba/memory.h>
#include <mgba-util/crc32.h>
#include <mgba-util/vfs.h>
#include <mgba-util/memory.h>

static const uint8_t _magic[16] = "mGBA-FX-DRAFT01";

static bool _readExact(struct VFile* vf, void* output, size_t size) {
	uint8_t* bytes = output;
	while (size) {
		ssize_t count = vf->read(vf, bytes, size);
		if (count <= 0 || (size_t) count > size) {
			return false;
		}
		bytes += count;
		size -= count;
	}
	return true;
}

static uint32_t _read32(const uint8_t* bytes) {
	return bytes[0] | ((uint32_t) bytes[1] << 8) |
	       ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

bool GBAFXHasFooter(struct VFile* vf) {
	if (!vf) {
		return false;
	}
	ssize_t size = vf->size(vf);
	off_t position = vf->seek(vf, 0, SEEK_CUR);
	uint8_t marker[sizeof(_magic)];
	bool present = size >= (ssize_t) sizeof(marker) &&
	               vf->seek(vf, size - sizeof(marker), SEEK_SET) >= 0 &&
	               _readExact(vf, marker, sizeof(marker)) &&
	               !memcmp(marker, _magic, sizeof(marker));
	if (position >= 0) {
		vf->seek(vf, position, SEEK_SET);
	}
	return present;
}

enum GBAFXDetection GBAFXReadFooter(struct VFile* vf, struct GBAFXState* state) {
	memset(state, 0, sizeof(*state));
	if (!vf || vf->size(vf) < 0) {
		return GBA_FX_INVALID;
	}
	ssize_t size = vf->size(vf);
	if (size < (ssize_t) sizeof(_magic)) {
		return GBA_FX_NONE;
	}
	uint8_t marker[sizeof(_magic)];
	if (vf->seek(vf, size - sizeof(marker), SEEK_SET) < 0 ||
	    !_readExact(vf, marker, sizeof(marker))) {
		return GBA_FX_INVALID;
	}
	if (memcmp(marker, _magic, sizeof(marker))) {
		return GBA_FX_NONE;
	}
	uint8_t footer[GBA_FX_FOOTER_SIZE];
	if (size < GBA_FX_FOOTER_SIZE ||
	    vf->seek(vf, size - GBA_FX_FOOTER_SIZE, SEEK_SET) < 0 ||
	    !_readExact(vf, footer, sizeof(footer))) {
		return GBA_FX_INVALID;
	}
	uint32_t bootSize = _read32(footer + 0x10);
	if (_read32(footer) != GBA_FX_VERSION ||
	    _read32(footer + 4) != GBA_FX_FOOTER_SIZE ||
	    (_read32(footer + 8) & ~GBA_FX_SUPPORTED_FEATURES) ||
	    bootSize < 0xC0 || bootSize > GBA_SIZE_ROM0 ||
	    (uint64_t) bootSize + GBA_FX_FOOTER_SIZE != (uint64_t) size ||
	    (_read32(footer + 0x18) && !(_read32(footer + 8) & GBA_FX_FEATURE_RAM)) ||
	    _read32(footer + 0x18) > GBA_FX_RAM_MAX || _read32(footer + 0x1C) ||
	    _read32(footer + 0x20) || _read32(footer + 0x24) || _read32(footer + 0x28) ||
	    _read32(footer + 0x2C) != doCrc32(footer, 0x2C)) {
		return GBA_FX_INVALID;
	}
	if (vf->seek(vf, 0, SEEK_SET) < 0) {
		return GBA_FX_INVALID;
	}
	uint8_t block[4096];
	uint32_t remaining = bootSize;
	uint32_t checksum = 0;
	while (remaining) {
		size_t count = remaining < sizeof(block) ? remaining : sizeof(block);
		if (!_readExact(vf, block, count)) {
			return GBA_FX_INVALID;
		}
		checksum = crc32(checksum, block, count);
		remaining -= count;
	}
	if (checksum != _read32(footer + 0x14)) {
		return GBA_FX_INVALID;
	}
	state->bootSize = bootSize;
	state->requiredFeatures = _read32(footer + 8);
	state->minimumRAM = _read32(footer + 0x18);
	state->alu2x = (state->requiredFeatures & GBA_FX_FEATURE_ALU2X) != 0;
	state->active = true;
	return GBA_FX_VALID;
}

uint16_t GBAFXReadRegister(const struct GBAFXState* state, uint32_t address) {
	uint32_t value = 0;
	unsigned effective = 0;
	const struct GBAFXBudget* budget = &state->budget;
	switch (address & ~3U) {
	case 0x3C0:
		value = budget->limitBytes / 1048576;
		break;
	case 0x3C4:
		value = budget->host.availableBytes / 1048576;
		break;
	case 0x3C8:
		value = budget->reservedBytes / 1048576;
		break;
	case 0x3CC:
		value = budget->headroomBytes / 1048576;
		break;
	case 0x3D0:
		value = budget->valid && !budget->policy.memoryMiB ? budget->policy.memoryPercent : 0;
		break;
	case 0x3D4:
		value = budget->policy.memoryMiB;
		break;
	case 0x3D8:
		value = budget->policy.cpuPercent;
		break;
	case 0x3DC:
		value = mHostResourceCPUStatus(0);
		break;
	case 0x3E0:
		value = GBA_FX_IO_MAGIC;
		break;
	case 0x3E4:
		value = GBA_FX_VERSION;
		break;
	case 0x3E8:
		value = GBA_FX_FEATURE_DETECTION | (budget->valid ? GBA_FX_FEATURE_POLICY : 0) |
		        (state->alu2x ? GBA_FX_FEATURE_ALU2X : 0) | (state->ram ? GBA_FX_FEATURE_RAM : 0);
		break;
	case 0x3EC:
		value = state->ramSize / 1048576;
		break;
	case 0x3F0:
		value = budget->host.logicalCPUs;
		break;
	case 0x3F4:
		value = state->alu2x ? 2 : 1; /* Arithmetic profile, not a whole-machine clock. */
		break;
	case 0x3F8:
		mHostResourceCPUStatus(&effective);
		value = effective;
		break;
	default:
		value = 0;
		break;
	}
	return value >> ((address & 2) * 8);
}

uint32_t GBAFXRAMGrant(const struct GBAFXBudget* budget, uint32_t minimum) {
	if (!budget->valid || minimum > GBA_FX_RAM_MAX) {
		return 0;
	}
	uint32_t size = GBA_FX_RAM_MAX;
	while (size > budget->headroomBytes && size > 1048576) {
		size >>= 1;
	}
	return size <= budget->headroomBytes && size >= minimum ? size : 0;
}

void GBAFXRAMDeinit(struct GBAFXState* state) {
	if (state->ram) {
		mappedMemoryFree(state->ram, state->ramSize);
	}
	state->ram = 0;
	state->ramSize = 0;
}
