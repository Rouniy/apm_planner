/*===================================================================
APM_PLANNER Open Source Ground Control Station

(c) 2021 APM_PLANNER PROJECT <http://www.ardupilot.com>

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
 * @file ApmCustomFirmwareConfig.cpp
 * @author Arne Wischmann <wischmann-a@gmx.de>
 * @date 20 Mrz 2021
 * @brief File providing implementation for selecting firmware for flashing.
 */

#include "ApmCustomFirmwareConfig.h"
#include "ui_apmcustomfirmwareconfig.h"
#include "MainWindow.h"

#include <QUrl>
#include <QFileDialog>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTimer>

ApmCustomFirmwareConfig::ApmCustomFirmwareConfig(QWidget *parent) :
    QWidget(parent),
    mp_Ui(new Ui::ApmCustomFWConfig)
{
    mp_Ui->setupUi(this);

    // setup table view for available devices
    mp_Ui->deviceListWidget->setColumnCount(static_cast<int>(DeviceListColumName::ColumCount));
    QStringList devHeaderLabels = {"Port", "Description", "Manufacturer", "Location", "Vendor-Ident.", "Product-Ident."};
    mp_Ui->deviceListWidget->setHorizontalHeaderLabels(devHeaderLabels);

    // setup table view for available firmwares
    mp_Ui->fwListWidget->setColumnCount(static_cast<int>(FirmwareListColumName::ColumCount));
    QStringList fwHeaderLabels = {"Board-ID", "Manufacturer", "Brand", "FW-Type", "FileName"};
    mp_Ui->fwListWidget->setHorizontalHeaderLabels(fwHeaderLabels);

    // set button state
    SetButtonState();

    mp_networkManager = new QNetworkAccessManager(this);
    m_manifestTimeout = new QTimer(this);
    m_manifestTimeout->setSingleShot(true);
    connect(m_manifestTimeout, &QTimer::timeout, this, [this]() {
        QPointer<QNetworkReply> reply = m_manifestReply;
        if (!reply) {
            return;
        }
        m_manifestReply.clear();
        m_manifestState = ManifestState::Error;
        m_lastManifestProgress = 0;
        disconnect(reply.data(), nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
        const QString message = tr("Fetching available firmwares timed out.");
        QLOG_WARN() << "ApmCustomFirmwareConfig:" << message;
        mp_Ui->statusInfoWidget->append(message);
        if (isVisible()) {
            QMessageBox::warning(this, tr("Error"), message);
        }
    });

    m_firmwareTimeout = new QTimer(this);
    m_firmwareTimeout->setSingleShot(true);
    connect(m_firmwareTimeout, &QTimer::timeout, this, [this]() {
        QPointer<QNetworkReply> reply = m_firmwareReply;
        if (!reply) {
            return;
        }
        m_firmwareReply.clear();
        m_lastFirmwareProgress = 0;
        disconnect(reply.data(), nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
        finishOperation();
        const QString message = tr("Downloading firmware timed out.");
        QLOG_WARN() << "ApmCustomFirmwareConfig:" << message;
        mp_Ui->statusInfoWidget->append(message);
        if (isVisible()) {
            QMessageBox::warning(this, tr("Error"), message);
        }
    });

    // setup progress bar
    mp_Ui->progressBar->setMinimum(0);
    mp_Ui->progressBar->setValue(0);

    connect(mp_Ui->refreshButton, &QPushButton::clicked, this, &ApmCustomFirmwareConfig::FillDeviceList);
    connect(mp_Ui->deviceListWidget, &QTableWidget::cellClicked, this, &ApmCustomFirmwareConfig::deviceTableCellClicked);
    connect(mp_Ui->flashLocalFWButton, &QPushButton::clicked, this, &ApmCustomFirmwareConfig::flashLocalFWButtonClicked);
    connect(mp_Ui->flashButton, &QPushButton::clicked, this, &ApmCustomFirmwareConfig::flashFWButtonClicked);

    connect(mp_Ui->fwListWidget, &QTableWidget::cellClicked, this, &ApmCustomFirmwareConfig::firmwareTableCellClicked);
    connect(mp_Ui->fwDownloadButton, &QPushButton::clicked, this, &ApmCustomFirmwareConfig::firmwareDownloadBtnClicked);
}

ApmCustomFirmwareConfig::~ApmCustomFirmwareConfig()
{
    m_manifestTimeout->stop();
    m_firmwareTimeout->stop();

    const auto abortReply = [this](QPointer<QNetworkReply> &reply) {
        QPointer<QNetworkReply> activeReply = reply;
        reply.clear();
        if (!activeReply) {
            return;
        }
        disconnect(activeReply.data(), nullptr, this, nullptr);
        activeReply->abort();
    };
    abortReply(m_manifestReply);
    abortReply(m_firmwareReply);
    if (mp_px4Updater) {
        disconnect(mp_px4Updater.data(), nullptr, this, nullptr);
        mp_px4Updater->stop();
        mp_px4Updater.reset();
    }
    delete mp_Ui;
}

void ApmCustomFirmwareConfig::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    QLOG_DEBUG() << "ApmCustomFirmwareConfig: Install Firmware selected";
    MainWindow::instance()->toolBar().disableConnectWidget(true);

    if (m_activationWorkScheduled) {
        return;
    }
    m_activationWorkScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_activationWorkScheduled = false;
        if (!isVisible()) {
            return;
        }
        if (m_availableFirmwares.empty()
            && (m_manifestState == ManifestState::Idle
                || m_manifestState == ManifestState::Error)) {
            fetchAvailableFirmware();
        }
        // Keep the existing automatic refresh, but run it only after the view
        // switch has returned to the event loop. QSerialPortInfo enumeration
        // remains synchronous and should move behind an injectable worker in a
        // follow-up change.
        FillDeviceList();
    });
}

