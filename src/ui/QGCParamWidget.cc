/*=====================================================================

QGroundControl Open Source Ground Control Station

(c) 2009, 2010 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>

This file is part of the QGROUNDCONTROL project

    QGROUNDCONTROL is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    QGROUNDCONTROL is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with QGROUNDCONTROL. If not, see <http://www.gnu.org/licenses/>.

======================================================================*/
/**
 * @file
 *   @brief Implementation of class QGCParamWidget
 *   @author Lorenz Meier <mail@qgroundcontrol.org>
 */

#include "logging.h"
#include "DownloadRemoteParamsDialog.h"
#include "QGCParamWidget.h"
#include "UASInterface.h"
#include "MainWindow.h"
#include "QGC.h"
#include "LinkManager.h"
#include "comm/VehicleCommandService.h"

#include <cmath>
#include <float.h>
#include <QGridLayout>
#include <QPushButton>
#include <QFileDialog>
#include <QFile>
#include <QList>
#include <QTime>
#include <QSettings>
#include <QMessageBox>
#include <QApplication>

namespace {
bool parameterValuesEqual(const QVariant &left, const QVariant &right)
{
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    if (leftOk && rightOk) {
        return leftValue == rightValue
            || std::abs(leftValue - rightValue) <= 1.0e-6;
    }
    return left == right;
}
}

/**
 * @param uas MAV to set the parameters on
 * @param parent Parent widget
 */
QGCParamWidget::QGCParamWidget(UASInterface* uas, QWidget *parent) :
    QWidget(parent),
    mav(uas),
    m_parameterManager(LinkManager::instance()->parameterManager()),
    components(new QMap<int, QTreeWidgetItem*>())
{
    // Load settings
    loadSettings();

    // Load default values and tooltips
    if (uas) {
        loadParameterInfoCSV(
            uas->getAutopilotTypeName(), uas->getSystemTypeName());
    }
    if (m_parameterManager) {
        m_parameterManager->setParamMetadata(
            paramMin, paramMax, paramDefault, paramToolTips);
    }

    // Create tree widget
    tree = new QTreeWidget(this);
    statusLabel = new QLabel();
    statusLabel->setAutoFillBackground(true);
    tree->setColumnWidth(70, 30);

    // Set tree widget as widget onto this component
    QGridLayout* horizontalLayout;
    //form->setAutoFillBackground(false);
    horizontalLayout = new QGridLayout(this);
    horizontalLayout->setHorizontalSpacing(6);
    horizontalLayout->setVerticalSpacing(6);
    horizontalLayout->setMargin(0);
    horizontalLayout->setSizeConstraint(QLayout::SetMinimumSize);
    //horizontalLayout->setSizeConstraint( QLayout::SetFixedSize );

    // Parameter tree
    horizontalLayout->addWidget(tree, 0, 0, 1, 3);

    // Status line
    statusLabel->setText(tr("Click refresh to download parameters"));
    horizontalLayout->addWidget(statusLabel, 1, 0, 1, 3);


    // BUTTONS
    QPushButton* refreshButton = new QPushButton(tr("Get"));
    refreshButton->setToolTip(tr("Load parameters currently in non-permanent memory of aircraft."));
    refreshButton->setWhatsThis(tr("Load parameters currently in non-permanent memory of aircraft."));
    connect(refreshButton, SIGNAL(clicked()), this, SLOT(requestParameterList()));
    horizontalLayout->addWidget(refreshButton, 2, 0);

    QPushButton* setButton = new QPushButton(tr("Set"));
    setButton->setToolTip(tr("Set current parameters in non-permanent onboard memory"));
    setButton->setWhatsThis(tr("Set current parameters in non-permanent onboard memory"));
    connect(setButton, SIGNAL(clicked()), this, SLOT(setParameters()));
    horizontalLayout->addWidget(setButton, 2, 1);

    QPushButton* writeButton = new QPushButton(tr("Write (ROM)"));
    writeButton->setToolTip(tr("Copy current parameters in non-permanent memory of the aircraft to permanent memory. Transmit your parameters first to write these."));
    writeButton->setWhatsThis(tr("Copy current parameters in non-permanent memory of the aircraft to permanent memory. Transmit your parameters first to write these."));
    connect(writeButton, SIGNAL(clicked()), this, SLOT(writeParameters()));
    horizontalLayout->addWidget(writeButton, 2, 2);

    QPushButton* loadFileButton = new QPushButton(tr("Load File"));
    loadFileButton->setToolTip(tr("Load parameters from a file on this computer in the view. To write them to the aircraft, use transmit after loading them."));
    loadFileButton->setWhatsThis(tr("Load parameters from a file on this computer in the view. To write them to the aircraft, use transmit after loading them."));
    connect(loadFileButton, SIGNAL(clicked()), this, SLOT(loadParametersButtonClicked()));
    horizontalLayout->addWidget(loadFileButton, 3, 0);

    QPushButton* saveFileButton = new QPushButton(tr("Save File"));
    saveFileButton->setToolTip(tr("Save parameters in this view to a file on this computer."));
    saveFileButton->setWhatsThis(tr("Save parameters in this view to a file on this computer."));
    connect(saveFileButton, SIGNAL(clicked()), this, SLOT(saveParametersButtonClicked()));
    horizontalLayout->addWidget(saveFileButton, 3, 1);

    QPushButton* readButton = new QPushButton(tr("Read (ROM)"));
    readButton->setToolTip(tr("Copy parameters from permanent memory to non-permanent current memory of aircraft. DOES NOT update the parameters in this view, click refresh after copying them to get them."));
    readButton->setWhatsThis(tr("Copy parameters from permanent memory to non-permanent current memory of aircraft. DOES NOT update the parameters in this view, click refresh after copying them to get them."));
    connect(readButton, SIGNAL(clicked()), this, SLOT(readParameters()));
    horizontalLayout->addWidget(readButton, 3, 2);

    // Set correct vertical scaling
    horizontalLayout->setRowStretch(0, 100);
    horizontalLayout->setRowStretch(1, 10);
    horizontalLayout->setRowStretch(2, 10);
    horizontalLayout->setRowStretch(3, 10);

    // Set layout
    this->setLayout(horizontalLayout);

    // Set header
    QStringList headerItems;
    headerItems.append("Parameter");
    headerItems.append("Value");
    tree->setHeaderLabels(headerItems);
    tree->setColumnCount(2);
    tree->setExpandsOnDoubleClick(true);

    // Connect signals/slots
    if (m_parameterManager) {
        connect(this, &QGCParamWidget::parameterChanged,
                m_parameterManager, &QGCUASParamManager::setParameter);
    }
    connect(tree, SIGNAL(itemChanged(QTreeWidgetItem*,int)), this, SLOT(parameterItemChanged(QTreeWidgetItem*,int)));

    // The application-owned exact-target facade is the only parameter source.
    // Full refresh values are replayed only after the staged store commits.
    if (m_parameterManager) {
        connect(m_parameterManager,
                QOverload<int, QString, QVariant>::of(
                    &QGCUASParamManager::parameterChanged),
                this,
                [this](int component, const QString &name,
                       const QVariant &value) {
            addParameter(mav ? mav->getUASID() : 0,
                         component, name, value);
        });
        connect(m_parameterManager,
                &QGCUASParamManager::parameterSnapshotAboutToChange,
                this, &QGCParamWidget::clear);
        connect(m_parameterManager,
                &QGCUASParamManager::parameterWriteAcknowledged,
                this,
                [this](int component, const QString &name,
                       const QVariant &value, int type) {
            Q_UNUSED(component)
            Q_UNUSED(name)
            Q_UNUSED(value)
            Q_UNUSED(type)
            statusLabel->setText(tr("SUCCESS: WROTE PARAMETER"));
            QPalette palette = statusLabel->palette();
            palette.setColor(backgroundRole(), QGC::colorGreen);
            statusLabel->setPalette(palette);
        });
        connect(m_parameterManager,
                &QGCUASParamManager::parameterWriteFailed,
                this,
                [this](qulonglong transactionId, qulonglong batchId,
                       int component, const QString &name,
                       int reason, const QString &message) {
            Q_UNUSED(transactionId)
            Q_UNUSED(batchId)
            Q_UNUSED(component)
            Q_UNUSED(reason)
            statusLabel->setText(
                tr("FAILURE: %1: %2").arg(name, message));
            QPalette palette = statusLabel->palette();
            palette.setColor(backgroundRole(), QGC::colorRed);
            statusLabel->setPalette(palette);
        });
        connect(m_parameterManager,
                &QGCUASParamManager::parameterWriteCancelled,
                this,
                [this](qulonglong transactionId, qulonglong batchId,
                       int component, const QString &name) {
            Q_UNUSED(transactionId)
            Q_UNUSED(batchId)
            Q_UNUSED(component)
            statusLabel->setText(
                tr("CANCELLED: %1 (vehicle target changed)").arg(name));
        });
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListLoadStarted,
                this, [this]() {
            transmissionListMode = true;
            m_parameterListReceivedCount = 0;
            m_parameterListReportedCount = 0;
            setParameterListReady(false);
            statusLabel->setText(tr("Requested param list.. waiting"));
            emit parameterListLoadStarted();
        });
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListUpToDate,
                this, [this](int component) {
            transmissionListMode = false;
            m_parameterListReceivedCount =
                m_parameterManager->parameterListReceivedCount();
            m_parameterListReportedCount =
                m_parameterManager->parameterListReportedCount();
            setParameterListReady(true);
            statusLabel->setText(tr("All received."));
            emit parameterListUpToDate(component);
        });
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListLoadFailed,
                this, [this](const QString &reason) {
            transmissionListMode = false;
            setParameterListReady(
                m_parameterManager->parameterListReady());
            statusLabel->setText(reason);
            emit parameterListLoadFailed(reason);
        });
        connect(m_parameterManager,
                &QGCUASParamManager::parameterListLoadCanceled,
                this, [this]() {
            transmissionListMode = false;
            setParameterListReady(
                m_parameterManager->parameterListReady());
            statusLabel->setText(tr("Parameter loading stopped."));
            emit parameterListLoadCanceled();
        });
    }

    // Connect retransmission guard
    connect(&retransmissionTimer, SIGNAL(timeout()), this, SLOT(retransmissionGuardTick()));
    initialParamTimer = new QTimer(this);
    connect(initialParamTimer,SIGNAL(timeout()),this,SLOT(initialParamCheckTick()));

    // A dock is a view, not the owner of a transfer. Reuse the committed
    // snapshot and wait for an explicit refresh or the shared setup workflow.
    if (m_parameterManager) {
        const QList<int> componentIds = m_parameterManager->getComponentIds();
        for (int component : componentIds) {
            const QList<QString> names =
                m_parameterManager->getParameterNames(component);
            for (const QString &name : names) {
                addParameter(mav ? mav->getUASID() : 0,
                             component, name,
                             m_parameterManager->getParameterValue(
                                 component, name));
            }
        }
        setParameterListReady(m_parameterManager->parameterListReady());
    }
}

