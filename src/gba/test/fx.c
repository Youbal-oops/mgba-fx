/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba/core/core.h>
#include <mgba/core/rewind.h>
#include <mgba/core/serialize.h>
#include <mgba/gba/core.h>
#ifdef M_CORE_GB
#include <mgba/gb/core.h>
#endif
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba-util/crc32.h>
#include <mgba-util/patch.h>
#include <mgba-util/vfs.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba-util/memory.h>

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
	_write32(rom + BOOT_SIZE + 8, GBA_FX_FEATURE_DETECTION | GBA_FX_FEATURE_POLICY);
	_footerChecksum(rom);
	assert_int_equal(_detect(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE, &fx), GBA_FX_VALID);
	assert_int_equal(fx.requiredFeatures, GBA_FX_FEATURE_DETECTION | GBA_FX_FEATURE_POLICY);
	free(rom);
}

M_TEST_DEFINE(rejectInvalidFields) {
	/* Recompute descriptor CRC so each rejection tests the field itself. */
	const uint32_t fields[][2] = {
		{0, 2}, {4, 63}, {8, 16}, {0x10, 0xBF}, {0x10, BOOT_SIZE - 1},
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
	assert_int_equal(GBAFXReadRegister(&fx, GBA_FX_IO_BASE + 8), GBA_FX_FEATURE_DETECTION);
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
	uint32_t expectedFeatures = GBA_FX_FEATURE_DETECTION | (gba->fx.budget.valid ? GBA_FX_FEATURE_POLICY : 0);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003E8, 0), expectedFeatures);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003EC, 0), 0);
	assert_int_equal(core->busRead32(core, 0x040003E0), GBA_FX_IO_MAGIC);
	assert_int_equal(core->busRead16(core, 0x040003E2), 0x5846);
	assert_int_equal(core->busRead8(core, 0x040003E1), 0x42);
	GBAStore32(gba->cpu, 0x040003E0, 0, 0);
	GBAStore16(gba->cpu, 0x040003E4, 0, 0);
	GBAStore8(gba->cpu, 0x040003E8, 0, 0);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003E0, 0), GBA_FX_IO_MAGIC);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003E4, 0), 1);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003E8, 0), expectedFeatures);
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

M_TEST_DEFINE(resourceBudget) {
	struct GBAFXPolicy policy;
	GBAFXPolicyDefaults(&policy);
	struct mHostResources host = {
		.availableBytes = 1000ULL * 1048576,
		.addressBytes = 2000ULL * 1048576,
		.committedBytes = 80ULL * 1048576,
		.logicalCPUs = 8
	};
	struct GBAFXBudget budget;
	assert_true(GBAFXCalculateBudget(&policy, &host, &budget));
	assert_int_equal(budget.limitBytes, 800ULL * 1048576);
	assert_int_equal(budget.reservedBytes, 112ULL * 1048576);
	assert_int_equal(budget.headroomBytes, 688ULL * 1048576);
	assert_int_equal(budget.policy.cpuPercent, 80);
	policy.memoryMiB = 256;
	assert_true(GBAFXCalculateBudget(&policy, &host, &budget));
	assert_int_equal(budget.limitBytes, 256ULL * 1048576);
	policy.memoryMiB = 1500;
	assert_true(GBAFXCalculateBudget(&policy, &host, &budget));
	assert_int_equal(budget.limitBytes, host.availableBytes);
	host.addressBytes = 300ULL * 1048576;
	assert_true(GBAFXCalculateBudget(&policy, &host, &budget));
	assert_int_equal(budget.limitBytes, host.addressBytes);
	host.availableBytes = UINT64_MAX;
	host.addressBytes = UINT64_MAX;
	policy.memoryMiB = 0;
	assert_true(GBAFXCalculateBudget(&policy, &host, &budget));
	assert_true(budget.limitBytes > UINT64_MAX / 2);
	assert_true(budget.limitBytes < UINT64_MAX);
	host.committedBytes = UINT64_MAX;
	assert_false(GBAFXCalculateBudget(&policy, &host, &budget));
	assert_false(budget.valid);
}