void ApmCustomFirmwareConfig::hideEvent(QHideEvent *event)
{
    MainWindow::instance()->toolBar().disableConnectWidget(false);
    QWidget::hideEvent(event);
}

QTableWidgetItem *ApmCustomFirmwareConfig::createItem(const QString &itemText) const
{
    QTableWidgetItem *p_Item = new QTableWidgetItem(itemText);
    p_Item->setFlags(Qt::ItemIsSelectable|Qt::ItemIsEnabled);
    p_Item->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    return p_Item;
}

void ApmCustomFirmwareConfig::FillDeviceList()
{
    if (operationBusy()) {
        return;
    }
    mp_Ui->deviceListWidget->clearContents();

    m_portInfoList = QSerialPortInfo::availablePorts();
    mp_Ui->deviceListWidget->setRowCount(m_portInfoList.size());

    for (int i = 0; i < m_portInfoList.size(); ++i)
    {
        QTableWidgetItem *p_Item = createItem(m_portInfoList[i].portName());
        mp_Ui->deviceListWidget->setItem(i, static_cast<int>(DeviceListColumName::Port), p_Item);
        p_Item = createItem(m_portInfoList[i].description());
        mp_Ui->deviceListWidget->setItem(i, static_cast<int>(DeviceListColumName::Description), p_Item);
        p_Item = createItem(m_portInfoList[i].manufacturer());
        mp_Ui->deviceListWidget->setItem(i, static_cast<int>(DeviceListColumName::Manufacturer), p_Item);
        p_Item = createItem(m_portInfoList[i].systemLocation());
        mp_Ui->deviceListWidget->setItem(i, static_cast<int>(DeviceListColumName::Location), p_Item);
        if(m_portInfoList[i].hasVendorIdentifier())
        {
            QString ident("0x");
            ident.append(QString::number(m_portInfoList[i].vendorIdentifier(), 16));
            p_Item = createItem(ident);
            mp_Ui->deviceListWidget->setItem(i, static_cast<int>(DeviceListColumName::Vendor), p_Item);
        }
        if(m_portInfoList[i].hasProductIdentifier())
        {
            QString ident("0x");
            ident.append(QString::number(m_portInfoList[i].productIdentifier(), 16));
            p_Item = createItem(ident);
            mp_Ui->deviceListWidget->setItem(i, static_cast<int>(DeviceListColumName::Product), p_Item);
        }
    }

    m_selectedDeviceIndex = s_InvalidIndex;
    SetButtonState();
}

void ApmCustomFirmwareConfig::deviceTableCellClicked(int row, int column)
{
    Q_UNUSED(column)
    if (operationBusy() || row < 0 || row >= m_portInfoList.size()) {
        return;
    }
    mp_Ui->deviceListWidget->selectRow(row);
    m_selectedDeviceIndex = row;

    QSerialPortInfo info = m_portInfoList.at(m_selectedDeviceIndex);

    QString vendor = QString("%1").arg(info.vendorIdentifier(), 4, 16, QLatin1Char( '0' ));
    vendor = QString("0x%1").arg(vendor.toUpper());
    QString prod   = QString("%1").arg(info.productIdentifier(), 4, 16, QLatin1Char( '0' ));
    prod = QString("0x%1").arg(prod.toUpper());

    QLOG_DEBUG() << "ApmCustomFirmwareConfig: Selected Device:" << info.portName() << ", " << info.description()
                 << ", " << info.manufacturer() << ", Vendor: " << vendor << ", ProdID: " << prod;

    SetButtonState();
}

void ApmCustomFirmwareConfig::firmwareTableCellClicked(int row, int column)
{
    Q_UNUSED(column)
    if (operationBusy() || row < 0 || row >= m_versionIndex.size()) {
        return;
    }
    mp_Ui->fwListWidget->selectRow(row);
    m_selectedFwIndex = m_versionIndex.at(row);
    QLOG_DEBUG() << "Selected Firmware on index:" << m_selectedFwIndex;

    auto fwObject = m_availableFirmwares[m_selectedFwIndex].toObject();
    QLOG_DEBUG() << "With info:" << fwObject;

    SetButtonState();
}

void ApmCustomFirmwareConfig::flashLocalFWButtonClicked()
{
    QLOG_DEBUG() << "ApmCustomFirmwareConfig: flashLocalFWButtonClicked.";
    if (operationBusy() || !captureSelectedDevice()) {
        return;
    }
    m_operation = Operation::DownloadAndFlash;
    SetButtonState();
    QFileDialog dialog(this);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFile);

    // reset firmware data object
    m_firmwareData = QJsonObject();
    m_flashImage.clear();

    QStringList fileNames;
    QFileInfo firmwareFile;
    QString message;
    if(dialog.exec())
    {
        fileNames = dialog.selectedFiles();
        firmwareFile.setFile(fileNames.at(0));
        QLOG_DEBUG() << "ApmCustomFirmwareConfig: Selected file:" << firmwareFile.absoluteFilePath();
        if(openFirmwareFile(firmwareFile))
        {
            mp_Ui->statusInfoWidget->append("Successfully opened " + firmwareFile.fileName());
            StartFlashFirmware(firmwareFile.absoluteFilePath());
        }
        else
        {
            mp_Ui->statusInfoWidget->append("Failed to open  " + firmwareFile.fileName());
            finishOperation();
        }
    }
    else
    {
        QLOG_DEBUG() << "Firmware select cancelled.";
        finishOperation();
    }
}

