#include "ConfigJoystickView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kMinimumButtonRows = 16;
constexpr int kDetectionThreshold = 8000;
constexpr int kDetectionTimeoutMs = 10000;

QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString boundedPath(const QString &path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
}

void retireCellWidgets(QTableWidget *table, int row)
{
    // setCellWidget defers deletion. Retired editors must not keep the live
    // object identity or emit edits while their replacement is being built.
    for (int column = 0; column < table->columnCount(); ++column) {
        if (auto *widget = table->cellWidget(row, column)) {
            widget->blockSignals(true);
            widget->setObjectName(QString());
            widget->hide();
        }
    }
}

} // namespace

ConfigJoystickView::ConfigJoystickView(QWidget *parent)
    : ConfigJoystickView(nullptr, nullptr, parent)
{
}

ConfigJoystickView::ConfigJoystickView(JoystickDevice *device,
                                       JoystickControlService *service,
                                       QWidget *parent)
    : QWidget(parent),
      m_device(device ? device : (service ? service->device() : nullptr)),
      m_service(service),
      m_detectionTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("ConfigJoystickView"));
    m_detectionTimer->setSingleShot(true);
    connect(m_detectionTimer, &QTimer::timeout, this, [this]() {
        const DetectionKind kind = m_detectionKind;
        cancelDetection(kind == DetectionAxis
            ? tr("No joystick axis movement was detected.")
            : tr("No new joystick button press was detected."));
    });

    buildUi();
    loadInitialProfile();
    connectSources();
    populateDevices();
    populateProfile();
    if (m_service) updatePreview(m_service->preview());
    refreshControls();
    refreshStatus();

    if (m_device) {
        m_device->refresh();
    }
}

ConfigJoystickView::~ConfigJoystickView()
{
    m_closing = true;
    ++m_promptRevision;
    cancelDetection();
    if (m_device && m_device->isCalibrating()) {
        m_device->cancelRangeCalibration();
    }
    dismissEnableConsent();

    auto dismissMessage = [](QPointer<QMessageBox> &box) {
        QPointer<QMessageBox> prompt = box;
        box.clear();
        if (!prompt) return;
        const bool blocked = prompt->blockSignals(true);
        prompt->reject();
        if (prompt) {
            prompt->blockSignals(blocked);
            prompt->deleteLater();
        }
    };
    dismissMessage(m_importConsent);
    dismissFileDialog(m_fileDialog);

    QPointer<QDialog> settings = m_buttonSettings;
    m_buttonSettings.clear();
    if (settings) {
        const bool blocked = settings->blockSignals(true);
        settings->reject();
        if (settings) {
            settings->blockSignals(blocked);
            settings->deleteLater();
        }
    }
    // The application-owned service deliberately remains enabled. This is
    // the MP10 behavior: joystick output continues while the operator returns
    // to Flight Data.
}

