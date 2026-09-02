/*===================================================================
APM_PLANNER Open Source Ground Control Station

(c) 2019 APM_PLANNER PROJECT <http://www.ardupilot.com>

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
 * @file LogAnalysisMap.cpp
 * @author Arne Wischmann <wischmann-a@gmx.de>
 * @date 17 Mrz 2019
 * @brief File providing implementation for the log analysing map class
 */

#include "LogAnalysisMap.h"
#include "logging.h"
#include "ui/map/AbstractMapWidget.h"
#include "ui/map/CompiledMapBackends.h"
#include "ui/map/MapWidgetFactory.h"

#include "ui_LogAnalysisMap.h"

#include <QGridLayout>
#include <QSettings>
#include <utility>

//************************************************************************************

LogAnalysisMap::LogAnalysisMap(QWidget *parent) :
    QWidget(parent),
    mp_Ui(new Ui::LogAnalysisMap)
 {
    QLOG_DEBUG() << "LogAnalysisMap::LogAnalysisMap - CTOR";
    mp_Ui->setupUi(this);

    RegisterCompiledMapBackends();
    m_mapBackend = MapWidgetFactory::instance()->CreateMapWidget(
        MapWidgetRole::LogAnalysis, mp_Ui->mapHost, this);
    if (!m_mapBackend || !m_mapBackend->Widget()) {
        QLOG_ERROR() << "LogAnalysisMap: no map backend is available";
        mp_Ui->zoomSlider->setEnabled(false);
        return;
    }
    auto *mapLayout = new QGridLayout(mp_Ui->mapHost);
    mapLayout->setContentsMargins(0, 0, 0, 0);
    mapLayout->setSpacing(0);
    m_mapBackend->Widget()->setObjectName(QStringLiteral("map"));
    mapLayout->addWidget(m_mapBackend->Widget());

    // setup zoom slider
    mp_Ui->zoomSlider->setMinimum(
        m_mapBackend->MinZoom() * s_MapScaling);
    mp_Ui->zoomSlider->setMaximum(
        m_mapBackend->MaxZoom() * s_MapScaling);
    setZoom(qRound(m_mapBackend->ZoomReal()));

    connect(mp_Ui->zoomSlider, &QSlider::valueChanged,
            this, &LogAnalysisMap::setMapZoom);
    connect(m_mapBackend, &AbstractMapWidget::ZoomChanged,
            this, &LogAnalysisMap::setZoom);

    loadSettings();

    m_mapBackend->SetAcceleratedRenderingEnabled(true);

}

LogAnalysisMap::~LogAnalysisMap()
{
    QLOG_DEBUG() << "LogAnalysisMap::~LogAnalysisMap - DTOR";
    saveSettings();
    delete mp_Ui;
}

void LogAnalysisMap::setDataStorage(LogdataStorage::Ptr dataPtr)
{
    m_dataStoragePtr = std::move(dataPtr);
    m_gpsType = {};
    m_attType = {};
    m_latName.clear();
    m_lonName.clear();
    m_headingName.clear();
    m_xValues.clear();
    m_latValues.clear();
    m_lonValues.clear();
    m_xValuesHeading.clear();
    m_headingValues.clear();
    m_validIndex = 0;
    if (m_mapBackend) {
        m_mapBackend->SetLogTrail({});
    }
    findDataNames();
}

void LogAnalysisMap::paintUAVTrail()
{
    if (!m_mapBackend) {
        return;
    }
    if (!m_dataStoragePtr) {
        m_mapBackend->SetLogTrail({});
        return;
    }
    // fetch data
    m_xValues.clear();
    m_latValues.clear();
    m_lonValues.clear();
    m_xValuesHeading.clear();
    m_headingValues.clear();
    QVector<double> longitudeXValues;
    const bool latitudeAvailable = m_dataStoragePtr->getValues(
        m_latName, false, m_xValues, m_latValues);
    const bool longitudeAvailable = m_dataStoragePtr->getValues(
        m_lonName, false, longitudeXValues, m_lonValues);
    m_dataStoragePtr->getValues(
        m_headingName, false, m_xValuesHeading, m_headingValues);

    if (latitudeAvailable && longitudeAvailable
        && !m_latValues.empty() && !m_lonValues.empty())
    {
        scaleData();
        const int count = qMin(m_latValues.size(), m_lonValues.size());
        if (m_validIndex >= count) {
            QLOG_INFO() << "LogAnalysisMap: no valid GPS coordinates";
            m_mapBackend->SetLogTrail({});
            return;
        }
        QVector<MapCoordinate> trail;
        trail.reserve(count - m_validIndex);
        for (int i = m_validIndex; i < count; ++i) {
            const double latitude = m_latValues.at(i);
            const double longitude = m_lonValues.at(i);
            if (qIsFinite(latitude) && qIsFinite(longitude)
                && (latitude != 0.0 || longitude != 0.0)) {
                trail.append({latitude, longitude, 10.0});
            }
        }
        m_mapBackend->SetLogTrail(trail);
    }
    else
    {
        QLOG_INFO() << "LogAnalysisMap: No GPS data - no trail";
        m_mapBackend->SetLogTrail({});
    }
}