void ApmCustomFirmwareConfig::StartFlashFirmware(const QString &firmwareFileName)
{
    if (mp_px4Updater || !m_operationDeviceValid) {
        QLOG_WARN() << "ApmCustomFirmwareConfig: refusing overlapping or targetless flash";
        finishOperation();
        return;
    }
    mp_Ui->statusInfoWidget->append("Start flashing firmware.");

    mp_px4Updater.reset(new PX4FirmwareUploader());

    connect(mp_px4Updater.data(), QOverload<QString>::of(&PX4FirmwareUploader::statusUpdate), this, &ApmCustomFirmwareConfig::statusUpdate);
    connect(mp_px4Updater.data(), QOverload<QString>::of(&PX4FirmwareUploader::debugUpdate), this, &ApmCustomFirmwareConfig::statusUpdate);
    connect(mp_px4Updater.data(), &PX4FirmwareUploader::startFlashing, this, &ApmCustomFirmwareConfig::startFlashing);
    connect(mp_px4Updater.data(), QOverload<QString>::of(&PX4FirmwareUploader::warning), this, &ApmCustomFirmwareConfig::flashWarn);
    connect(mp_px4Updater.data(), QOverload<QString>::of(&PX4FirmwareUploader::error), this, &ApmCustomFirmwareConfig::flashError);
    connect(mp_px4Updater.data(), QOverload<qint64, qint64>::of(&PX4FirmwareUploader::flashProgress), this, &ApmCustomFirmwareConfig::downloadProgress);
    connect(mp_px4Updater.data(), &PX4FirmwareUploader::complete, this, &ApmCustomFirmwareConfig::flashingDone);
    connect(mp_px4Updater.data(), &PX4FirmwareUploader::requestDevicePlug, this, &ApmCustomFirmwareConfig::requestDeviceReplug);
    connect(mp_px4Updater.data(), &PX4FirmwareUploader::devicePlugDetected, this, &ApmCustomFirmwareConfig::deviceReplugDetected);

    mp_px4Updater->loadFile(firmwareFileName);
    SetButtonState();
}

bool ApmCustomFirmwareConfig::openFirmwareFile(const QFileInfo &firmwareFile)
{
    QLOG_DEBUG() << "ApmCustomFirmwareConfig: Trying to open firmware:" << firmwareFile.absoluteFilePath();

    QByteArray fileContent;
    QFile fileToRead(firmwareFile.absoluteFilePath());
    fileToRead.open(QIODevice::ReadOnly);
    fileContent = fileToRead.readAll();
    fileToRead.close();

    if(fileContent.size() == 0)
    {
        QLOG_INFO() << "ApmCustomFirmwareConfig: Failed to open/read " << firmwareFile.absoluteFilePath();
        return false;
    }

    // Firmware is contained in json structure
    QJsonParseError parseError;
    QJsonDocument firmwareData = QJsonDocument::fromJson(fileContent, &parseError);
    if(parseError.error != QJsonParseError::NoError)
    {
        QLOG_INFO() << "ApmCustomFirmwareConfig: Failed to parse firmware: " << parseError.errorString();
        return false;
    }
    m_firmwareData = firmwareData.object();

    // Extract image size
    QJsonObject::iterator jsonIter = m_firmwareData.find("image_size");
    if(jsonIter == m_firmwareData.end())
    {
        QLOG_INFO() << "ApmCustomFirmwareConfig: Failed to extract image size. Firmware seems to be corrupt.";
        return false;
    }
    qint32 firmwareSize = jsonIter.value().toInt(0);

    // Extract image data
    QByteArray tempImage;
    tempImage.append(static_cast<qint8>((firmwareSize >> 24) & 0xFF));
    tempImage.append(static_cast<qint8>((firmwareSize >> 16) & 0xFF));
    tempImage.append(static_cast<qint8>((firmwareSize >>  8) & 0xFF));
    tempImage.append(static_cast<qint8>((firmwareSize >>  0) & 0xFF));

    jsonIter = m_firmwareData.find("image");
    if(jsonIter != m_firmwareData.end())
    {
        QByteArray tempRaw64 = jsonIter.value().toString().toUtf8();
        tempImage.append(QByteArray::fromBase64(tempRaw64));
        m_flashImage = qUncompress(tempImage);

        QLOG_INFO() << "Firmware size:" << m_flashImage.size() << "expected" << firmwareSize << "bytes";
        if (m_flashImage.size() != firmwareSize)
        {
            QLOG_INFO() << "Error in decompressing firmware. Please re-download and try again";
            m_flashImage.clear();
            return false;
        }
        //Per QUpgrade, pad it to a 4 byte multiple.
        while ((m_flashImage.count() % 4) != 0)
        {
            m_flashImage.append(static_cast<qint8>(0xFF));
        }
//        m_localChecksum = crc32(uncompressed);

        QLOG_DEBUG() << "Successful parsed firmware file: " << firmwareFile.absoluteFilePath();
        return true;
    }
    else
    {
        QLOG_INFO() << "Reading  Firmware image failed.";
    }
    return false;
}

void ApmCustomFirmwareConfig::flashFWButtonClicked()
{
    QLOG_DEBUG() << "ApmCustomFirmwareConfig::flashFWButtonClicked";
    if (operationBusy() || !captureSelectedDevice()) {
        return;
    }
    m_operation = Operation::DownloadAndFlash;
    startDownloadFirmware();
}