void ConfigJoystickView::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);

    auto *title = new QLabel(tr("Joystick"), this);
    title->setObjectName(QStringLiteral("JoystickTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("JoystickStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    root->addWidget(m_status);

    auto *deviceRow = new QGridLayout;
    deviceRow->setColumnStretch(1, 1);
    deviceRow->addWidget(new QLabel(tr("Device"), this), 0, 0);
    m_deviceCombo = new QComboBox(this);
    m_deviceCombo->setObjectName(QStringLiteral("JoystickDevice"));
    m_deviceCombo->setMinimumContentsLength(24);
    deviceRow->addWidget(m_deviceCombo, 0, 1);
    m_refresh = new QPushButton(tr("Refresh"), this);
    m_refresh->setObjectName(QStringLiteral("JoystickRefreshButton"));
    deviceRow->addWidget(m_refresh, 0, 2);
    m_elevons = new QCheckBox(tr("Elevons"), this);
    m_elevons->setObjectName(QStringLiteral("JoystickElevons"));
    deviceRow->addWidget(m_elevons, 0, 3);
    m_manualControl = new QCheckBox(tr("Manual Control"), this);
    m_manualControl->setObjectName(QStringLiteral("JoystickManualControl"));
    deviceRow->addWidget(m_manualControl, 0, 4);
    m_calibrateRange = new QPushButton(tr("Calibrate Range"), this);
    m_calibrateRange->setObjectName(
        QStringLiteral("JoystickCalibrateRangeButton"));
    deviceRow->addWidget(m_calibrateRange, 1, 1, 1, 2);
    m_resetRange = new QPushButton(tr("Reset Range"), this);
    m_resetRange->setObjectName(QStringLiteral("JoystickResetRangeButton"));
    deviceRow->addWidget(m_resetRange, 1, 3, 1, 2);
    root->addLayout(deviceRow);

    m_rawInput = new QLabel(
        tr("Raw input appears here after a joystick is selected."), this);
    m_rawInput->setObjectName(QStringLiteral("JoystickRawInput"));
    m_rawInput->setTextFormat(Qt::PlainText);
    m_rawInput->setWordWrap(true);
    QFont rawFont = m_rawInput->font();
    rawFont.setFamily(QStringLiteral("monospace"));
    m_rawInput->setFont(rawFont);
    root->addWidget(m_rawInput);

    auto *tables = new QSplitter(Qt::Vertical, this);
    tables->setObjectName(QStringLiteral("JoystickTables"));
    tables->setChildrenCollapsible(false);

    m_axes = new QTableWidget(0, 6, tables);
    m_axes->setObjectName(QStringLiteral("JoystickAxisTable"));
    m_axes->setHorizontalHeaderLabels({tr("Channel"), tr("Axis"),
        tr("Value"), tr("Expo"), tr("Reverse"), tr("Auto Detect")});
    m_axes->verticalHeader()->setVisible(false);
    m_axes->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_axes->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_axes->setSelectionMode(QAbstractItemView::NoSelection);
    m_axes->setMinimumHeight(220);
    tables->addWidget(m_axes);

    m_buttons = new QTableWidget(0, 6, tables);
    m_buttons->setObjectName(QStringLiteral("JoystickButtonTable"));
    m_buttons->setHorizontalHeaderLabels({tr("Slot"), tr("Button No."),
        tr("Detect"), tr("Function"), tr("Pressed"), tr("Settings")});
    m_buttons->verticalHeader()->setVisible(false);
    m_buttons->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_buttons->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_buttons->setSelectionMode(QAbstractItemView::NoSelection);
    m_buttons->setMinimumHeight(190);
    tables->addWidget(m_buttons);
    tables->setStretchFactor(0, 1);
    tables->setStretchFactor(1, 1);
    root->addWidget(tables, 1);

    auto *bottom = new QHBoxLayout;
    m_enable = new QPushButton(tr("Enable"), this);
    m_enable->setObjectName(QStringLiteral("JoystickEnableButton"));
    bottom->addWidget(m_enable);
    m_save = new QPushButton(tr("Save"), this);
    m_save->setObjectName(QStringLiteral("JoystickSaveButton"));
    bottom->addWidget(m_save);
    m_export = new QPushButton(tr("Export…"), this);
    m_export->setObjectName(QStringLiteral("JoystickExportButton"));
    bottom->addWidget(m_export);
    m_import = new QPushButton(tr("Import…"), this);
    m_import->setObjectName(QStringLiteral("JoystickImportButton"));
    bottom->addWidget(m_import);
    m_loadedConfig = new QLabel(tr("Loaded config: defaults"), this);
    m_loadedConfig->setObjectName(QStringLiteral("JoystickLoadedConfig"));
    m_loadedConfig->setTextFormat(Qt::PlainText);
    bottom->addWidget(m_loadedConfig, 1);
    root->addLayout(bottom);

    connect(m_refresh, &QPushButton::clicked, this, [this]() {
        if (m_device) m_device->refresh();
    });
    connect(m_deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ConfigJoystickView::selectDevice);
    connect(m_elevons, &QCheckBox::toggled, this, [this](bool value) {
        if (m_syncing) return;
        m_profile.elevons = value;
        QString error;
        QPointer<ConfigJoystickView> guard(this);
        const bool applied = applyProfileToService(&error);
        if (!guard) return;
        if (!applied && !error.isEmpty()) setStatus(error);
    });
    connect(m_manualControl, &QCheckBox::toggled, this, [this](bool value) {
        if (m_syncing) return;
        m_profile.manualControl = value;
        QString error;
        QPointer<ConfigJoystickView> guard(this);
        const bool applied = applyProfileToService(&error);
        if (!guard) return;
        if (!applied && !error.isEmpty()) setStatus(error);
        populateAxisRows();
        refreshControls();
    });
    connect(m_calibrateRange, &QPushButton::clicked,
            this, &ConfigJoystickView::toggleRangeCalibration);
    connect(m_resetRange, &QPushButton::clicked,
            this, &ConfigJoystickView::resetRangeCalibration);
    connect(m_enable, &QPushButton::clicked,
            this, &ConfigJoystickView::toggleEnable);
    connect(m_save, &QPushButton::clicked,
            this, &ConfigJoystickView::saveConfiguration);
    connect(m_export, &QPushButton::clicked,
            this, &ConfigJoystickView::beginExport);
    connect(m_import, &QPushButton::clicked,
            this, &ConfigJoystickView::beginImport);
}

void ConfigJoystickView::connectSources()
{
    if (m_device) {
        connect(m_device, &JoystickDevice::devicesChanged,
                this, &ConfigJoystickView::populateDevices);
        connect(m_device, &JoystickDevice::selectionChanged, this, [this]() {
            if (m_enableConsent) {
                QPointer<ConfigJoystickView> guard(this);
                ++m_promptRevision;
                dismissEnableConsent();
                if (!guard || m_closing) return;
                setStatus(tr("Joystick enable cancelled because the selected device changed."));
            }
            populateDevices();
            populateProfile();
            refreshControls();
        });
        connect(m_device, &JoystickDevice::snapshotChanged,
                this, &ConfigJoystickView::updateSnapshot);
        connect(m_device, &JoystickDevice::disconnected,
                this, [this](const QString &reason) {
            cancelDetection();
            if (m_enableConsent) {
                QPointer<ConfigJoystickView> guard(this);
                ++m_promptRevision;
                dismissEnableConsent();
                if (!guard) return;
            }
            setStatus(reason.isEmpty() ? tr("Joystick disconnected.") : reason);
            refreshControls();
        });
        connect(m_device, &JoystickDevice::calibrationChanged,
                this, &ConfigJoystickView::refreshControls);
        connect(m_device, &QObject::destroyed, this, [this]() {
            m_device.clear();
            cancelDetection();
            populateDevices();
            refreshControls();
        });
    }
    if (m_service) {
        connect(m_service, &JoystickControlService::stateChanged, this, [this]() {
            if (!m_service) return;
            if (m_enableConsent) {
                const JoystickControlService::EnablePlan plan = m_enablePlan;
                QPointer<ConfigJoystickView> guard(this);
                QPointer<JoystickControlService> service = m_service;
                QString error;
                const bool valid = service->validate(plan, &error);
                if (!guard || !service || service != m_service) return;
                if (!valid) {
                    QPointer<ConfigJoystickView> dismissGuard(this);
                    ++m_promptRevision;
                    dismissEnableConsent();
                    if (!dismissGuard) return;
                    refreshControls();
                    setStatus(error.isEmpty()
                        ? tr("Joystick enable cancelled because its exact route changed.")
                        : error);
                    return;
                }
            }
            if (m_submittingProfile) {
                refreshControls();
                refreshStatus();
                return;
            }
            m_profile = m_service->configuration();
            populateProfile();
            refreshControls();
            refreshStatus();
        });
        connect(m_service, &JoystickControlService::previewChanged,
                this, &ConfigJoystickView::updatePreview);
        connect(m_service, &JoystickControlService::stopped,
                this, [this](const QString &reason) {
            refreshControls();
            setStatus(reason);
        });
        connect(m_service, &JoystickControlService::buttonActionFinished,
                this, [this](const QString &function, bool accepted,
                             const QString &description) {
            setStatus(tr("Button %1: %2 (%3)")
                          .arg(function, description,
                               accepted ? tr("accepted") : tr("not accepted")));
        });
        connect(m_service, &QObject::destroyed, this, [this]() {
            m_service.clear();
            QPointer<ConfigJoystickView> guard(this);
            ++m_promptRevision;
            dismissEnableConsent();
            if (!guard) return;
            refreshControls();
            setStatus(tr("Joystick output service is unavailable. Local configuration remains editable."));
        });
    }
}

void ConfigJoystickView::loadInitialProfile()
{
    if (m_service) {
        m_profile = m_service->configuration();
    } else {
        QSettings settings;
        QString error;
        if (!JoystickConfiguration::load(&settings, &m_profile, &error)) {
            m_profile = JoystickConfiguration::defaults();
            if (!error.isEmpty()) setStatus(tr("Stored joystick configuration could not be loaded: %1").arg(error));
        }
    }
    JoystickConfiguration::normalize(&m_profile);
}

void ConfigJoystickView::populateDevices()
{
    if (!m_deviceCombo) return;
    const QSignalBlocker blocker(m_deviceCombo);
    const qint32 selectedInstance = m_device ? m_device->selectedDevice().instanceId : -1;
    const QString configuredId = m_profile.deviceId;
    m_deviceCombo->clear();
    if (!m_device) {
        m_deviceCombo->addItem(tr("Joystick service unavailable"), -1);
        return;
    }
    const QVector<JoystickDevice::Info> devices = m_device->devices();
    for (const JoystickDevice::Info &info : devices) {
        const QString label = info.name.isEmpty()
            ? info.id
            : tr("%1 — %2 axes, %3 buttons")
                  .arg(info.name).arg(info.axes).arg(info.buttons);
        m_deviceCombo->addItem(label, info.instanceId);
        m_deviceCombo->setItemData(m_deviceCombo->count() - 1, info.id,
                                   Qt::UserRole + 1);
    }
    if (devices.isEmpty()) {
        m_deviceCombo->addItem(tr("No joystick detected"), -1);
        return;
    }
    int index = m_deviceCombo->findData(selectedInstance);
    if (index < 0 && !configuredId.isEmpty()) {
        for (int i = 0; i < m_deviceCombo->count(); ++i) {
            if (m_deviceCombo->itemData(i, Qt::UserRole + 1).toString()
                    == configuredId) {
                index = i;
                break;
            }
        }
    }
    index = index >= 0 ? index : 0;
    m_deviceCombo->setCurrentIndex(index);
    if (selectedInstance < 0 && !(m_service && m_service->isEnabled())) {
        const qint32 candidate = m_deviceCombo->itemData(index).toInt();
        QTimer::singleShot(0, this, [this, candidate]() {
            if (!m_closing && m_device && !m_device->isOpen()
                && candidate >= 0) {
                const int current = m_deviceCombo->findData(candidate);
                if (current >= 0) selectDevice(current);
            }
        });
    }
}

void ConfigJoystickView::populateProfile()
{
    JoystickConfiguration::normalize(&m_profile);
    m_syncing = true;
    {
        const QSignalBlocker elevonSignals(m_elevons);
        const QSignalBlocker manualSignals(m_manualControl);
        m_elevons->setChecked(m_profile.elevons);
        m_manualControl->setChecked(m_profile.manualControl);
    }
    m_syncing = false;
    populateAxisRows();
    populateButtonRows();
    updateSnapshot(m_device ? m_device->snapshot() : JoystickDevice::Snapshot());
}

void ConfigJoystickView::populateAxisRows()
{
    m_syncing = true;
    const QSignalBlocker tableSignals(m_axes);
    m_axes->setRowCount(JoystickConfiguration::ChannelCount);
    const JoystickDevice::Info info = m_device ? m_device->selectedDevice()
                                               : JoystickDevice::Info();
    QStringList axes = JoystickConfiguration::axisNames(info.axes, info.hats);
    if (!axes.contains(QStringLiteral("None"))) axes.prepend(QStringLiteral("None"));
    for (int row = 0; row < JoystickConfiguration::ChannelCount; ++row) {
        retireCellWidgets(m_axes, row);
        const JoystickConfiguration::Channel channel = m_profile.channels.value(row);
        m_axes->setItem(row, 0, readOnlyItem(tr("RC %1").arg(row + 1)));
        auto *axis = new QComboBox(m_axes);
        axis->setObjectName(QStringLiteral("JoystickAxis_%1").arg(row + 1));
        axis->addItems(axes);
        if (axis->findText(channel.axis, Qt::MatchFixedString) < 0) {
            axis->addItem(channel.axis);
        }
        axis->setCurrentText(channel.axis);
        m_axes->setCellWidget(row, 1, axis);
        connect(axis, &QComboBox::currentTextChanged, this,
                [this, row]() { updateChannelFromRow(row); });

        auto *value = new QProgressBar(m_axes);
        value->setObjectName(QStringLiteral("JoystickValue_%1").arg(row + 1));
        value->setTextVisible(true);
        value->setRange(m_profile.manualControl ? -1000 : 1000,
                        m_profile.manualControl ? 1000 : 2000);
        value->setValue(m_profile.manualControl ? 0 : 1500);
        value->setFormat(QStringLiteral("%v"));
        m_axes->setCellWidget(row, 2, value);

        auto *expo = new QSpinBox(m_axes);
        expo->setObjectName(QStringLiteral("JoystickExpo_%1").arg(row + 1));
        expo->setRange(-100, 100);
        expo->setSuffix(QStringLiteral("%"));
        expo->setValue(channel.expo);
        m_axes->setCellWidget(row, 3, expo);
        connect(expo, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this, row]() { updateChannelFromRow(row); });

        auto *reverse = new QCheckBox(m_axes);
        reverse->setObjectName(QStringLiteral("JoystickReverse_%1").arg(row + 1));
        reverse->setChecked(channel.reverse);
        reverse->setStyleSheet(QStringLiteral("margin-left: 20px"));
        m_axes->setCellWidget(row, 4, reverse);
        connect(reverse, &QCheckBox::toggled, this,
                [this, row]() { updateChannelFromRow(row); });

        auto *detect = new QPushButton(tr("Auto Detect"), m_axes);
        detect->setObjectName(QStringLiteral("JoystickDetectAxis_%1").arg(row + 1));
        m_axes->setCellWidget(row, 5, detect);
        connect(detect, &QPushButton::clicked, this,
                [this, row]() { beginAxisDetection(row); });
    }
    m_syncing = false;
}

