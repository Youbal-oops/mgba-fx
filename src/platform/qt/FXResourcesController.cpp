/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "FXResources.h"
#include "CoreController.h"

using namespace QGBA;

FXResources::FXResources(std::shared_ptr<CoreController> controller, QWidget* parent)
	: FXResources([controller]() {
		CoreController::Interrupter interrupter(controller);
		mCoreThread* thread = controller->thread();
		FXResourceInfo info = FXResourceInfo::capture(thread->core);
		if (thread->impl && thread->impl->rewind.currentState) {
			mCoreRewindContext* rewind = &thread->impl->rewind;
			if (rewind->onThread) {
				MutexLock(&rewind->mutex);
			}
			info.rewind = tr("%1 captured; capacity %2 entries").arg(rewind->size)
			              .arg(mCoreRewindPatchesSize(&rewind->patchMemory));
			if (rewind->onThread) {
				MutexUnlock(&rewind->mutex);
			}
		} else if (thread->core->opts.rewindEnable) {
			info.rewind = tr("Disabled by memory budget");
		}
		return info;
	}, parent)
{
	connect(controller.get(), &CoreController::stopping, this, &QWidget::close);
}