void LogAnalysisMap::setUavCursor(int index)
{
    if (!m_mapBackend || m_latValues.isEmpty() || m_lonValues.isEmpty()) {
        return;
    }
    auto bestGpsIndex = findBestIndexMatch(index, m_xValues);
    const int coordinateCount = qMin(m_latValues.size(), m_lonValues.size());
    if (m_validIndex >= coordinateCount) {
        return;
    }
    bestGpsIndex = qBound(m_validIndex, bestGpsIndex, coordinateCount - 1);
    const auto coordinateIsValid = [this](int coordinateIndex) {
        const double latitude = m_latValues.at(coordinateIndex);
        const double longitude = m_lonValues.at(coordinateIndex);
        return qIsFinite(latitude) && qIsFinite(longitude)
            && latitude >= -90.0 && latitude <= 90.0
            && longitude >= -180.0 && longitude <= 180.0
            && (latitude != 0.0 || longitude != 0.0);
    };
    if (!coordinateIsValid(bestGpsIndex)) {
        int nearestValid = -1;
        for (int offset = 1; offset < coordinateCount; ++offset) {
            const int before = bestGpsIndex - offset;
            const int after = bestGpsIndex + offset;
            if (before >= m_validIndex && coordinateIsValid(before)) {
                nearestValid = before;
                break;
            }
            if (after < coordinateCount && coordinateIsValid(after)) {
                nearestValid = after;
                break;
            }
        }
        if (nearestValid < 0) {
            return;
        }
        bestGpsIndex = nearestValid;
    }
    double heading = 0.0;
    if (!m_xValuesHeading.isEmpty() && !m_headingValues.isEmpty()) {
        const int bestHeadingIndex = qBound(
            0, findBestIndexMatch(index, m_xValuesHeading),
            m_headingValues.size() - 1);
        heading = m_headingValues.at(bestHeadingIndex);
    }
    m_mapBackend->SetLogCursor(
        {m_latValues.at(bestGpsIndex), m_lonValues.at(bestGpsIndex), 10.0},
        heading);
}

void LogAnalysisMap::loadSettings()
{
    QSettings settings;
    settings.beginGroup("LOGANALYSIS_MAP_SETTINGS");

    restoreGeometry(settings.value("GEOMETRY").toByteArray());

    settings.endGroup();
}

void LogAnalysisMap::saveSettings()
{
    QSettings settings;
    settings.beginGroup("LOGANALYSIS_MAP_SETTINGS");

    settings.setValue("GEOMETRY", saveGeometry());

    settings.endGroup();
}