int ConfigJoystickView::visibleButtonRows() const
{
    const int physical = m_device ? m_device->selectedDevice().buttons : 0;
    return std::min(JoystickConfiguration::MaximumControls,
                    std::max(kMinimumButtonRows, physical));
}

void ConfigJoystickView::populateButtonRows()
{
    m_syncing = true;
    const QSignalBlocker tableSignals(m_buttons);
    const int rows = visibleButtonRows();
    m_buttons->setRowCount(rows);
    const QStringList functions = JoystickConfiguration::buttonFunctions();
    for (int row = 0; row < rows; ++row) {
        retireCellWidgets(m_buttons, row);
        const JoystickConfiguration::Button button = m_profile.buttons.value(row);
        m_buttons->setItem(row, 0, readOnlyItem(tr("But %1").arg(row + 1)));
        auto *number = new QSpinBox(m_buttons);
        number->setObjectName(QStringLiteral("JoystickButtonNumber_%1").arg(row + 1));
        number->setRange(-1, JoystickConfiguration::MaximumControls - 1);
        number->setSpecialValueText(tr("None"));
        number->setValue(button.buttonno);
        m_buttons->setCellWidget(row, 1, number);
        connect(number, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this, row]() { updateButtonFromRow(row); });

        auto *detect = new QPushButton(tr("Detect"), m_buttons);
        detect->setObjectName(QStringLiteral("JoystickDetectButton_%1").arg(row + 1));
        m_buttons->setCellWidget(row, 2, detect);
        connect(detect, &QPushButton::clicked, this,
                [this, row]() { beginButtonDetection(row); });

        auto *function = new QComboBox(m_buttons);
        function->setObjectName(QStringLiteral("JoystickButtonFunction_%1").arg(row + 1));
        function->addItems(functions);
        if (function->findText(button.function, Qt::MatchFixedString) < 0) {
            function->addItem(button.function);
        }
        function->setCurrentText(button.function);
        // Preserve imported values but do not offer actions without a working
        // parameter/pointing workflow as if a press could execute them.
        auto *functionModel = qobject_cast<QStandardItemModel *>(function->model());
        for (const QString &unavailable : {QStringLiteral("Toggle_Pan_Stab"),
                                          QStringLiteral("Gimbal_pnt_track")}) {
            const int index = function->findText(unavailable);
            if (functionModel && index >= 0) {
                functionModel->item(index)->setEnabled(false);
                functionModel->item(index)->setToolTip(tr("Not ported yet: requires the mount parameter/pointing workflow."));
            }
        }
        m_buttons->setCellWidget(row, 3, function);
        connect(function, &QComboBox::currentTextChanged, this,
                [this, row]() { updateButtonFromRow(row); });

        auto *pressed = new QCheckBox(m_buttons);
        pressed->setObjectName(QStringLiteral("JoystickButtonPressed_%1").arg(row + 1));
        pressed->setEnabled(false);
        pressed->setStyleSheet(QStringLiteral("margin-left: 20px"));
        m_buttons->setCellWidget(row, 4, pressed);

        auto *settings = new QPushButton(tr("Settings"), m_buttons);
        settings->setObjectName(QStringLiteral("JoystickButtonSettings_%1").arg(row + 1));
        m_buttons->setCellWidget(row, 5, settings);
        connect(settings, &QPushButton::clicked, this,
                [this, row]() { showButtonSettings(row); });
    }
    m_syncing = false;
}

