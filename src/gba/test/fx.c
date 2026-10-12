/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba-util/crc32.h>
#include <mgba-util/patch.h>
#include <mgba-util/vfs.h>

#define BOOT_SIZE 4096

static void _write32(uint8_t* output, uint32_t value) {
	unsigned i;
	for (i = 0; i < 4; ++i) {
		output[i] = value >> (i * 8);
	}
}

static void _footerChecksum(uint8_t* rom) {
	_write32(rom + BOOT_SIZE + 0x2C, doCrc32(rom + BOOT_SIZE, 0x2C));
}

static uint8_t* _rom(void) {
	uint8_t* rom = calloc(1, BOOT_SIZE + GBA_FX_FOOTER_SIZE);
	assert_non_null(rom);
	_write32(rom, 0xEA00002E); /* Branch past the cartridge header. */
	rom[0xB2] = 0x96;
	_write32(rom + 0xC0, 0xEAFFFFFE); /* Original idle program. */
	uint8_t* footer = rom + BOOT_SIZE;
	_write32(footer, GBA_FX_VERSION);
	_write32(footer + 4, GBA_FX_FOOTER_SIZE);
	_write32(footer + 8, GBA_FX_FEATURE_DETECTION);
	_write32(footer + 0x10, BOOT_SIZE);
	_write32(footer + 0x14, doCrc32(rom, BOOT_SIZE));
	memcpy(footer + 0x30, "mGBA-FX-DRAFT01", 16);
	_footerChecksum(rom);
	return rom;
}

static enum GBAFXDetection _detect(uint8_t* rom, size_t size, struct GBAFXState* fx) {
	struct VFile* vf = VFileFromConstMemory(rom, size);
	assert_non_null(vf);
	enum GBAFXDetection result = GBAFXReadFooter(vf, fx);
	vf->close(vf);
	return result;
}

static struct mCore* _core(void) {
	struct mCore* core = GBACoreCreate();
	assert_non_null(core);
	assert_true(core->init(core));
	mCoreInitConfig(core, 0);
	return core;
}

static void _destroy(struct mCore* core) {
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
}

static bool _load(struct mCore* core, const uint8_t* rom, size_t size) {
	struct VFile* vf = VFileMemChunk(rom, size);
	assert_non_null(vf);
	bool loaded = core->loadROM(core, vf);
	if (!loaded) {
		vf->close(vf);
	}
	return loaded;
}

static ssize_t (*_originalRead)(struct VFile*, void*, size_t);

static ssize_t _shortRead(struct VFile* vf, void* output, size_t size) {
	return _originalRead(vf, output, size > 7 ? 7 : size);
}

static ssize_t _failedRead(struct VFile* vf, void* output, size_t size) {
	UNUSED(vf);
	UNUSED(output);
	UNUSED(size);
	return -1;
}

static ssize_t _failedSize(struct VFile* vf) {
	UNUSED(vf);
	return -1;
}

M_TEST_DEFINE(fileReadBehavior) {
	uint8_t* rom = _rom();
	struct VFile* vf = VFileFromConstMemory(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE);
	_originalRead = vf->read;
	vf->read = _shortRead;
	struct GBAFXState fx;
	assert_int_equal(GBAFXReadFooter(vf, &fx), GBA_FX_VALID);
	vf->read = _failedRead;
	assert_int_equal(GBAFXReadFooter(vf, &fx), GBA_FX_INVALID);
	assert_false(fx.active);
	vf->size = _failedSize;
	assert_int_equal(GBAFXReadFooter(vf, &fx), GBA_FX_INVALID);
	assert_false(fx.active);
	vf->close(vf);
	free(rom);
}

M_TEST_DEFINE(validFooter) {
	uint8_t* rom = _rom();
	struct GBAFXState fx;
	assert_int_equal(_detect(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE, &fx), GBA_FX_VALID);
	assert_true(fx.active);
	assert_int_equal(fx.bootSize, BOOT_SIZE);
	free(rom);
}

M_TEST_DEFINE(rejectInvalidFields) {
	/* Recompute descriptor CRC so each rejection tests the field itself. */
	const uint32_t fields[][2] = {
		{0, 2}, {4, 63}, {8, 2}, {0x10, 0xBF}, {0x10, BOOT_SIZE - 1},
		{0x10, GBA_SIZE_ROM0 + 1}, {0x18, 1}, {0x1C, 1},
		{0x20, 1}, {0x24, 1}, {0x28, 1}
	};
	size_t i;
	for (i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
		uint8_t* rom = _rom();
		_write32(rom + BOOT_SIZE + fields[i][0], fields[i][1]);
		_footerChecksum(rom);
		struct GBAFXState fx = { .active = true, .bootSize = 123 };
		assert_int_equal(_detect(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE, &fx), GBA_FX_INVALID);
		assert_false(fx.active);
		assert_int_equal(fx.bootSize, 0);
		free(rom);
	}
}