void ApmCustomFirmwareConfig::firmwareDownloadBtnClicked()
{
    QLOG_DEBUG() << "ApmCustomFirmwareConfig::firmwareDownloadBtnClicked";
    if (operationBusy()) {
        return;
    }
    m_operationDeviceValid = false;
    m_operation = Operation::DownloadOnly;
    startDownloadFirmware();
}

void ApmCustomFirmwareConfig::fetchAvailableFirmware()
{
    if (m_manifestState == ManifestState::Loading || m_manifestReply) {
        return;
    }

    QUrl url(s_JSONUrl);
    QLOG_DEBUG() << "ApmCustomFirmwareConfig: Fetching available firmwares from: " << url.toString();
    mp_Ui->progressbarLabel->setText("<h4>Downloading Firmware information</h4>");
    mp_Ui->statusInfoWidget->append("Fetching available firmwares from: " + url.toString());
    m_manifestState = ManifestState::Loading;
    QNetworkReply *reply = mp_networkManager->get(QNetworkRequest(url));
    m_manifestReply = reply;
    m_lastManifestProgress = 0;

    connect(reply, &QNetworkReply::finished, this,
            &ApmCustomFirmwareConfig::availFirmwarefetchFinished);
    connect(reply, &QNetworkReply::downloadProgress, this,
            &ApmCustomFirmwareConfig::downloadProgress);
    m_manifestTimeout->start(s_NetworkTimeoutMs);
}

void ApmCustomFirmwareConfig::startDownloadFirmware()
{
    if (m_firmwareReply || mp_px4Updater) {
        return;
    }
    if (m_operation == Operation::None
        || (m_operation == Operation::DownloadAndFlash
            && !m_operationDeviceValid)
        || m_selectedFwIndex < 0
        || m_selectedFwIndex >= m_availableFirmwares.size())
    {
        QLOG_DEBUG() << "No Firmware selected";
        finishOperation();
        return;
    }

    auto fwObject = m_availableFirmwares[m_selectedFwIndex].toObject();
    auto iter = fwObject.find(s_Url);
    if (iter != fwObject.end())
    {
        QUrl url(iter->toString());
        if (!url.isValid() || url.isEmpty()) {
            QLOG_WARN() << "Selected firmware has an invalid URL:" << url;
            finishOperation();
            return;
        }
        QLOG_DEBUG() << "Downloading " << url.toString();
        auto pfIter = fwObject.find(s_Platform);
        QString label("<h4>Downloading Firmware for ");
        label.append(pfIter->toString());
        label.append("</h4>");
        mp_Ui->progressbarLabel->setText(label);
        mp_Ui->progressBar->reset();
        mp_Ui->statusInfoWidget->append("Downloading firmware " + url.toString());
        m_downloadFirmwareIndex = m_selectedFwIndex;
        QNetworkReply *reply = mp_networkManager->get(QNetworkRequest(url));
        m_firmwareReply = reply;
        m_lastFirmwareProgress = 0;

        connect(reply, &QNetworkReply::finished, this,
                &ApmCustomFirmwareConfig::downloadFirmwareFinished);
        connect(reply, &QNetworkReply::downloadProgress, this,
                &ApmCustomFirmwareConfig::downloadProgress);
        m_firmwareTimeout->start(s_NetworkTimeoutMs);
        SetButtonState();
    }
    else
    {
        QLOG_WARN() << "No valid URL found in selected firmware - not downloading anything.";
        QMessageBox::warning(this ,"Error", "No valid URL found in selected firmware entry");
        finishOperation();
    }
}

bool ApmCustomFirmwareConfig::operationBusy() const
{
    return m_operation != Operation::None || m_firmwareReply || mp_px4Updater;
}

bool ApmCustomFirmwareConfig::captureSelectedDevice()
{
    if (m_selectedDeviceIndex < 0
        || m_selectedDeviceIndex >= m_portInfoList.size()) {
        m_operationDeviceValid = false;
        return false;
    }
    m_operationDevice = m_portInfoList.at(m_selectedDeviceIndex);
    m_operationDeviceValid = true;
    return true;
}

void ApmCustomFirmwareConfig::finishOperation()
{
    m_operation = Operation::None;
    m_downloadFirmwareIndex = s_InvalidIndex;
    m_operationDeviceValid = false;
    SetButtonState();
}

void ApmCustomFirmwareConfig::SetButtonState()
{
    const bool busy = operationBusy();
    bool flash = false;
    bool download = false;
    bool flashlocal = false;

    if(m_selectedFwIndex >= 0
        && m_selectedFwIndex < m_availableFirmwares.size())
    {
        // we only can flash apj or px4 files hex must be flashed woth other tools
        auto fwObject = m_availableFirmwares[m_selectedFwIndex].toObject();
        QJsonObject::iterator jsonIter = fwObject.find(s_FWFormat);
        if(jsonIter != fwObject.end())
        {
            if ((jsonIter.value().toString() == "apj") || (jsonIter.value().toString() == "px4"))
            {
                if (m_selectedDeviceIndex >= 0
                    && m_selectedDeviceIndex < m_portInfoList.size())
                {
                    flash = !busy; // only apj or px4 and selected device (target)
                }
            }
            else
            {
                mp_Ui->statusInfoWidget->append("Only *.apj or *.px4 files can be flashed. All other formats need external tools.");
            }
        }
        download = !busy;
    }

    if (m_selectedDeviceIndex >= 0
        && m_selectedDeviceIndex < m_portInfoList.size())
    {
        flashlocal = !busy;
    }

    mp_Ui->flashButton->setEnabled(flash);
    mp_Ui->fwDownloadButton->setEnabled(download);
    mp_Ui->flashLocalFWButton->setEnabled(flashlocal);
    mp_Ui->refreshButton->setEnabled(!busy);
    mp_Ui->deviceListWidget->setEnabled(!busy);
    mp_Ui->fwListWidget->setEnabled(!busy);
    mp_Ui->fwCategoryBox->setEnabled(!busy);
    mp_Ui->fwPlatformBox->setEnabled(!busy);
    mp_Ui->fwTypeBox->setEnabled(!busy);
    mp_Ui->fwVersionBox->setEnabled(!busy);
}