void ConfigJoystickView::refreshControls()
{
    if (m_closing) return;
    const bool outputEnabled = m_service && m_service->isEnabled();
    const bool prompt = m_enableConsent || m_importConsent || m_fileDialog
        || m_buttonSettings;
    const bool calibrating = m_device && m_device->isCalibrating();
    const bool detecting = detectionActive();
    const bool deviceSelected = m_device && m_device->isOpen();
    const bool editable = !outputEnabled && !prompt && !calibrating && !detecting;

    m_deviceCombo->setEnabled(editable && m_device && !m_device->devices().isEmpty());
    m_refresh->setEnabled(editable && m_device);
    m_elevons->setEnabled(editable);
    m_manualControl->setEnabled(editable);
    m_axes->setEnabled(editable || detecting);
    m_buttons->setEnabled(editable || detecting);
    m_calibrateRange->setEnabled(m_device && deviceSelected && !outputEnabled
                                 && !prompt && !detecting);
    m_calibrateRange->setText(calibrating ? tr("Finish Calibration")
                                          : tr("Calibrate Range"));
    m_resetRange->setEnabled(editable && deviceSelected);
    m_save->setEnabled(editable);
    m_export->setEnabled(editable);
    m_import->setEnabled(editable);
    m_enable->setText(outputEnabled ? tr("Disable") : tr("Enable"));
    m_enable->setEnabled(m_service && (outputEnabled || (deviceSelected && !prompt
                                                         && !calibrating
                                                         && !detecting)));
    if (!m_service) {
        m_enable->setToolTip(tr("A connected vehicle and the guarded joystick output service are required to enable control."));
    } else if (!outputEnabled && !deviceSelected) {
        m_enable->setToolTip(tr("Select an available joystick first."));
    } else {
        m_enable->setToolTip(QString());
    }
}

void ConfigJoystickView::refreshStatus()
{
    if (m_service && !m_service->statusText().isEmpty()) {
        setStatus(m_service->statusText());
        return;
    }
    if (!m_device) {
        setStatus(tr("Local joystick mappings are available, but live joystick input and vehicle output services are not bound."));
    } else if (m_device->devices().isEmpty()) {
        setStatus(tr("No joystick detected. Connect a joystick and click Refresh."));
    } else if (!m_device->isOpen()) {
        setStatus(tr("Select a joystick to inspect raw input and configure its mappings."));
    } else {
        setStatus(tr("Map each RC channel to an axis. Enabling output requires separate consent for the exact connected vehicle."));
    }
}

void ConfigJoystickView::updatePreview(
    const JoystickControlService::Preview &preview)
{
    if (m_closing) return;
    for (int row = 0; row < m_axes->rowCount(); ++row) {
        if (auto *bar = qobject_cast<QProgressBar *>(m_axes->cellWidget(row, 2))) {
            if (row < preview.channels.size()) {
                const QSignalBlocker blocker(bar);
                bar->setValue(preview.channels.at(row));
            }
        }
    }
    for (int row = 0; row < m_buttons->rowCount(); ++row) {
        auto *pressed = qobject_cast<QCheckBox *>(m_buttons->cellWidget(row, 4));
        const int physical = m_profile.buttons.value(row).buttonno;
        if (pressed) {
            const QSignalBlocker blocker(pressed);
            pressed->setChecked(physical >= 0 && physical < preview.buttons.size()
                                && preview.buttons.at(physical));
        }
    }
}

void ConfigJoystickView::updateSnapshot(const JoystickDevice::Snapshot &snapshot)
{
    if (m_closing) return;
    m_rawInput->setText(rawSnapshotText(snapshot));
    if (!m_service || !m_service->isEnabled()) {
        JoystickControlService::Preview preview;
        preview.connected = snapshot.connected;
        preview.deviceGeneration = snapshot.generation;
        preview.buttons = snapshot.buttons;
        preview.channels.fill(m_profile.manualControl ? 0 : 1500,
                              JoystickConfiguration::ChannelCount);
        preview.mapped.fill(false, JoystickConfiguration::ChannelCount);
        for (int row = 0; row < m_profile.channels.size()
                           && row < JoystickConfiguration::ChannelCount; ++row) {
            quint16 normalized = 32768;
            if (JoystickConfiguration::axisValue(m_profile.channels.at(row).axis,
                                                  snapshot.axes, snapshot.hats,
                                                  &normalized)) {
                preview.mapped[row] = true;
                preview.channels[row] = JoystickConfiguration::channelValue(
                    normalized, m_profile.channels.at(row),
                    m_profile.manualControl);
            }
        }
        updatePreview(preview);
    }
    processDetection(snapshot);
}

