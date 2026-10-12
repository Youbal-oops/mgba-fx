/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/serialize.h>

#include <mgba/internal/arm/macros.h>
#include <mgba/internal/gba/bios.h>
#include <mgba/internal/gba/io.h>

#include <mgba-util/memory.h>
#include <mgba-util/crc32.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>

MGBA_EXPORT const uint32_t GBASavestateMagic = 0x01000000;
MGBA_EXPORT const uint32_t GBASavestateVersion = 0x0000000B;

mLOG_DEFINE_CATEGORY(GBA_STATE, "GBA Savestate", "gba.serialize");

struct GBABundledState {
	struct GBASerializedState* state;
	struct mStateExtdata* extdata;
};

size_t GBAFXStateSize(const struct GBA* gba) {
	return sizeof(struct GBASerializedState) + (gba->fx.active ? GBA_FX_STATE_HEADER_SIZE + gba->fx.ramSize : 0);
}

static uint32_t _fxCRC32(uint32_t checksum, const uint8_t* data, size_t size) {
	if (size < 4096) {
		return crc32(checksum, data, size);
	}
	// Local slicing tables avoid shared lazy-initialization races. This is
	// the same reflected IEEE polynomial and streaming convention as zlib.
	uint32_t table[8][256];
	unsigned i, slice;
	for (i = 0; i < 256; ++i) {
		uint32_t value = i;
		for (slice = 0; slice < 8; ++slice) {
			value = (value >> 1) ^ ((value & 1) ? 0xEDB88320U : 0);
		}
		table[0][i] = value;
	}
	for (slice = 1; slice < 8; ++slice) {
		for (i = 0; i < 256; ++i) {
			uint32_t value = table[slice - 1][i];
			table[slice][i] = (value >> 8) ^ table[0][value & 255];
		}
	}
	checksum = ~checksum;
	while (size >= 8) {
		uint32_t first = (uint32_t) data[0] | ((uint32_t) data[1] << 8) |
		                 ((uint32_t) data[2] << 16) | ((uint32_t) data[3] << 24);
		uint32_t second = (uint32_t) data[4] | ((uint32_t) data[5] << 8) |
		                  ((uint32_t) data[6] << 16) | ((uint32_t) data[7] << 24);
		first ^= checksum;
		checksum = table[7][first & 255] ^ table[6][(first >> 8) & 255] ^
		           table[5][(first >> 16) & 255] ^ table[4][first >> 24] ^
		           table[3][second & 255] ^ table[2][(second >> 8) & 255] ^
		           table[1][(second >> 16) & 255] ^ table[0][second >> 24];
		data += 8;
		size -= 8;
	}
	while (size--) {
		checksum = (checksum >> 8) ^ table[0][(checksum ^ *data++) & 255];
	}
	return ~checksum;
}

static uint32_t _fxStateCRC(const uint8_t* state, size_t size) {
	size_t offset = sizeof(struct GBASerializedState) + 0x34;
	uint32_t checksum = _fxCRC32(0, state, offset);
	return _fxCRC32(checksum, state + offset + 4, size - offset - 4);
}

bool GBAFXSerialize(struct GBA* gba, void* state) {
	if (!gba->fx.active) {
		GBASerialize(gba, state);
		return true;
	}
	// Every byte of extra RAM is copied below; only the prefix has padding.
	memset(state, 0, sizeof(struct GBASerializedState) + GBA_FX_STATE_HEADER_SIZE);
	GBASerialize(gba, state);
	STORE_32(GBA_FX_STATE_MAGIC, 0, state);
	uint8_t* header = (uint8_t*) state + sizeof(struct GBASerializedState);
	STORE_32(GBA_FX_STATE_MAGIC, 0, header);
	STORE_32(GBA_FX_STATE_HEADER_SIZE, 4, header);
	STORE_32(gba->fx.ramSize, 8, header);
	STORE_32(gba->fx.requiredFeatures, 0x0C, header);
	STORE_32(gba->fx.bootSize, 0x10, header);
	STORE_32(gba->romCrc32, 0x14, header);
	STORE_32(gba->fx.contentSize, 0x18, header);
	STORE_32(gba->fx.contentCRC, 0x1C, header);
	STORE_32(gba->fx.fileCRC, 0x20, header);
	STORE_32(gba->fx.alu2x ? 2 : 1, 0x24, header);
	STORE_32(gba->fx.aluPhase, 0x28, header);
	STORE_32(gba->fx.contentBank, 0x2C, header);
	STORE_32(gba->fx.invalidBank, 0x30, header);
	STORE_64LE(gba->fx.acceleratedInstructions, 0x38, header);
	if (gba->fx.ramSize) {
		memcpy(header + GBA_FX_STATE_HEADER_SIZE, gba->fx.ram, gba->fx.ramSize);
	}
	STORE_32(_fxStateCRC(state, GBAFXStateSize(gba)), 0x34, header);
	return true;
}

