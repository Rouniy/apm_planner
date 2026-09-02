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

/**
 * @file
 *   @brief Base class for configuration screens, handles UAS connections.
 *
 *   @author Michael Carpenter <malcom2073@gmail.com>
 */

#ifndef AP2CONFIGWIDGET_H
#define AP2CONFIGWIDGET_H

#include <QWidget>
#include <QPointer>
#include "UASManager.h"
#include "UASInterface.h"
class QGCUASParamManager;
class AP2ConfigWidget : public QWidget
{
    Q_OBJECT
public:
    explicit AP2ConfigWidget(QWidget *parent = nullptr);

protected:
    UASInterface *m_uas{nullptr};
    QPointer<QGCUASParamManager> m_parameterManager;
    bool showNullMAVErrorMessageBox();
    void initConnections();
    void replayCachedParameters();
    
public slots:
    virtual void activeUASSet(UASInterface *uas);
    virtual void parameterChanged(int uas, int component, QString parameterName, QVariant value);
    virtual void parameterChanged(int uas, int component, int parameterCount, int parameterId, QString parameterName, QVariant value);
};

#endif // AP2CONFIGWIDGET_H
