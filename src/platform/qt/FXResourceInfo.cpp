/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "FXResourceInfo.h"

#include <QCoreApplication>
#include <mgba/core/core.h>
#include <mgba/core/rewind.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/gba.h>
#endif

using namespace QGBA;

static QString _text(const char* text) {
	return QCoreApplication::translate("FXResources", text);
}

static QString _mib(uint64_t bytes) {
	return _text("%1 MiB").arg(double(bytes) / 1048576, 0, 'f', 1);
}

FXResourceInfo FXResourceInfo::capture(mCore* core) {
	FXResourceInfo info;
	info.hostValid = mHostResourcesQuery(&info.host);
	bool enhanced = false;
#ifdef M_CORE_GBA
	const GBAFXState* fx = nullptr;
	if (core && core->platform(core) == mPLATFORM_GBA) {
		fx = &static_cast<const GBA*>(core->board)->fx;
		enhanced = fx->active;
	}
#endif
	info.rows.append({_text("Game mode"), enhanced ? _text("Enhanced GBA (detected automatically)") : _text("Standard hardware")});
#ifdef M_CORE_GBA
	info.rows.append({_text("Extra game RAM"), _mib(enhanced ? fx->ramSize : 0)});
	info.rows.append({_text("Additional content"), _mib(enhanced ? fx->contentSize : 0)});
	info.rows.append({_text("CPU profile"), enhanced && fx->alu2x ? _text("Experimental 2x IWRAM arithmetic") : _text("Original hardware timing")});
	info.rows.append({_text("Active memory budget"), enhanced && fx->budget.valid ? _mib(fx->budget.limitBytes) : _text("No enhanced budget")});
	info.rows.append({_text("Planned memory reservation"), enhanced && fx->budget.valid ? _mib(fx->budget.reservedBytes) : _text("None")});
	info.rows.append({_text("Remaining launch budget"), enhanced && fx->budget.valid ? _mib(fx->budget.headroomBytes) : _text("None")});
	info.rows.append({_text("Requested host CPU limit"), enhanced && fx->budget.valid ? _text("%1 %").arg(fx->budget.policy.cpuPercent) : _text("None")});
	if (enhanced && fx->budget.valid) {
		const GBAFXPolicy& pending = static_cast<const GBA*>(core->board)->fxPolicy;
		bool changed = pending.memoryPercent != fx->budget.policy.memoryPercent ||
		               pending.memoryMiB != fx->budget.policy.memoryMiB || pending.cpuPercent != fx->budget.policy.cpuPercent;
		info.rows.append({_text("Resource settings"), changed ? _text("Changed; reopen the game to apply") : _text("Active for this run")});
	} else {
		info.rows.append({_text("Resource settings"), _text("No enhanced budget")});
	}
#endif
	unsigned effective = 0;
	enum mHostCPUStatus status = mHostResourceCPUStatus(&effective);
	QString limiting;
	switch (status) {
	case M_HOST_CPU_ENFORCED: limiting = _text("Enforced: %1 % of process CPU capacity").arg(effective); break;
	case M_HOST_CPU_SHARED: limiting = _text("Not enforced: a standard game shares the process"); break;
	case M_HOST_CPU_EXTERNAL_JOB: limiting = _text("Not enforced: external Windows job"); break;
	case M_HOST_CPU_UNSUPPORTED: limiting = _text("Not enforced: unavailable on this host"); break;
	case M_HOST_CPU_ERROR: limiting = _text("Not enforced: operating-system request failed"); break;
	default: limiting = _text("No active process CPU limit"); break;
	}
	info.rows.append({_text("CPU limiting status"), limiting});
	info.rows.append({_text("Process committed RAM"), info.hostValid ? _mib(info.host.committedBytes) : _text("Unavailable")});
	info.rows.append({_text("Available host RAM"), info.hostValid ? _mib(info.host.availableBytes) : _text("Unavailable")});
	info.rewind = _text("Disabled in settings");
	if (core && core->opts.rewindEnable) {
		size_t requested = core->opts.rewindBufferCapacity > 0 ? core->opts.rewindBufferCapacity : 0;
		info.rewind = _text("Budget permits %1 entries").arg(mCoreRewindCapacity(core, requested));
	}
	return info;
}