bool GBAFXDeserialize(struct GBA* gba, const void* state) {
	if (!gba->fx.active) {
		return GBADeserialize(gba, state);
	}
	uint32_t value;
	LOAD_32(value, 0, state);
	if (value != GBA_FX_STATE_MAGIC) {
		return false;
	}
	const uint8_t* header = (const uint8_t*) state + sizeof(struct GBASerializedState);
	const uint32_t expected[] = {
		GBA_FX_STATE_MAGIC, GBA_FX_STATE_HEADER_SIZE, gba->fx.ramSize,
		gba->fx.requiredFeatures, gba->fx.bootSize, gba->romCrc32,
		gba->fx.contentSize, gba->fx.contentCRC, gba->fx.fileCRC,
		gba->fx.alu2x ? 2U : 1U
	};
	size_t i;
	for (i = 0; i < sizeof(expected) / sizeof(*expected); ++i) {
		LOAD_32(value, i * 4, header);
		if (value != expected[i]) {
			return false;
		}
	}
	uint32_t phase;
	uint32_t bank;
	uint32_t invalid;
	LOAD_32(phase, 0x28, header);
	LOAD_32(bank, 0x2C, header);
	LOAD_32(invalid, 0x30, header);
	LOAD_32(value, 0x34, header);
	if (phase > (gba->fx.alu2x ? 1U : 0U) || invalid > 1 ||
	    (gba->fx.contentSize ? bank > (gba->fx.contentSize - 1) / GBA_FX_CONTENT_WINDOW : bank != 0) ||
	    value != _fxStateCRC(state, GBAFXStateSize(gba))) {
		return false;
	}
	struct GBASerializedState* base = malloc(sizeof(*base));
	if (!base) {
		return false;
	}
	memcpy(base, state, sizeof(*base));
	STORE_32(GBASavestateMagic + GBASavestateVersion, 0, &base->versionMagic);
	uint32_t pc;
	uint32_t cpsr;
	LOAD_32(pc, ARM_PC * 4, base->cpu.gprs);
	LOAD_32(cpsr, 0, &base->cpu.cpsr.packed);
	unsigned region = pc >> BASE_OFFSET;
	unsigned privilege = cpsr & 0x1F;
	bool validMode = privilege == 0x10 || privilege == 0x11 || privilege == 0x12 ||
	                 privilege == 0x13 || privilege == 0x17 || privilege == 0x1B || privilege == 0x1F;
	bool validPC = region == GBA_REGION_BIOS || region == GBA_REGION_EWRAM ||
	               region == GBA_REGION_IWRAM || region == GBA_REGION_PALETTE_RAM ||
	               region == GBA_REGION_VRAM || region == GBA_REGION_OAM ||
	               (region >= GBA_REGION_ROM0 && region <= GBA_REGION_ROM2_EX) ||
	               (region == (GBA_FX_RAM_BASE >> BASE_OFFSET) && gba->fx.ram);
	if (region >= GBA_REGION_ROM0 && region <= GBA_REGION_ROM2_EX) {
		validPC = gba->memory.rom && (pc & (GBA_SIZE_ROM0 - 1)) < gba->memory.romSize;
	}
	if (!validMode || !validPC || (pc & ((cpsr & 0x20) ? 1 : 3)) || !GBAValidateState(gba, base)) {
		free(base);
		return false;
	}
	// Validation finishes before changing RAM or rebuilding the CPU pipeline.
	if (gba->fx.ramSize) {
		memcpy(gba->fx.ram, header + GBA_FX_STATE_HEADER_SIZE, gba->fx.ramSize);
	}
	GBADeserialize(gba, base);
	free(base);
	gba->fx.aluPhase = phase;
	gba->fx.contentBank = bank;
	gba->fx.invalidBank = invalid;
	LOAD_64LE(gba->fx.acceleratedInstructions, 0x38, header);
	mCALLBACKS_INVOKE(gba, memoryBlocksChanged);
	return true;
}

