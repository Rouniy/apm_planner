#include "MavlinkSerialTcpBridgeWindow.h"

#include "comm/MavlinkSerialTcpBridgeService.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
QString baudLabel(quint32 baud)
{
    if (baud == 0)
        return MavlinkSerialTcpBridgeWindow::tr("Keep current baud");
    return QStringLiteral("%L1").arg(baud);
}
} // namespace

MavlinkSerialTcpBridgeWindow::MavlinkSerialTcpBridgeWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setObjectName(QStringLiteral("MavlinkSerialTcpBridgeWindow"));
    setWindowTitle(tr("MAVLink Serial TCP Bridge"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(680, 570);
    setMinimumSize(620, 520);
    buildUi();
    refresh();
}

MavlinkSerialTcpBridgeWindow::~MavlinkSerialTcpBridgeWindow()
{
    m_closing = true;
    ++m_bindingRevision;
    ++m_promptRevision;
    if (QCoreApplication::closingDown()) {
        if (m_prompt)
            m_prompt->blockSignals(true);
        m_prompt.clear();
    } else {
        dismissPrompt();
    }

    const QPointer<MavlinkSerialTcpBridgeService> service(m_service);
    const quint64 operationId = m_ownedOperationId;
    m_ownedOperationId = 0;
    if (service) {
        disconnect(service, nullptr, this, nullptr);
        if (operationId != 0 && service->busy()
            && service->operationId() == operationId) {
            QString ignored;
            service->stop(operationId, &ignored);
        }
    }
}

void MavlinkSerialTcpBridgeWindow::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *title = new QLabel(tr("MAVLink Serial TCP Bridge"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto *description = new QLabel(
        tr("One TCP client can exchange bytes with one autopilot UART through "
           "MAVLink SERIAL_CONTROL. The UART is released best-effort when the "
           "client disconnects or this bridge stops."), this);
    description->setObjectName(QStringLiteral(
        "MavlinkSerialTcpBridgeDescriptionLabel"));
    description->setTextFormat(Qt::PlainText);
    description->setWordWrap(true);
    root->addWidget(description);

    m_target = new QLabel(tr("No connected vehicle."), this);
    m_target->setObjectName(QStringLiteral(
        "MavlinkSerialTcpBridgeTargetLabel"));
    m_target->setTextFormat(Qt::PlainText);
    m_target->setWordWrap(true);
    m_target->setStyleSheet(QStringLiteral(
        "QLabel { background: rgba(60,110,150,45); border: 1px solid #5080a0; "
        "padding: 8px; }"));
    root->addWidget(m_target);

    auto *settings = new QGroupBox(tr("Bridge settings"), this);
    auto *settingsLayout = new QGridLayout(settings);
    settingsLayout->addWidget(new QLabel(tr("Autopilot UART"), settings), 0, 0);
    m_device = new QComboBox(settings);
    m_device->setObjectName(QStringLiteral(
        "MavlinkSerialTcpBridgeDeviceComboBox"));
    const QList<QPair<QString, int>> devices = {
        {tr("TELEM1 — first telemetry port"), SERIAL_CONTROL_DEV_TELEM1},
        {tr("TELEM2 — second telemetry port"), SERIAL_CONTROL_DEV_TELEM2},
        {tr("GPS1 — first GPS port"), SERIAL_CONTROL_DEV_GPS1},
        {tr("GPS2 — second GPS port"), SERIAL_CONTROL_DEV_GPS2},
        {tr("SHELL — system shell"), SERIAL_CONTROL_DEV_SHELL},
        {tr("SERIAL0"), SERIAL_CONTROL_SERIAL0},
        {tr("SERIAL1"), SERIAL_CONTROL_SERIAL1},
        {tr("SERIAL2"), SERIAL_CONTROL_SERIAL2},
        {tr("SERIAL3"), SERIAL_CONTROL_SERIAL3},
        {tr("SERIAL4"), SERIAL_CONTROL_SERIAL4},
        {tr("SERIAL5"), SERIAL_CONTROL_SERIAL5},
        {tr("SERIAL6"), SERIAL_CONTROL_SERIAL6},
        {tr("SERIAL7"), SERIAL_CONTROL_SERIAL7},
        {tr("SERIAL8"), SERIAL_CONTROL_SERIAL8},
        {tr("SERIAL9"), SERIAL_CONTROL_SERIAL9}};
    for (const auto &device : devices)
        m_device->addItem(device.first, device.second);
    m_device->setCurrentIndex(2);
    settingsLayout->addWidget(m_device, 0, 1);

    settingsLayout->addWidget(new QLabel(tr("UART baud"), settings), 1, 0);
    m_baud = new QComboBox(settings);
    m_baud->setObjectName(QStringLiteral(
        "MavlinkSerialTcpBridgeBaudComboBox"));
    const QList<quint32> baudRates = {
        0, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
    for (quint32 baud : baudRates)
        m_baud->addItem(baudLabel(baud), QVariant::fromValue(baud));
    settingsLayout->addWidget(m_baud, 1, 1);

    settingsLayout->addWidget(new QLabel(tr("TCP listen port"), settings), 2, 0);
    m_listenPort = new QSpinBox(settings);
    m_listenPort->setObjectName(QStringLiteral(
        "MavlinkSerialTcpBridgeListenPortSpinBox"));
    m_listenPort->setRange(1, 65535);
    m_listenPort->setValue(500);
    settingsLayout->addWidget(m_listenPort, 2, 1);
    settingsLayout->setColumnStretch(1, 1);
    root->addWidget(settings);

    m_allowRemote = new QCheckBox(
        tr("Allow remote TCP clients (listen on all IPv4 interfaces)"), this);
    m_allowRemote->setObjectName(QStringLiteral(
        "AllowRemoteSerialBridgeClientsCheckBox"));
    m_allowRemote->setChecked(false);
    m_allowRemote->setToolTip(tr(
        "Unencrypted and unauthenticated. Leave disabled for a localhost-only listener."));
    root->addWidget(m_allowRemote);

    auto *warning = new QLabel(
        tr("SERIAL_CONTROL has no target-system field. The bridge requires one "
           "fresh disarmed autopilot on an exact single-system link and stops "
           "when that vehicle or physical link session changes. Common "
           "ArduPilot firmware does not implement the SHELL device. MP10 polling "
           "can make the firmware handler wait up to 100 ms per request."), this);
    warning->setObjectName(QStringLiteral(
        "MavlinkSerialTcpBridgeWarningLabel"));
    warning->setTextFormat(Qt::PlainText);
    warning->setWordWrap(true);
    warning->setStyleSheet(QStringLiteral(
        "QLabel { background: rgba(160,48,48,45); border: 1px solid #c05050; "
        "padding: 8px; }"));
    root->addWidget(warning);

    m_toggle = new QPushButton(tr("Start"), this);
    m_toggle->setObjectName(QStringLiteral(
        "ToggleMavlinkSerialTcpBridgeButton"));
    m_toggle->setMinimumWidth(110);
    m_toggle->setDefault(false);
    m_toggle->setAutoDefault(false);
    connect(m_toggle, &QPushButton::clicked,
            this, &MavlinkSerialTcpBridgeWindow::toggleBridge);
    root->addWidget(m_toggle, 0, Qt::AlignLeft);

    auto *activity = new QGroupBox(tr("Bridge activity"), this);
    auto *activityLayout = new QGridLayout(activity);
    m_status = new QLabel(tr("Stopped."), activity);
    m_status->setObjectName(QStringLiteral(
        "MavlinkSerialTcpBridgeStatusLabel"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    activityLayout->addWidget(m_status, 0, 0, 1, 3);
    m_tcpToVehicle = new QLabel(tr("TCP → vehicle: 0 B"), activity);
    m_tcpToVehicle->setObjectName(QStringLiteral(
        "MavlinkSerialTcpBridgeTcpToVehicleCounter"));
    m_vehicleToTcp = new QLabel(tr("Vehicle → TCP: 0 B"), activity);
    m_vehicleToTcp->setObjectName(QStringLiteral(
        "MavlinkSerialTcpBridgeVehicleToTcpCounter"));
    m_dropped = new QLabel(tr("Dropped: 0 B"), activity);
    m_dropped->setObjectName(QStringLiteral(
        "MavlinkSerialTcpBridgeDroppedCounter"));
    activityLayout->addWidget(m_tcpToVehicle, 1, 0);
    activityLayout->addWidget(m_vehicleToTcp, 1, 1);
    activityLayout->addWidget(m_dropped, 1, 2);
    activityLayout->setColumnStretch(0, 1);
    activityLayout->setColumnStretch(1, 1);
    activityLayout->setColumnStretch(2, 1);
    root->addWidget(activity);
    root->addStretch(1);
}

void MavlinkSerialTcpBridgeWindow::setService(
    MavlinkSerialTcpBridgeService *service)
{
    if (m_closing)
        return;
    if (m_service == service) {
        refresh();
        return;
    }

    const quint64 revision = ++m_bindingRevision;
    ++m_promptRevision;
    const QPointer<MavlinkSerialTcpBridgeWindow> guard(this);
    const QPointer<MavlinkSerialTcpBridgeService> incoming(service);
    const QPointer<MavlinkSerialTcpBridgeService> old(m_service);
    const quint64 owned = m_ownedOperationId;
    dismissPrompt();
    if (!guard || revision != m_bindingRevision)
        return;

    m_service.clear();
    m_ownedOperationId = 0;
    if (old) {
        disconnect(old, nullptr, this, nullptr);
        if (owned != 0 && old->busy() && old->operationId() == owned) {
            QString ignored;
            old->stop(owned, &ignored);
        }
    }
    if (!guard || revision != m_bindingRevision)
        return;

    m_service = incoming;
    if (incoming) {
        connect(incoming, &MavlinkSerialTcpBridgeService::stateChanged,
                this, &MavlinkSerialTcpBridgeWindow::refresh);
        connect(incoming, &MavlinkSerialTcpBridgeService::finished,
                this, [this, revision](quint64 operationId,
                                      const QString &description) {
            if (revision != m_bindingRevision || m_closing)
                return;
            if (operationId == m_ownedOperationId)
                m_ownedOperationId = 0;
            const QPointer<MavlinkSerialTcpBridgeWindow> guard(this);
            refresh();
            if (!guard)
                return;
            setStatus(description);
            QTimer::singleShot(0, this, [this, revision]() {
                if (revision == m_bindingRevision && !m_closing)
                    refresh();
            });
        });
        connect(incoming, &QObject::destroyed, this, [this, revision]() {
            if (revision != m_bindingRevision)
                return;
            const QPointer<MavlinkSerialTcpBridgeWindow> guard(this);
            ++m_bindingRevision;
            ++m_promptRevision;
            m_service.clear();
            m_ownedOperationId = 0;
            const QPointer<QDialog> prompt(m_prompt);
            m_prompt.clear();
            if (m_closing || QCoreApplication::closingDown())
                return;
            if (prompt) {
                prompt->blockSignals(true);
                prompt->reject();
            }
            if (!guard)
                return;
            setStatus(tr("The guarded serial bridge service is unavailable."));
            refresh();
        });
    }
    refresh();
}

bool MavlinkSerialTcpBridgeWindow::ownsOperation() const noexcept
{
    return m_service && m_ownedOperationId != 0 && m_service->busy()
        && m_service->operationId() == m_ownedOperationId;
}

void MavlinkSerialTcpBridgeWindow::toggleBridge()
{
    if (ownsOperation())
        stopOwnedBridge(tr("Stopped by the operator."));
    else
        startBridge();
}

void MavlinkSerialTcpBridgeWindow::startBridge()
{
    const QPointer<MavlinkSerialTcpBridgeWindow> guard(this);
    const QPointer<MavlinkSerialTcpBridgeService> service(m_service);
    if (m_closing || !service || m_prompt || m_preparing
        || service->busy()) {
        refresh();
        return;
    }

    MavlinkSerialTcpBridgeService::Options options;
    options.device = static_cast<quint8>(m_device->currentData().toUInt());
    options.baudRate = m_baud->currentData().toUInt();
    options.listenPort = static_cast<quint16>(m_listenPort->value());
    options.allowRemoteClients = m_allowRemote->isChecked();
    const QString deviceLabel = selectedDeviceLabel();

    const quint64 bindingRevision = m_bindingRevision;
    const quint64 promptRevision = ++m_promptRevision;
    m_preparing = true;
    refresh();
    MavlinkSerialTcpBridgeService::Plan plan;
    QString error;
    const bool prepared = service->prepare(options, &plan, &error);
    if (!guard)
        return;
    m_preparing = false;
    if (m_closing || bindingRevision != m_bindingRevision
        || promptRevision != m_promptRevision || service != m_service
        || m_prompt || service->busy()) {
        refresh();
        return;
    }
    if (!prepared || !plan.isValid()) {
        const QString status = error.isEmpty()
            ? tr("The exact serial bridge target is unavailable.") : error;
        refresh();
        if (guard)
            setStatus(status);
        return;
    }

    const QString exposure = options.allowRemoteClients
        ? tr("The listener accepts clients on every IPv4 interface. It is "
             "unencrypted and unauthenticated; use only a trusted network and "
             "restrict the port with a firewall.")
        : tr("The listener accepts clients only from this computer (127.0.0.1)." );
    const QString baud = options.baudRate == 0
        ? tr("keep the current UART baud")
        : tr("request %L1 baud").arg(options.baudRate);
    const QString text = tr(
        "Start this exact MAVLink Serial TCP Bridge?\n\n"
        "Target: %1\nUART: %2\nBaud policy: %3\nTCP listen port: %4\n\n"
        "%5\n\nSERIAL_CONTROL takes exclusive ownership only while a TCP "
        "client is connected. Choosing a telemetry or GPS UART can interrupt "
        "this vehicle's telemetry or navigation input; recovery may require "
        "another connection or a vehicle reboot. Baud and flow-control "
        "settings are not restored. Start, release, and received data have no "
        "acknowledgement that guarantees UART ownership or release. The bridge "
        "stops if the exact vehicle/link session changes or becomes armed. "
        "A graceful TCP close drains already accepted bytes before release; "
        "Stop or target loss cancels that queued data.")
        .arg(plan.description(), deviceLabel, baud,
             QString::number(options.listenPort), exposure);

    auto *dialog = new QMessageBox(
        QMessageBox::Warning, tr("Start MAVLink Serial TCP Bridge"), text,
        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setObjectName(QStringLiteral(
        "MavlinkSerialTcpBridgeStartConfirmation"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    dialog->button(QMessageBox::Yes)->setText(tr("Start Bridge"));
    if (auto *yes = qobject_cast<QPushButton *>(
            dialog->button(QMessageBox::Yes))) {
        yes->setAutoDefault(false);
    }
    m_prompt = dialog;
    connect(dialog, &QDialog::finished, this,
            [this, service, dialog, plan, bindingRevision,
             promptRevision](int result) {
        if (m_closing || service != m_service
            || bindingRevision != m_bindingRevision
            || promptRevision != m_promptRevision
            || m_prompt != dialog) {
            return;
        }
        m_prompt.clear();
        if (result != QMessageBox::Yes) {
            refresh();
            setStatus(tr("Serial bridge start cancelled."));
            return;
        }

        const QPointer<MavlinkSerialTcpBridgeWindow> guard(this);
        QString error;
        if (!service->validate(plan, &error)) {
            if (!guard || m_closing || !service || service != m_service
                || bindingRevision != m_bindingRevision
                || promptRevision != m_promptRevision) {
                return;
            }
            const QString status = error.isEmpty()
                ? tr("The exact target changed after confirmation; start was cancelled.")
                : error;
            refresh();
            if (guard)
                setStatus(status);
            return;
        }
        if (!guard || !service || m_closing || service != m_service
            || bindingRevision != m_bindingRevision
            || promptRevision != m_promptRevision || service->busy()) {
            return;
        }

        m_ownedOperationId = 0;
        const bool started = service->start(
            plan, &m_ownedOperationId, &error);
        if (!guard || m_closing || !service || service != m_service
            || bindingRevision != m_bindingRevision
            || promptRevision != m_promptRevision) {
            return;
        }
        if (!started) {
            if (ownsOperation()) {
                refresh();
                return;
            }
            m_ownedOperationId = 0;
            const QString status = error.isEmpty()
                ? tr("The bridge was not started.") : error;
            refresh();
            if (guard)
                setStatus(status);
            return;
        } else if (ownsOperation()) {
            setStatus(service->status().isEmpty()
                ? tr("Starting the exact-target serial bridge…")
                : service->status());
        }
        refresh();
    });
    dialog->open();
    refresh();
}

void MavlinkSerialTcpBridgeWindow::stopOwnedBridge(const QString &reason)
{
    const QPointer<MavlinkSerialTcpBridgeWindow> guard(this);
    const QPointer<MavlinkSerialTcpBridgeService> service(m_service);
    const quint64 operationId = m_ownedOperationId;
    const quint64 bindingRevision = m_bindingRevision;
    if (!service || operationId == 0 || !service->busy()
        || service->operationId() != operationId) {
        m_ownedOperationId = 0;
        refresh();
        return;
    }

    QString error;
    const bool accepted = service->stop(operationId, &error);
    if (!guard || service != m_service
        || bindingRevision != m_bindingRevision
        || (m_ownedOperationId != 0
            && m_ownedOperationId != operationId)) {
        return;
    }
    if (!accepted) {
        const QString status = error.isEmpty()
            ? tr("The owned bridge is no longer current.") : error;
        refresh();
        if (guard)
            setStatus(status);
        return;
    } else if (!reason.isEmpty() && service && service->busy()
               && service->operationId() == operationId) {
        refresh();
        if (guard)
            setStatus(reason);
        return;
    }
    refresh();
}

void MavlinkSerialTcpBridgeWindow::dismissPrompt()
{
    ++m_promptRevision;
    const QPointer<QDialog> prompt(m_prompt);
    m_prompt.clear();
    if (prompt) {
        prompt->blockSignals(true);
        prompt->reject();
        if (prompt)
            prompt->deleteLater();
    }
}

void MavlinkSerialTcpBridgeWindow::refresh()
{
    if (m_refreshing || m_closing || QCoreApplication::closingDown())
        return;
    m_refreshing = true;
    const QPointer<MavlinkSerialTcpBridgeService> service(m_service);

    if (m_ownedOperationId != 0
        && (!service || !service->busy()
            || service->operationId() != m_ownedOperationId)) {
        m_ownedOperationId = 0;
    }
    const bool own = ownsOperation();
    const bool busy = service && service->busy();
    const bool settingsEnabled = service && !busy && !m_prompt && !m_preparing;
    m_device->setEnabled(settingsEnabled);
    m_baud->setEnabled(settingsEnabled);
    m_listenPort->setEnabled(settingsEnabled);
    m_allowRemote->setEnabled(settingsEnabled);
    m_toggle->setText(own ? tr("Stop") : tr("Start"));
    m_toggle->setEnabled(own || (service && !busy && !m_prompt && !m_preparing));
    m_toggle->setToolTip(own
        ? tr("Stop only the exact bridge operation started by this window.")
        : busy
            ? tr("Another application-owned serial bridge operation is active.")
            : service
                ? tr("Review and confirm the selected exact-target bridge.")
                : tr("The guarded serial bridge service is unavailable."));

    if (service) {
        const QString targetText = service->targetDescription();
        m_target->setText(targetText.isEmpty()
            ? tr("No eligible connected vehicle.") : targetText);
        const QString serviceStatus = service->status();
        if (!serviceStatus.isEmpty())
            m_status->setText(serviceStatus);
        m_tcpToVehicle->setText(tr("TCP → vehicle: %L1 B")
                                    .arg(service->bytesFromTcp()));
        m_vehicleToTcp->setText(tr("Vehicle → TCP: %L1 B")
                                    .arg(service->bytesToTcp()));
        m_dropped->setText(tr("Dropped: %L1 B")
                               .arg(service->droppedBytes()));
    } else {
        m_target->setText(tr("No connected vehicle."));
        m_tcpToVehicle->setText(tr("TCP → vehicle: 0 B"));
        m_vehicleToTcp->setText(tr("Vehicle → TCP: 0 B"));
        m_dropped->setText(tr("Dropped: 0 B"));
    }
    m_refreshing = false;
}

QString MavlinkSerialTcpBridgeWindow::selectedDeviceLabel() const
{
    return m_device ? m_device->currentText() : QString();
}

void MavlinkSerialTcpBridgeWindow::setStatus(const QString &status)
{
    if (m_status && !status.isEmpty())
        m_status->setText(status);
}

void MavlinkSerialTcpBridgeWindow::closeEvent(QCloseEvent *event)
{
    m_closing = true;
    ++m_promptRevision;
    dismissPrompt();
    const QPointer<MavlinkSerialTcpBridgeWindow> guard(this);
    stopOwnedBridge(QString());
    if (!guard)
        return;
    QWidget::closeEvent(event);
}