M_TEST_DEFINE(checksums) {
	uint8_t* rom = _rom();
	struct GBAFXState fx;
	rom[BOOT_SIZE + 0x2C] ^= 1;
	assert_int_equal(_detect(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE, &fx), GBA_FX_INVALID);
	_footerChecksum(rom);
	rom[0xC0] ^= 1;
	assert_int_equal(_detect(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE, &fx), GBA_FX_INVALID);
	free(rom);
}

M_TEST_DEFINE(optionalFeatures) {
	uint8_t* rom = _rom();
	_write32(rom + BOOT_SIZE + 0x0C, 0xFFFFFFFF);
	_footerChecksum(rom);
	struct GBAFXState fx;
	assert_int_equal(_detect(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE, &fx), GBA_FX_VALID);
	assert_int_equal(GBAFXReadRegister(GBA_FX_IO_BASE + 8), GBA_FX_FEATURE_DETECTION);
	free(rom);
}

M_TEST_DEFINE(markerBoundaries) {
	uint8_t* rom = _rom();
	struct GBAFXState fx;
	assert_int_equal(_detect(rom + BOOT_SIZE + 0x30, 16, &fx), GBA_FX_INVALID);
	assert_int_equal(_detect(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE - 1, &fx), GBA_FX_NONE);
	memcpy(rom + 0x200, "mGBA-FX-DRAFT01", 16);
	assert_int_equal(_detect(rom, BOOT_SIZE, &fx), GBA_FX_NONE);
	rom[BOOT_SIZE + 0x30] ^= 1;
	assert_int_equal(_detect(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE, &fx), GBA_FX_NONE);
	free(rom);
}

M_TEST_DEFINE(probePreservesPosition) {
	uint8_t* rom = _rom();
	struct VFile* vf = VFileFromConstMemory(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE);
	assert_int_equal(vf->seek(vf, 123, SEEK_SET), 123);
	assert_true(GBAFXHasFooter(vf));
	assert_int_equal(vf->seek(vf, 0, SEEK_CUR), 123);
	vf->close(vf);
	free(rom);
}

M_TEST_DEFINE(registersAndMapping) {
	uint8_t* rom = _rom();
	struct mCore* core = _core();
	assert_true(_load(core, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	struct GBA* gba = core->board;
	assert_true(gba->fx.active);
	assert_int_equal(gba->memory.romSize, BOOT_SIZE);
	assert_int_equal(gba->pristineRomSize, BOOT_SIZE);
	assert_int_equal(gba->romCrc32, doCrc32(rom, BOOT_SIZE));
	core->reset(core);
	assert_true(gba->fx.active);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003E0, 0), GBA_FX_IO_MAGIC);
	assert_int_equal(GBALoad16(gba->cpu, 0x040003E2, 0), 0x5846);
	assert_int_equal(GBALoad8(gba->cpu, 0x040003E1, 0), 0x42);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003E4, 0), 1);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003E8, 0), GBA_FX_FEATURE_DETECTION);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003EC, 0), 0);
	assert_int_equal(core->busRead32(core, 0x040003E0), GBA_FX_IO_MAGIC);
	assert_int_equal(core->busRead16(core, 0x040003E2), 0x5846);
	assert_int_equal(core->busRead8(core, 0x040003E1), 0x42);
	GBAStore32(gba->cpu, 0x040003E0, 0, 0);
	GBAStore16(gba->cpu, 0x040003E4, 0, 0);
	GBAStore8(gba->cpu, 0x040003E8, 0, 0);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003E0, 0), GBA_FX_IO_MAGIC);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003E4, 0), 1);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003E8, 0), GBA_FX_FEATURE_DETECTION);
	_destroy(core);
	free(rom);
}