M_TEST_DEFINE(resourcePolicyValidation) {
	struct mCoreConfig config;
	mCoreConfigInit(&config, 0);
	struct GBAFXPolicy policy;
	struct GBAFXBudget budget;
	struct mHostResources host = {
		.availableBytes = 1000ULL * 1048576,
		.addressBytes = 1000ULL * 1048576,
		.logicalCPUs = 4
	};
	GBAFXPolicyLoad(&policy, &config);
	assert_true(GBAFXCalculateBudget(&policy, &host, &budget));
	mCoreConfigSetValue(&config, "gba.fxCPUPercent", "invalid");
	GBAFXPolicyLoad(&policy, &config);
	assert_false(GBAFXCalculateBudget(&policy, &host, &budget));
	mCoreConfigSetValue(&config, "gba.fxCPUPercent", "4294967376");
	GBAFXPolicyLoad(&policy, &config);
	assert_false(GBAFXCalculateBudget(&policy, &host, &budget));
	mCoreConfigSetIntValue(&config, "gba.fxCPUPercent", 101);
	GBAFXPolicyLoad(&policy, &config);
	assert_false(GBAFXCalculateBudget(&policy, &host, &budget));
	mCoreConfigSetIntValue(&config, "gba.fxCPUPercent", 25);
	mCoreConfigSetIntValue(&config, "gba.fxMemoryPercent", 0);
	GBAFXPolicyLoad(&policy, &config);
	assert_false(GBAFXCalculateBudget(&policy, &host, &budget));
	mCoreConfigSetIntValue(&config, "gba.fxMemoryPercent", 50);
	mCoreConfigSetIntValue(&config, "gba.fxMemoryMiB", -1);
	GBAFXPolicyLoad(&policy, &config);
	assert_false(GBAFXCalculateBudget(&policy, &host, &budget));
	mCoreConfigSetIntValue(&config, "gba.fxMemoryMiB", 1);
	GBAFXPolicyLoad(&policy, &config);
	assert_false(GBAFXCalculateBudget(&policy, &host, &budget));
	mCoreConfigDeinit(&config);
}

