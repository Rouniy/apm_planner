/*===================================================================
APM_PLANNER Open Source Ground Control Station

(c) 2015 Bill Bonney <billbonney@communistech.com>

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
#include "logging.h"
#include "EKFMonitor.h"
#include "configuration.h"
#include "UASManager.h"
#include "LinkManager.h"

#include <QVBoxLayout>
#include <QQmlContext>
#include <QQuickItem>
#include <QQmlEngine>
#include <QMessageBox>


EKFMonitor::EKFMonitor(QWidget *parent) : QWidget(parent)
{
    QUrl url = QUrl::fromLocalFile(QGC::shareDirectory() + "/qml/EKFMonitor.qml");
    QLOG_DEBUG() << url;
    if (!QFile::exists(QGC::shareDirectory() + "/qml/EKFMonitor.qml"))
    {
        QMessageBox::information(nullptr, "Error", QGC::shareDirectory() + "/qml/EKFMonitor.qml not found. Please reinstall the application and try again");
        exit(-1);
    }
    m_ptrDeclarativeView.reset(new QQuickView());
    m_ptrDeclarativeView->engine()->addImportPath(QGC::shareDirectory() +"/qml"); //For installed linux builds
    m_ptrDeclarativeView->setSource(url);
    QSurfaceFormat format = m_ptrDeclarativeView->format();
    format.setSamples(4);
    m_ptrDeclarativeView->setFormat(format);

    QLOG_DEBUG() << "EKFMonitor QML Status:" << m_ptrDeclarativeView->status();
    m_ptrDeclarativeView->setResizeMode(QQuickView::SizeRootObjectToView);
    auto *p_layout = new QVBoxLayout();
    QWidget *viewcontainer = QWidget::createWindowContainer(m_ptrDeclarativeView.get());
    p_layout->addWidget(viewcontainer);
    setLayout(p_layout);
    setContentsMargins(0,0,0,0);
    show();

    // Connect with UAS
    connect(UASManager::instance(), SIGNAL(activeUASSet(UASInterface*)), this,
            SLOT(setActiveUAS(UASInterface*)), Qt::UniqueConnection);
    setActiveUAS(UASManager::instance()->getActiveUAS());

}

void EKFMonitor::setActiveUAS(UASInterface *p_uas)
{
    mp_uasInterface = p_uas;

    if (mp_uasInterface)
    {
//        connect(uas,SIGNAL(textMessageReceived(int,int,int,QString)),this,SLOT(uasTextMessage(int,int,int,QString)));

        UASObject *uasObject = LinkManager::instance()->getUasObject(mp_uasInterface->getUASID());
        if (!uasObject) {
            QLOG_ERROR() << "EKFMonitor::setActiveUAS() Invalid UAS object";
            return;
        }
        VehicleOverview *p_vehicleOverview = uasObject->getVehicleOverview();
        if (p_vehicleOverview)
        {
            m_ptrDeclarativeView->rootContext()->setContextProperty("vehicleOverview", p_vehicleOverview);
        }
        else
        {
            QLOG_ERROR() << "EKFMonitor::setActiveUAS() Invalid vehicleOverview!";
        }

        if (QObject *root = m_ptrDeclarativeView->rootObject()) {
            QMetaObject::invokeMethod(root, "activeUasSet");
        } else {
            QLOG_ERROR() << "EKFMonitor is unavailable because its QML failed to load";
        }
    }
}
