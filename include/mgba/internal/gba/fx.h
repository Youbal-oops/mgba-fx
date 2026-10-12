/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_FX_H
#define GBA_FX_H

#include <mgba-util/common.h>

CXX_GUARD_START

#define GBA_FX_FOOTER_SIZE 64
#define GBA_FX_VERSION 1
#define GBA_FX_FEATURE_DETECTION 1U
#define GBA_FX_IO_BASE 0x3E0
#define GBA_FX_IO_MAGIC 0x58464247U

struct VFile;

struct GBAFXState {
	bool active;
	uint32_t bootSize;
};

enum GBAFXDetection {
	GBA_FX_NONE,
	GBA_FX_VALID,
	GBA_FX_INVALID
};

/* Probe without validating, so malformed marked files bypass multiboot heuristics. */
bool GBAFXHasFooter(struct VFile* vf);
enum GBAFXDetection GBAFXReadFooter(struct VFile* vf, struct GBAFXState* state);
uint16_t GBAFXReadRegister(uint32_t address);

CXX_GUARD_END

#endif