QGCParamWidget::~QGCParamWidget()
{
    qDeleteAll(paramGroups);
    qDeleteAll(parameters);
    qDeleteAll(changedValues);
    qDeleteAll(transmissionMissingPackets);
    qDeleteAll(transmissionMissingWriteAckPackets);
    delete components;
}

void QGCParamWidget::setParameterListReady(bool ready)
{
    if (m_parameterListReady == ready) {
        return;
    }
    m_parameterListReady = ready;
    emit parameterListReadyChanged(ready);
}

QString QGCParamWidget::summaryInfoFromFile(const QString &filename)
{
    QString summaryText;

    if(filename.length() == 0) {
        return QString();
    }

    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly))
    {
        return QString();
    }

    QString paramString = file.readAll();
    file.close();

    QStringList paramSplit = paramString.split(QGC::paramLineSplitRegExp());

    foreach (QString paramLine, paramSplit) {
        if (paramLine.startsWith("#")) {
            QLOG_DEBUG() << "Comment: " << paramLine;
            // removes the '#' and any whites space before or after
            summaryText.append(paramLine.remove(0,1).trimmed() + "\n");

        } else {
            break; // Summary Complete
        }

    }
    QLOG_DEBUG() << "param file summary: " << summaryText;
    return summaryText;
}