void ApmCustomFirmwareConfig::availFirmwarefetchFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) {
        return;
    }
    if (reply != m_manifestReply.data()) {
        reply->deleteLater();
        return;
    }

    QLOG_DEBUG() << "ApmCustomFirmwareConfig: Fetching available firmwares finished";

    m_manifestTimeout->stop();
    m_manifestReply.clear();
    m_lastManifestProgress = 0;
    disconnect(reply, nullptr, this, nullptr);

    if (reply->error() != QNetworkReply::NoError) {
        m_manifestState = ManifestState::Error;
        const QString message = tr("Fetching available firmwares failed: %1")
                                    .arg(reply->errorString());
        QLOG_WARN() << "ApmCustomFirmwareConfig:" << message;
        mp_Ui->statusInfoWidget->append(message);
        reply->deleteLater();
        if (isVisible()) {
            QMessageBox::warning(this, tr("Error"), message);
        }
        return;
    }

    mp_Ui->statusInfoWidget->append("Fetching done.");

    QJsonParseError parseError;
    QJsonDocument availFirmware = QJsonDocument::fromJson(reply->readAll(), &parseError);
    reply->deleteLater();
    if(parseError.error != QJsonParseError::NoError)
    {
        m_manifestState = ManifestState::Error;
        QLOG_INFO() << "ApmCustomFirmwareConfig: Failed to parse available firmware list: " << parseError.errorString();
        if (isVisible()) {
            QMessageBox::warning(this ,"Error", "Failed to parse available firmware list: " + parseError.errorString());
        }
        return;
    }

    // Do some validations
    bool validManifest = false;
    auto jsonObject = availFirmware.object();
    auto jsonIter = jsonObject.find(s_JSONFormat);
    if(jsonIter != jsonObject.end())
    {
        QLOG_DEBUG() << "ApmCustomFirmwareConfig: Firmware list version is " << jsonIter.value().toString();
        if(jsonIter.value().toString() == s_JSONFormatVer)
        {
            jsonIter = jsonObject.find(s_JSONFWName);
            if(jsonIter != jsonObject.end() && jsonIter->isArray())
            {
                m_availableFirmwares = jsonIter.value().toArray();
                validManifest = true;
                QLOG_DEBUG() << "ApmCustomFirmwareConfig: Found " << m_availableFirmwares.size() << " firmwares.";
                mp_Ui->statusInfoWidget->append("Found " + QString::number(m_availableFirmwares.size()) + " firmwares.");
            }
            else
            {
                QLOG_INFO() << "ApmCustomFirmwareConfig: Available firmware list does not contain any firmwares.";
                mp_Ui->statusInfoWidget->append("Available firmware list does not contain any firmwares.");
            }
        }
        else
        {
            QLOG_INFO() << "ApmCustomFirmwareConfig: Firmware list has wrong version should be " << s_JSONFormat;
            QString temp("Firmware list has wrong version should be ");
            temp.append(s_JSONFormat);
            mp_Ui->statusInfoWidget->append(temp);

        }
    }
    else
    {
        QLOG_INFO() << "ApmCustomFirmwareConfig: Available firmware list does not have format field.";
        mp_Ui->statusInfoWidget->append("Firmware list does not have format field.");
    }

    m_manifestState = validManifest ? ManifestState::Ready
                                    : ManifestState::Error;
    if (validManifest) {
        // Trigger once to fill initially.
        fillFirmwareCategoryBox();
    }
}

void ApmCustomFirmwareConfig::fillFirmwareVersionBox(int index)
{
    Q_UNUSED(index)
    disconnect(mp_Ui->fwVersionBox , QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ApmCustomFirmwareConfig::fillFirmwareList);

    QString mavtype = mp_Ui->fwTypeBox->currentText();
    m_mavTypeIndex.clear();
    mp_Ui->fwVersionBox->clear();
    mp_Ui->fwVersionBox->setInsertPolicy(QComboBox::InsertAlphabetically);

    // fill firmware types into drop down list
    // use only the ones from the selected platform
    for (auto const val : m_platformIndex)
    {
        auto fwObject = m_availableFirmwares[val].toObject();
        auto fwObjectIter = fwObject.find(s_MavType);
        if((fwObjectIter != fwObject.end()) && (fwObjectIter->toString() == mavtype))
        {
            m_mavTypeIndex.push_back(val);
            fwObjectIter = fwObject.find(s_FWVersion);
            if(fwObjectIter != fwObject.end())
            {
                // Add to dropdown list if not already there
                if(mp_Ui->fwVersionBox->findText(fwObjectIter->toString()) == -1)
                {
                    mp_Ui->fwVersionBox->addItem(fwObjectIter->toString());
                }
            }
        }
    }
    QLOG_DEBUG() << "Found " << m_mavTypeIndex.size() << " elements for mavtype " << mavtype;
    mp_Ui->fwVersionBox->model()->sort(0);
    fillFirmwareList(0);
    connect(mp_Ui->fwVersionBox , QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ApmCustomFirmwareConfig::fillFirmwareList);
}