void GBASerialize(struct GBA* gba, struct GBASerializedState* state) {
	STORE_32(GBASavestateMagic + GBASavestateVersion, 0, &state->versionMagic);
	STORE_32(gba->biosChecksum, 0, &state->biosChecksum);
	STORE_32(gba->romCrc32, 0, &state->romCrc32);
	STORE_32(gba->timing.masterCycles, 0, &state->masterCycles);
	STORE_64LE(gba->timing.globalCycles, 0, &state->globalCycles);

	if (gba->memory.rom) {
		switch (gba->memory.unl.type) {
		case GBA_UNL_CART_NONE:
		case GBA_UNL_CART_VFAME:
			state->id = ((struct GBACartridge*) gba->memory.rom)->id;
			memcpy(state->title, ((struct GBACartridge*) gba->memory.rom)->title, sizeof(state->title));
			break;
		case GBA_UNL_CART_MULTICART:
			state->id = ((struct GBACartridge*) gba->memory.unl.multi.rom)->id;
			memcpy(state->title, ((struct GBACartridge*) gba->memory.unl.multi.rom)->title, sizeof(state->title));
			break;
		}
	} else {
		state->id = 0;
		memset(state->title, 0, sizeof(state->title));
	}

	int i;
	for (i = 0; i < 16; ++i) {
		STORE_32(gba->cpu->gprs[i], i * sizeof(state->cpu.gprs[0]), state->cpu.gprs);
	}
	STORE_32(gba->cpu->cpsr.packed, 0, &state->cpu.cpsr.packed);
	STORE_32(gba->cpu->spsr.packed, 0, &state->cpu.spsr.packed);
	STORE_32(gba->cpu->cycles, 0, &state->cpu.cycles);
	STORE_32(gba->cpu->nextEvent, 0, &state->cpu.nextEvent);
	for (i = 0; i < 6; ++i) {
		int j;
		for (j = 0; j < 7; ++j) {
			STORE_32(gba->cpu->bankedRegisters[i][j], (i * 7 + j) * sizeof(gba->cpu->bankedRegisters[0][0]), state->cpu.bankedRegisters);
		}
		STORE_32(gba->cpu->bankedSPSRs[i], i * sizeof(gba->cpu->bankedSPSRs[0]), state->cpu.bankedSPSRs);
	}

	STORE_32(gba->memory.biosPrefetch, 0, &state->biosPrefetch);
	STORE_32(gba->cpu->prefetch[0], 0, state->cpuPrefetch);
	STORE_32(gba->cpu->prefetch[1], 4, state->cpuPrefetch);
	STORE_32(gba->memory.lastPrefetchedPc, 0, &state->lastPrefetchedPc);

	GBASerializedMiscFlags miscFlags = 0;
	miscFlags = GBASerializedMiscFlagsSetHalted(miscFlags, gba->cpu->halted);
	miscFlags = GBASerializedMiscFlagsSetPOSTFLG(miscFlags, gba->memory.io[GBA_REG(POSTFLG)] & 1);
	if (mTimingIsScheduled(&gba->timing, &gba->irqEvent)) {
		miscFlags = GBASerializedMiscFlagsFillIrqPending(miscFlags);
		STORE_32(gba->irqEvent.when - mTimingCurrentTime(&gba->timing), 0, &state->nextIrq);
	}
	miscFlags = GBASerializedMiscFlagsSetBlocked(miscFlags, gba->cpuBlocked);
	miscFlags = GBASerializedMiscFlagsSetKeyIRQKeys(miscFlags, gba->keysLast);
	STORE_32(miscFlags, 0, &state->miscFlags);
	STORE_32(gba->biosStall, 0, &state->biosStall);

	GBAMemorySerialize(&gba->memory, state);
	GBAIOSerialize(gba, state);
	GBAUnlCartSerialize(gba, state);
	GBAVideoSerialize(&gba->video, state);
	GBAAudioSerialize(&gba->audio, state);
	GBASavedataSerialize(&gba->memory.savedata, state);

	if (gba->memory.matrix.size) {
		GBAMatrixSerialize(gba, state);
	}
}