bool QGCParamWidget::loadParamsFromFile(const QString &filename,ParamFileType type)
{
    if (type == CommaSeperatedValues)
    {
        //Load the filename, it will be a CSV of PARAM,VALUE\n
        // APM Param file support (doesn't support extra fields of TAB version)
        QFile paramfile(filename);
        if (!paramfile.open(QIODevice::ReadOnly))
        {
            return false;
        }

        while (!paramfile.atEnd())
        {
            QString line = paramfile.readLine();
            if (!line.startsWith("#"))
            {
                if (line.indexOf(QGC::paramSplitRegExp()) != -1)
                {
                    setParameter(1,line.split(QGC::paramSplitRegExp())[0],line.split(QGC::paramSplitRegExp())[1].toFloat());
                }
            }
        }
        paramfile.close();
    }
    else if (type == TabSeperatedValues)
    {
        // Pixhawk Param file support
        QFile file(filename);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;

        bool userWarned = false;

        QTextStream in(&file);
        while (!in.atEnd()) {
            QString line = in.readLine();
            if (!line.startsWith("#")) {
                QStringList wpParams = line.split("\t");
                if (wpParams.size() == 5) {
                    // Only load parameters for right mav
                    if (!userWarned && (mav->getUASID() != wpParams.at(0).toInt())) {
                        MainWindow::instance()->showCriticalMessage(tr("Parameter loading warning"), tr("The parameters from the file %1 have been saved from system %2, but the currently selected system has the ID %3. If this is unintentional, please click on <READ> to revert to the parameters that are currently onboard").arg(filename).arg(wpParams.at(0).toInt()).arg(mav->getUASID()));
                        userWarned = true;
                    }

                    bool changed = false;
                    int component = wpParams.at(1).toInt();
                    QString parameterName = wpParams.at(2);
                    if (!parameters.contains(component) ||
                            fabs((static_cast<float>(parameters.value(component)->value(parameterName, wpParams.at(3).toDouble()).toDouble())) - (wpParams.at(3).toDouble())) > 2.0f * FLT_EPSILON) {
                        changed = true;
                        QLOG_DEBUG() << "Changed" << parameterName << "VAL" << wpParams.at(3).toDouble();
                    }

                    // Set parameter value

                    // Create changed values data structure if necessary
                    if (changed && !changedValues.contains(wpParams.at(1).toInt())) {
                        changedValues.insert(wpParams.at(1).toInt(), new QMap<QString, QVariant>());
                    }

                    // Add to changed values
                    if (changed && changedValues.value(wpParams.at(1).toInt())->contains(wpParams.at(2))) {
                        changedValues.value(wpParams.at(1).toInt())->remove(wpParams.at(2));
                    }

                    switch (wpParams.at(4).toUInt())
                    {
                    case (int)MAV_PARAM_TYPE_REAL32:
                        addParameter(wpParams.at(0).toInt(), wpParams.at(1).toInt(), wpParams.at(2), wpParams.at(3).toFloat());
                        if (changed) {
                            changedValues.value(wpParams.at(1).toInt())->insert(wpParams.at(2), wpParams.at(3).toFloat());
                            setParameter(wpParams.at(1).toInt(), wpParams.at(2), wpParams.at(3).toFloat());
                            QLOG_DEBUG() << "FLOAT PARAM CHANGED";
                        }
                        break;
                    case (int)MAV_PARAM_TYPE_UINT32:
                        addParameter(wpParams.at(0).toInt(), wpParams.at(1).toInt(), wpParams.at(2), wpParams.at(3).toUInt());
                        if (changed) {
                            changedValues.value(wpParams.at(1).toInt())->insert(wpParams.at(2), wpParams.at(3).toUInt());
                            setParameter(wpParams.at(1).toInt(), wpParams.at(2), QVariant(wpParams.at(3).toUInt()));
                        }
                        break;
                    case (int)MAV_PARAM_TYPE_INT32:
                        addParameter(wpParams.at(0).toInt(), wpParams.at(1).toInt(), wpParams.at(2), wpParams.at(3).toInt());
                        if (changed) {
                            changedValues.value(wpParams.at(1).toInt())->insert(wpParams.at(2), wpParams.at(3).toInt());
                            setParameter(wpParams.at(1).toInt(), wpParams.at(2), QVariant(wpParams.at(3).toInt()));
                        }
                        break;
                    default:
                        QLOG_DEBUG() << "FAILED LOADING PARAM" << wpParams.at(2) << "NO KNOWN DATA TYPE";
                    }

                    //QLOG_DEBUG() << "MARKING COMP" << wpParams.at(1).toInt() << "PARAM" << wpParams.at(2) << "VALUE" << (float)wpParams.at(3).toDouble() << "AS CHANGED";

                    // Mark in UI


                }
            }
        }
        file.close();
    }
    return true;
}
void QGCParamWidget::saveParamsToFile(const QString &filename,ParamFileType type)
{
    if (type == CommaSeperatedValues)
    {
    }
    else if (type == TabSeperatedValues)
    {
        QFile file(filename);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return;
        }
        QTextStream in(&file);

        in << "# Onboard parameters for system " << mav->getUASName() << "\n";
        in << "#\n";
        in << "# MAV ID  COMPONENT ID  PARAM NAME  VALUE (FLOAT)\n";

        // Iterate through all components, through all parameters and emit them
        QMap<int, QMap<QString, QVariant>*>::iterator i;
        for (i = parameters.begin(); i != parameters.end(); ++i) {
            // Iterate through the parameters of the component
            int compid = i.key();
            QMap<QString, QVariant>* comp = i.value();
            {
                QMap<QString, QVariant>::iterator j;
                for (j = comp->begin(); j != comp->end(); ++j)
                {
                    QString paramValue("%1");
                    QString paramType("%1");
                    switch (static_cast<QMetaType::Type>(j.value().type()))
                    {
                    case QMetaType::Int:
                        paramValue = paramValue.arg(j.value().toInt());
                        paramType = paramType.arg(MAV_PARAM_TYPE_INT32);
                        break;
                    case QMetaType::UInt:
                        paramValue = paramValue.arg(j.value().toUInt());
                        paramType = paramType.arg(MAV_PARAM_TYPE_UINT32);
                        break;
                    case QMetaType::Double:
                    case QMetaType::Float:
                        paramValue = paramValue.arg(j.value().toDouble(), 25, 'g', 12);
                        paramType = paramType.arg(MAV_PARAM_TYPE_REAL32);
                        break;
                    default:
                        qCritical() << "ABORTED PARAM WRITE TO FILE, NO VALID QVARIANT TYPE" << j.value();
                        return;
                    }
                    in << mav->getUASID() << "\t" << compid << "\t" << j.key() << "\t" << paramValue << "\t" << paramType << "\n";
                    in.flush();
                }
            }
        }
        file.close();
    }
}