void ApmCustomFirmwareConfig::fillMavtypeBox(int index)
{
    Q_UNUSED(index)
    disconnect(mp_Ui->fwTypeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ApmCustomFirmwareConfig::fillFirmwareVersionBox);
    QString platform = mp_Ui->fwPlatformBox->currentText();
    m_platformIndex.clear();
    mp_Ui->fwTypeBox->clear();
    mp_Ui->fwTypeBox->setInsertPolicy(QComboBox::InsertAlphabetically);

    // fill firmware types into drop down list
    // use only the selected category (Stable, Dev, beta, ...)
    for (auto const val : m_categoryIndex)
    {
        auto fwObject = m_availableFirmwares[val].toObject();
        auto fwObjectIter = fwObject.find(s_Platform);
        if((fwObjectIter != fwObject.end()) && (fwObjectIter->toString() == platform))
        {
            m_platformIndex.push_back(val);
            fwObjectIter = fwObject.find(s_MavType);
            if(fwObjectIter != fwObject.end())
            {
                // Add to dropdown list if not already there
                if(mp_Ui->fwTypeBox->findText(fwObjectIter.value().toString()) == -1)
                {
                    mp_Ui->fwTypeBox->addItem(fwObjectIter.value().toString());
                }
            }
        }
    }
    QLOG_DEBUG() << "Found " << m_platformIndex.size() << " elements for platform " << platform;
    mp_Ui->fwTypeBox->model()->sort(0);
    fillFirmwareVersionBox(0);
    connect(mp_Ui->fwTypeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ApmCustomFirmwareConfig::fillFirmwareVersionBox);
}

void ApmCustomFirmwareConfig::fillPlatformBox(int index)
{
    Q_UNUSED(index)
    disconnect(mp_Ui->fwPlatformBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ApmCustomFirmwareConfig::fillMavtypeBox);

    QString category = mp_Ui->fwCategoryBox->currentText();
    m_categoryIndex.clear();
    mp_Ui->fwPlatformBox->clear();
    mp_Ui->fwPlatformBox->setInsertPolicy(QComboBox::InsertAlphabetically);

    // fill firmware types into drop down list
    for(int i = 0; i < m_availableFirmwares.size(); ++i)
    {
        auto fwObject = m_availableFirmwares[i].toObject();
        auto fwObjectIter = fwObject.find(s_FWVersionType);

        // JSON contains version in type field so we cut it here to Stable
        QString temp = fwObjectIter->toString().contains("STABLE", Qt::CaseSensitivity::CaseInsensitive) ? "STABLE" : fwObjectIter->toString();
        if((fwObjectIter != fwObject.end()) && (temp == category))
        {
            m_categoryIndex.push_back(i);
            fwObjectIter = fwObject.find(s_Platform);
            if(fwObjectIter != fwObject.end())
            {
                // Add to dropdown list if not already there
                if(mp_Ui->fwPlatformBox->findText(fwObjectIter.value().toString()) == -1)
                {
                    mp_Ui->fwPlatformBox->addItem(fwObjectIter.value().toString());
                }
            }
        }
    }
    QLOG_DEBUG() << "Found " << m_categoryIndex.size() << " elements ins category " << category;
    mp_Ui->fwPlatformBox->model()->sort(0);
    fillMavtypeBox(0);
    connect(mp_Ui->fwPlatformBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ApmCustomFirmwareConfig::fillMavtypeBox);
}

void ApmCustomFirmwareConfig::fillFirmwareCategoryBox()
{
    mp_Ui->fwCategoryBox->clear();
    mp_Ui->fwCategoryBox->setInsertPolicy(QComboBox::InsertAlphabetically);
    for(auto iter = m_availableFirmwares.begin(); iter != m_availableFirmwares.end(); ++iter)
    {
        QJsonObject fwObject = iter->toObject();
        auto fwObjectIter = fwObject.find(s_FWVersionType);
        if(fwObjectIter != fwObject.end())
        {
            // JSON contains version in type field so we cut it here to Stable
            QString temp = fwObjectIter->toString().contains("STABLE", Qt::CaseSensitivity::CaseInsensitive) ? "STABLE" : fwObjectIter->toString();

            // Add to dropdown list if not already there
            if(mp_Ui->fwCategoryBox->findText(temp) == -1)
            {
                mp_Ui->fwCategoryBox->addItem(temp);
            }
        }
    }
    mp_Ui->fwCategoryBox->model()->sort(0);
    fillPlatformBox(0);
    connect(mp_Ui->fwCategoryBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ApmCustomFirmwareConfig::fillPlatformBox);
}