void ConfigJoystickView::selectDevice(int comboIndex)
{
    if (m_syncing || m_closing || !m_device || comboIndex < 0) return;
    if (m_service && m_service->isEnabled()) return;
    const qint32 instance = m_deviceCombo->itemData(comboIndex).toInt();
    if (instance < 0) return;
    QString error;
    QPointer<ConfigJoystickView> guard(this);
    QPointer<JoystickDevice> device = m_device;
    const bool selected = device->selectDevice(instance, &error);
    if (!guard || !device || device != m_device) return;
    if (!selected) {
        setStatus(error.isEmpty() ? tr("Unable to open the selected joystick.") : error);
        return;
    }
    const JoystickDevice::Info info = m_device->selectedDevice();
    m_profile.deviceId = info.id;
    m_profile.deviceName = info.name;
    if (m_profile.calibration.contains(info.id)
        && !device->setRanges(m_profile.calibration.value(info.id), &error)) {
        if (!guard || !device || device != m_device) return;
        setStatus(tr("Stored range calibration was rejected: %1").arg(error));
    }
    if (!guard || !device || device != m_device) return;
    const bool applied = applyProfileToService(&error);
    if (!guard) return;
    if (!applied && !error.isEmpty()) setStatus(error);
    populateProfile();
    refreshControls();
}

void ConfigJoystickView::updateChannelFromRow(int row)
{
    if (m_syncing || row < 0 || row >= m_profile.channels.size()) return;
    auto *axis = qobject_cast<QComboBox *>(m_axes->cellWidget(row, 1));
    auto *expo = qobject_cast<QSpinBox *>(m_axes->cellWidget(row, 3));
    auto *reverse = qobject_cast<QCheckBox *>(m_axes->cellWidget(row, 4));
    if (!axis || !expo || !reverse) return;
    JoystickConfiguration::Channel &channel = m_profile.channels[row];
    channel.channel = row + 1;
    channel.axis = axis->currentText();
    channel.expo = expo->value();
    channel.reverse = reverse->isChecked();
    QString error;
    QPointer<ConfigJoystickView> guard(this);
    const bool applied = applyProfileToService(&error);
    if (!guard) return;
    if (!applied && !error.isEmpty()) setStatus(error);
    updateSnapshot(m_device ? m_device->snapshot() : JoystickDevice::Snapshot());
}

void ConfigJoystickView::updateButtonFromRow(int row)
{
    if (m_syncing || row < 0 || row >= m_profile.buttons.size()) return;
    auto *number = qobject_cast<QSpinBox *>(m_buttons->cellWidget(row, 1));
    auto *function = qobject_cast<QComboBox *>(m_buttons->cellWidget(row, 3));
    if (!number || !function) return;
    m_profile.buttons[row].buttonno = number->value();
    m_profile.buttons[row].function = function->currentText();
    QString error;
    QPointer<ConfigJoystickView> guard(this);
    const bool applied = applyProfileToService(&error);
    if (!guard) return;
    if (!applied && !error.isEmpty()) setStatus(error);
}

bool ConfigJoystickView::applyProfileToService(QString *error)
{
    m_profile.elevons = m_elevons->isChecked();
    m_profile.manualControl = m_manualControl->isChecked();
    JoystickConfiguration::normalize(&m_profile);
    if (!JoystickConfiguration::validate(m_profile, error)) return false;
    if (!m_service) return true;
    m_submittingProfile = true;
    QPointer<ConfigJoystickView> guard(this);
    QPointer<JoystickControlService> service = m_service;
    const JoystickConfiguration::Profile submitted = m_profile;
    const bool result = service->setConfiguration(submitted, error);
    // setConfiguration may synchronously delete this through another listener;
    // callers pin the view before invoking this method and never inspect this
    // member after a failed lifetime guard.
    if (guard) guard->m_submittingProfile = false;
    return result;
}

void ConfigJoystickView::toggleEnable()
{
    if (!m_service || m_closing) return;
    if (m_service->isEnabled()) {
        QPointer<ConfigJoystickView> guard(this);
        const bool stopped = m_service->disable(
            tr("Joystick disabled by the operator; RC override release was requested."));
        if (!guard) return;
        refreshControls();
        if (!stopped) setStatus(tr("Joystick output could not be disabled immediately; check the service status."));
        return;
    }

    QString error;
    QPointer<ConfigJoystickView> guard(this);
    if (!applyProfileToService(&error)) {
        if (!guard) return;
        setStatus(error);
        return;
    }
    if (!guard || !m_service || m_closing) return;
    JoystickControlService::EnablePlan plan;
    QPointer<JoystickControlService> service = m_service;
    if (!service->prepareEnable(&plan, &error)) {
        if (guard) setStatus(error);
        return;
    }
    if (!guard || !service || service != m_service || m_closing) return;
    showEnableConsent(plan);
}