M_TEST_DEFINE(resourceLifecycle) {
	struct mHostResources host;
	if (!mHostResourcesQuery(&host)) {
		return;
	}
	uint8_t* rom = _rom();
	struct mCore* core = _core();
	struct GBA* gba = core->board;
	mCoreConfigSetIntValue(&core->config, "gba.fxMemoryMiB", 256);
	mCoreConfigSetIntValue(&core->config, "gba.fxCPUPercent", 25);
	core->loadConfig(core, &core->config);
	assert_true(_load(core, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	assert_int_equal(core->busRead32(core, 0x040003C0), 256);
	assert_int_equal(core->busRead32(core, 0x040003D8), 25);
	assert_int_equal(core->busRead32(core, 0x040003EC), 0);
	assert_int_equal(GBALoad32(gba->cpu, 0x040003C0, 0), 256);
	assert_int_equal(GBALoad16(gba->cpu, 0x040003C0, 0), 256);
	assert_int_equal(GBALoad8(gba->cpu, 0x040003C1, 0), 1);
	GBAStore32(gba->cpu, 0x040003C0, 0, 0);
	assert_int_equal(core->busRead32(core, 0x040003C0), 256);
	gba->haltPending = true;
	GBAIORead(gba, 0x3DC);
	assert_false(gba->haltPending);
	mCoreConfigSetIntValue(&core->config, "gba.fxCPUPercent", 60);
	core->loadConfig(core, &core->config);
	core->reset(core);
	assert_int_equal(core->busRead32(core, 0x040003D8), 25);
	struct mCore* ordinary = _core();
	assert_true(_load(ordinary, rom, BOOT_SIZE));
	assert_int_equal(mHostResourceCPUStatus(0), M_HOST_CPU_SHARED);
	_destroy(ordinary);
#ifdef M_CORE_GB
	struct mCore* gb = GBCoreCreate();
	assert_non_null(gb);
	assert_true(gb->init(gb));
	mCoreInitConfig(gb, 0);
	assert_int_equal(mHostResourceCPUStatus(0), M_HOST_CPU_SHARED);
	_destroy(gb);
#endif
	assert_true(_load(core, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	assert_int_equal(core->busRead32(core, 0x040003D8), 60);
	mCoreConfigSetIntValue(&core->config, "gba.fxMemoryMiB", 1);
	core->loadConfig(core, &core->config);
	assert_false(_load(core, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	assert_false(gba->fx.active);
	assert_int_equal(mHostResourceCPUStatus(0), M_HOST_CPU_NONE);
	assert_true(_load(core, rom, BOOT_SIZE));
	assert_false(gba->fx.active);
	_destroy(core);
	free(rom);
}

M_TEST_DEFINE(experimentalFeatureGate) {
	uint8_t* rom = _rom();
	_write32(rom + BOOT_SIZE + 8, GBA_FX_FEATURE_DETECTION | GBA_FX_FEATURE_ALU2X);
	_footerChecksum(rom);
	struct GBAFXState fx;
#ifdef GBA_FX_TIMING_EXPERIMENT
	assert_int_equal(_detect(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE, &fx), GBA_FX_VALID);
	assert_true(fx.alu2x);
#else
	assert_int_equal(_detect(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE, &fx), GBA_FX_INVALID);
	assert_false(fx.active);
#endif
	_write32(rom + BOOT_SIZE + 8, GBA_FX_FEATURE_DETECTION);
	_write32(rom + BOOT_SIZE + 0xC, GBA_FX_FEATURE_ALU2X);
	_footerChecksum(rom);
	assert_int_equal(_detect(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE, &fx), GBA_FX_VALID);
	assert_false(fx.alu2x);
	free(rom);
}

#ifdef GBA_FX_TIMING_EXPERIMENT
struct TimingTrace {
	struct mAVStream stream;
	unsigned samples;
	uint32_t audioHash;
};

static void _audioTrace(struct mAVStream* stream, int16_t left, int16_t right) {
	struct TimingTrace* trace = (struct TimingTrace*) stream;
	trace->audioHash = trace->audioHash * 33 + (uint16_t) left;
	trace->audioHash = trace->audioHash * 33 + (uint16_t) right;
	++trace->samples;
}

static struct mCore* _timingCore(bool fast, bool thumb) {
	struct mCore* core = _core();
	core->opts.skipBios = true;
	core->opts.useBios = false;
	uint8_t* rom = _rom();
	_write32(rom + BOOT_SIZE + 8, GBA_FX_FEATURE_DETECTION | (fast ? GBA_FX_FEATURE_ALU2X : 0));
	_footerChecksum(rom);
	assert_true(_load(core, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	free(rom);
	core->reset(core);
	struct GBA* gba = core->board;
	struct ARMCore* cpu = core->cpu;
	unsigned i;
	for (i = 0; i < 64; ++i) {
		if (thumb) {
			core->busWrite16(core, GBA_BASE_IWRAM + i * 2, 0x3001);
		} else {
			core->busWrite32(core, GBA_BASE_IWRAM + i * 4, 0xE2800001);
		}
	}
	if (thumb) {
		core->busWrite16(core, GBA_BASE_IWRAM + 128, 0xE7BE);
		_ARMSetMode(cpu, MODE_THUMB);
	} else {
		core->busWrite32(core, GBA_BASE_IWRAM + 256, 0xEAFFFFBE);
		_ARMSetMode(cpu, MODE_ARM);
	}
	cpu->gprs[0] = 0;
	cpu->gprs[ARM_PC] = GBA_BASE_IWRAM;
	if (thumb) {
		ThumbWritePC(cpu);
	} else {
		ARMWritePC(cpu);
	}
	while (cpu->cycles >= cpu->nextEvent) {
		cpu->irqh.processEvents(cpu);
	}
	assert_int_equal(gba->fx.aluPhase, 0);
	return core;
}

M_TEST_DEFINE(fractionalRetirement) {
	struct mCore* core = _timingCore(true, false);
	struct GBA* gba = core->board;
	uint32_t before = mTimingCurrentTime(&gba->timing);
	core->step(core);
	assert_int_equal(((struct ARMCore*) core->cpu)->gprs[0], 1);
	assert_int_equal(gba->fx.aluPhase, 1);
	assert_int_equal((uint32_t) mTimingCurrentTime(&gba->timing), before);
	core->step(core);
	assert_int_equal(((struct ARMCore*) core->cpu)->gprs[0], 2);
	assert_int_equal(gba->fx.aluPhase, 0);
	assert_int_equal((uint32_t) mTimingCurrentTime(&gba->timing), before + 1);
	unsigned i;
	for (i = 0; i < 62; ++i) {
		core->step(core);
	}
	assert_int_equal(gba->fx.acceleratedInstructions, 64);
	assert_int_equal((uint32_t) mTimingCurrentTime(&gba->timing), before + 32);
	core->step(core); /* Branch and its pipeline refill retain their full cost. */
	assert_int_equal((uint32_t) mTimingCurrentTime(&gba->timing), before + 35);
	core->reset(core);
	assert_int_equal(gba->fx.aluPhase, 0);
	assert_int_equal(gba->fx.acceleratedInstructions, 0);
	assert_true(gba->fx.alu2x);
	_destroy(core);
}

static void _timingComparison(bool thumb) {
	struct mCore* normal = _timingCore(false, thumb);
	struct mCore* fast = _timingCore(true, thumb);
	struct TimingTrace slowTrace = { .stream.postAudioFrame = _audioTrace };
	struct TimingTrace fastTrace = { .stream.postAudioFrame = _audioTrace };
	normal->setAVStream(normal, &slowTrace.stream);
	fast->setAVStream(fast, &fastTrace.stream);
	struct mCore* cores[] = {normal, fast};
	unsigned i, frame;
	for (i = 0; i < 2; ++i) {
		struct mCore* core = cores[i];
		core->busWrite16(core, 0x04000100, 0);
		core->busWrite16(core, 0x04000102, 0x81); /* Timer 0, board clock / 64. */
		core->busWrite16(core, 0x04000084, 0x80);
		core->busWrite16(core, 0x04000080, 0x1177);
		core->busWrite16(core, 0x04000082, 2);
		core->busWrite16(core, 0x04000062, 0xF080);
		core->busWrite16(core, 0x04000064, 0x8600);
		for (frame = 0; frame < 12; ++frame) {
			core->runFrame(core);
		}
	}
	struct GBA* slowGBA = normal->board;
	struct GBA* fastGBA = fast->board;
	uint32_t slowWork = ((struct ARMCore*) normal->cpu)->gprs[0];
	uint32_t fastWork = ((struct ARMCore*) fast->cpu)->gprs[0];
	assert_true(fastWork > slowWork * 18ULL / 10);
	assert_true(fastWork < slowWork * 20ULL / 10);
	assert_int_equal(slowGBA->video.frameCounter, fastGBA->video.frameCounter);
	assert_int_equal(slowTrace.samples, fastTrace.samples);
	assert_true(slowTrace.samples >= 4096);
	assert_true(slowTrace.audioHash != 0);
	assert_int_equal(slowTrace.audioHash, fastTrace.audioHash);
	int64_t difference = (int32_t) ((uint32_t) mTimingCurrentTime(&fastGBA->timing) -
	                               (uint32_t) mTimingCurrentTime(&slowGBA->timing));
	assert_true(difference >= -3 && difference <= 3);
	int timerDifference = (int) GBALoad16(fast->cpu, 0x04000100, 0) - GBALoad16(normal->cpu, 0x04000100, 0);
	assert_true(timerDifference >= -1 && timerDifference <= 1);
	printf("%s: work_1x=%u work_2x=%u board_delta=%" PRId64 " samples=%u audio_hash=%08" PRIX32 "\n",
	       thumb ? "Thumb" : "ARM", slowWork, fastWork, difference, slowTrace.samples, slowTrace.audioHash);
	_destroy(fast);
	_destroy(normal);
}

M_TEST_DEFINE(armComputeAndPeripherals) {
	_timingComparison(false);
}

M_TEST_DEFINE(thumbComputeAndPeripherals) {
	_timingComparison(true);
}

M_TEST_DEFINE(haltAndInterruptTiming) {
	uint32_t woke[2];
	unsigned i;
	for (i = 0; i < 2; ++i) {
		struct mCore* core = _timingCore(i != 0, false);
		struct GBA* gba = core->board;
		struct ARMCore* cpu = core->cpu;
		cpu->cpsr.i = 1; /* Wake HALT without entering the BIOS IRQ handler. */
		core->busWrite16(core, 0x04000200, 1 << GBA_IRQ_TIMER0);
		core->busWrite16(core, 0x04000208, 1);
		core->busWrite16(core, 0x04000100, 0xFFF0);
		core->busWrite16(core, 0x04000102, 0xC0);
		gba->fx.aluPhase = i;
		GBAHalt(gba);
		core->runLoop(core);
		assert_false(cpu->halted);
		assert_true(gba->memory.io[GBA_REG(IF)] & (1 << GBA_IRQ_TIMER0));
		assert_int_equal(gba->fx.aluPhase, 0);
		assert_int_equal(cpu->gprs[0], 0);
		woke[i] = mTimingCurrentTime(&gba->timing);
		_destroy(core);
	}
	assert_int_equal(woke[0], woke[1]);
}

M_TEST_DEFINE(dmaTimingAndData) {
	uint32_t finished[2];
	unsigned mode, i;
	for (mode = 0; mode < 2; ++mode) {
		struct mCore* core = _timingCore(mode != 0, false);
		struct GBA* gba = core->board;
		for (i = 0; i < 16; ++i) {
			core->busWrite32(core, 0x02001000 + i * 4, 0x12345678 ^ i);
		}
		gba->fx.aluPhase = mode;
		core->busWrite32(core, 0x040000B0, 0x02001000);
		core->busWrite32(core, 0x040000B4, 0x02002000);
		core->busWrite32(core, 0x040000B8, 0x84000010);
		for (i = 0; i < 1000 && (gba->memory.io[GBA_REG(DMA0CNT_HI)] & 0x8000); ++i) {
			core->step(core);
		}
		assert_true(i < 1000);
		finished[mode] = gba->timing.masterCycles;
		for (i = 0; i < 16; ++i) {
			assert_int_equal(core->busRead32(core, 0x02002000 + i * 4), 0x12345678 ^ i);
		}
		_destroy(core);
	}
	assert_int_equal(finished[0], finished[1]);
}

M_TEST_DEFINE(experimentalStateRejected) {
	struct mCore* core = _timingCore(true, false);
	size_t size = core->stateSize(core);
	uint8_t* stateBuffer = malloc(size);
	assert_non_null(stateBuffer);
	memset(stateBuffer, 0xA5, size);
	assert_false(core->saveState(core, stateBuffer));
	assert_int_equal(stateBuffer[0], 0xA5);
	assert_false(core->loadState(core, stateBuffer));
	assert_true(((struct GBA*) core->board)->fx.alu2x);
	free(stateBuffer);
	_destroy(core);
}

M_TEST_DEFINE(legacyInstructionCosts) {
	uint32_t elapsed[2];
	unsigned mode;
	for (mode = 0; mode < 2; ++mode) {
		struct mCore* core = _timingCore(mode != 0, false);
		struct GBA* gba = core->board;
		struct ARMCore* cpu = core->cpu;
		core->busWrite32(core, GBA_BASE_IWRAM, 0xE5830000); /* STR r0, [r3] */
		core->busWrite32(core, GBA_BASE_IWRAM + 4, 0xE5931000); /* LDR r1, [r3] */
		core->busWrite32(core, GBA_BASE_IWRAM + 8, 0xE1A01210); /* MOV r1, r0, LSL r2 */
		cpu->gprs[0] = 42;
		cpu->gprs[2] = 1;
		cpu->gprs[3] = 0x02001000;
		cpu->gprs[ARM_PC] = GBA_BASE_IWRAM;
		ARMWritePC(cpu);
		uint32_t before = mTimingCurrentTime(&gba->timing);
		core->step(core);
		core->step(core);
		core->step(core);
		elapsed[mode] = (uint32_t) mTimingCurrentTime(&gba->timing) - before;
		assert_int_equal(cpu->gprs[1], 84);
		assert_int_equal(core->busRead32(core, 0x02001000), 42);
		assert_int_equal(gba->fx.acceleratedInstructions, 0);
		_destroy(core);
	}
	assert_int_equal(elapsed[0], elapsed[1]);
}

struct TimingProbe {
	unsigned* count;
	unsigned* order;
	unsigned id;
	uint32_t when;
};

static void _timingProbe(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct TimingProbe* probe = context;
	probe->order[(*probe->count)++] = probe->id;
	probe->when = (uint32_t) mTimingCurrentTime(timing) - cyclesLate;
}

M_TEST_DEFINE(wrappingAndEventOrder) {
	unsigned mode;
	for (mode = 0; mode < 2; ++mode) {
		struct mCore* core = _timingCore(mode != 0, false);
		struct GBA* gba = core->board;
		uint32_t offset = UINT32_MAX - 15;
		struct mTimingEvent* event;
		for (event = gba->timing.root; event; event = event->next) {
			event->when += offset;
		}
		for (event = gba->timing.reroot; event; event = event->next) {
			event->when += offset;
		}
		gba->timing.masterCycles += offset;
		unsigned count = 0;
		unsigned order[2] = {0};
		struct TimingProbe probes[] = {{&count, order, 1, 0}, {&count, order, 2, 0}};
		struct mTimingEvent events[] = {
			{ .context = &probes[1], .callback = _timingProbe, .name = "late priority", .priority = 0x30 },
			{ .context = &probes[0], .callback = _timingProbe, .name = "early priority", .priority = 0x10 }
		};
		mTimingSchedule(&gba->timing, &events[0], 20);
		mTimingSchedule(&gba->timing, &events[1], 20);
		core->runLoop(core);
		assert_int_equal(count, 2);
		assert_int_equal(order[0], 1);
		assert_int_equal(order[1], 2);
		assert_int_equal(probes[0].when, offset + 20);
		assert_int_equal(probes[1].when, offset + 20);
		assert_int_equal(((struct ARMCore*) core->cpu)->gprs[0], mode ? 40 : 20);
		_destroy(core);
	}
}
#endif

M_TEST_DEFINE(ramGrant) {
	struct GBAFXBudget budget = { .valid = true };
	unsigned i;
	for (i = 0; i < 5; ++i) {
		uint32_t size = 1048576U << i;
		budget.headroomBytes = size + 1024;
		assert_int_equal(GBAFXRAMGrant(&budget, size), size);
		assert_int_equal(GBAFXRAMGrant(&budget, size + 1), 0);
	}
	budget.headroomBytes = 1048575;
	assert_int_equal(GBAFXRAMGrant(&budget, 0), 0);
	budget.headroomBytes = UINT64_MAX;
	assert_int_equal(GBAFXRAMGrant(&budget, 0), GBA_FX_RAM_MAX);
	assert_int_equal(GBAFXRAMGrant(&budget, GBA_FX_RAM_MAX + 1), 0);
	budget.valid = false;
	assert_int_equal(GBAFXRAMGrant(&budget, 0), 0);
}

static struct mCore* _ramCore(uint32_t size) {
	struct mCore* core = _core();
	uint8_t* rom = _rom();
	assert_true(_load(core, rom, BOOT_SIZE));
	free(rom);
	core->opts.skipBios = true;
	core->reset(core);
	struct GBA* gba = core->board;
	gba->fx.active = true;
	gba->fx.requiredFeatures = GBA_FX_FEATURE_RAM;
	gba->fx.ramSize = size;
	gba->fx.ram = anonymousMemoryMap(size);
	assert_non_null(gba->fx.ram);
	return core;
}

M_TEST_DEFINE(ramAccess) {
	unsigned sizeIndex;
	for (sizeIndex = 0; sizeIndex < 3; ++sizeIndex) {
		uint32_t size = 1048576U << (sizeIndex * 2);
		struct mCore* core = _ramCore(size);
		struct GBA* gba = core->board;
		uint32_t base = GBA_FX_RAM_BASE;
		uint32_t end = base + size;
		assert_int_equal(core->busRead32(core, end - 4), 0);
		core->busWrite32(core, end - 1, 0x12345678);
		assert_int_equal(core->busRead32(core, end - 4), 0x12345678);
		assert_int_equal(core->busRead32(core, end - 3), 0x78123456);
		core->busWrite16(core, base + 1, 0xABCD);
		assert_int_equal(core->busRead16(core, base), 0xABCD);
		assert_int_equal(core->busRead16(core, base + 1), 0xCD0000AB);
		core->busWrite8(core, base + 2, 0xEF);
		assert_int_equal(core->busRead8(core, base + 2), 0xEF);
		core->rawWrite32(core, base + 0x40000, -1, 0xCAFEBABE);
		assert_int_equal(core->rawRead32(core, base + 0x40000, -1), 0xCAFEBABE);
		core->rawWrite16(core, base + 0x40004, -1, 0xCAFE);
		core->rawWrite8(core, base + 0x40006, -1, 0xBA);
		assert_int_equal(core->rawRead16(core, base + 0x40004, -1), 0xCAFE);
		assert_int_equal(core->rawRead8(core, base + 0x40006, -1), 0xBA);
		if (size < GBA_FX_RAM_MAX) {
			assert_int_equal(core->busRead32(core, end + 0x40000), 0xCAFEBABE);
		}
		size_t blockSize = 0;
		assert_ptr_equal(core->getMemoryBlock(core, GBA_REGION_FX_RAM, &blockSize), gba->fx.ram);
		assert_int_equal(blockSize, size);
		const struct mCoreMemoryBlock* blocks;
		size_t count = core->listMemoryBlocks(core, &blocks);
		assert_int_equal(blocks[count - 1].id, GBA_REGION_FX_RAM);
		assert_int_equal(blocks[count - 1].size, size);
		assert_int_equal(blocks[count - 1].end, end);
		assert_int_equal(core->busRead32(core, 0x040003EC), size / 1048576);
		assert_true(core->busRead32(core, 0x040003E8) & GBA_FX_FEATURE_RAM);
		core->reset(core);
		assert_int_equal(core->busRead32(core, base + 0x40000), 0);
		assert_int_equal(gba->fx.ramSize, size);
		_destroy(core);
	}
}

M_TEST_DEFINE(ramExecutionAndTransfers) {
	struct mCore* core = _ramCore(1048576);
	struct ARMCore* cpu = core->cpu;
	uint32_t base = GBA_FX_RAM_BASE;
	core->busWrite32(core, base, 0xE2800001); /* ADD r0, r0, #1 */
	core->busWrite32(core, base + 4, 0xE12FFF11); /* BX r1 */
	core->busWrite16(core, base + 0x100, 0x3002); /* ADD r0, #2 */
	core->busWrite16(core, base + 0x102, 0x4710); /* BX r2 */
	core->busWrite32(core, GBA_BASE_IWRAM, 0xE2800004);
	cpu->gprs[0] = 0;
	cpu->gprs[1] = base + 0x101;
	cpu->gprs[2] = GBA_BASE_IWRAM;
	cpu->gprs[ARM_PC] = base;
	ARMWritePC(cpu);
	core->step(core);
	core->step(core);
	assert_int_equal(cpu->executionMode, MODE_THUMB);
	core->step(core);
	core->step(core);
	assert_int_equal(cpu->executionMode, MODE_ARM);
	core->step(core);
	assert_int_equal(cpu->gprs[0], 7);
	core->busWrite32(core, base + 1048572, 0xE2800008);
	cpu->gprs[ARM_PC] = base + 1048572;
	ARMWritePC(cpu);
	core->step(core);
	core->step(core);
	assert_int_equal(cpu->gprs[0], 16);
	/* Block transfers wrap at the small grant's mirrored boundary. */
	cpu->gprs[0] = 0x11223344;
	cpu->gprs[1] = 0x55667788;
	int cycles = 0;
	assert_int_equal(GBAStoreMultiple(cpu, base + 1048572, 3, LSM_IA, &cycles), base + 1048580);
	assert_int_equal(core->busRead32(core, base + 1048572), 0x11223344);
	assert_int_equal(core->busRead32(core, base), 0x55667788);
	cpu->gprs[0] = cpu->gprs[1] = 0;
	GBALoadMultiple(cpu, base + 1048572, 3, LSM_IA, &cycles);
	assert_int_equal(cpu->gprs[0], 0x11223344);
	assert_int_equal(cpu->gprs[1], 0x55667788);
	/* The bundled HLE BIOS CpuSet stages FX RAM into ordinary EWRAM. */
	core->busWrite32(core, GBA_BASE_IWRAM, 0xEF0B0000);
	core->busWrite32(core, GBA_BASE_IWRAM + 4, 0xEAFFFFFE);
	cpu->gprs[0] = base;
	cpu->gprs[1] = GBA_BASE_EWRAM;
	cpu->gprs[2] = 0x04000001;
	cpu->gprs[ARM_PC] = GBA_BASE_IWRAM;
	ARMWritePC(cpu);
	core->runFrame(core);
	assert_int_equal(core->busRead32(core, GBA_BASE_EWRAM), 0x55667788);
	/* The original DMA controller masks out the new address bit. */
	assert_int_equal(GBADMAWriteSAD(core->board, 3, base + 0x40000), 0x40000);
	assert_int_equal(GBADMAWriteDAD(core->board, 3, base + 0x40000), 0x40000);
	_destroy(core);
}

M_TEST_DEFINE(ramLifecycle) {
	uint8_t* rom = _rom();
	_write32(rom + BOOT_SIZE + 8, GBA_FX_FEATURE_RAM | GBA_FX_FEATURE_DETECTION);
	_write32(rom + BOOT_SIZE + 0x18, 1048576);
	_footerChecksum(rom);
	struct GBAFXState fx;
	assert_int_equal(_detect(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE, &fx), GBA_FX_VALID);
	assert_int_equal(fx.minimumRAM, 1048576);
	struct mCore* core = _core();
	struct GBA* gba = core->board;
#ifdef _WIN32
	assert_true(_load(core, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	assert_non_null(gba->fx.ram);
	assert_true(gba->fx.ramSize >= 1048576);
	assert_int_equal(gba->fx.budget.reservedBytes + gba->fx.budget.headroomBytes, gba->fx.budget.limitBytes);
	struct VFile* vf = VFileMemChunk(rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE);
	vf->map = _failedMap;
	assert_false(core->loadROM(core, vf));
	vf->close(vf);
	assert_null(gba->fx.ram);
	assert_false(gba->fx.active);
	assert_true(_load(core, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
#else
	assert_false(_load(core, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
#endif
	assert_true(_load(core, rom, BOOT_SIZE));
	assert_null(gba->fx.ram);
	assert_int_equal(gba->fx.ramSize, 0);
	assert_int_equal(core->rawRead32(core, GBA_FX_RAM_BASE, -1), 0);
	const struct mCoreMemoryBlock* blocks;
	size_t count = core->listMemoryBlocks(core, &blocks);
	assert_true(blocks[count - 1].id != GBA_REGION_FX_RAM);
	mCoreConfigSetIntValue(&core->config, "gba.fxMemoryMiB", 100);
	core->loadConfig(core, &core->config);
	_write32(rom + BOOT_SIZE + 0x18, GBA_FX_RAM_MAX);
	_footerChecksum(rom);
	assert_false(_load(core, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	assert_null(gba->fx.ram);
	_write32(rom + BOOT_SIZE + 0x18, GBA_FX_RAM_MAX + 1);
	_footerChecksum(rom);
	assert_false(_load(core, rom, BOOT_SIZE + GBA_FX_FOOTER_SIZE));
	assert_null(gba->fx.ram);
	_destroy(core);
	free(rom);
}

M_TEST_DEFINE(ramStateRejected) {
	struct mCore* core = _ramCore(1048576);
	uint8_t* snapshot = malloc(core->stateSize(core));
	assert_non_null(snapshot);
	memset(snapshot, 0xA5, core->stateSize(core));
	assert_false(core->saveState(core, snapshot));
	assert_int_equal(snapshot[0], 0xA5);
	core->busWrite32(core, GBA_FX_RAM_BASE, 42);
	assert_false(core->loadState(core, snapshot));
	assert_int_equal(core->busRead32(core, GBA_FX_RAM_BASE), 42);
	struct VFile* output = VFileMemChunk("KEEP", 4);
	assert_false(mCoreSaveStateNamed(core, output, 0));
	assert_int_equal(output->size(output), 4);
	char retained[4];
	output->seek(output, 0, SEEK_SET);
	assert_int_equal(output->read(output, retained, 4), 4);
	assert_memory_equal(retained, "KEEP", 4);
	mColor* pixels = calloc(240 * 160, sizeof(*pixels));
	assert_non_null(pixels);
	core->setVideoBuffer(core, pixels, 240);
	core->reset(core);
	assert_false(mCoreSaveStateNamed(core, output, SAVESTATE_SCREENSHOT));
	assert_int_equal(output->size(output), 4);
	output->close(output);
	struct mCoreRewindContext rewind = {0};
	mCoreRewindContextInit(&rewind, 4, false);
	struct VFile* previous = rewind.previousState;
	mCoreRewindAppend(&rewind, core);
	assert_ptr_equal(rewind.previousState, previous);
	assert_int_equal(rewind.size, 0);
	mCoreRewindContextDeinit(&rewind);
	uint8_t* rom = _rom();
	assert_true(_load(core, rom, BOOT_SIZE));
	free(rom);
	core->reset(core);
	unsigned format;
	for (format = 0; format < 2; ++format) {
		struct VFile* normalState = VFileMemChunk(0, 0);
		core->busWrite32(core, GBA_BASE_EWRAM, 42);
		assert_true(mCoreSaveStateNamed(core, normalState, format ? SAVESTATE_SCREENSHOT : 0));
		core->busWrite32(core, GBA_BASE_EWRAM, 13);
		assert_true(mCoreLoadStateNamed(core, normalState, 0));
		assert_int_equal(core->busRead32(core, GBA_BASE_EWRAM), 42);
		normalState->close(normalState);
	}
	free(snapshot);
	_destroy(core);
	free(pixels);
}

M_TEST_SUITE_DEFINE(GBAFX,
	cmocka_unit_test(ramGrant),
	cmocka_unit_test(ramAccess),
	cmocka_unit_test(ramExecutionAndTransfers),
	cmocka_unit_test(ramLifecycle),
	cmocka_unit_test(ramStateRejected),
	cmocka_unit_test(experimentalFeatureGate),
#ifdef GBA_FX_TIMING_EXPERIMENT
	cmocka_unit_test(fractionalRetirement),
	cmocka_unit_test(armComputeAndPeripherals),
	cmocka_unit_test(thumbComputeAndPeripherals),
	cmocka_unit_test(haltAndInterruptTiming),
	cmocka_unit_test(dmaTimingAndData),
	cmocka_unit_test(experimentalStateRejected),
	cmocka_unit_test(legacyInstructionCosts),
	cmocka_unit_test(wrappingAndEventOrder),
#endif
	cmocka_unit_test(resourceBudget),
	cmocka_unit_test(resourcePolicyValidation),
	cmocka_unit_test(resourceLifecycle),
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