bool GBAValidateState(struct GBA* gba, const struct GBASerializedState* state) {
	bool error = false;
	int32_t check;
	uint32_t ucheck;
	LOAD_32(ucheck, 0, &state->versionMagic);
	if (ucheck > GBASavestateMagic + GBASavestateVersion) {
		mLOG(GBA_STATE, WARN, "Invalid or too new savestate: expected %08X, got %08X", GBASavestateMagic + GBASavestateVersion, ucheck);
		error = true;
	} else if (ucheck < GBASavestateMagic) {
		mLOG(GBA_STATE, WARN, "Invalid savestate: expected %08X, got %08X", GBASavestateMagic + GBASavestateVersion, ucheck);
		error = true;
	} else if (ucheck < GBASavestateMagic + GBASavestateVersion) {
		mLOG(GBA_STATE, WARN, "Old savestate: expected %08X, got %08X, continuing anyway", GBASavestateMagic + GBASavestateVersion, ucheck);
	}
	LOAD_32(ucheck, 0, &state->biosChecksum);
	if (ucheck != gba->biosChecksum) {
		mLOG(GBA_STATE, WARN, "Savestate created using a different version of the BIOS: expected %08X, got %08X", gba->biosChecksum, ucheck);
		uint32_t pc;
		LOAD_32(pc, ARM_PC * sizeof(state->cpu.gprs[0]), state->cpu.gprs);
		if ((ucheck == GBA_BIOS_CHECKSUM || gba->biosChecksum == GBA_BIOS_CHECKSUM) && pc < GBA_SIZE_BIOS && pc >= 0x20) {
			error = true;
		}
	}
	if (gba->memory.rom) {
		struct GBACartridge* cart;
		switch (gba->memory.unl.type) {
		case GBA_UNL_CART_NONE:
		case GBA_UNL_CART_VFAME:
		default:
			cart = (struct GBACartridge*) gba->memory.rom;
			break;
		case GBA_UNL_CART_MULTICART:
			cart = (struct GBACartridge*) gba->memory.unl.multi.rom;
			break;
		}
		if (state->id != cart->id || memcmp(state->title, cart->title, sizeof(state->title))) {
			mLOG(GBA_STATE, WARN, "Savestate is for a different game");
			error = true;
		}
	} else if (!gba->memory.rom && state->id != 0) {
		mLOG(GBA_STATE, WARN, "Savestate is for a game, but no game loaded");
		error = true;
	}
	LOAD_32(ucheck, 0, &state->romCrc32);
	if (ucheck != gba->romCrc32) {
		mLOG(GBA_STATE, WARN, "Savestate is for a different version of the game");
	}
	LOAD_32(check, 0, &state->cpu.cycles);
	if (check < 0) {
		mLOG(GBA_STATE, WARN, "Savestate is corrupted: CPU cycles are negative");
		error = true;
	}
	if (check >= (int32_t) GBA_ARM7TDMI_FREQUENCY) {
		mLOG(GBA_STATE, WARN, "Savestate is corrupted: CPU cycles are too high");
		error = true;
	}
	LOAD_32(check, ARM_PC * sizeof(state->cpu.gprs[0]), state->cpu.gprs);
	int region = (check >> BASE_OFFSET);
	if ((region == GBA_REGION_ROM0 || region == GBA_REGION_ROM1 || region == GBA_REGION_ROM2) && ((check - WORD_SIZE_ARM) & GBA_SIZE_ROM0) >= gba->memory.romSize - WORD_SIZE_ARM) {
		mLOG(GBA_STATE, WARN, "Savestate created using a differently sized version of the ROM");
		error = true;
	}
	if (error) {
		return false;
	}
	return true;
}