void ConfigJoystickView::showEnableConsent(
    const JoystickControlService::EnablePlan &plan)
{
    if (m_enableConsent || !m_service || !plan.isValid() || m_closing) return;
    m_enablePlan = plan;
    const quint64 revision = ++m_promptRevision;
    auto *box = new QMessageBox(
        QMessageBox::Warning, tr("Enable Joystick Control"),
        tr("Enable continuous joystick control for this frozen route?\n\n"
           "%1\n\n"
           "RC override or MANUAL_CONTROL frames can move motors, servos and the vehicle. "
           "Keep the vehicle restrained and verify every mapping first. The selected device, "
           "its generation, configuration and exact vehicle target are revalidated before output starts.\n\n"
           "Closing this setup page does not disable an admitted joystick session; use Disable to release it.")
            .arg(plan.description()),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    box->setObjectName(QStringLiteral("JoystickEnableConfirmation"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    box->setDefaultButton(QMessageBox::Cancel);
    box->setEscapeButton(QMessageBox::Cancel);
    if (QPushButton *yes = qobject_cast<QPushButton *>(
            box->button(QMessageBox::Yes))) {
        yes->setText(tr("Enable"));
        yes->setAutoDefault(false);
    }
    m_enableConsent = box;
    connect(box, &QMessageBox::finished, this,
            [this, box, revision, plan](int result) {
        if (m_enableConsent == box) m_enableConsent.clear();
        if (revision != m_promptRevision || m_closing) return;
        ++m_promptRevision;
        m_enablePlan = JoystickControlService::EnablePlan();
        refreshControls();
        if (result != QMessageBox::Yes || !m_service) {
            setStatus(tr("Joystick enable cancelled; no control frames were started."));
            return;
        }
        QPointer<ConfigJoystickView> guard(this);
        QPointer<JoystickControlService> service = m_service;
        QString error;
        if (!service->validate(plan, &error)) {
            if (guard) setStatus(error);
            return;
        }
        if (!guard || !service || service != m_service || m_closing) return;
        if (!service->enable(plan, &error)) {
            if (guard) setStatus(error);
            return;
        }
        if (!guard || !service || service != m_service) return;
        refreshControls();
        refreshStatus();
    });
    QPointer<ConfigJoystickView> guard(this);
    box->open();
    if (guard) refreshControls();
}

void ConfigJoystickView::dismissEnableConsent()
{
    QPointer<QMessageBox> prompt = m_enableConsent;
    m_enableConsent.clear();
    m_enablePlan = JoystickControlService::EnablePlan();
    if (!prompt) return;
    const bool blocked = prompt->blockSignals(true);
    prompt->reject();
    if (prompt) {
        prompt->blockSignals(blocked);
        prompt->deleteLater();
    }
}

void ConfigJoystickView::saveConfiguration()
{
    QString error;
    QPointer<ConfigJoystickView> guard(this);
    if (!applyProfileToService(&error)) {
        if (!guard) return;
        setStatus(error);
        return;
    }
    if (!guard) return;
    QSettings settings;
    if (!JoystickConfiguration::save(&settings, m_profile, &error)) {
        setStatus(tr("Joystick configuration was not saved: %1").arg(error));
        return;
    }
    m_loadedConfig->setText(tr("Loaded config: saved profile"));
    setStatus(tr("Joystick configuration saved."));
}

void ConfigJoystickView::beginImport()
{
    if (m_importConsent || m_fileDialog || m_closing
        || (m_service && m_service->isEnabled())) return;
    const quint64 revision = ++m_promptRevision;
    auto *box = new QMessageBox(
        QMessageBox::Warning, tr("Import Joystick Config"),
        tr("Importing a .joycfg file replaces the editable axis, button and device profile. "
           "Export or save the current configuration first if it is needed."),
        QMessageBox::Yes | QMessageBox::Cancel, this);
    box->setObjectName(QStringLiteral("JoystickImportConfirmation"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    box->setDefaultButton(QMessageBox::Cancel);
    box->setEscapeButton(QMessageBox::Cancel);
    if (QPushButton *yes = qobject_cast<QPushButton *>(
            box->button(QMessageBox::Yes))) {
        yes->setText(tr("Choose File…"));
        yes->setAutoDefault(false);
    }
    m_importConsent = box;
    connect(box, &QMessageBox::finished, this,
            [this, box, revision](int result) {
        if (m_importConsent == box) m_importConsent.clear();
        if (revision != m_promptRevision || m_closing) return;
        ++m_promptRevision;
        refreshControls();
        if (result == QMessageBox::Yes) showImportPicker();
        else setStatus(tr("Joystick configuration import cancelled."));
    });
    QPointer<ConfigJoystickView> guard(this);
    box->open();
    if (guard) refreshControls();
}

void ConfigJoystickView::showImportPicker()
{
    if (m_fileDialog || m_closing) return;
    const quint64 revision = ++m_promptRevision;
    auto *picker = new QFileDialog(this, tr("Import joystick config"));
    picker->setObjectName(QStringLiteral("JoystickImportFileDialog"));
    picker->setAttribute(Qt::WA_DeleteOnClose);
    picker->setWindowModality(Qt::WindowModal);
    picker->setFileMode(QFileDialog::ExistingFile);
    picker->setAcceptMode(QFileDialog::AcceptOpen);
    picker->setNameFilters({tr("Joystick config (*.joycfg)"), tr("All files (*)")});
    m_fileDialog = picker;
    connect(picker, &QFileDialog::finished, this,
            [this, picker, revision](int result) {
        if (m_fileDialog == picker) m_fileDialog.clear();
        if (revision != m_promptRevision || m_closing) return;
        ++m_promptRevision;
        refreshControls();
        if (result != QDialog::Accepted || picker->selectedFiles().isEmpty()) return;
        const QString path = picker->selectedFiles().constFirst();
        JoystickConfiguration::Profile imported;
        QString error;
        if (!JoystickConfiguration::importConfig(path, &imported, &error)) {
            setStatus(tr("Import failed: %1").arg(error));
            return;
        }
        JoystickConfiguration::normalize(&imported);
        // A joycfg describes mappings, not permission to silently select a
        // similarly named controller. Apply it to the device the operator is
        // visibly configuring, matching MP10's import workflow.
        if (m_device && m_device->isOpen()) {
            const JoystickDevice::Info selected = m_device->selectedDevice();
            imported.deviceId = selected.id;
            imported.deviceName = selected.name;
        }
        QPointer<ConfigJoystickView> guard(this);
        if (m_service && !m_service->setConfiguration(imported, &error)) {
            if (guard) setStatus(tr("Import failed: %1").arg(error));
            return;
        }
        if (!guard) return;
        m_profile = imported;
        populateDevices();
        populateProfile();
        m_loadedConfig->setText(tr("Loaded config: %1").arg(QFileInfo(path).fileName()));
        setStatus(tr("Imported %1. Click Save to persist it, then Enable when ready.")
                      .arg(boundedPath(path)));
    });
    QPointer<ConfigJoystickView> guard(this);
    picker->open();
    if (guard) refreshControls();
}

void ConfigJoystickView::beginExport()
{
    if (m_fileDialog || m_closing || (m_service && m_service->isEnabled())) return;
    QString error;
    QPointer<ConfigJoystickView> guard(this);
    if (!applyProfileToService(&error)) {
        if (!guard) return;
        setStatus(error);
        return;
    }
    if (!guard) return;
    const JoystickConfiguration::Profile frozen = m_profile;
    const quint64 revision = ++m_promptRevision;
    auto *picker = new QFileDialog(this, tr("Export joystick config"));
    picker->setObjectName(QStringLiteral("JoystickExportFileDialog"));
    picker->setAttribute(Qt::WA_DeleteOnClose);
    picker->setWindowModality(Qt::WindowModal);
    picker->setFileMode(QFileDialog::AnyFile);
    picker->setAcceptMode(QFileDialog::AcceptSave);
    picker->setDefaultSuffix(QStringLiteral("joycfg"));
    picker->setNameFilters({tr("Joystick config (*.joycfg)"), tr("All files (*)")});
    m_fileDialog = picker;
    connect(picker, &QFileDialog::finished, this,
            [this, picker, revision, frozen](int result) {
        if (m_fileDialog == picker) m_fileDialog.clear();
        if (revision != m_promptRevision || m_closing) return;
        ++m_promptRevision;
        refreshControls();
        if (result != QDialog::Accepted || picker->selectedFiles().isEmpty()) return;
        QString path = picker->selectedFiles().constFirst();
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".joycfg");
        QString error;
        if (!JoystickConfiguration::exportConfig(path, frozen, &error)) {
            setStatus(tr("Export failed: %1").arg(error));
            return;
        }
        m_loadedConfig->setText(tr("Loaded config: %1").arg(QFileInfo(path).fileName()));
        setStatus(tr("Exported joystick configuration to %1").arg(boundedPath(path)));
    });
    QPointer<ConfigJoystickView> pickerGuard(this);
    picker->open();
    if (pickerGuard) refreshControls();
}

void ConfigJoystickView::showButtonSettings(int row)
{
    if (m_buttonSettings || m_closing || row < 0
        || row >= m_profile.buttons.size()
        || (m_service && m_service->isEnabled())) return;
    updateButtonFromRow(row);
    const JoystickConfiguration::Button frozen = m_profile.buttons.at(row);
    const quint64 revision = ++m_promptRevision;
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("JoystickButtonSettingsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Joystick Button %1 Settings").arg(row + 1));
    dialog->setWindowModality(Qt::WindowModal);
    auto *layout = new QVBoxLayout(dialog);
    auto *intro = new QLabel(
        tr("Configure the optional arguments for %1. Unused values are retained so exported .joycfg files round-trip exactly.")
            .arg(frozen.function), dialog);
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::PlainText);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    auto *mode = new QLineEdit(frozen.mode, dialog);
    mode->setObjectName(QStringLiteral("JoystickButtonMode"));
    form->addRow(tr("Flight mode"), mode);
    QVector<QDoubleSpinBox *> parameters;
    const double values[] = {frozen.p1, frozen.p2, frozen.p3, frozen.p4};
    for (int i = 0; i < 4; ++i) {
        auto *box = new QDoubleSpinBox(dialog);
        box->setObjectName(QStringLiteral("JoystickButtonP%1").arg(i + 1));
        box->setDecimals(6);
        box->setRange(-1000000000.0, 1000000000.0);
        box->setValue(values[i]);
        form->addRow(tr("P%1").arg(i + 1), box);
        parameters.append(box);
    }
    QStringList parameterLabels;
    bool showMode = false;
    if (frozen.function == QLatin1String("ChangeMode")) {
        showMode = true;
    } else if (frozen.function == QLatin1String("Mount_Mode")) {
        parameterLabels = QStringList{tr("Mount mode")};
    } else if (frozen.function == QLatin1String("Do_Set_Relay")) {
        parameterLabels = QStringList{tr("Relay number")};
    } else if (frozen.function == QLatin1String("Do_Set_Servo")) {
        parameterLabels = QStringList{tr("Servo number"), tr("PWM")};
    } else if (frozen.function == QLatin1String("Do_Repeat_Relay")) {
        parameterLabels = QStringList{tr("Relay number"), tr("Repeat count"),
                           tr("Cycle time")};
    } else if (frozen.function == QLatin1String("Do_Repeat_Servo")) {
        parameterLabels = QStringList{tr("Servo number"), tr("PWM"),
                           tr("Repeat time"), tr("Delay (ms)")};
    } else if (frozen.function == QLatin1String("Button_axis0")
               || frozen.function == QLatin1String("Button_axis1")) {
        parameterLabels = QStringList{tr("PWM 1"), tr("PWM 2")};
    }
    mode->setVisible(showMode);
    if (QWidget *label = form->labelForField(mode)) label->setVisible(showMode);
    for (int i = 0; i < parameters.size(); ++i) {
        const bool used = i < parameterLabels.size();
        parameters.at(i)->setVisible(used);
        if (QWidget *label = form->labelForField(parameters.at(i))) {
            label->setVisible(used);
            if (auto *text = qobject_cast<QLabel *>(label)) {
                text->setText(used ? parameterLabels.at(i)
                                   : tr("P%1").arg(i + 1));
            }
        }
    }
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save
                                         | QDialogButtonBox::Cancel, dialog);
    buttons->setObjectName(QStringLiteral("JoystickButtonSettingsButtons"));
    if (QPushButton *cancel = buttons->button(QDialogButtonBox::Cancel)) {
        cancel->setDefault(true);
        cancel->setAutoDefault(true);
    }
    if (QPushButton *save = buttons->button(QDialogButtonBox::Save)) {
        save->setAutoDefault(false);
    }
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    m_buttonSettings = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, dialog, revision, row, mode, parameters](int result) {
        if (m_buttonSettings == dialog) m_buttonSettings.clear();
        if (revision != m_promptRevision || m_closing) return;
        ++m_promptRevision;
        refreshControls();
        if (result != QDialog::Accepted || row < 0
            || row >= m_profile.buttons.size()) return;
        JoystickConfiguration::Button &button = m_profile.buttons[row];
        button.mode = mode->text();
        button.p1 = parameters.at(0)->value();
        button.p2 = parameters.at(1)->value();
        button.p3 = parameters.at(2)->value();
        button.p4 = parameters.at(3)->value();
        QString error;
        QPointer<ConfigJoystickView> guard(this);
        if (!applyProfileToService(&error)) {
            if (!guard) return;
            setStatus(error);
            return;
        }
        if (!guard) return;
        setStatus(tr("Button %1 settings updated for %2.")
                      .arg(row + 1).arg(button.function));
    });
    QPointer<ConfigJoystickView> guard(this);
    dialog->open();
    if (guard) refreshControls();
}

