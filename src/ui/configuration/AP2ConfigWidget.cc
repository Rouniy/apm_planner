/*===================================================================
APM_PLANNER Open Source Ground Control Station

(c) 2013 APM_PLANNER PROJECT <http://www.diydrones.com>

This file is part of the APM_PLANNER project

    APM_PLANNER is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    APM_PLANNER is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with APM_PLANNER. If not, see <http://www.gnu.org/licenses/>.

======================================================================*/

#include <QMessageBox>
#include <QTimer>
#include "AP2ConfigWidget.h"
#include "LinkManager.h"
#include "QGCUASParamManager.h"

AP2ConfigWidget::AP2ConfigWidget(QWidget *parent) : QWidget(parent)
{
}

void AP2ConfigWidget::initConnections()
{
    connect(UASManager::instance(),SIGNAL(activeUASSet(UASInterface*)),this,SLOT(activeUASSet(UASInterface*)));
    activeUASSet(UASManager::instance()->getActiveUAS());
}

void AP2ConfigWidget::activeUASSet(UASInterface *uas)
{
    if (m_parameterManager) {
        disconnect(m_parameterManager, nullptr, this, nullptr);
    }
    m_uas = uas;
    m_parameterManager = LinkManager::instance()->parameterManager();
    if (m_uas && m_parameterManager) {
        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                this,
                [this](int component, const QString &name,
                       const QVariant &value) {
            const int systemId = m_uas ? m_uas->getUASID() : -1;
            parameterChanged(systemId, component, name, value);
        });
        connect(m_parameterManager,
                &QGCUASParamManager::parameterValueReceived,
                this,
                [this](int component, int count, int index,
                       const QString &name, const QVariant &value, int type) {
            Q_UNUSED(type)
            const int systemId = m_uas ? m_uas->getUASID() : -1;
            parameterChanged(systemId, component, count, index, name, value);
        });
        // Derived activeUASSet implementations often populate combo boxes after
        // calling this base method. Replay on the next event-loop turn so the
        // complete page UI exists before cached values are applied.
        QTimer::singleShot(0, this, [this]() { replayCachedParameters(); });
    }
}

void AP2ConfigWidget::replayCachedParameters()
{
    if (!m_uas || !m_parameterManager) {
        return;
    }
    QGCUASParamManager *manager = m_parameterManager.data();
    for (int component : manager->getComponentIds()) {
        const QList<QString> names = manager->getParameterNames(component);
        for (int index = 0; index < names.size(); ++index) {
            const QString &name = names.at(index);
            const QVariant value = manager->getParameterValue(component, name);
            parameterChanged(m_uas->getUASID(), component, name, value);
            parameterChanged(m_uas->getUASID(), component, names.size(),
                             index, name, value);
        }
    }
}

void AP2ConfigWidget::parameterChanged(int uas, int component, QString parameterName, QVariant value)
{
    Q_UNUSED(uas)
    Q_UNUSED(component)
    Q_UNUSED(parameterName)
    Q_UNUSED(value)
}

void AP2ConfigWidget::parameterChanged(int uas, int component, int parameterCount, int parameterId, QString parameterName, QVariant value)
{
    Q_UNUSED(uas)
    Q_UNUSED(component)
    Q_UNUSED(parameterName)
    Q_UNUSED(value)
    Q_UNUSED(parameterCount)
    Q_UNUSED(parameterId)
}

bool AP2ConfigWidget::showNullMAVErrorMessageBox()
{
    if (!m_uas)
    {
        QMessageBox::information(this ,tr("Error"), tr("Please connect to a MAV before attempting to set configuration"));
        return true;
    }
    return false;
}