void QGCParamWidget::loadSettings()
{
    QSettings settings;
    settings.beginGroup("QGC_MAVLINK_PROTOCOL");
    bool ok;
    int temp = settings.value("PARAMETER_RETRANSMISSION_TIMEOUT", retransmissionTimeout).toInt(&ok);
    if (ok) retransmissionTimeout = temp;
    temp = settings.value("PARAMETER_REWRITE_TIMEOUT", rewriteTimeout).toInt(&ok);
    if (ok) rewriteTimeout = temp;
    settings.endGroup();
}

void QGCParamWidget::loadParameterInfoCSV(const QString& autopilot, const QString& airframe)
{
    Q_UNUSED(airframe);

    QLOG_DEBUG() << "ATTEMPTING TO LOAD CSV";

    QDir appDir = QApplication::applicationDirPath();
    appDir.cd("files");
    QString fileName = QString("%1/%2/parameter_tooltips/tooltips.txt").arg(appDir.canonicalPath()).arg(autopilot.toLower());
    QFile paramMetaFile(fileName);

    QLOG_DEBUG() << "AUTOPILOT:" << autopilot;
    QLOG_DEBUG() << "FILENAME: " << fileName;

    // Load CSV data
    if (!paramMetaFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        //QLOG_DEBUG() << "COULD NOT OPEN PARAM META INFO FILE:" << fileName;
        return;
    }

    // Extract header

    // Read in values
    // Find all keys
    QTextStream in(&paramMetaFile);

    // First line is header
    // there might be more lines, but the first
    // line is assumed to be at least header
    QString header = in.readLine();

    // Ignore top-level comment lines
    while (header.startsWith('#') || header.startsWith('/')
           || header.startsWith('=') || header.startsWith('^'))
    {
        header = in.readLine();
    }

    bool charRead = false;
    QString separator = "";
    QList<QChar> sepCandidates;
    sepCandidates << '\t';
    sepCandidates << ',';
    sepCandidates << ';';
    //sepCandidates << ' ';
    sepCandidates << '~';
    sepCandidates << '|';

    // Iterate until separator is found
    // or full header is parsed
    for (int i = 0; i < header.length(); i++)
    {
        if (sepCandidates.contains(header.at(i)))
        {
            // Separator found
            if (charRead)
            {
                separator += header[i];
            }
        }
        else
        {
            // Char found
            charRead = true;
            // If the separator is not empty, this char
            // has been read after a separator, so detection
            // is now complete
            if (separator != "") break;
        }
    }

    bool stripFirstSeparator = false;
    bool stripLastSeparator = false;

    // Figure out if the lines start with the separator (e.g. wiki syntax)
    if (header.startsWith(separator)) stripFirstSeparator = true;

    // Figure out if the lines end with the separator (e.g. wiki syntax)
    if (header.endsWith(separator)) stripLastSeparator = true;

    QString out = separator;
    out.replace("\t", "<tab>");
    //QLOG_DEBUG() << " Separator: \"" << out << "\"";
    //QLOG_DEBUG() << "READING CSV:" << header;


    // Read data
    while (!in.atEnd())
    {
        QString line = in.readLine();

        //QLOG_DEBUG() << "LINE PRE-STRIP" << line;

        // Strip separtors if necessary
        if (stripFirstSeparator) line.remove(0, separator.length());
        if (stripLastSeparator) line.remove(line.length()-separator.length(), line.length()-1);

        //QLOG_DEBUG() << "LINE POST-STRIP" << line;

        // Keep empty parts here - we still have to act on them
#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
        QStringList parts = line.split(separator, QString::KeepEmptyParts);
#else
        QStringList parts = line.split(separator, Qt::KeepEmptyParts);
#endif

        // Each line is:
        // variable name, Min, Max, Default, Multiplier, Enabled (0 = no, 1 = yes), Comment


        // Fill in min, max and default values
        if (parts.count() > 1)
        {
            // min
            paramMin.insert(parts.at(0).trimmed(), parts.at(1).toDouble());
        }
        if (parts.count() > 2)
        {
            // max
            paramMax.insert(parts.at(0).trimmed(), parts.at(2).toDouble());
        }
        if (parts.count() > 3)
        {
            // default
            paramDefault.insert(parts.at(0).trimmed(), parts.at(3).toDouble());
        }
        // IGNORING 4 and 5 for now
        if (parts.count() > 6)
        {
            // tooltip
            paramToolTips.insert(parts.at(0).trimmed(), parts.at(6).trimmed());
            QLOG_DEBUG() << "PARAM META:" << parts.at(0).trimmed();
        }
    }
}

/**
 * @return The MAV of this widget. Unless the MAV object has been destroyed, this
 *         pointer is never zero.
 */
UASInterface* QGCParamWidget::getUAS()
{
    return mav.data();
}

void QGCParamWidget::setUAS(UASInterface *uas)
{
    if (mav == uas) {
        return;
    }
    mav = uas;
    paramMin.clear();
    paramMax.clear();
    paramDefault.clear();
    paramToolTips.clear();
    if (mav) {
        loadParameterInfoCSV(
            mav->getAutopilotTypeName(), mav->getSystemTypeName());
    }
    if (m_parameterManager) {
        m_parameterManager->setParamMetadata(
            paramMin, paramMax, paramDefault, paramToolTips);
    }
}

/**
 *
 * @param uas System which has the component
 * @param component id of the component
 * @param componentName human friendly name of the component
 */
void QGCParamWidget::addComponent(int uas, int component, QString componentName)
{
    Q_UNUSED(uas);
    if (components->contains(component)) {
        // Update existing
        components->value(component)->setData(0, Qt::DisplayRole, QString("%1 (#%2)").arg(componentName).arg(component));
        //components->value(component)->setData(1, Qt::DisplayRole, QString::number(component));
        components->value(component)->setFirstColumnSpanned(true);
    } else {
        // Add new
        QStringList list(QString("%1 (#%2)").arg(componentName).arg(component));
        QTreeWidgetItem* comp = new QTreeWidgetItem(list);
        comp->setFirstColumnSpanned(true);
        components->insert(component, comp);
        // Create grouping and update maps
        paramGroups.insert(component, new QMap<QString, QTreeWidgetItem*>());
        tree->addTopLevelItem(comp);
        tree->update();
        // Create map in parameters
        if (!parameters.contains(component)) {
            parameters.insert(component, new QMap<QString, QVariant>());
        }
        // Create map in changed parameters
        if (!changedValues.contains(component)) {
            changedValues.insert(component, new QMap<QString, QVariant>());
        }
    }
}