void ConfigJoystickView::beginAxisDetection(int row)
{
    if (m_detectionKind == DetectionAxis && m_detectionRow == row) {
        cancelDetection(tr("Joystick axis detection cancelled."));
        return;
    }
    if (!m_device || !m_device->isOpen() || row < 0
        || row >= m_profile.channels.size()) {
        setStatus(tr("Select and open a joystick before detecting an axis."));
        return;
    }
    cancelDetection();
    m_detectionKind = DetectionAxis;
    m_detectionRow = row;
    m_detectionBaseline = m_device->snapshot();
    m_detectionTimer->start(kDetectionTimeoutMs);
    setStatus(tr("Move the axis to assign to RC %1. Click Auto Detect again to cancel.")
                  .arg(row + 1));
    refreshControls();
    if (auto *button = qobject_cast<QPushButton *>(m_axes->cellWidget(row, 5))) {
        button->setEnabled(true);
        button->setText(tr("Cancel"));
    }
}

void ConfigJoystickView::beginButtonDetection(int row)
{
    if (m_detectionKind == DetectionButton && m_detectionRow == row) {
        cancelDetection(tr("Joystick button detection cancelled."));
        return;
    }
    if (!m_device || !m_device->isOpen() || row < 0
        || row >= m_profile.buttons.size()) {
        setStatus(tr("Select and open a joystick before detecting a button."));
        return;
    }
    cancelDetection();
    m_detectionKind = DetectionButton;
    m_detectionRow = row;
    m_detectionBaseline = m_device->snapshot();
    m_detectionTimer->start(kDetectionTimeoutMs);
    setStatus(tr("Press the physical button to assign to slot %1. Click Detect again to cancel.")
                  .arg(row + 1));
    refreshControls();
    if (auto *button = qobject_cast<QPushButton *>(m_buttons->cellWidget(row, 2))) {
        button->setEnabled(true);
        button->setText(tr("Cancel"));
    }
}