void ApmCustomFirmwareConfig::fillFirmwareList(int index)
{
    Q_UNUSED(index)
    // selection is invalid as soon as we refill the list.
    m_selectedFwIndex = s_InvalidIndex;
    SetButtonState();

    m_versionIndex.clear();
    mp_Ui->fwListWidget->clearContents();
    int rowCount = 0;
    QString fwVersion = mp_Ui->fwVersionBox->currentText();
    for (auto const val : m_mavTypeIndex)
    {
        auto fwObject = m_availableFirmwares[val].toObject();
        auto fwObjectIter = fwObject.find(s_FWVersion);
        if((fwObjectIter != fwObject.end()) && (fwObjectIter->toString() == fwVersion))
        {
            QLOG_DEBUG() << "Found valid Firmware for version: " << fwVersion << " on index: " << val;
            m_versionIndex.push_back(val);

            mp_Ui->fwListWidget->setRowCount(rowCount + 1); // in row 0 we have one row
            fwObjectIter = fwObject.find("board_id");
            QTableWidgetItem *p_Item = nullptr;
            if (fwObjectIter != fwObject.end())
            {
                p_Item = createItem(QString::number(fwObjectIter->toInt()));
            }
            else
            {
                p_Item = createItem("Unknown");
            }
            mp_Ui->fwListWidget->setItem(rowCount, static_cast<int>(FirmwareListColumName::BoardId), p_Item);

            fwObjectIter = fwObject.find("manufacturer");
            if (fwObjectIter != fwObject.end())
            {
                p_Item = createItem(fwObjectIter->toString());
            }
            else
            {
                p_Item = createItem("Unknown");
            }
            mp_Ui->fwListWidget->setItem(rowCount, static_cast<int>(FirmwareListColumName::Manufacturer), p_Item);

            fwObjectIter = fwObject.find("brand_name");
            if (fwObjectIter != fwObject.end())
            {
                p_Item = createItem(fwObjectIter->toString());
            }
            else
            {
                p_Item = createItem("Unknown");
            }
            mp_Ui->fwListWidget->setItem(rowCount, static_cast<int>(FirmwareListColumName::BrandName), p_Item);

            fwObjectIter = fwObject.find(s_FWFormat);
            if (fwObjectIter != fwObject.end())
            {
                p_Item = createItem(fwObjectIter->toString());
            }
            else
            {
                p_Item = createItem("Unknown");
            }
            mp_Ui->fwListWidget->setItem(rowCount, static_cast<int>(FirmwareListColumName::FwType), p_Item);

            fwObjectIter = fwObject.find(s_Url);
            if (fwObjectIter != fwObject.end())
            {
                QUrl url(fwObjectIter->toString());
                p_Item = createItem(url.fileName());
            }
            else
            {
                p_Item = createItem("Unknown");
            }
            mp_Ui->fwListWidget->setItem(rowCount, static_cast<int>(FirmwareListColumName::FileName), p_Item);

            rowCount++;
        }
    }
}

void ApmCustomFirmwareConfig::downloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    if (QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender())) {
        if (reply == m_manifestReply.data()) {
            if (bytesReceived > m_lastManifestProgress) {
                m_lastManifestProgress = bytesReceived;
                m_manifestTimeout->start(s_NetworkTimeoutMs);
            }
        } else if (reply == m_firmwareReply.data()) {
            if (bytesReceived > m_lastFirmwareProgress) {
                m_lastFirmwareProgress = bytesReceived;
                m_firmwareTimeout->start(s_NetworkTimeoutMs);
            }
        } else {
            return;
        }
    }
    mp_Ui->progressBar->setMaximum(static_cast<int>(bytesTotal));
    mp_Ui->progressBar->setValue(static_cast<int>(bytesReceived));
}