bool GBADeserialize(struct GBA* gba, const struct GBASerializedState* state) {
	if (!GBAValidateState(gba, state)) {
		return false;
	}
	mTimingClear(&gba->timing);
	LOAD_32(gba->timing.masterCycles, 0, &state->masterCycles);
	LOAD_64LE(gba->timing.globalCycles, 0, &state->globalCycles);

	size_t i;
	for (i = 0; i < 16; ++i) {
		LOAD_32(gba->cpu->gprs[i], i * sizeof(gba->cpu->gprs[0]), state->cpu.gprs);
	}
	LOAD_32(gba->cpu->cpsr.packed, 0, &state->cpu.cpsr.packed);
	LOAD_32(gba->cpu->spsr.packed, 0, &state->cpu.spsr.packed);
	LOAD_32(gba->cpu->cycles, 0, &state->cpu.cycles);
	LOAD_32(gba->cpu->nextEvent, 0, &state->cpu.nextEvent);
	for (i = 0; i < 6; ++i) {
		int j;
		for (j = 0; j < 7; ++j) {
			LOAD_32(gba->cpu->bankedRegisters[i][j], (i * 7 + j) * sizeof(gba->cpu->bankedRegisters[0][0]), state->cpu.bankedRegisters);
		}
		LOAD_32(gba->cpu->bankedSPSRs[i], i * sizeof(gba->cpu->bankedSPSRs[0]), state->cpu.bankedSPSRs);
	}
	gba->cpu->privilegeMode = gba->cpu->cpsr.priv;
	if (gba->cpu->gprs[ARM_PC] & 1) {
		mLOG(GBA_STATE, WARN, "Savestate has unaligned PC and is probably corrupted");
		gba->cpu->gprs[ARM_PC] &= ~1;
	}

	// Since this can remap the ROM, we need to do this before we reset the pipeline
	GBAUnlCartDeserialize(gba, state);
	gba->memory.activeRegion = -1;
	gba->cpu->memory.setActiveRegion(gba->cpu, gba->cpu->gprs[ARM_PC]);
	if (state->biosPrefetch) {
		LOAD_32(gba->memory.biosPrefetch, 0, &state->biosPrefetch);
	}
	LOAD_32(gba->memory.lastPrefetchedPc, 0, &state->lastPrefetchedPc);
	if (gba->cpu->cpsr.t) {
		gba->cpu->executionMode = MODE_THUMB;
		if (state->cpuPrefetch[0] && state->cpuPrefetch[1]) {
			LOAD_32(gba->cpu->prefetch[0], 0, state->cpuPrefetch);
			LOAD_32(gba->cpu->prefetch[1], 4, state->cpuPrefetch);
			gba->cpu->prefetch[0] &= 0xFFFF;
			gba->cpu->prefetch[1] &= 0xFFFF;
		} else {
			// Maintain backwards compat
			LOAD_16(gba->cpu->prefetch[0], (gba->cpu->gprs[ARM_PC] - WORD_SIZE_THUMB) & gba->cpu->memory.activeMask, gba->cpu->memory.activeRegion);
			LOAD_16(gba->cpu->prefetch[1], (gba->cpu->gprs[ARM_PC]) & gba->cpu->memory.activeMask, gba->cpu->memory.activeRegion);
		}
	} else {
		gba->cpu->executionMode = MODE_ARM;
		if (state->cpuPrefetch[0] && state->cpuPrefetch[1]) {
			LOAD_32(gba->cpu->prefetch[0], 0, state->cpuPrefetch);
			LOAD_32(gba->cpu->prefetch[1], 4, state->cpuPrefetch);
		} else {
			// Maintain backwards compat
			LOAD_32(gba->cpu->prefetch[0], (gba->cpu->gprs[ARM_PC] - WORD_SIZE_ARM) & gba->cpu->memory.activeMask, gba->cpu->memory.activeRegion);
			LOAD_32(gba->cpu->prefetch[1], (gba->cpu->gprs[ARM_PC]) & gba->cpu->memory.activeMask, gba->cpu->memory.activeRegion);
		}
	}
	GBASerializedMiscFlags miscFlags = 0;
	LOAD_32(miscFlags, 0, &state->miscFlags);
	gba->cpu->halted = GBASerializedMiscFlagsGetHalted(miscFlags);
	gba->memory.io[GBA_REG(POSTFLG)] = GBASerializedMiscFlagsGetPOSTFLG(miscFlags);
	if (GBASerializedMiscFlagsIsIrqPending(miscFlags)) {
		int32_t when;
		LOAD_32(when, 0, &state->nextIrq);
		mTimingSchedule(&gba->timing, &gba->irqEvent, when);
	}
	gba->cpuBlocked = GBASerializedMiscFlagsGetBlocked(miscFlags);
	gba->keysLast = GBASerializedMiscFlagsGetKeyIRQKeys(miscFlags);
	LOAD_32(gba->biosStall, 0, &state->biosStall);

	GBAVideoDeserialize(&gba->video, state);
	GBAMemoryDeserialize(&gba->memory, state);
	GBAIODeserialize(gba, state);
	GBAAudioDeserialize(&gba->audio, state);
	GBASavedataDeserialize(&gba->memory.savedata, state);

	if (gba->memory.matrix.size) {
		GBAMatrixDeserialize(gba, state);
	}

	mTimingInterrupt(&gba->timing);

	return true;
}