void ConfigJoystickView::cancelDetection(const QString &status)
{
    if (m_detectionTimer) m_detectionTimer->stop();
    const DetectionKind kind = m_detectionKind;
    const int row = m_detectionRow;
    m_detectionKind = DetectionNone;
    m_detectionRow = -1;
    m_detectionBaseline = JoystickDevice::Snapshot();
    if (kind == DetectionAxis && row >= 0 && row < m_axes->rowCount()) {
        if (auto *button = qobject_cast<QPushButton *>(m_axes->cellWidget(row, 5))) {
            button->setText(tr("Auto Detect"));
        }
    } else if (kind == DetectionButton && row >= 0 && row < m_buttons->rowCount()) {
        if (auto *button = qobject_cast<QPushButton *>(m_buttons->cellWidget(row, 2))) {
            button->setText(tr("Detect"));
        }
    }
    if (!status.isEmpty() && !m_closing) setStatus(status);
    if (!m_closing) refreshControls();
}

void ConfigJoystickView::processDetection(
    const JoystickDevice::Snapshot &snapshot)
{
    if (m_detectionKind == DetectionNone || !snapshot.connected
        || snapshot.generation != m_detectionBaseline.generation) return;
    if (m_detectionKind == DetectionAxis) {
        int best = -1;
        int distance = kDetectionThreshold;
        const int count = std::min(snapshot.axes.size(),
                                   m_detectionBaseline.axes.size());
        for (int i = 0; i < count; ++i) {
            const int moved = std::abs(int(snapshot.axes.at(i))
                                       - int(m_detectionBaseline.axes.at(i)));
            if (moved > distance) {
                best = i;
                distance = moved;
            }
        }
        if (best >= 0 && m_detectionRow >= 0) {
            const int row = m_detectionRow;
            const QString name = axisNameForIndex(best);
            cancelDetection();
            QPointer<ConfigJoystickView> guard(this);
            if (auto *axis = qobject_cast<QComboBox *>(m_axes->cellWidget(row, 1))) {
                if (axis->findText(name) < 0) axis->addItem(name);
                axis->setCurrentText(name);
            }
            if (!guard) return;
            setStatus(tr("RC %1 mapped to %2.").arg(row + 1).arg(name));
        }
        return;
    }
    const int count = std::min(snapshot.buttons.size(),
                               m_detectionBaseline.buttons.size());
    for (int i = 0; i < count; ++i) {
        if (snapshot.buttons.at(i) && !m_detectionBaseline.buttons.at(i)) {
            const int row = m_detectionRow;
            cancelDetection();
            QPointer<ConfigJoystickView> guard(this);
            if (auto *number = qobject_cast<QSpinBox *>(m_buttons->cellWidget(row, 1))) {
                number->setValue(i);
            }
            if (!guard) return;
            setStatus(tr("Button slot %1 assigned to physical button %2.")
                          .arg(row + 1).arg(i));
            break;
        }
    }
}

void ConfigJoystickView::toggleRangeCalibration()
{
    if (!m_device || (m_service && m_service->isEnabled())) return;
    QPointer<ConfigJoystickView> guard(this);
    QPointer<JoystickDevice> device = m_device;
    if (!m_device->isCalibrating()) {
        const bool started = m_device->isOpen()
            && m_device->beginRangeCalibration();
        if (!guard || !device || device != m_device) return;
        if (!started) {
            setStatus(device->lastError().isEmpty()
                ? tr("Unable to start joystick range calibration.")
                : device->lastError());
            return;
        }
        setStatus(tr("Control output is paused. Move every required stick and dial to both endpoints, then return throttle low and other sticks to centre before clicking Finish Calibration."));
    } else {
        const JoystickDevice::CalibrationResult result =
            device->finishRangeCalibration();
        if (!guard || !device || device != m_device) return;
        const QString id = device->selectedDevice().id;
        if (!id.isEmpty()) m_profile.calibration[id] = device->ranges();
        QString error;
        applyProfileToService(&error);
        if (!guard) return;
        setStatus(result.calibratedAxes > 0
            ? tr("Joystick range calibration saved for %1 axes; %2 axes were ignored.")
                  .arg(result.calibratedAxes).arg(result.ignoredAxes)
            : tr("No full axis movements were detected."));
    }
    refreshControls();
}

void ConfigJoystickView::resetRangeCalibration()
{
    if (!m_device || (m_service && m_service->isEnabled())) return;
    QPointer<ConfigJoystickView> guard(this);
    QPointer<JoystickDevice> device = m_device;
    const QString id = m_device->selectedDevice().id;
    device->resetRangeCalibration();
    if (!guard || !device || device != m_device) return;
    if (!id.isEmpty()) m_profile.calibration.remove(id);
    QString error;
    applyProfileToService(&error);
    if (!guard) return;
    setStatus(tr("Joystick range calibration reset; the native device range is active."));
    refreshControls();
}

void ConfigJoystickView::setStatus(const QString &status)
{
    if (m_status && !m_closing) m_status->setText(status);
}

QString ConfigJoystickView::rawSnapshotText(
    const JoystickDevice::Snapshot &snapshot) const
{
    if (!snapshot.connected) {
        return tr("Raw input appears here after a joystick is selected.");
    }
    QStringList axes;
    const int axisCount = std::min(snapshot.rawAxes.size(), 24);
    for (int i = 0; i < axisCount; ++i) {
        axes.append(QStringLiteral("%1=%2").arg(axisNameForIndex(i))
                    .arg(snapshot.rawAxes.at(i)));
    }
    QStringList pressed;
    for (int i = 0; i < snapshot.buttons.size() && pressed.size() < 32; ++i) {
        if (snapshot.buttons.at(i)) pressed.append(QString::number(i));
    }
    QStringList hats;
    for (int i = 0; i < snapshot.hats.size(); ++i) {
        hats.append(QStringLiteral("H%1=0x%2").arg(i)
                    .arg(snapshot.hats.at(i), 0, 16));
    }
    return tr("Axes: %1\nButtons pressed: %2%3")
        .arg(axes.join(QStringLiteral("  ")),
             pressed.isEmpty() ? tr("none") : pressed.join(QStringLiteral(", ")),
             hats.isEmpty() ? QString() : tr("\nHats: %1").arg(hats.join(QStringLiteral("  "))));
}

QString ConfigJoystickView::axisNameForIndex(int index) const
{
    static const QStringList nativeAliases = QStringLiteral(
        "X Y Z Rx Ry Rz Slider1 AZ AY AX Slider2").split(QLatin1Char(' '));
    return index >= 0 && index < nativeAliases.size()
        ? nativeAliases.at(index)
        : QStringLiteral("Axis%1").arg(index);
}

void ConfigJoystickView::dismissFileDialog(QPointer<QFileDialog> &dialog)
{
    QPointer<QFileDialog> picker = dialog;
    dialog.clear();
    if (!picker) return;
    const bool blocked = picker->blockSignals(true);
    picker->reject();
    if (picker) {
        picker->blockSignals(blocked);
        picker->deleteLater();
    }
}
