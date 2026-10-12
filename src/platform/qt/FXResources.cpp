/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "FXResources.h"
#include "moc_FXResources.cpp"

#include <QDialogButtonBox>
#include <QCloseEvent>
#include <QFormLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

using namespace QGBA;

FXResources::FXResources(std::function<FXResourceInfo()> capture, QWidget* parent)
	: QDialog(parent)
	, m_capture(capture)
	, m_form(new QFormLayout)
	, m_cpuUsage(new QLabel(tr("Measuring...")))
	, m_rewind(new QLabel)
	, m_timer(new QTimer(this))
{
	setWindowTitle(tr("Game resources"));
	auto layout = new QVBoxLayout(this);
	layout->addLayout(m_form);
	refresh();
	m_form->addRow(tr("Process CPU usage"), m_cpuUsage);
	m_form->addRow(tr("Rewind history"), m_rewind);
	auto explanation = new QLabel(tr("Host measurements cover all emulator windows in this process. Memory budgets are planning ceilings; they are not game RAM or an operating-system RAM cap. Enhanced budgets are shared conservatively: another game may require lower limits. Change limits in Settings > Enhancements, then reopen the game."));
	explanation->setWordWrap(true);
	layout->addWidget(explanation);
	auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
	layout->addWidget(buttons);
	resize(610, sizeHint().height());
	connect(m_timer, &QTimer::timeout, this, &FXResources::refresh);
	m_timer->start(1000);
}

void FXResources::closeEvent(QCloseEvent* event) {
	m_timer->stop();
	QDialog::closeEvent(event);
}

void FXResources::refresh() {
	FXResourceInfo info = m_capture();
	if (m_values.isEmpty()) {
		for (const auto& row : info.rows) {
			auto value = new QLabel(row.second);
			value->setWordWrap(true);
			m_form->addRow(row.first, value);
			m_values.append(value);
		}
	} else {
		for (int i = 0; i < info.rows.size() && i < m_values.size(); ++i) {
			m_values[i]->setText(info.rows[i].second);
		}
	}
	if (info.hostValid && m_previous.hostValid && info.host.monotonicMs > m_previous.host.monotonicMs &&
	    info.host.cpuTime100ns >= m_previous.host.cpuTime100ns && info.host.logicalCPUs) {
		double percent = double(info.host.cpuTime100ns - m_previous.host.cpuTime100ns) /
		                 (info.host.monotonicMs - m_previous.host.monotonicMs) / info.host.logicalCPUs / 100;
		m_cpuUsage->setText(tr("%1 % of available CPU capacity").arg(percent, 0, 'f', 1));
	}
	if (!info.hostValid) {
		m_cpuUsage->setText(tr("Unavailable"));
	}
	m_previous = info;
	m_rewind->setText(info.rewind);
}
