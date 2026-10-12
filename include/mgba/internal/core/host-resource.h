/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef HOST_RESOURCE_H
#define HOST_RESOURCE_H

#include <mgba-util/common.h>

CXX_GUARD_START

struct mHostResources {
	uint64_t availableBytes;
	uint64_t addressBytes;
	uint64_t committedBytes;
	uint64_t cpuTime100ns;
	uint64_t monotonicMs;
	unsigned logicalCPUs;
};

enum mHostCPUStatus {
	M_HOST_CPU_NONE = 0,
	M_HOST_CPU_ENFORCED = 1,
	M_HOST_CPU_UNSUPPORTED = 2,
	M_HOST_CPU_SHARED = 3,
	M_HOST_CPU_EXTERNAL_JOB = 4,
	M_HOST_CPU_ERROR = 5
};

bool mHostResourcesQuery(struct mHostResources* resources);
/* Reserve a whole run budget against other enhanced cores in this process. */
bool mHostResourceClaimMemory(const void* owner, uint64_t bytes, uint64_t available);
void mHostResourceReleaseMemory(const void* owner);
/* The predecessor must be paused during admission and closed on success. */
void mHostResourceSetMemoryReplacement(const void* owner, const void* predecessor);
/* All initialized cores register, so ordinary cores suppress process-wide limits. */
void mHostResourceRegister(const void* owner);
void mHostResourceUnregister(const void* owner);
void mHostResourceRequestCPU(const void* owner, unsigned percent);
enum mHostCPUStatus mHostResourceCPUStatus(unsigned* effectivePercent);

CXX_GUARD_END

#endif