void LogAnalysisMap::findDataNames()
{
    if (m_dataStoragePtr.isNull())
    {
        QLOG_WARN() << "LogAnalysisMap: m_dataStoragePtr is not set. Can not create UAV trail!";
        return;
    }

    // fetch all datatypes and try to find GPS and ATT data
    QVector<LogdataStorage::dataType> types = m_dataStoragePtr->getAllDataTypes();
    bool foundGps = false;
    bool foundAtt = false;
    for (const LogdataStorage::dataType &val : types)
    {
        if(!foundGps &&  (val.m_name == "GPS" || val.m_name == "GPS_RAW_INT")) // TODO MAgic Constant
        {
            m_gpsType = val;
            foundGps = true;
        }
        if(!foundAtt && (val.m_name == "ATT" || val.m_name == "ATTITUDE")) // TODO MAgic Constant
        {
            m_attType = val;
            foundAtt = true;
        }
        if (foundAtt && foundGps)
        {
            break;
        }
    }
    // check if type is valid
    if (m_gpsType.m_length == 0)
    {
        QLOG_INFO() << "LogAnalysisMap: No GPS data found. Can't create trail.";
        return;
    }
    if (m_attType.m_length == 0)
    {
        QLOG_INFO() << "LogAnalysisMap: No heading data found.";
    }

    m_latName = m_gpsType.m_name;
    m_latName.append('.');
    m_lonName = m_latName;

    for (const QString &label : m_gpsType.m_labels)
    {
        if (label.compare("lat", Qt::CaseInsensitive) == 0)// TODO MAgic Constant
        {
            m_latName.append(label);
        }
        if (label.compare("lng", Qt::CaseInsensitive) == 0)// TODO MAgic Constant
        {
            m_lonName.append(label);
        }
        if (label.compare("lon", Qt::CaseInsensitive) == 0)// TODO MAgic Constant
        {
            m_lonName.append(label);
        }
    }

    m_headingName = m_attType.m_name;
    m_headingName.append('.');
    for(const QString &label : m_attType.m_labels)
    {
        if (label.compare("yaw", Qt::CaseInsensitive) == 0)   // TODO Magic constant
        {
            m_headingName.append(label);
        }
    }

    QLOG_DEBUG() << "LogAnalysisMap: datamodel name for Gps latitude:" << m_latName;
    QLOG_DEBUG() << "LogAnalysisMap: datamodel name for Gps longitudes:" << m_lonName;
    QLOG_DEBUG() << "LogAnalysisMap: datamodel name for heading:" << m_headingName;
}

int LogAnalysisMap::findBestIndexMatch(int index, const QVector<double> &data)
{
    if (data.isEmpty()) {
        return 0;
    }
    int intervalSize = data.size();
    int intervalStart = 0;
    int middle = 0;

    if (index <= 0.0)
    {
        middle = 0;
    }
    else if (index >= data.back())
    {
        middle = data.size() - 1;
    }
    else
    {
        while (intervalSize > 1)
        {
            middle = intervalStart + intervalSize / 2;
            auto temp = static_cast<int>(data.at(middle) + 0.5);
            if (index > temp)
            {
                intervalStart = middle;
            }
            else if (index == temp)
            {
                break;
            }
            intervalSize = intervalSize / 2;
        }
    }

    return middle;
}

void LogAnalysisMap::scaleData()
{
    double latVal = 0.0;
    double lonVal = 0.0;
    double scaling = 1.0;

    // first find value which is not 0 for lat and lon and store its index
    const int coordinateCount = qMin(
        m_latValues.size(), m_lonValues.size());
    for (m_validIndex = 0; m_validIndex < coordinateCount; ++m_validIndex)
    {
        const double latitude = m_latValues.at(m_validIndex);
        const double longitude = m_lonValues.at(m_validIndex);
        if (qIsFinite(latitude) && qIsFinite(longitude)
            && (latitude != 0.0 || longitude != 0.0))
        {
            latVal = latitude;
            lonVal = longitude;
            break;
        }
    }

    // ... then find the value which scales lat and lon into the 180, -180 interval...
    while (((latVal / scaling) > 180.0) || ((latVal / scaling) < -180.0))
    {
        scaling *= 10.0;
    }
    while (((lonVal / scaling) > 180.0) || ((lonVal / scaling) < -180.0))
    {
        scaling *= 10.0;
    }

    // ...then scale the whole data arrays
    if (scaling > 1.0)
    {
        QLOG_INFO() << "LogAnalysisMap: Scale the GPS-Values by " << scaling;
        for(double &val: m_latValues)
        {
            val /= scaling;
        }

        for(double &val: m_lonValues)
        {
            val /= scaling;
        }
    }
}

void LogAnalysisMap::setMapZoom(int value)
{
    if (m_mapBackend) {
        m_mapBackend->SetZoom(
            static_cast<double>(value) / s_MapScaling);
    }
}

void LogAnalysisMap::setZoom(int value)
{
    mp_Ui->zoomSlider->setValue(value * s_MapScaling);
}
