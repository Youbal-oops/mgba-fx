/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/core/host-resource.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>

static SRWLOCK _lock = SRWLOCK_INIT;
static struct {
	const void* owner;
	unsigned percent;
	uint64_t memoryBytes;
	const void* predecessor;
} _clients[64];
static HANDLE _job;
static bool _overflow;
static enum mHostCPUStatus _status;
static unsigned _effective;

static unsigned _count(DWORD_PTR mask) {
	unsigned count = 0;
	while (mask) {
		mask &= mask - 1;
		++count;
	}
	return count;
}

bool mHostResourcesQuery(struct mHostResources* resources) {
	memset(resources, 0, sizeof(*resources));
	MEMORYSTATUSEX memory = { .dwLength = sizeof(memory) };
	PROCESS_MEMORY_COUNTERS_EX process = { .cb = sizeof(process) };
	DWORD_PTR affinity, systemAffinity;
	FILETIME created, exited, kernel, user;
	if (!GlobalMemoryStatusEx(&memory) ||
	    !GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*) &process, sizeof(process)) ||
	    !GetProcessAffinityMask(GetCurrentProcess(), &affinity, &systemAffinity) ||
	    !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
		return false;
	}
	resources->availableBytes = memory.ullAvailPhys;
	resources->addressBytes = memory.ullAvailVirtual;
	resources->committedBytes = process.PrivateUsage;
	resources->logicalCPUs = _count(affinity);
	resources->monotonicMs = GetTickCount64();
	resources->cpuTime100ns = (((uint64_t) kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
	                         (((uint64_t) user.dwHighDateTime << 32) | user.dwLowDateTime);
	return resources->logicalCPUs != 0;
}

/* Called under the exclusive lock. */
static bool _disable(void) {
	JOBOBJECT_CPU_RATE_CONTROL_INFORMATION disabled = {0};
	return !_job || SetInformationJobObject(_job, JobObjectCpuRateControlInformation, &disabled, sizeof(disabled));
}

/* Called under the exclusive lock. No guest timing or frame counters are changed. */
static void _apply(void) {
	unsigned desired = 100, active = 0, ordinary = 0;
	unsigned i;
	for (i = 0; i < 64; ++i) {
		if (!_clients[i].owner) {
			continue;
		}
		if (!_clients[i].percent) {
			++ordinary;
			continue;
		}
		++active;
		if (_clients[i].percent < desired) {
			desired = _clients[i].percent;
		}
	}
	_effective = 0;
	if (_overflow || ordinary || !active) {
		if (!_disable()) {
			_status = M_HOST_CPU_ERROR;
			return;
		}
		_status = _overflow ? M_HOST_CPU_UNSUPPORTED : active ? M_HOST_CPU_SHARED : M_HOST_CPU_NONE;
		return;
	}
	DWORD_PTR affinity, systemAffinity;
	if (GetActiveProcessorGroupCount() != 1 ||
	    !GetProcessAffinityMask(GetCurrentProcess(), &affinity, &systemAffinity) || !_count(systemAffinity)) {
		_status = _disable() ? M_HOST_CPU_UNSUPPORTED : M_HOST_CPU_ERROR;
		return;
	}
	if (!_job) {
		BOOL inJob = FALSE;
		if (!IsProcessInJob(GetCurrentProcess(), 0, &inJob) || inJob) {
			/* A nested cap would be relative to an unknown parent quota. */
			_status = M_HOST_CPU_EXTERNAL_JOB;
			return;
		}
		HANDLE job = CreateJobObjectW(0, 0);
		if (!job || !AssignProcessToJobObject(job, GetCurrentProcess())) {
			if (job) {
				CloseHandle(job);
			}
			_status = M_HOST_CPU_UNSUPPORTED;
			return;
		}
		/* Retained for this process's lifetime; limits are disabled when unused. */
		_job = job;
	}
	JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate = {0};
	rate.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
	rate.CpuRate = desired * 100 * _count(affinity) / _count(systemAffinity);
	if (!rate.CpuRate) {
		rate.CpuRate = 1;
	}
	if (!SetInformationJobObject(_job, JobObjectCpuRateControlInformation, &rate, sizeof(rate))) {
		_status = _disable() ? M_HOST_CPU_UNSUPPORTED : M_HOST_CPU_ERROR;
		return;
	}
	_status = M_HOST_CPU_ENFORCED;
	_effective = desired;
}

void mHostResourceRegister(const void* owner) {
	AcquireSRWLockExclusive(&_lock);
	unsigned i;
	for (i = 0; i < 64 && _clients[i].owner; ++i);
	if (i < 64) {
		_clients[i].owner = owner;
		_clients[i].percent = 0;
		_clients[i].memoryBytes = 0;
		_clients[i].predecessor = 0;
	} else {
		_overflow = true;
	}
	_apply();
	ReleaseSRWLockExclusive(&_lock);
}

void mHostResourceUnregister(const void* owner) {
	AcquireSRWLockExclusive(&_lock);
	unsigned i;
	for (i = 0; i < 64; ++i) {
		if (_clients[i].owner == owner) {
			_clients[i].owner = 0;
			_clients[i].percent = 0;
			_clients[i].memoryBytes = 0;
			_clients[i].predecessor = 0;
			break;
		}
	}
	_apply();
	ReleaseSRWLockExclusive(&_lock);
}

bool mHostResourceClaimMemory(const void* owner, uint64_t bytes, uint64_t available) {
	AcquireSRWLockExclusive(&_lock);
	unsigned client = 64;
	unsigned i;
	const void* predecessor = 0;
	for (i = 0; i < 64; ++i) {
		if (_clients[i].owner == owner) {
			predecessor = _clients[i].predecessor;
			break;
		}
	}
	for (i = 0; i < 64; ++i) {
		if (_clients[i].owner == owner) {
			client = i;
		} else if (_clients[i].owner && _clients[i].owner != predecessor) {
			uint64_t claimed = _clients[i].memoryBytes;
			available = claimed > available ? 0 : available - claimed;
		}
	}
	bool admitted = !_overflow && client < 64 && bytes <= available;
	if (admitted) {
		_clients[client].memoryBytes = bytes;
	}
	ReleaseSRWLockExclusive(&_lock);
	return admitted;
}

void mHostResourceSetMemoryReplacement(const void* owner, const void* predecessor) {
	AcquireSRWLockExclusive(&_lock);
	unsigned i;
	for (i = 0; i < 64; ++i) {
		if (_clients[i].owner == owner) {
			_clients[i].predecessor = predecessor;
			break;
		}
	}
	ReleaseSRWLockExclusive(&_lock);
}

void mHostResourceReleaseMemory(const void* owner) {
	AcquireSRWLockExclusive(&_lock);
	unsigned i;
	for (i = 0; i < 64; ++i) {
		if (_clients[i].owner == owner) {
			_clients[i].memoryBytes = 0;
			break;
		}
	}
	ReleaseSRWLockExclusive(&_lock);
}

void mHostResourceRequestCPU(const void* owner, unsigned percent) {
	AcquireSRWLockExclusive(&_lock);
	unsigned i;
	for (i = 0; i < 64; ++i) {
		if (_clients[i].owner == owner) {
			_clients[i].percent = percent <= 100 ? percent : 0;
			break;
		}
	}
	_apply();
	ReleaseSRWLockExclusive(&_lock);
}

enum mHostCPUStatus mHostResourceCPUStatus(unsigned* effectivePercent) {
	AcquireSRWLockShared(&_lock);
	enum mHostCPUStatus status = _status;
	if (effectivePercent) {
		*effectivePercent = _effective;
	}
	ReleaseSRWLockShared(&_lock);
	return status;
}
#else
void mHostResourceSetMemoryReplacement(const void* owner, const void* predecessor) {
	UNUSED(owner);
	UNUSED(predecessor);
}
bool mHostResourceClaimMemory(const void* owner, uint64_t bytes, uint64_t available) {
	UNUSED(owner);
	UNUSED(bytes);
	UNUSED(available);
	return false;
}
void mHostResourceReleaseMemory(const void* owner) {
	UNUSED(owner);
}
bool mHostResourcesQuery(struct mHostResources* resources) {
	memset(resources, 0, sizeof(*resources));
	return false;
}
void mHostResourceRegister(const void* owner) {
	UNUSED(owner);
}
void mHostResourceUnregister(const void* owner) {
	UNUSED(owner);
}
void mHostResourceRequestCPU(const void* owner, unsigned percent) {
	UNUSED(owner);
	UNUSED(percent);
}
enum mHostCPUStatus mHostResourceCPUStatus(unsigned* percent) {
	if (percent) {
		*percent = 0;
	}
	return M_HOST_CPU_UNSUPPORTED;
}
#endif
