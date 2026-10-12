/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef QGBA_FX_RESOURCES
#define QGBA_FX_RESOURCES

#include "FXResourceInfo.h"
#include <QDialog>
#include <memory>
#include <functional>

class QFormLayout;
class QLabel;
class QTimer;

namespace QGBA {
class CoreController;

class FXResources : public QDialog {
	Q_OBJECT
public:
	FXResources(std::shared_ptr<CoreController> controller, QWidget* parent = nullptr);
	FXResources(std::function<FXResourceInfo()> capture, QWidget* parent = nullptr);
	void refresh();

protected:
	void closeEvent(QCloseEvent* event) override;

private:
	std::function<FXResourceInfo()> m_capture;
	QFormLayout* m_form;
	QLabel* m_cpuUsage;
	QLabel* m_rewind;
	QTimer* m_timer;
	FXResourceInfo m_previous;
	QList<QLabel*> m_values;
};
}
#endif
