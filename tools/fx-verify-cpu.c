/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/core/host-resource.h>
#include <windows.h>

static LONG _stop;

static DWORD WINAPI _worker(void* context) {
	UNUSED(context);
	while (!InterlockedCompareExchange(&_stop, 0, 0));
	return 0;
}

static double _measure(unsigned workers) {
	HANDLE threads[64];
	struct mHostResources before, after;
	if (!mHostResourcesQuery(&before)) {
		return -1;
	}
	InterlockedExchange(&_stop, 0);
	unsigned i;
	for (i = 0; i < workers; ++i) {
		threads[i] = CreateThread(0, 0, _worker, 0, 0, 0);
		if (!threads[i]) {
			break;
		}
	}
	unsigned started = i;
	Sleep(2000);
	InterlockedExchange(&_stop, 1);
	for (i = 0; i < started; ++i) {
		WaitForSingleObject(threads[i], INFINITE);
		CloseHandle(threads[i]);
	}
	if (started != workers || !mHostResourcesQuery(&after) || after.monotonicMs <= before.monotonicMs) {
		return -1;
	}
	return (after.cpuTime100ns - before.cpuTime100ns) * 100.0 /
	       ((after.monotonicMs - before.monotonicMs) * 10000.0 * workers);
}

int main(void) {
	struct mHostResources host;
	if (!mHostResourcesQuery(&host) || host.logicalCPUs > 64) {
		return 1;
	}
	int owner;
	mHostResourceRegister(&owner);
	mHostResourceRequestCPU(&owner, 20);
	unsigned effective;
	enum mHostCPUStatus status = mHostResourceCPUStatus(&effective);
	if (status != M_HOST_CPU_ENFORCED) {
		printf("CPU cap unavailable: status=%u; enforcement measurement skipped\n", status);
		mHostResourceUnregister(&owner);
		return 0;
	}
	double capped = _measure(host.logicalCPUs);
	mHostResourceRequestCPU(&owner, 0);
	double uncapped = _measure(host.logicalCPUs);
	mHostResourceUnregister(&owner);
	printf("CPU target=%u%% measured_capped=%.2f%% measured_uncapped=%.2f%% logical_cpus=%u\n",
	       effective, capped, uncapped, host.logicalCPUs);
	return capped < 0 || uncapped < 0 || capped > 35 || uncapped <= capped ? 1 : 0;
}
