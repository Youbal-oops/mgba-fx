/* Copyright (c) 2013-2017 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "CoreManager.h"

#include <QMessageBox>

#include "GBAApp.h"
#include "CoreController.h"
#include "LogController.h"
#include "VFileDevice.h"
#include "utils.h"

#include <QDir>

#ifdef M_CORE_GBA
#include <mgba/gba/core.h>
#include <mgba/internal/gba/fx.h>
#endif

#include <mgba/core/core.h>
#include <mgba-util/string.h>
#include <mgba-util/vfs.h>

#include "moc_CoreManager.cpp"

using namespace QGBA;

void CoreManager::setConfig(const mCoreConfig* config) {
	m_config = config;
}

void CoreManager::setMultiplayerController(MultiplayerController* multiplayer) {
	m_multiplayer = multiplayer;
}

CoreController* CoreManager::loadGame(const QString& path, CoreController* replacement) {
	QFileInfo info(path);
	if (!info.isReadable()) {
		// Open specific file in archive
		QString fname = info.fileName();
		QString base = info.path();
		if (base.endsWith("/") || base.endsWith(QDir::separator())) {
			base.chop(1);
		}
		VDir* dir = VDirOpenArchive(base.toUtf8().constData());
		if (dir) {
			VFile* vf = dir->openFile(dir, fname.toUtf8().constData(), O_RDONLY);
			if (vf) {
				struct VFile* vfclone = VFileDevice::openMemory(vf->size(vf));
				VFileDevice::copyFile(vf, vfclone);
				vf = vfclone;
			}
			dir->close(dir);
			return loadGame(vf, fname, base, replacement);
		} else {
			LOG(QT, ERROR) << tr("Failed to open game file: %1").arg(path);
		}
		return nullptr;
	}
	VFile* vf = nullptr;
	VDir* archive = VDirOpenArchive(path.toUtf8().constData());
	if (archive) {
		// Open first file in archive
		VFile* vfOriginal = VDirFindFirst(archive, [](VFile* vf) {
			return mCoreIsCompatible(vf) != mPLATFORM_NONE;
		});
		if (vfOriginal) {
			ssize_t size = vfOriginal->size(vfOriginal);
			if (size > 0) {
				struct VFile* vfclone = VFileDevice::openMemory(vfOriginal->size(vfOriginal));
				VFileDevice::copyFile(vfOriginal, vfclone);
				vf = vfclone;
			}
			vfOriginal->close(vfOriginal);
		}
		archive->close(archive);
	}
	if (!vf) {
		// Open bare file
		vf = VFileOpen(info.canonicalFilePath().toUtf8().constData(), O_RDONLY);
	}

	if (!vf) {
		return nullptr;
	}

	QDir dir(info.dir());
	QDir tmpdir(QDir::tempPath());
	if (info.canonicalFilePath().startsWith(tmpdir.canonicalPath())) {
		bool bad = false;
		if (m_config) {
			mCoreOptions opts{};
			mCoreConfigMap(m_config, &opts);
			bad = bad || !opts.savegamePath;
			bad = bad || !opts.savestatePath;
			bad = bad || !opts.screenshotPath;
			bad = bad || !opts.cheatsPath;
			mCoreConfigFreeOpts(&opts);
		} else {
			bad = true;
		}
		if (bad) {
			QString newPath = saveFailed(vf, tr("Temporary file loaded"),
			                             tr("The ROM appears to be loaded from a temporary directory, perhaps automatically extracted from an archive (e.g. a zip file)."),
			                             "*." + info.suffix());
			if (!newPath.isEmpty()) {
				vf->close(vf);
				return loadGame(newPath, replacement);
			}
		}
	}

	return loadGame(vf, info.fileName(), dir.canonicalPath(), replacement);
}

CoreController* CoreManager::loadGame(VFile* vf, const QString& path, const QString& base, CoreController* replacement) {
	if (!vf) {
		return nullptr;
	}

	mCore* core = mCoreFindVF(vf);
	if (!core) {
		vf->close(vf);
		LOG(QT, ERROR) << tr("Could not load game. Are you sure it's in the correct format?");
		return nullptr;
	}

	core->init(core);
	CoreController::Interrupter interrupter(replacement);
#ifdef M_CORE_GBA
	if (replacement && core->platform(core) == mPLATFORM_GBA && replacement->platform() == mPLATFORM_GBA) {
		mHostResourceSetMemoryReplacement(core->board, replacement->thread()->core->board);
	}
#endif
	mCoreInitConfig(core, nullptr);

	if (m_config) {
		mCoreLoadForeignConfig(core, m_config);
	}

	bool enhancedCandidate = false;
#ifdef M_CORE_GBA
	enhancedCandidate = core->platform(core) == mPLATFORM_GBA && GBAFXHasFooter(vf);
#endif
	bool loaded;
	if (m_preload) {
		loaded = mCorePreloadVF(core, vf);
	} else {
		loaded = core->loadROM(core, vf);
		if (!loaded) {
			vf->close(vf);
		}
	}
	if (!loaded) {
		if (enhancedCandidate) {
			LOG(QT, ERROR) << tr("Could not load enhanced game. Tools > View logs shows the specific format or resource failure. Check Settings > Enhancements and other enhanced windows, then reopen the game.");
		} else {
			LOG(QT, ERROR) << tr("Could not load game. Check the ROM format and resource settings.");
		}
		mCoreConfigDeinit(&core->config);
		core->deinit(core);
		return nullptr;
	}
#ifdef M_CORE_GBA
	if (core->platform(core) == mPLATFORM_GBA) {
		mHostResourceSetMemoryReplacement(core->board, nullptr);
	}
#endif

	QByteArray bytes(path.toUtf8());
	separatePath(bytes.constData(), nullptr, core->dirs.baseName, nullptr);

	QFileInfo info(base);
	if (info.isDir()) {
		info = QFileInfo(base + "/" + path);
	}
	bytes = info.dir().canonicalPath().toUtf8();
	mDirectorySetAttachBase(&core->dirs, VDirOpen(bytes.constData()));
	if (!mCoreAutoloadSave(core)) {
		QString filter = romFilters(false, core->platform(core), true);
		QString newPath = saveFailed(vf, tr("Could not open save file"),
		                             tr("Failed to open save file; in-game saves cannot be updated."),
		                             filter);
		if (!newPath.isEmpty()) {
			mCoreConfigDeinit(&core->config);
			core->deinit(core);
			return loadGame(newPath, replacement);
		}
	}
	mCoreAutoloadCheats(core);

	CoreController* cc = new CoreController(core);
	if (m_multiplayer) {
		cc->setMultiplayerController(m_multiplayer);
	}

	info = QFileInfo(base);
	cc->setPath(path, info.canonicalFilePath());
	emit coreLoaded(cc);
	return cc;
}

CoreController* CoreManager::loadBIOS(int platform, const QString& path) {
	QFileInfo info(path);
	VFile* vf = VFileOpen(info.canonicalFilePath().toUtf8().constData(), O_RDONLY);
	if (!vf) {
		return nullptr;
	}

	mCore* core = nullptr;
	switch (platform) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		core = GBACoreCreate();
		break;
#endif
	default:
		vf->close(vf);
		return nullptr;
	}
	if (!core) {
		vf->close(vf);
		return nullptr;
	}

	core->init(core);
	mCoreInitConfig(core, nullptr);

	if (m_config) {
		mCoreLoadForeignConfig(core, m_config);
	}

	core->loadBIOS(core, vf, 0);

	mCoreConfigSetOverrideIntValue(&core->config, "useBios", 1);
	mCoreConfigSetOverrideIntValue(&core->config, "skipBios", 0);

	QByteArray bytes(info.baseName().toUtf8());
	strlcpy(core->dirs.baseName, bytes.constData(), sizeof(core->dirs.baseName));

	bytes = info.dir().canonicalPath().toUtf8();
	mDirectorySetAttachBase(&core->dirs, VDirOpen(bytes.constData()));

	CoreController* cc = new CoreController(core);
	cc->blockSave();
	if (m_multiplayer) {
		cc->setMultiplayerController(m_multiplayer);
	}
	emit coreLoaded(cc);
	return cc;
}

QString CoreManager::saveFailed(VFile* vf, const QString& title, const QString& summary, const QString& filter) {
	int result = QMessageBox::critical(nullptr, title,
		summary + "\n\n" + tr("Would you like to copy the ROM to a different location? If you don't, this will likely lead to data loss (e.g. saves, screenshots, etc.)."),
		QMessageBox::Ok | QMessageBox::Ignore,QMessageBox::Ok);
	if (result == QMessageBox::Ignore) {
		return QString();
	}

	auto retry = []() {
		int result = QMessageBox::critical(nullptr, tr("Copy failed"), tr("Failed to copy ROM. Do you want to try again?"),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
		return result == QMessageBox::Yes;
	};

	bool ok = true;
	while (ok) {
		QString newPath = GBAApp::app()->getSaveFileName(nullptr, tr("New ROM location"), filter);
		if (newPath.isEmpty()) {
			return QString();
		}

		QFile newFile(newPath);
		if (!newFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			if (!retry()) {
				ok = false;
			}
			continue;
		}

		vf->seek(vf, 0, SEEK_SET);
		char buffer[4096];
		while (ok) {
			ssize_t read = vf->read(vf, buffer, sizeof(buffer));
			if (read < 0) {
				ok = false;
			}
			if (read <= 0) {
				break;
			}

			qint64 written = newFile.write(buffer, read);
			if (written < read) {
				newFile.remove();
				if (!retry()) {
					ok = false;
				}
				break;
			}
		}

		if (ok) {
			return newPath;
		}
	}

	return QString();
}