/**
 * @param uas System which has the component
 * @param component id of the component
 * @param parameterName human friendly name of the parameter
 */
void QGCParamWidget::addParameter(int uas, int component, int paramCount, int paramId, QString parameterName, QVariant value)
{
    addParameter(uas, component, parameterName, value);

    // Missing packets list has to be instantiated for all components
    if (!transmissionMissingPackets.contains(component)) {
        transmissionMissingPackets.insert(component, new QList<int>());
    }

    // PARAM_REQUEST_LIST is addressed to the primary autopilot. Unsolicited
    // AP_Periph/camera packets may arrive concurrently, but must not expand
    // or complete this load transaction.
    const bool requestedListPacket = transmissionListMode
        && component == MAV_COMP_ID_PRIMARY;
    if (requestedListPacket) {
        // Only accept the list size once on the first packet from
        // each component
        if (!transmissionListSizeKnown.contains(component))
        {
            // Mark list size as known
            transmissionListSizeKnown.insert(component, true);
            m_parameterListReportedCount = qMax(0, paramCount);
            m_parameterListReceivedCount = 0;

            // Mark all parameters as missing
            for (int i = 0; i < paramCount; ++i)
            {
                if (!transmissionMissingPackets.value(component)->contains(i))
                {
                    transmissionMissingPackets.value(component)->append(i);
                }
            }

            // There is only one transmission timeout for all components
            // since components do not manage their transmission,
            // the longest timeout is safe for all components.
            quint64 thisTransmissionTimeout = QGC::groundTimeMilliseconds() + ((paramCount)*retransmissionTimeout);
            if (thisTransmissionTimeout > transmissionTimeout)
            {
                transmissionTimeout = thisTransmissionTimeout;
            }
        }

        // Start retransmission guard
        // or reset timer
        setRetransmissionGuardEnabled(true);
    }

    // Mark this parameter as received in read list
    int index = transmissionMissingPackets.value(component)->indexOf(paramId);
    // If the MAV sent the parameter without request, it wont be in missing list
    if (index != -1) transmissionMissingPackets.value(component)->removeAt(index);
    if (requestedListPacket && m_parameterListReportedCount > 0) {
        m_parameterListReceivedCount = qBound(
            0,
            m_parameterListReportedCount
                - transmissionMissingPackets.value(component)->count(),
            m_parameterListReportedCount);
    }

    bool justWritten = false;
    bool writeMismatch = false;
    //bool lastWritten = false;
    // Mark this parameter as received in write ACK list
    QMap<QString, QVariant>* map = transmissionMissingWriteAckPackets.value(component);
    QVariant expectedWriteValue;
    if (map && map->contains(parameterName))
    {
        justWritten = true;
        expectedWriteValue = map->value(parameterName);
        if (!parameterValuesEqual(expectedWriteValue, value))
        {
            writeMismatch = true;
        } else {
            map->remove(parameterName);
        }
    }

    int missCount = 0;
    if (transmissionListMode) {
        const QList<int> *primaryMissing = transmissionMissingPackets.value(
            MAV_COMP_ID_PRIMARY, nullptr);
        missCount = primaryMissing ? primaryMissing->count() : 0;
    } else {
        foreach (int key, transmissionMissingPackets.keys())
        {
            missCount += transmissionMissingPackets.value(key)->count();
        }
    }

    int missWriteCount = 0;
    foreach (int key, transmissionMissingWriteAckPackets.keys())
    {
        missWriteCount += transmissionMissingWriteAckPackets.value(key)->count();
    }

    if (justWritten && !writeMismatch && missWriteCount == 0)
    {
        // Just wrote one and count went to 0 - this was the last missing write parameter
        statusLabel->setText(tr("SUCCESS: WROTE ALL PARAMETERS"));
        QPalette pal = statusLabel->palette();
        pal.setColor(backgroundRole(), QGC::colorGreen);
        statusLabel->setPalette(pal);
    } else if (justWritten && !writeMismatch)
    {
        statusLabel->setText(tr("SUCCESS: Wrote %2 (#%1/%4): %3").arg(paramId+1).arg(parameterName).arg(value.toDouble()).arg(paramCount));
        QPalette pal = statusLabel->palette();
        pal.setColor(backgroundRole(), QGC::colorGreen);
        statusLabel->setPalette(pal);
    } else if (justWritten && writeMismatch)
    {
        // Mismatch, tell user
        QPalette pal = statusLabel->palette();
        pal.setColor(backgroundRole(), QGC::colorRed);
        statusLabel->setPalette(pal);
        statusLabel->setText(
            tr("FAILURE: Wrote %1: sent %2 != onboard %3")
                .arg(parameterName)
                .arg(expectedWriteValue.toDouble())
                .arg(value.toDouble()));
    }
    else
    {
        if (missCount > 0)
        {
            QPalette pal = statusLabel->palette();
            pal.setColor(backgroundRole(), QGC::colorOrange);
            statusLabel->setPalette(pal);
        }
        else
        {
            QPalette pal = statusLabel->palette();
            pal.setColor(backgroundRole(), QGC::colorGreen);
            statusLabel->setPalette(pal);
        }
        QString val = QString("%1").arg(value.toFloat(), 5, 'f', 1, QChar(' '));
        //statusLabel->setText(tr("OK: %1 %2 #%3/%4, %5 miss").arg(parameterName).arg(val).arg(paramId+1).arg(paramCount).arg(missCount));
        if (missCount == 0)
        {
            // Transmission done
            QTime time = QTime::currentTime();
            QString timeString = time.toString();
            statusLabel->setText(tr("All received. (updated at %1)").arg(timeString));
        }
        else
        {
            // Transmission in progress
            statusLabel->setText(tr("OK: %1 %2 (%3/%4)").arg(parameterName).arg(val).arg(paramCount-missCount).arg(paramCount));
        }
    }

    // Check if the requested full list was completed before clearing the
    // transfer-mode flag. Backstage pages use this terminal signal instead of
    // guessing readiness from an arbitrary last packet index.
    const bool completedParameterList = requestedListPacket
        && component == MAV_COMP_ID_PRIMARY && missCount == 0;
    if (completedParameterList) {
        transmissionListMode = false;
        transmissionListSizeKnown.clear();
        foreach (int key, transmissionMissingPackets.keys()) {
            transmissionMissingPackets.value(key)->clear();
        }
        setParameterListReady(true);
        emit parameterListUpToDate(component);
    }
    if (!transmissionListMode && missCount == 0 && missWriteCount == 0)
    {
        this->transmissionActive = false;

        // Expand visual tree
        tree->expandItem(tree->topLevelItem(0));
    }
}

