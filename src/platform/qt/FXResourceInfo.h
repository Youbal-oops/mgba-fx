/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef QGBA_FX_RESOURCE_INFO
#define QGBA_FX_RESOURCE_INFO

#include <QList>
#include <QPair>
#include <QString>
#include <mgba/internal/core/host-resource.h>

struct mCore;

namespace QGBA {

struct FXResourceInfo {
	QList<QPair<QString, QString>> rows;
	struct mHostResources host{};
	bool hostValid = false;
	QString rewind;
	static FXResourceInfo capture(mCore* core);
};

}
#endif
