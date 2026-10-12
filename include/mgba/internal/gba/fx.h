/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_FX_H
#define GBA_FX_H

#include <mgba-util/common.h>
#include <mgba/internal/core/host-resource.h>

CXX_GUARD_START

#define GBA_FX_FOOTER_SIZE 64
#define GBA_FX_VERSION 1
#define GBA_FX_FEATURE_DETECTION 1U
#define GBA_FX_FEATURE_POLICY 2U
#define GBA_FX_FEATURE_ALU2X 4U
#define GBA_FX_FEATURE_RAM 8U
#define GBA_FX_FEATURE_CONTENT 16U
#define GBA_FX_CONTENT_BASE 0x11000000U
#define GBA_FX_CONTENT_WINDOW 0x01000000U
#define GBA_FX_CONTENT_MAX 0x08000000U
#define GBA_FX_RAM_BASE 0x10000000U
#define GBA_FX_RAM_MAX 0x01000000U
#ifdef GBA_FX_TIMING_EXPERIMENT
#define GBA_FX_SUPPORTED_FEATURES 31U
#else
#define GBA_FX_SUPPORTED_FEATURES 27U
#endif
#define GBA_FX_IO_START 0x3A0
#define GBA_FX_IO_END 0x400
#define GBA_FX_IO_BASE 0x3E0
#define GBA_FX_IO_MAGIC 0x58464247U

struct VFile;
struct mCoreConfig;

struct GBAFXPolicy {
	int memoryPercent;
	int memoryMiB;
	int cpuPercent;
	bool valid;
};

struct GBAFXBudget {
	bool valid;
	uint64_t limitBytes;
	uint64_t reservedBytes;
	uint64_t headroomBytes;
	struct mHostResources host;
	struct GBAFXPolicy policy;
};

struct GBAFXState {
	bool active;
	uint32_t bootSize;
	uint32_t requiredFeatures;
	uint32_t minimumRAM;
	uint32_t ramSize;
	uint32_t* ram;
	uint32_t contentSize;
	uint32_t contentCRC;
	uint32_t fileCRC;
	uint32_t contentBank;
	bool invalidBank;
	void* contentMapping;
	const uint8_t* content;
	struct GBAFXBudget budget;
	bool alu2x;
	uint8_t aluPhase;
	uint64_t acceleratedInstructions;
};

enum GBAFXDetection {
	GBA_FX_NONE,
	GBA_FX_VALID,
	GBA_FX_INVALID
};

/* Probe without validating, so malformed marked files bypass multiboot heuristics. */
bool GBAFXHasFooter(struct VFile* vf);
enum GBAFXDetection GBAFXReadFooter(struct VFile* vf, struct GBAFXState* state);
uint16_t GBAFXReadRegister(const struct GBAFXState* state, uint32_t address);
void GBAFXPolicyDefaults(struct GBAFXPolicy* policy);
void GBAFXPolicyLoad(struct GBAFXPolicy* policy, const struct mCoreConfig* config);
bool GBAFXCalculateBudget(const struct GBAFXPolicy* policy, const struct mHostResources* host,
                          struct GBAFXBudget* budget);
uint32_t GBAFXRAMGrant(const struct GBAFXBudget* budget, uint32_t minimum);
void GBAFXRAMDeinit(struct GBAFXState* state);
void GBAFXDeinit(struct GBAFXState* state, struct VFile* vf);
uint32_t GBAFXContentBankSize(const struct GBAFXState* state);
uint32_t GBAFXContentRead(const struct GBAFXState* state, uint32_t address, unsigned width);
void GBAFXWriteRegister(struct GBAFXState* state, uint32_t address, uint32_t value, unsigned width);

CXX_GUARD_END

#endif