M_TEST_DEFINE(lifecycleAndIsolation) {
	uint8_t* rom = _rom();
	struct mCore* enhanced = _core();
	struct mCore* ordinary = _core();
	struct GBA* gba = enhanced->board;
	struct GBA* normal = ordinary->board;
	assert_true(GBALoadROM(normal, VFileMemChunk(rom, BOOT_SIZE)));
	ordinary->reset(ordinary);
	uint16_t normalIO = GBAIORead(normal, GBA_FX_IO_BASE);
	assert_true(_load(enhanced, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	assert_false(normal->fx.active);
	assert_int_equal(GBAIORead(normal, GBA_FX_IO_BASE), normalIO);
	struct VFile* vf = VFileMemChunk(rom, BOOT_SIZE);
	assert_true(GBALoadROM(gba, vf));
	assert_false(gba->fx.active);
	assert_true(_load(enhanced, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	GBAUnloadROM(gba);
	assert_false(gba->fx.active);
	assert_true(_load(enhanced, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	rom[BOOT_SIZE] = 2;
	assert_false(_load(enhanced, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	assert_false(gba->fx.active);
	assert_null(gba->memory.rom);
	assert_true(GBALoadROM(gba, VFileMemChunk(rom, BOOT_SIZE)));
	assert_false(gba->fx.active);
	rom[BOOT_SIZE] = 1;
	assert_true(_load(enhanced, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	assert_false(enhanced->loadROM(enhanced, 0));
	assert_false(gba->fx.active);
	assert_true(_load(enhanced, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	assert_true(GBALoadMB(gba, VFileMemChunk(rom, BOOT_SIZE)));
	assert_false(gba->fx.active);
	_destroy(enhanced);
	_destroy(ordinary);
	free(rom);
}

static void* _failedMap(struct VFile* vf, size_t size, int flags) {
	UNUSED(vf);
	UNUSED(size);
	UNUSED(flags);
	return 0;
}

M_TEST_DEFINE(mappingFailure) {
	uint8_t* rom = _rom();
	struct mCore* core = _core();
	struct VFile* vf = VFileMemChunk(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE);
	vf->map = _failedMap;
	assert_false(core->loadROM(core, vf));
	assert_false(((struct GBA*) core->board)->fx.active);
	assert_null(((struct GBA*) core->board)->memory.rom);
	vf->close(vf);
	_destroy(core);
	free(rom);
}

M_TEST_DEFINE(nonPowerOfTwoBoot) {
	uint8_t* rom = _rom();
	uint32_t size = BOOT_SIZE - 3;
	memmove(rom + size, rom + BOOT_SIZE, GBA_FX_FOOTER_SIZE);
	_write32(rom + size + 0x10, size);
	_write32(rom + size + 0x14, doCrc32(rom, size));
	_write32(rom + size + 0x2C, doCrc32(rom + size, 0x2C));
	struct mCore* core = _core();
	assert_true(_load(core, rom, size + GBA_FX_FOOTER_SIZE));
	struct GBA* gba = core->board;
	assert_true(gba->fx.active);
	assert_int_equal(gba->fx.bootSize, size);
	assert_int_equal(gba->pristineRomSize, size);
	assert_int_equal(gba->romCrc32, doCrc32(rom, size));
	assert_int_equal(gba->memory.romSize, GBA_SIZE_ROM0);
	assert_memory_equal(gba->memory.rom, rom, size);
	_destroy(core);
	free(rom);
}

M_TEST_DEFINE(standardRomBoundary) {
	/* A 32 MiB boot plus footer would hit the ordinary loader's size clamp. */
	uint8_t* fixture = _rom();
	uint8_t* rom = calloc(1, GBA_SIZE_ROM0 + GBA_FX_FOOTER_SIZE);
	assert_non_null(rom);
	memcpy(rom, fixture, BOOT_SIZE);
	rom[GBA_SIZE_ROM0 - 1] = 0x5A;
	uint8_t* footer = rom + GBA_SIZE_ROM0;
	memcpy(footer, fixture + BOOT_SIZE, GBA_FX_FOOTER_SIZE);
	_write32(footer + 0x10, GBA_SIZE_ROM0);
	_write32(footer + 0x14, doCrc32(rom, GBA_SIZE_ROM0));
	_write32(footer + 0x2C, doCrc32(footer, 0x2C));
	struct mCore* core = _core();
	assert_true(_load(core, rom, GBA_SIZE_ROM0 + GBA_FX_FOOTER_SIZE));
	struct GBA* gba = core->board;
	assert_true(gba->fx.active);
	assert_int_equal(gba->fx.bootSize, GBA_SIZE_ROM0);
	assert_int_equal(gba->memory.romSize, GBA_SIZE_ROM0);
	assert_int_equal(gba->romCrc32, doCrc32(rom, GBA_SIZE_ROM0));
	assert_int_equal(((uint8_t*) gba->memory.rom)[GBA_SIZE_ROM0 - 1], 0x5A);
	_destroy(core);
	free(fixture);
	free(rom);
}

static size_t _unexpectedPatchSize(struct Patch* patch, size_t inputSize) {
	UNUSED(patch);
	UNUSED(inputSize);
	fail_msg("Enhanced ROM patching must be rejected before touching the patch");
	return 0;
}

M_TEST_DEFINE(enhancedPatchingRejected) {
	uint8_t* rom = _rom();
	struct mCore* core = _core();
	assert_true(_load(core, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	struct GBA* gba = core->board;
	uint32_t checksum = gba->romCrc32;
	struct Patch patch = { .outputSize = _unexpectedPatchSize };
	GBAApplyPatch(gba, &patch);
	struct VFile* vf = VFileFromConstMemory("PATCHEOF", 8);
	assert_false(core->loadPatch(core, vf));
	vf->close(vf);
	assert_true(gba->fx.active);
	assert_int_equal(gba->romCrc32, checksum);
	assert_memory_equal(gba->memory.rom, rom, BOOT_SIZE);
	_destroy(core);
	free(rom);
}

M_TEST_SUITE_DEFINE(GBAFX,
	cmocka_unit_test(fileReadBehavior),
	cmocka_unit_test(validFooter),
	cmocka_unit_test(rejectInvalidFields),
	cmocka_unit_test(checksums),
	cmocka_unit_test(optionalFeatures),
	cmocka_unit_test(markerBoundaries),
	cmocka_unit_test(probePreservesPosition),
	cmocka_unit_test(registersAndMapping),
	cmocka_unit_test(mappingFailure),
	cmocka_unit_test(nonPowerOfTwoBoot),
	cmocka_unit_test(standardRomBoundary),
	cmocka_unit_test(enhancedPatchingRejected),
	cmocka_unit_test(lifecycleAndIsolation))