/**
 * @param uas System which has the component
 * @param component id of the component
 * @param parameterName human friendly name of the parameter
 */
void QGCParamWidget::addParameter(int uas, int component, QString parameterName, QVariant value)
{
    //QLOG_DEBUG() << "PARAM WIDGET GOT PARAM:" << value;
    Q_UNUSED(uas);
    if (component == MAV_COMP_ID_PRIMARY && initialParamTimer->isActive())
    {
        initialParamTimer->stop();
    }
    // Reference to item in tree
    QTreeWidgetItem* parameterItem = NULL;

    // Get component
    if (!components->contains(component))
    {
        //        QString componentName;
        //        switch (component)
        //        {
        //        case MAV_COMP_ID_CAMERA:
        //            componentName = tr("Camera (#%1)").arg(component);
        //            break;
        //        case MAV_COMP_ID_IMU:
        //            componentName = tr("IMU (#%1)").arg(component);
        //            break;
        //        default:
        //            componentName = tr("Component #").arg(component);
        //            break;
        //        }
        QString componentName = tr("Component #%1").arg(component);
        addComponent(uas, component, componentName);
    }

    // Replace value in map

    // FIXME
    if (parameters.value(component)->contains(parameterName)) parameters.value(component)->remove(parameterName);
    parameters.value(component)->insert(parameterName, value);


    QString splitToken = "_";
    // Check if auto-grouping can work
    if (parameterName.contains(splitToken))
    {
        QString parent = parameterName.section(splitToken, 0, 0, QString::SectionSkipEmpty);
        QMap<QString, QTreeWidgetItem*>* compParamGroups = paramGroups.value(component);
        if (!compParamGroups->contains(parent))
        {
            // Insert group item
            QStringList glist;
            glist.append(parent);
            QTreeWidgetItem* item = new QTreeWidgetItem(glist);
            compParamGroups->insert(parent, item);
            components->value(component)->addChild(item);
        }

        // Append child to group
        bool found = false;
        QTreeWidgetItem* parentItem = compParamGroups->value(parent);
        for (int i = 0; i < parentItem->childCount(); i++) {
            QTreeWidgetItem* child = parentItem->child(i);
            QString key = child->data(0, Qt::DisplayRole).toString();
            if (key == parameterName)
            {
                //QLOG_DEBUG() << "UPDATED CHILD";
                parameterItem = child;
                if (value.type() == QVariant::Char)
                {
                    parameterItem->setData(1, Qt::DisplayRole, value.toUInt());
                }
                else
                {
                    parameterItem->setData(1, Qt::DisplayRole, value);
                }
                found = true;
            }
        }

        if (!found)
        {
            // Insert parameter into map
            QStringList plist;
            plist.append(parameterName);
            // CREATE PARAMETER ITEM
            parameterItem = new QTreeWidgetItem(plist);
            // CONFIGURE PARAMETER ITEM
            if (value.type() == QVariant::Char)
            {
                parameterItem->setData(1, Qt::DisplayRole, value.toUInt());
            }
            else
            {
                parameterItem->setData(1, Qt::DisplayRole, value);
            }

            compParamGroups->value(parent)->addChild(parameterItem);
            parameterItem->setFlags(parameterItem->flags() | Qt::ItemIsEditable);
        }
    }
    else
    {
        bool found = false;
        QTreeWidgetItem* parent = components->value(component);
        for (int i = 0; i < parent->childCount(); i++)
        {
            QTreeWidgetItem* child = parent->child(i);
            QString key = child->data(0, Qt::DisplayRole).toString();
            if (key == parameterName)
            {
                //QLOG_DEBUG() << "UPDATED CHILD";
                parameterItem = child;
                parameterItem->setData(1, Qt::DisplayRole, value);
                found = true;
            }
        }

        if (!found)
        {
            // Insert parameter into map
            QStringList plist;
            plist.append(parameterName);
            // CREATE PARAMETER ITEM
            parameterItem = new QTreeWidgetItem(plist);
            // CONFIGURE PARAMETER ITEM
            parameterItem->setData(1, Qt::DisplayRole, value);

            components->value(component)->addChild(parameterItem);
            parameterItem->setFlags(parameterItem->flags() | Qt::ItemIsEditable);
        }
        //tree->expandAll();
    }
    // Reset background color
    parameterItem->setBackground(0, Qt::NoBrush);
    parameterItem->setBackground(1, Qt::NoBrush);
    // Add tooltip
    QString tooltipFormat;
    if (paramDefault.contains(parameterName))
    {
        tooltipFormat = tr("Default: %1, %2");
        tooltipFormat = tooltipFormat.arg(paramDefault.value(parameterName, 0.0f)).arg(paramToolTips.value(parameterName, ""));
    }
    else
    {
        tooltipFormat = paramToolTips.value(parameterName, "");
    }
    parameterItem->setToolTip(0, tooltipFormat);
    parameterItem->setToolTip(1, tooltipFormat);

    //tree->update();
    if (changedValues.contains(component)) changedValues.value(component)->remove(parameterName);
}

/**
 * Send a request to deliver the list of onboard parameters
 * to the MAV.
 */
void QGCParamWidget::requestParameterList()
{
    if (!m_parameterManager) {
        statusLabel->setText(tr("No exact vehicle target selected."));
        return;
    }
    m_parameterManager->requestParameterList();
}

void QGCParamWidget::cancelParameterList()
{
    if (m_parameterManager) {
        m_parameterManager->cancelParameterList();
    }
}