void ApmCustomFirmwareConfig::downloadFirmwareFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) {
        return;
    }
    if (reply != m_firmwareReply.data()) {
        reply->deleteLater();
        return;
    }

    QLOG_DEBUG() << "ApmCustomFirmwareConfig: firmware download finished";

    m_firmwareTimeout->stop();
    m_firmwareReply.clear();
    m_lastFirmwareProgress = 0;
    disconnect(reply, nullptr, this, nullptr);

    const Operation completedOperation = m_operation;
    const int firmwareIndex = m_downloadFirmwareIndex;
    m_downloadFirmwareIndex = s_InvalidIndex;

    if (reply->error() != QNetworkReply::NoError) {
        const QString message = tr("Downloading firmware failed: %1")
                                    .arg(reply->errorString());
        QLOG_WARN() << "ApmCustomFirmwareConfig:" << message;
        mp_Ui->statusInfoWidget->append(message);
        reply->deleteLater();
        finishOperation();
        if (isVisible()) {
            QMessageBox::warning(this, tr("Error"), message);
        }
        return;
    }

    if (completedOperation != Operation::DownloadOnly
        && completedOperation != Operation::DownloadAndFlash) {
        QLOG_WARN() << "ApmCustomFirmwareConfig: ignoring firmware reply for stale operation";
        reply->deleteLater();
        finishOperation();
        return;
    }

    const QByteArray firmwarePayload = reply->readAll();
    reply->deleteLater();
    if (firmwareIndex < 0 || firmwareIndex >= m_availableFirmwares.size()) {
        QLOG_WARN() << "ApmCustomFirmwareConfig: downloaded firmware selection is stale";
        mp_Ui->statusInfoWidget->append(
            tr("Downloaded firmware selection is no longer available."));
        finishOperation();
        return;
    }

    if (completedOperation == Operation::DownloadOnly)
    {
        auto fwObject = m_availableFirmwares[firmwareIndex].toObject();
        auto iter = fwObject.find(s_FWFormat);
        if (iter != fwObject.end())
        {
            QString type(iter->toString());
            // Build file name
            iter = fwObject.find(s_Platform);
            QString fileName(iter->toString());

            iter = fwObject.find(s_MavType);
            fileName.append('_');
            fileName.append(iter->toString());

            iter = fwObject.find(s_FWVersion);
            fileName.append('_');
            fileName.append(iter->toString());

            fileName.append('.');
            fileName.append(type);

            QFileDialog dialog(this, "Save firmware to file", QDir::homePath(), "File format (*.hex *.px4 *.apj)");
            dialog.setAcceptMode(QFileDialog::AcceptSave);
            dialog.selectFile(fileName);
            QLOG_DEBUG() << "Suggested Filename: " << fileName;

            if(dialog.exec())
            {
                QString outputFileName = dialog.selectedFiles().at(0);
                QFile output(outputFileName);

                if (!output.open(QIODevice::ReadWrite | QIODevice::Truncate))
                {
                    QLOG_WARN() << "ApmCustomFirmwareConfig unable to open file for writing.";
                }
                output.write(firmwarePayload);
                output.close();
                if(type == "hex")
                {
                    QMessageBox::information(this, "How to flash", "For flashing *.hex files you will need an additional application."
                                                                   "Apm Planner does not support native flashing of hex files.<br>"
                                                                   "<a href=\"https://ardupilot.org/planner/docs/common-loading-firmware-onto-chibios-only-boards.html\">"
                                                                   "https://ardupilot.org/planner/docs/common-loading-firmware-onto-chibios-only-boards.html</a>");
                }
            }
        }
        else
        {
            QLOG_INFO() << "Firmwareobject has no valid format - dropping all data";
        }
        finishOperation();
    }
    else if (completedOperation == Operation::DownloadAndFlash)
    {
        mp_tempFWFile.reset(new QTemporaryFile);
        if (!mp_tempFWFile->open()
            || mp_tempFWFile->write(firmwarePayload) != firmwarePayload.size()) {
            mp_Ui->statusInfoWidget->append(
                tr("Unable to create a temporary firmware file."));
            mp_tempFWFile.reset();
            finishOperation();
            return;
        }
        mp_tempFWFile->flush();
        mp_tempFWFile->close();

        StartFlashFirmware(mp_tempFWFile->fileName());
    }

 }

 void ApmCustomFirmwareConfig::statusUpdate(QString update)
 {
     mp_Ui->statusInfoWidget->append(update);
 }

 void ApmCustomFirmwareConfig::flashWarn(QString message)
 {
     QMessageBox::information(this, "Warning", "Warning: " + message, "Continue");
 }

 void ApmCustomFirmwareConfig::flashError(QString error)
 {
     QMessageBox::information(this, "Error", "Error during upload:" + error);
     mp_Ui->statusInfoWidget->append("Error during upload: " + error);
     // Some uploader error paths emit complete(), others do not. Defer
     // cleanup so deleting the signal sender never happens in its own stack.
     QTimer::singleShot(0, this, [this]() {
         if (mp_px4Updater) {
             disconnect(mp_px4Updater.data(), nullptr, this, nullptr);
             mp_px4Updater->stop();
             mp_px4Updater.reset();
         }
         mp_tempFWFile.reset();
         finishOperation();
     });
 }

 void ApmCustomFirmwareConfig::startFlashing()
 {
     QLOG_DEBUG() << "ApmCustomFirmwareConfig::startFlashing";
     QString label("<h4>Flashing Firmware...</h4>");
     mp_Ui->progressbarLabel->setText(label);
 }

 void ApmCustomFirmwareConfig::flashingDone()
 {
     QLOG_DEBUG() << "ApmCustomFirmwareConfig::flashingDone";
     mp_Ui->statusInfoWidget->append("Flashing done.");

     QString label("<h4>Done!</h4>");
     mp_Ui->progressbarLabel->setText(label);
     mp_Ui->progressBar->setValue(mp_Ui->progressBar->maximum()); // force to 100%

     if (mp_px4Updater) {
         disconnect(mp_px4Updater.data(), nullptr, this, nullptr);
         mp_px4Updater->stop();
         mp_px4Updater.reset();
     }
     mp_tempFWFile.reset();
     finishOperation();
 }

 void ApmCustomFirmwareConfig::requestDeviceReplug()
 {
     QLOG_DEBUG() << "ApmCustomFirmwareConfig::requestDeviceReplug";

     if (!mp_px4Updater || !m_operationDeviceValid) {
         mp_Ui->statusInfoWidget->append(
             tr("The selected flashing device is no longer available."));
         QTimer::singleShot(0, this, [this]() { cancelButtonClicked(); });
         return;
     }

     mp_Ui->statusInfoWidget->append(
         "Waiting for replug of fc connected to " + m_operationDevice.portName());
     mp_requestDeviceReplug.reset(new QMessageBox);
     mp_requestDeviceReplug->setText("Please unplug, and plug back in the flight controller. After replugging the selected Firmware will be flashed!");
     mp_requestDeviceReplug->setStandardButtons(QMessageBox::Cancel);
     mp_requestDeviceReplug->open();

     connect(mp_requestDeviceReplug.data(), &QMessageBox::buttonClicked, this, &ApmCustomFirmwareConfig::cancelButtonClicked);

 }

 void ApmCustomFirmwareConfig::deviceReplugDetected()
 {
     QLOG_DEBUG() << "ApmCustomFirmwareConfig::deviceReplugDetected";

     if (mp_requestDeviceReplug) {
         mp_requestDeviceReplug->hide();
         mp_requestDeviceReplug.reset();
     }

     mp_Ui->statusInfoWidget->append("Flight controller reconnected - reading bootloader data.");
 }

 void ApmCustomFirmwareConfig::cancelButtonClicked()
 {
     QLOG_DEBUG() << "Cancel button pressed";

     if (mp_requestDeviceReplug) {
         mp_requestDeviceReplug->hide();
         mp_requestDeviceReplug.reset();
     }
     if (mp_px4Updater) {
         disconnect(mp_px4Updater.data(), nullptr, this, nullptr);
         mp_px4Updater->stop();
         mp_px4Updater.reset();
     }
     mp_tempFWFile.reset();
     finishOperation();

     mp_Ui->statusInfoWidget->append("Canceled");
 }
