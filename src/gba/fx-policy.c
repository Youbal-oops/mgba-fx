/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/fx.h>
#include <mgba/core/config.h>
#include <errno.h>
#include <limits.h>

void GBAFXPolicyDefaults(struct GBAFXPolicy* policy) {
	*policy = (struct GBAFXPolicy) { .memoryPercent = 80, .cpuPercent = 80, .valid = true };
}

void GBAFXPolicyLoad(struct GBAFXPolicy* policy, const struct mCoreConfig* config) {
	GBAFXPolicyDefaults(policy);
	const char* keys[] = { "gba.fxMemoryPercent", "gba.fxMemoryMiB", "gba.fxCPUPercent" };
	int* values[] = { &policy->memoryPercent, &policy->memoryMiB, &policy->cpuPercent };
	unsigned i;
	for (i = 0; i < 3; ++i) {
		const char* text = mCoreConfigGetValue(config, keys[i]);
		if (!text) {
			continue;
		}
		char* end;
		errno = 0;
		long value = strtol(text, &end, 10);
		if (errno == ERANGE || end == text || *end || value < INT_MIN || value > INT_MAX) {
			policy->valid = false;
		} else {
			*values[i] = value;
		}
	}
}

bool GBAFXCalculateBudget(const struct GBAFXPolicy* policy, const struct mHostResources* host, struct GBAFXBudget* budget) {
	memset(budget, 0, sizeof(*budget));
	if (!policy->valid || policy->memoryPercent < 1 || policy->memoryPercent > 100 ||
	    policy->cpuPercent < 1 || policy->cpuPercent > 100 || policy->memoryMiB < 0 ||
	    policy->memoryMiB > 1048576 || !host->logicalCPUs) {
		return false;
	}
	uint64_t limit = host->availableBytes / 100 * policy->memoryPercent +
	                 host->availableBytes % 100 * policy->memoryPercent / 100;
	if (policy->memoryMiB) {
		limit = (uint64_t) policy->memoryMiB * 1048576;
	}
	if (limit > host->availableBytes) {
		limit = host->availableBytes;
	}
	if (limit > host->addressBytes) {
		limit = host->addressBytes;
	}
	uint64_t overhead = host->committedBytes > 64 * 1048576 ? host->committedBytes : 64 * 1048576;
	if (overhead > UINT64_MAX - 32 * 1048576) {
		return false;
	}
	overhead += 32 * 1048576; /* Headroom for baseline state and frontend buffers. */
	if (limit < overhead) {
		return false;
	}
	budget->valid = true;
	budget->limitBytes = limit;
	budget->reservedBytes = overhead;
	budget->headroomBytes = limit - overhead;
	budget->host = *host;
	budget->policy = *policy;
	return true;
}