void QGCParamWidget::parameterItemChanged(QTreeWidgetItem* current, int column)
{
    if (current && column > 0) {
        QTreeWidgetItem* parent = current->parent();
        while (parent->parent() != NULL) {
            parent = parent->parent();
        }
        // Parent is now top-level component
        int key = components->key(parent);
        if (!changedValues.contains(key)) {
            changedValues.insert(key, new QMap<QString, QVariant>());
        }
        QMap<QString, QVariant>* map = changedValues.value(key, NULL);
        if (map) {
            QString str = current->data(0, Qt::DisplayRole).toString();
            QVariant value = current->data(1, Qt::DisplayRole);
            // Set parameter on changed list to be transmitted to MAV
            QPalette pal = statusLabel->palette();
            pal.setColor(backgroundRole(), QGC::colorOrange);
            statusLabel->setPalette(pal);
            statusLabel->setText(tr("Transmit pend. %1:%2: %3").arg(key).arg(str).arg(value.toFloat(), 5, 'f', 1, QChar(' ')));
            //QLOG_DEBUG() << "PARAM CHANGED: COMP:" << key << "KEY:" << str << "VALUE:" << value;
            // Changed values list
            if (map->contains(str)) map->remove(str);
            map->insert(str, value);

            // Check if the value was numerically changed
            if (!parameters.value(key)->contains(str) || parameters.value(key)->value(str, value.toDouble()-1) != value) {
                current->setBackground(0, QBrush(QColor(QGC::colorOrange)));
                current->setBackground(1, QBrush(QColor(QGC::colorOrange)));
            }

            switch (static_cast<QMetaType::Type>(parameters.value(key)->value(str).type()))
            {
            case QMetaType::Int:
            {
                QVariant fixedValue(value.toInt());
                parameters.value(key)->insert(str, fixedValue);
            }
                break;
            case QMetaType::UInt:
            {
                QVariant fixedValue(value.toUInt());
                parameters.value(key)->insert(str, fixedValue);
            }
                break;
            case QMetaType::Double:
            case QMetaType::Float:
            {
                QVariant fixedValue(value.toFloat());
                parameters.value(key)->insert(str, fixedValue);
            }
                break;
            case QMetaType::QChar:
            {
                QVariant fixedValue(QChar((unsigned char)value.toUInt()));
                parameters.value(key)->insert(str, fixedValue);
            }
                break;
            default:
                qCritical() << "ABORTED PARAM UPDATE, NO VALID QVARIANT TYPE";
                return;
            }
        }
    }
}

void QGCParamWidget::saveParametersButtonClicked()
{
    if (!mav) return;
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save File"), QGC::parameterDirectory(),
                                                    tr("Parameter File (*.txt)"));
    saveParamsToFile(fileName,TabSeperatedValues);


}

void QGCParamWidget::loadParametersButtonClicked()
{
    if (!mav) return;
    QString fileName = QFileDialog::getOpenFileName(this, tr("Load File"), QGC::parameterDirectory(),
                                                    tr("Parameter file (*.txt)"));
    loadParamsFromFile(fileName,TabSeperatedValues);

}

/**
 * Enabling the retransmission guard enables the parameter widget to track
 * dropped parameters and to re-request them. This works for both individual
 * parameter reads as well for whole list requests.
 *
 * @param enabled True if retransmission checking should be enabled, false else
 */
void QGCParamWidget::setRetransmissionGuardEnabled(bool enabled)
{
    if (enabled) {
        retransmissionTimer.start(retransmissionTimeout);
    } else {
        retransmissionTimer.stop();
    }
}

void QGCParamWidget::retransmissionGuardTick()
{
    if (transmissionActive) {
        //QLOG_DEBUG() << __FILE__ << __LINE__ << "RETRANSMISSION GUARD ACTIVE, CHECKING FOR DROPS..";

        // Check for timeout
        // stop retransmission attempts on timeout
        if (QGC::groundTimeMilliseconds() > transmissionTimeout) {
            const bool parameterListTimedOut = transmissionListMode;
            setRetransmissionGuardEnabled(false);
            transmissionActive = false;
            transmissionListMode = false;

            // Empty read retransmission list
            // Empty write retransmission list
            int missingReadCount = 0;
            QList<int> readKeys = transmissionMissingPackets.keys();
            foreach (int component, readKeys) {
                missingReadCount += transmissionMissingPackets.value(component)->count();
                transmissionMissingPackets.value(component)->clear();
            }

            const QString reason = tr("TIMEOUT! MISSING: %1 read.")
                .arg(missingReadCount);
            statusLabel->setText(reason);
            QLOG_WARN() << reason;
            if (parameterListTimedOut) {
                setParameterListReady(false);
                emit parameterListLoadFailed(reason);
            }
            return;
        }

        // Re-request at maximum retransmissionBurstRequestSize parameters at once
        // to prevent link flooding
        QMap<int, QMap<QString, QVariant>*>::iterator i;
        for (i = parameters.begin(); i != parameters.end(); ++i) {
            // Iterate through the parameters of the component
            int component = i.key();
            // Request n parameters from this component (at maximum)
            QList<int> * paramList = transmissionMissingPackets.value(component, NULL);
            if (paramList) {
                int count = 0;
                foreach (int id, *paramList) {
                    if (count < retransmissionBurstRequestSize) {
                        //QLOG_DEBUG() << __FILE__ << __LINE__ << "RETRANSMISSION GUARD REQUESTS RETRANSMISSION OF PARAM #" << id << "FROM COMPONENT #" << component;
                        emit requestParameter(component, id);
                        statusLabel->setText(tr("Requested retransmission of #%1").arg(id+1));
                        QLOG_INFO() <<tr("Requested retransmission of #%1").arg(id+1);
                        count++;
                    } else {
                        break;
                    }
                }
            }
        }
    } else {
        //QLOG_DEBUG() << __FILE__ << __LINE__ << "STOPPING RETRANSMISSION GUARD GRACEFULLY";
        setRetransmissionGuardEnabled(false);
    }
}


/**
 * The .. signal is emitted
 */
void QGCParamWidget::requestParameterUpdate(int component, const QString& parameter)
{
    if (m_parameterManager) {
        m_parameterManager->requestParameterUpdate(component, parameter);
    }
}

/**
 * @param component the subsystem which has the parameter
 * @param parameterName name of the parameter, as delivered by the system
 * @param value value of the parameter
 */
