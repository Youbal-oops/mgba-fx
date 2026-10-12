/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "platform/qt/FXResources.h"

#include <QLabel>
#include <QTest>
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/crc32.h>
#include <mgba-util/vfs.h>

using namespace QGBA;

class FXResourcesTest : public QObject {
	Q_OBJECT
private:
	mCore* m_core = nullptr;
	QString value(const FXResourceInfo& info, const QString& label) {
		for (const auto& row : info.rows) {
			if (row.first == label) {
				return row.second;
			}
		}
		return {};
	}

private slots:
	void init() {
		m_core = GBACoreCreate();
		QVERIFY(m_core->init(m_core));
		mCoreInitConfig(m_core, nullptr);
	}

	void cleanup() {
		mCoreConfigDeinit(&m_core->config);
		m_core->deinit(m_core);
	}

	void standardHardware() {
		FXResourceInfo info = FXResourceInfo::capture(m_core);
		QCOMPARE(value(info, "Game mode"), QString("Standard hardware"));
		QCOMPARE(value(info, "Extra game RAM"), QString("0.0 MiB"));
		QCOMPARE(value(info, "Active memory budget"), QString("No enhanced budget"));
	}

	void detectedGrantAndPendingSettings() {
#ifndef _WIN32
		QSKIP("Enhanced resource admission currently requires Windows");
#endif
		QByteArray rom(4096 + GBA_FX_FOOTER_SIZE, 0);
		uint8_t* boot = reinterpret_cast<uint8_t*>(rom.data());
		STORE_32LE(0xEA00002E, 0, boot);
		boot[0xB2] = 0x96;
		STORE_32LE(0xEAFFFFFE, 0xC0, boot);
		uint8_t* footer = boot + 4096;
		STORE_32LE(GBA_FX_VERSION, 0, footer);
		STORE_32LE(GBA_FX_FOOTER_SIZE, 4, footer);
		STORE_32LE(GBA_FX_FEATURE_DETECTION | GBA_FX_FEATURE_POLICY | GBA_FX_FEATURE_RAM, 8, footer);
		STORE_32LE(4096, 0x10, footer);
		STORE_32LE(doCrc32(boot, 4096), 0x14, footer);
		STORE_32LE(1048576, 0x18, footer);
		memcpy(footer + 0x30, "mGBA-FX-DRAFT01", 16);
		STORE_32LE(doCrc32(footer, 0x2C), 0x2C, footer);
		GBA* gba = static_cast<GBA*>(m_core->board);
		gba->fxPolicy.memoryMiB = 256;
		VFile* vf = VFileMemChunk(boot, rom.size());
		QVERIFY(m_core->loadROM(m_core, vf));
		FXResourceInfo info = FXResourceInfo::capture(m_core);
		QCOMPARE(value(info, "Game mode"), QString("Enhanced GBA (detected automatically)"));
		QCOMPARE(value(info, "Extra game RAM"), QString("16.0 MiB"));
		QCOMPARE(value(info, "Active memory budget"), QString("256.0 MiB"));
		gba->fxPolicy.memoryMiB = 512;
		FXResources view([this]() { return FXResourceInfo::capture(m_core); });
		view.show();
		QTest::qWait(20);
		QVERIFY(view.findChildren<QLabel*>().size() > 20);
		info = FXResourceInfo::capture(m_core);
		QCOMPARE(value(info, "Resource settings"), QString("Changed; reopen the game to apply"));
		QCOMPARE(value(info, "Active memory budget"), QString("256.0 MiB"));
		QString output = qEnvironmentVariable("MGBA_FX_UI_CAPTURE");
		if (!output.isEmpty()) {
			QVERIFY(view.grab().save(output));
		}
		view.close();
	}

	void processCPUAccounting() {
		FXResourceInfo info = FXResourceInfo::capture(m_core);
		info.hostValid = true;
		info.host.logicalCPUs = 4;
		info.host.monotonicMs = 1000;
		info.host.cpuTime100ns = 0;
		FXResources view([&info]() { return info; });
		info.host.monotonicMs = 2000;
		info.host.cpuTime100ns = 4000000;
		view.refresh();
		bool found = false;
		for (QLabel* label : view.findChildren<QLabel*>()) {
			found |= label->text() == "10.0 % of available CPU capacity";
		}
		QVERIFY(found);
		view.close();
	}
};

QTEST_MAIN(FXResourcesTest)
#include "FXResources.moc"
