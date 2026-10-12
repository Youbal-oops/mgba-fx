/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_FX_TEST_H
#define GBA_FX_TEST_H

#include <stdbool.h>
#include <stdint.h>

/* Internal diagnostic helpers, not a stable public SDK. */
#define GBA_FX_TEST_RAM_BASE 0x10000000U
#define GBA_FX_TEST_CONTENT_BASE 0x11000000U
#define GBA_FX_TEST_REQUIRED_BYTES 65536U
#define GBA_FX_TEST_CACHE_MAX 1048576U
#define GBA_FX_TEST_CACHE_DIVISOR 16U

struct GBAFXTestGrant {
	uint32_t version;
	uint32_t features;
	uint32_t ramBytes;
	uint32_t contentBytes;
	uint32_t arithmeticProfile;
};

struct GBAFXTestArena {
	uint32_t next;
	uint32_t end;
};

static inline bool GBAFXTestQuery(struct GBAFXTestGrant* grant,
                                uint32_t (*read32)(uint32_t, void*), void* context) {
	*grant = (struct GBAFXTestGrant) {0};
	if (read32(0x040003E0, context) != 0x58464247U || read32(0x040003E4, context) != 1) {
		return false;
	}
	grant->version = 1;
	grant->features = read32(0x040003E8, context);
	uint32_t mib = read32(0x040003EC, context);
	grant->contentBytes = read32(0x040003A4, context);
	grant->arithmeticProfile = read32(0x040003F4, context);
	if (mib > 16 || (grant->arithmeticProfile != 1 && grant->arithmeticProfile != 2)) {
		*grant = (struct GBAFXTestGrant) {0};
		return false;
	}
	grant->ramBytes = mib * 1048576U;
	return true;
}

static inline bool GBAFXTestArenaInit(struct GBAFXTestArena* arena, const struct GBAFXTestGrant* grant) {
	*arena = (struct GBAFXTestArena) {0};
	if (!(grant->features & 8U) || !grant->ramBytes || grant->ramBytes > 16 * 1048576U) {
		return false;
	}
	arena->next = GBA_FX_TEST_RAM_BASE;
	arena->end = GBA_FX_TEST_RAM_BASE + grant->ramBytes;
	return true;
}

static inline uint32_t GBAFXTestAllocate(struct GBAFXTestArena* arena, uint32_t bytes, uint32_t alignment) {
	if (!bytes || !alignment || (alignment & (alignment - 1)) ||
	    arena->next > UINT32_MAX - (alignment - 1)) {
		return 0;
	}
	uint32_t start = (arena->next + alignment - 1) & ~(alignment - 1);
	if (start > arena->end || bytes > arena->end - start) {
		return 0;
	}
	arena->next = start + bytes;
	return start;
}

static inline uint32_t GBAFXTestCacheSize(const struct GBAFXTestGrant* grant) {
	if (grant->ramBytes < GBA_FX_TEST_REQUIRED_BYTES) {
		return 0;
	}
	uint32_t size = grant->ramBytes / GBA_FX_TEST_CACHE_DIVISOR;
	if (size > GBA_FX_TEST_CACHE_MAX) {
		size = GBA_FX_TEST_CACHE_MAX;
	}
	if (size > grant->contentBytes) {
		size = grant->contentBytes;
	}
	if (size > grant->ramBytes - GBA_FX_TEST_REQUIRED_BYTES) {
		size = grant->ramBytes - GBA_FX_TEST_REQUIRED_BYTES;
	}
	return size & ~3U;
}

#endif