void QGCParamWidget::setParameter(int component, QString parameterName, QVariant value)
{
    if (paramMin.contains(parameterName) && value.toDouble() < paramMin.value(parameterName))
    {
        statusLabel->setText(tr("REJ. %1 < min").arg(value.toDouble()));
        QLOG_INFO() << "setParameter: Value for" << parameterName << "is too small." << value.toDouble()
                    << "<" << paramMin.value(parameterName);
        return;
    }
    if (paramMax.contains(parameterName) && value.toDouble() > paramMax.value(parameterName))
    {
        statusLabel->setText(tr("REJ. %1 > max").arg(value.toDouble()));
        QLOG_INFO() << "setParameter: Value for" << parameterName << "is too big." << value.toDouble()
                    << ">" << paramMax.value(parameterName);
        return;
    }

    QMap<QString, QVariant>* parameterList = parameters.value(component);

    if (parameterList == NULL){
        QLOG_ERROR() << " No parameter list for component: " << component;
        return;
    }

    if (parameterValuesEqual(parameterList->value(parameterName), value))
    {
        statusLabel->setText(tr("REJ. %1 > max").arg(value.toDouble()));
        QLOG_INFO() << "setParameter: Value for" << parameterName << "did not change." << value.toDouble()
                    << "=" << parameterList->value(parameterName);
        return;
    }

    QVariant sentValue;
    switch (static_cast<QMetaType::Type>(parameterList->value(parameterName).type()))
    {
    case QMetaType::QChar:
    {
        QVariant fixedValue(QChar((unsigned char)value.toInt()));
        sentValue = fixedValue;
        emit parameterChanged(component, parameterName, fixedValue);
        //QLOG_DEBUG() << "PARAM WIDGET SENT:" << fixedValue;
    }
        break;
    case QMetaType::Int:
    {
        QVariant fixedValue(value.toInt());
        sentValue = fixedValue;
        emit parameterChanged(component, parameterName, fixedValue);
        //QLOG_DEBUG() << "PARAM WIDGET SENT:" << fixedValue;
    }
        break;
    case QMetaType::UInt:
    {
        QVariant fixedValue(value.toUInt());
        sentValue = fixedValue;
        emit parameterChanged(component, parameterName, fixedValue);
        //QLOG_DEBUG() << "PARAM WIDGET SENT:" << fixedValue;
    }
        break;
    case QMetaType::Double:
    case QMetaType::Float:
    {
        QVariant fixedValue(value.toFloat());
        sentValue = fixedValue;
        emit parameterChanged(component, parameterName, fixedValue);
        //QLOG_DEBUG() << "PARAM WIDGET SENT:" << fixedValue;
    }
        break;
    default:
        if (!parameterList->contains(parameterName)) {
            qCritical() << "ABORTED PARAM SEND, UNKNOWN PARAM NAME:" << parameterName;
        } else {
            qCritical() << "ABORTED PARAM SEND, NO VALID QVARIANT TYPE. PARAM NAME:" << parameterName << "Type:" << parameterList->value(parameterName).type();
        }
        return;
    }

    Q_UNUSED(sentValue)
}

/**
 * Set all parameter in the parameter tree on the MAV
 */
void QGCParamWidget::setParameters()
{
    int parametersSent = 0;
    if (m_parameterManager) {
        for (auto component = changedValues.cbegin();
             component != changedValues.cend(); ++component) {
            QVariantList changes;
            const QMap<QString, QVariant> *values = component.value();
            if (!values) {
                continue;
            }
            for (auto parameter = values->cbegin();
                 parameter != values->cend(); ++parameter) {
                changes.append(QVariantMap{
                    {QStringLiteral("name"), parameter.key()},
                    {QStringLiteral("value"), parameter.value()}
                });
            }
            if (!changes.isEmpty()
                && m_parameterManager->writeParameters(
                       component.key(), changes) != 0) {
                parametersSent += changes.size();
            }
        }
    }

    // Change transmission status if necessary
    if (parametersSent == 0) {
        statusLabel->setText(tr("No transmission: No changed values."));
    } else {
        statusLabel->setText(tr("Transmitting %1 parameters.").arg(parametersSent));
    }
}

/**
 * Write the current onboard parameters from RAM into
 * permanent storage, e.g. EEPROM or harddisk
 */
void QGCParamWidget::writeParameters()
{
    int changedParamCount = 0;

    QMap<int, QMap<QString, QVariant>*>::iterator i;
    for (i = changedValues.begin(); i != changedValues.end(); ++i)
    {
        // Iterate through the parameters of the component
        QMap<QString, QVariant>* comp = i.value();
        {
            QMap<QString, QVariant>::iterator j;
            for (j = comp->begin(); j != comp->end(); ++j)
            {
                changedParamCount++;
            }
        }
    }

    if (changedParamCount > 0)
    {
        QMessageBox msgBox;
        msgBox.setText(tr("There are locally changed parameters. Please transmit them first (<TRANSMIT>) or update them with the onboard values (<REFRESH>) before storing onboard from RAM to ROM."));
        msgBox.exec();
    }
    else
    {
        VehicleCommandService *const commands =
            LinkManager::instance()->vehicleCommandService();
        if (!commands
            || commands->sendCurrentCommandLong(
                   MAV_CMD_PREFLIGHT_STORAGE, 0,
                   1.0F, 0.0F, 0.0F, 0.0F,
                   0.0F, 0.0F, 0.0F)
                != int(VehicleCommandService::SendResult::Sent)) {
            statusLabel->setText(
                tr("Unable to commit parameters on the selected link."));
        }
    }
}

void QGCParamWidget::readParameters()
{
    VehicleCommandService *const commands =
        LinkManager::instance()->vehicleCommandService();
    if (!commands
        || commands->sendCurrentCommandLong(
               MAV_CMD_PREFLIGHT_STORAGE, 0,
               0.0F, 0.0F, 0.0F, 0.0F,
               0.0F, 0.0F, 0.0F)
            != int(VehicleCommandService::SendResult::Sent)) {
        statusLabel->setText(
            tr("Unable to read parameters on the selected link."));
    }
}

/**
 * Clear all data in the parameter widget
 */
void QGCParamWidget::clear()
{
    const bool blocked = tree->blockSignals(true);
    tree->clear();
    tree->blockSignals(blocked);
    components->clear();
    qDeleteAll(paramGroups);
    paramGroups.clear();
    qDeleteAll(parameters);
    parameters.clear();
    qDeleteAll(changedValues);
    changedValues.clear();
    qDeleteAll(transmissionMissingPackets);
    transmissionMissingPackets.clear();
    qDeleteAll(transmissionMissingWriteAckPackets);
    transmissionMissingWriteAckPackets.clear();
    received.clear();
    transmissionListSizeKnown.clear();
    transmissionListMode = false;
    transmissionActive = false;
    transmissionTimeout = 0;
    setRetransmissionGuardEnabled(false);
    setParameterListReady(false);
}
void QGCParamWidget::initialParamCheckTick()
{
    if (m_parameterManager) {
        QLOG_DEBUG() << "QGCParamWidget: retrying through exact parameter service";
        m_parameterManager->requestParameterList();
    }
}
