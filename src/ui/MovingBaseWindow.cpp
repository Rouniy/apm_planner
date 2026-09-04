#include "MovingBaseWindow.h"

#include "comm/MovingBaseNmeaLog.h"
#include "comm/VehicleTargetManager.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace
{
constexpr int ModeRole = Qt::UserRole;
constexpr int SerialPortRole = Qt::UserRole + 1;

const QString InputKey = QStringLiteral("MovingBaseInput");
const QString BaudKey = QStringLiteral("MovingBaseBaud");
const QString HostKey = QStringLiteral("MovingBaseHost");
const QString PortKey = QStringLiteral("MovingBasePort");
const QString RateKey = QStringLiteral("MovingBaseRate");
const QString RallyKey = QStringLiteral("MovingBaseUpdateRally");
const QString RelativeAltitudeKey = QStringLiteral("MovingBaseRelativeAlt");
}

MovingBaseWindow::MovingBaseWindow(
    Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_targetManager(dependencies.targetManager)
    , m_service(dependencies.service)
    , m_transportFactory(std::move(dependencies.transportFactory))
    , m_enumeratePorts(std::move(dependencies.enumeratePorts))
    , m_validateSerial(std::move(dependencies.validateSerial))
    , m_validateUdpHostPort(
          std::move(dependencies.validateUdpHostPort))
    , m_readSetting(std::move(dependencies.readSetting))
    , m_writeSettings(std::move(dependencies.writeSettings))
    , m_rawLogPath(std::move(dependencies.rawLogPath))
{
    buildUi(owner);
    loadSettings();
    connectDependencies();
    refreshTargetDescription();
    syncInputFields();
    syncUi();
}

MovingBaseWindow::~MovingBaseWindow()
{
    shutdown();
}

QString MovingBaseWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

QString MovingBaseWindow::locationText() const
{
    return m_location ? m_location->text() : QString();
}

void MovingBaseWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("MovingBaseWindow"));
    setWindowTitle(tr("Moving Base"));
    setWindowFlag(Qt::Window, true);
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(WindowWidth, WindowHeight);
    setMinimumSize(MinimumWindowWidth, MinimumWindowHeight);
    setStyleSheet(QStringLiteral(
        "QWidget#MovingBaseWindow { background: #434445; color: #dddddd; }"
        "QLabel#movingBaseDescription, QLabel#movingBaseLocationCaption, "
        "QLabel#movingBaseLogNote { color: #aaaaaa; }"
        "QLabel#movingBaseTargetDescription, QLabel#movingBaseStatus { color: #34d399; }"
        "QFrame#movingBaseStatusPanel { background: #303132; border: 1px solid #555657; "
        "border-radius: 4px; }"));
    if (owner) {
        move(owner->frameGeometry().center()
             - QPoint(WindowWidth / 2, WindowHeight / 2));
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(9);

    auto *header = new QLabel(tr("Moving Base"), this);
    header->setObjectName(QStringLiteral("movingBaseHeader"));
    QFont headerFont = header->font();
    headerFont.setPointSize(16);
    header->setFont(headerFont);
    root->addWidget(header);

    auto *description = new QLabel(tr(
        "Reads GGA positions from an external NMEA GPS and shows the moving "
        "base on Flight Data. The session is bound to the exact selected "
        "modem, system and component and stops after a selection change."),
        this);
    description->setObjectName(QStringLiteral("movingBaseDescription"));
    description->setWordWrap(true);
    description->setTextFormat(Qt::PlainText);
    root->addWidget(description);

    m_target = new QLabel(this);
    m_target->setObjectName(QStringLiteral("movingBaseTargetDescription"));
    m_target->setWordWrap(true);
    m_target->setTextFormat(Qt::PlainText);
    root->addWidget(m_target);

    auto *inputGrid = new QGridLayout;
    inputGrid->setHorizontalSpacing(7);
    inputGrid->setVerticalSpacing(7);
    auto *inputLabel = new QLabel(tr("NMEA input"), this);
    inputLabel->setObjectName(QStringLiteral("movingBaseInputLabel"));
    inputGrid->addWidget(inputLabel, 0, 0);
    m_input = new QComboBox(this);
    m_input->setObjectName(QStringLiteral("movingBaseInput"));
    inputGrid->addWidget(m_input, 0, 1, 1, 2);
    m_refresh = new QPushButton(tr("Refresh"), this);
    m_refresh->setObjectName(QStringLiteral("movingBaseRefreshInputs"));
    inputGrid->addWidget(m_refresh, 0, 3);

    m_baudLabel = new QLabel(tr("Baud"), this);
    m_baudLabel->setObjectName(QStringLiteral("movingBaseBaudLabel"));
    inputGrid->addWidget(m_baudLabel, 1, 0);
    m_baud = new QComboBox(this);
    m_baud->setObjectName(QStringLiteral("movingBaseBaud"));
    for (int baud : MovingBaseInputTransport::supportedBaudRates()) {
        m_baud->addItem(QString::number(baud), baud);
    }
    inputGrid->addWidget(m_baud, 1, 1);

    m_hostLabel = new QLabel(tr("Remote host"), this);
    m_hostLabel->setObjectName(QStringLiteral("movingBaseHostLabel"));
    inputGrid->addWidget(m_hostLabel, 1, 0);
    m_host = new QLineEdit(this);
    m_host->setObjectName(QStringLiteral("movingBaseHost"));
    inputGrid->addWidget(m_host, 1, 1);

    m_portLabel = new QLabel(tr("Local port"), this);
    m_portLabel->setObjectName(QStringLiteral("movingBasePortLabel"));
    inputGrid->addWidget(m_portLabel, 1, 2);
    m_port = new QSpinBox(this);
    m_port->setObjectName(QStringLiteral("movingBasePort"));
    m_port->setRange(1, 65535);
    inputGrid->addWidget(m_port, 1, 3);
    inputGrid->setColumnStretch(1, 1);
    inputGrid->setColumnStretch(3, 1);
    root->addLayout(inputGrid);

    auto *rateGrid = new QGridLayout;
    rateGrid->setHorizontalSpacing(7);
    auto *rateLabel = new QLabel(tr("Update rate (Hz)"), this);
    rateLabel->setObjectName(QStringLiteral("movingBaseRateLabel"));
    rateGrid->addWidget(rateLabel, 0, 0);
    m_rate = new QComboBox(this);
    m_rate->setObjectName(QStringLiteral("movingBaseRate"));
    for (double rate : MovingBaseService::supportedRates()) {
        m_rate->addItem(QString::number(rate, 'g', 2), rate);
    }
    rateGrid->addWidget(m_rate, 0, 1);
    m_relativeAltitude = new QCheckBox(
        tr("Show relative altitude"), this);
    m_relativeAltitude->setObjectName(
        QStringLiteral("movingBaseRelativeAltitude"));
    m_relativeAltitude->setEnabled(false);
    m_relativeAltitude->setToolTip(tr(
        "Unavailable until the selected target's exact home-altitude source "
        "is connected; Moving Base positions remain AMSL."));
    rateGrid->addWidget(m_relativeAltitude, 0, 2, 1, 2);
    rateGrid->setColumnStretch(1, 1);
    rateGrid->setColumnStretch(3, 1);
    root->addLayout(rateGrid);

    m_updateRally = new QCheckBox(
        tr("Update vehicle rally point 0 every five seconds"), this);
    m_updateRally->setObjectName(QStringLiteral("movingBaseUpdateRally"));
    m_updateRally->setEnabled(false);
    m_updateRally->setToolTip(tr(
        "Unavailable until a current exact-target MAV_MISSION_TYPE_RALLY "
        "transaction is acknowledged and reconciled."));
    root->addWidget(m_updateRally);

    auto *buttonRow = new QHBoxLayout;
    m_toggle = new QPushButton(tr("Connect"), this);
    m_toggle->setObjectName(QStringLiteral("ToggleMovingBaseButton"));
    m_toggle->setMinimumWidth(100);
    buttonRow->addWidget(m_toggle);
    auto *logNote = new QLabel(tr(
        "Raw NMEA is kept in a bounded MovingBase.txt log."), this);
    logNote->setObjectName(QStringLiteral("movingBaseLogNote"));
    logNote->setWordWrap(true);
    buttonRow->addWidget(logNote, 1);
    root->addLayout(buttonRow);

    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("movingBaseStatusPanel"));
    panel->setFrameShape(QFrame::StyledPanel);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(10, 8, 10, 8);
    panelLayout->setSpacing(4);
    m_status = new QLabel(tr("Stopped."), panel);
    m_status->setObjectName(QStringLiteral("movingBaseStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    panelLayout->addWidget(m_status);
    auto *locationCaption = new QLabel(tr("Current base:"), panel);
    locationCaption->setObjectName(
        QStringLiteral("movingBaseLocationCaption"));
    panelLayout->addWidget(locationCaption);
    m_location = new QLabel(
        tr("No moving-base fix received."), panel);
    m_location->setObjectName(QStringLiteral("movingBaseLocation"));
    m_location->setWordWrap(true);
    m_location->setTextFormat(Qt::PlainText);
    m_location->setFont(
        QFontDatabase::systemFont(QFontDatabase::FixedFont));
    panelLayout->addWidget(m_location);
    root->addWidget(panel, 1);

    connect(m_input, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MovingBaseWindow::syncInputFields);
    connect(m_refresh, &QPushButton::clicked,
            this, &MovingBaseWindow::refreshInputs);
    connect(m_toggle, &QPushButton::clicked,
            this, &MovingBaseWindow::toggleSession);
}

void MovingBaseWindow::connectDependencies()
{
    if (m_targetManager) {
        connect(m_targetManager,
                &VehicleTargetManager::targetGenerationSettled,
                this, [this](qulonglong) {
                    if (!isRunning()) {
                        refreshTargetDescription();
                    }
                });
        connect(m_targetManager, &QObject::destroyed, this, [this]() {
            m_targetManager = nullptr;
            if (isRunning()) {
                stopAfterInputFailure(tr(
                    "Moving Base stopped: the vehicle target service closed."));
            } else {
                refreshTargetDescription();
                syncUi();
            }
        });
    }
    if (m_service) {
        connect(m_service, &MovingBaseService::statusChanged,
                this, [this](const QString &status) {
            if (m_service
                && sessionMatches(m_service->activeSession())) {
                m_status->setText(status);
            }
        });
        connect(m_service, &MovingBaseService::fixPublished,
                this, [this](const MovingBaseService::SessionToken &session,
                             const MovingBasePositionSnapshot &snapshot) {
            if (!sessionMatches(session)) {
                return;
            }
            m_location->setText(locationText(snapshot));
            m_status->setText(tr(
                "Moving-base position is active and visible on Flight Data."));
        });
        connect(m_service, &MovingBaseService::positionCleared,
                this, [this](const MovingBaseService::SessionToken &session,
                             const QString &reason) {
            if (!sessionMatches(session)) {
                return;
            }
            m_location->setText(tr("No moving-base fix received."));
            m_status->setText(reason);
        });
        connect(m_service, &MovingBaseService::sessionEnded,
                this, [this](const MovingBaseService::SessionToken &session,
                             MovingBaseService::RequestResult,
                             const QString &description) {
            if (sessionMatches(session)) {
                finishFromService(description);
            }
        });
        connect(m_service, &QObject::destroyed, this, [this]() {
            m_service = nullptr;
            if (isRunning()) {
                finishFromService(tr(
                    "Moving Base stopped: the shared service closed."));
            }
            syncUi();
        });
    }
}

void MovingBaseWindow::loadSettings()
{
    auto read = [this](const QString &key, const QVariant &fallback) {
        return m_readSetting ? m_readSetting(key, fallback) : fallback;
    };
    m_loadedInput = read(InputKey, QString()).toString().trimmed();
    refreshInputs();
    const int baud = read(BaudKey,
                          MovingBaseInputTransport::DefaultSerialBaud).toInt();
    int baudIndex = m_baud->findData(baud);
    if (baudIndex < 0) {
        baudIndex = m_baud->findData(
            MovingBaseInputTransport::DefaultSerialBaud);
    }
    m_baud->setCurrentIndex(qMax(0, baudIndex));
    m_host->setText(read(HostKey, QStringLiteral("127.0.0.1"))
                        .toString().trimmed());
    m_port->setValue(qBound(
        1, read(PortKey,
                MovingBaseInputTransport::DefaultNetworkPort).toInt(),
        65535));
    const double rate = read(
        RateKey, MovingBaseService::DefaultRateHz).toDouble();
    int rateIndex = m_rate->findData(rate);
    if (rateIndex < 0) {
        rateIndex = m_rate->findData(MovingBaseService::DefaultRateHz);
    }
    m_rate->setCurrentIndex(qMax(0, rateIndex));

    // Retain the reference controls but fail closed until their exact data and
    // command paths exist. Old true values cannot silently re-enable them.
    m_relativeAltitude->setChecked(false);
    m_updateRally->setChecked(false);
}

void MovingBaseWindow::persistSettingsWhenReady()
{
    if (m_settingsPersisted || !isRunning() || !m_transport
        || (m_transport->state() != MovingBaseInputTransport::State::Ready
            && m_transport->state()
                != MovingBaseInputTransport::State::Listening)) {
        return;
    }
    QVariantMap values;
    values.insert(InputKey, selectedInputSetting());
    values.insert(BaudKey, m_baud->currentData().toInt());
    values.insert(HostKey, m_host->text().trimmed());
    values.insert(PortKey, m_port->value());
    values.insert(RateKey, selectedRateHz());
    values.insert(RallyKey, false);
    values.insert(RelativeAltitudeKey, false);
    QString error;
    if (m_writeSettings && !m_writeSettings(values, &error)) {
        m_status->setText(tr(
            "Moving Base is active, but its settings could not be saved: %1")
            .arg(error.isEmpty() ? tr("settings backend error") : error));
        return;
    }
    m_settingsPersisted = true;
}

void MovingBaseWindow::refreshInputs()
{
    const QString desired = !m_loadedInput.isEmpty()
        ? m_loadedInput : selectedInputSetting();
    m_loadedInput.clear();
    m_input->blockSignals(true);
    m_input->clear();
    QStringList ports = m_enumeratePorts ? m_enumeratePorts() : QStringList();
    ports.removeDuplicates();
    ports.sort(Qt::CaseInsensitive);
    for (const QString &port : ports) {
        m_input->addItem(port);
        const int index = m_input->count() - 1;
        m_input->setItemData(
            index, static_cast<int>(MovingBaseInputTransport::Mode::Serial),
            ModeRole);
        m_input->setItemData(index, port, SerialPortRole);
    }
    for (MovingBaseInputTransport::Mode mode : {
             MovingBaseInputTransport::Mode::TcpHost,
             MovingBaseInputTransport::Mode::TcpClient,
             MovingBaseInputTransport::Mode::UdpHost,
             MovingBaseInputTransport::Mode::UdpClient}) {
        m_input->addItem(MovingBaseInputTransport::modeLabel(mode));
        m_input->setItemData(
            m_input->count() - 1, static_cast<int>(mode), ModeRole);
    }

    int selected = -1;
    MovingBaseInputTransport::Mode savedMode;
    if (modeFromSetting(desired, &savedMode)) {
        for (int i = 0; i < m_input->count(); ++i) {
            if (m_input->itemData(i, ModeRole).toInt()
                == static_cast<int>(savedMode)) {
                selected = i;
                break;
            }
        }
    } else if (!desired.isEmpty()) {
        for (int i = 0; i < m_input->count(); ++i) {
            if (m_input->itemData(i, SerialPortRole).toString()
                == desired) {
                selected = i;
                break;
            }
        }
    }
    m_input->setCurrentIndex(selected >= 0 ? selected : 0);
    m_input->blockSignals(false);
    syncInputFields();
}

void MovingBaseWindow::syncInputFields()
{
    const MovingBaseInputTransport::Mode mode = selectedMode();
    const bool serial = mode == MovingBaseInputTransport::Mode::Serial;
    const bool network = !serial;
    const bool client = mode == MovingBaseInputTransport::Mode::TcpClient
        || mode == MovingBaseInputTransport::Mode::UdpClient;
    m_baudLabel->setVisible(serial);
    m_baud->setVisible(serial);
    m_hostLabel->setVisible(client);
    m_host->setVisible(client);
    m_portLabel->setVisible(network);
    m_port->setVisible(network);
    m_portLabel->setText(
        client ? tr("Remote port") : tr("Local port"));
}

void MovingBaseWindow::syncUi()
{
    const bool running = isRunning();
    const bool available = m_targetManager && m_service
        && static_cast<bool>(m_transportFactory);
    m_input->setEnabled(!running);
    m_refresh->setEnabled(!running);
    m_baud->setEnabled(!running);
    m_host->setEnabled(!running);
    m_port->setEnabled(!running);
    m_rate->setEnabled(!running);
    m_toggle->setEnabled(available && !m_stopping);
    m_toggle->setText(running ? tr("Stop") : tr("Connect"));
    if (!running) {
        refreshTargetDescription();
    }
}

void MovingBaseWindow::refreshTargetDescription()
{
    if (!m_target || isRunning()) {
        return;
    }
    const VehicleTargetLease target = m_targetManager
        ? m_targetManager->acquireTarget() : VehicleTargetLease{};
    m_target->setText(target.isValid()
        ? tr("Ready for vehicle %1:%2 on the selected modem.")
              .arg(target.endpoint.systemId)
              .arg(target.endpoint.componentId)
        : tr("No suitable vehicle selected."));
}

void MovingBaseWindow::toggleSession()
{
    if (isRunning()) {
        stopSession(tr("Stopped."));
    } else {
        beginStart();
    }
}

void MovingBaseWindow::beginStart()
{
    if (m_closing || m_stopping || isRunning()) {
        return;
    }
    if (!m_targetManager || !m_service || !m_transportFactory) {
        m_status->setText(tr("Moving Base services are unavailable."));
        return;
    }

    MovingBaseInputTransport::Settings settings;
    settings.mode = selectedMode();
    settings.serialPort = selectedSerialPort();
    settings.baud = m_baud->currentData().toInt();
    const bool networkClient =
        settings.mode == MovingBaseInputTransport::Mode::TcpClient
        || settings.mode == MovingBaseInputTransport::Mode::UdpClient;
    // Host modes match MP10 and listen on all IPv4 interfaces. Never reuse a
    // previously persisted remote-client address as a local bind address.
    settings.host = networkClient
        ? m_host->text().trimmed() : QStringLiteral("0.0.0.0");
    settings.port = static_cast<quint16>(m_port->value());
    if (settings.mode == MovingBaseInputTransport::Mode::Serial
        && m_validateSerial) {
        const QString conflict = m_validateSerial(settings.serialPort);
        if (!conflict.isEmpty()) {
            m_status->setText(conflict);
            return;
        }
    }
    if (settings.mode == MovingBaseInputTransport::Mode::UdpHost
        && m_validateUdpHostPort) {
        const QString conflict = m_validateUdpHostPort(settings.port);
        if (!conflict.isEmpty()) {
            m_status->setText(conflict);
            return;
        }
    }

    const VehicleTargetLease lease = m_targetManager->acquireTarget();
    MovingBaseService::SessionToken session;
    const MovingBaseService::RequestResult started = m_service->start(
        this, lease, selectedRateHz(), &session);
    if (started != MovingBaseService::RequestResult::Started) {
        m_status->setText(tr("Moving Base could not start: %1")
            .arg(MovingBaseService::resultDescription(started)));
        refreshTargetDescription();
        return;
    }
    m_session = session;
    m_target->setText(tr("Bound to vehicle %1:%2 on the selected modem.")
        .arg(lease.endpoint.systemId)
        .arg(lease.endpoint.componentId));
    m_settingsPersisted = false;
    if (!m_rawLogPath.isEmpty()) {
        m_log = std::make_unique<MovingBaseNmeaLog>(m_rawLogPath);
    }

    MovingBaseInputTransport *transport =
        m_transportFactory(settings, this);
    if (!transport) {
        stopAfterInputFailure(tr(
            "Moving Base could not create the selected input."));
        return;
    }
    m_transport = transport;
    const quint64 operation = ++m_operationGeneration;
    connectTransport(transport, operation);
    syncUi();

    QString error;
    const bool transportStarted = transport->start(&error);
    if (operation != m_operationGeneration || m_transport != transport) {
        return;
    }
    if (!transportStarted) {
        stopAfterInputFailure(tr("Moving Base connection failed: %1")
            .arg(error.isEmpty() ? transport->lastError() : error));
        return;
    }
    m_status->setText(transport->statusText());
    persistSettingsWhenReady();
    syncUi();
}

void MovingBaseWindow::connectTransport(
    MovingBaseInputTransport *transport, quint64 operation)
{
    connect(transport, &MovingBaseInputTransport::rawLineReceived,
            this, [this, transport, operation](const QByteArray &line) {
        if (operation == m_operationGeneration && m_transport == transport) {
            appendRawLine(line);
        }
    });
    connect(transport, &MovingBaseInputTransport::fixReceived,
            this, [this, transport, operation](const NmeaGgaFix &fix) {
        if (operation != m_operationGeneration || m_transport != transport
            || !m_service || !isRunning()) {
            return;
        }
        const MovingBaseService::RequestResult result =
            m_service->submitFix(m_session, fix);
        if (result != MovingBaseService::RequestResult::Published
            && result != MovingBaseService::RequestResult::Queued) {
            stopAfterInputFailure(tr("Moving Base input was rejected: %1")
                .arg(MovingBaseService::resultDescription(result)));
        }
    });
    connect(transport, &MovingBaseInputTransport::noPositionFix,
            this, [this, transport, operation](const QString &description) {
        if (operation == m_operationGeneration && m_transport == transport
            && m_service && isRunning()) {
            m_service->reportNoFix(m_session, description);
        }
    });
    connect(transport, &MovingBaseInputTransport::statusChanged,
            this, [this, transport, operation](const QString &status) {
        if (operation == m_operationGeneration && m_transport == transport
            && isRunning()) {
            m_status->setText(status);
            persistSettingsWhenReady();
        }
    });
    connect(transport, &MovingBaseInputTransport::stateChanged,
            this, [this, transport, operation](
                      MovingBaseInputTransport::State) {
        if (operation == m_operationGeneration && m_transport == transport) {
            persistSettingsWhenReady();
        }
    });
    connect(transport, &MovingBaseInputTransport::sourceChanged,
            this, [this, transport, operation](const QString &source) {
        if (operation == m_operationGeneration && m_transport == transport
            && isRunning() && !source.isEmpty()) {
            m_status->setText(tr("Reading moving-base NMEA from %1.")
                                  .arg(source));
        }
    });
    connect(transport, &MovingBaseInputTransport::errorOccurred,
            this, [this, transport, operation](const QString &error) {
        if (operation == m_operationGeneration && m_transport == transport) {
            if (transport->state()
                == MovingBaseInputTransport::State::Error) {
                stopAfterInputFailure(tr("Moving Base input stopped: %1")
                                          .arg(error));
            } else {
                // TCP Host can lose one peer while its listener remains
                // healthy and ready for a replacement client.
                m_status->setText(error);
            }
        }
    });
    connect(transport, &QObject::destroyed,
            this, [this, transport, operation]() {
        if (operation == m_operationGeneration && m_transport == transport
            && !m_closing) {
            m_transport = nullptr;
            stopAfterInputFailure(tr(
                "Moving Base input closed unexpectedly."));
        }
    });
}

void MovingBaseWindow::stopSession(const QString &status)
{
    if (m_stopping) {
        return;
    }
    m_stopping = true;
    ++m_operationGeneration;
    const MovingBaseService::SessionToken session = m_session;
    m_session = {};
    releaseTransport();
    if (m_service && session.isValid()) {
        m_service->stop(session);
    }
    m_location->setText(tr("No moving-base fix received."));
    m_status->setText(status);
    m_stopping = false;
    syncUi();
}

void MovingBaseWindow::stopAfterInputFailure(const QString &status)
{
    if (m_stopping) {
        return;
    }
    m_stopping = true;
    ++m_operationGeneration;
    const MovingBaseService::SessionToken session = m_session;
    m_session = {};
    releaseTransport();
    if (m_service && session.isValid()) {
        m_service->stop(session);
    }
    m_location->setText(tr("No moving-base fix received."));
    m_status->setText(status);
    m_stopping = false;
    syncUi();
}

void MovingBaseWindow::finishFromService(const QString &status)
{
    ++m_operationGeneration;
    m_session = {};
    releaseTransport();
    m_location->setText(tr("No moving-base fix received."));
    m_status->setText(status);
    syncUi();
}

void MovingBaseWindow::releaseTransport()
{
    MovingBaseInputTransport *transport = m_transport.data();
    m_transport = nullptr;
    if (transport) {
        disconnect(transport, nullptr, this, nullptr);
        transport->stop();
        transport->deleteLater();
    }
    if (m_log) {
        m_log->close();
        m_log.reset();
    }
}

void MovingBaseWindow::appendRawLine(const QByteArray &line)
{
    if (!m_log) {
        return;
    }
    QString error;
    if (!m_log->appendLine(line, &error)) {
        m_log.reset();
        m_status->setText(tr(
            "Moving Base remains active, but raw NMEA logging stopped: %1")
            .arg(error));
    }
}

void MovingBaseWindow::shutdown()
{
    if (m_closing) {
        return;
    }
    m_closing = true;
    ++m_operationGeneration;
    const MovingBaseService::SessionToken session = m_session;
    m_session = {};
    releaseTransport();
    if (m_service && session.isValid()) {
        m_service->stop(session);
    }
}

MovingBaseInputTransport::Mode MovingBaseWindow::selectedMode() const
{
    if (!m_input || m_input->currentIndex() < 0) {
        return MovingBaseInputTransport::Mode::Serial;
    }
    return static_cast<MovingBaseInputTransport::Mode>(
        m_input->currentData(ModeRole).toInt());
}

QString MovingBaseWindow::selectedSerialPort() const
{
    return selectedMode() == MovingBaseInputTransport::Mode::Serial
        ? m_input->currentData(SerialPortRole).toString() : QString();
}

double MovingBaseWindow::selectedRateHz() const
{
    return m_rate ? m_rate->currentData().toDouble() : 0.0;
}

QString MovingBaseWindow::selectedInputSetting() const
{
    if (!m_input || m_input->currentIndex() < 0) {
        return QString();
    }
    return selectedMode() == MovingBaseInputTransport::Mode::Serial
        ? selectedSerialPort() : stableModeSetting(selectedMode());
}

bool MovingBaseWindow::sessionMatches(
    const MovingBaseService::SessionToken &session) const
{
    return m_session.isValid() && session.isValid()
        && session.owner.data() == this
        && session.generation == m_session.generation
        && sameLease(session.target, m_session.target);
}

bool MovingBaseWindow::sameLease(
    const VehicleTargetLease &left,
    const VehicleTargetLease &right) noexcept
{
    return left.generation == right.generation
        && left.endpoint.sameIdentity(right.endpoint);
}

QString MovingBaseWindow::locationText(
    const MovingBasePositionSnapshot &snapshot)
{
    if (!snapshot.isValid()) {
        return tr("No moving-base fix received.");
    }
    return QStringLiteral("%1 %2 %3 m AMSL; sats %4, HDOP %5")
        .arg(snapshot.fix.latitudeDegrees, 0, 'f', 7)
        .arg(snapshot.fix.longitudeDegrees, 0, 'f', 7)
        .arg(snapshot.fix.altitudeAmslMetres, 0, 'f', 1)
        .arg(snapshot.fix.satellites)
        .arg(snapshot.fix.hdop, 0, 'g', 3);
}

QString MovingBaseWindow::stableModeSetting(
    MovingBaseInputTransport::Mode mode)
{
    switch (mode) {
    case MovingBaseInputTransport::Mode::TcpHost:
        return QStringLiteral("TCP Host");
    case MovingBaseInputTransport::Mode::TcpClient:
        return QStringLiteral("TCP Client");
    case MovingBaseInputTransport::Mode::UdpHost:
        return QStringLiteral("UDP Host");
    case MovingBaseInputTransport::Mode::UdpClient:
        return QStringLiteral("UDP Client");
    case MovingBaseInputTransport::Mode::Serial:
        break;
    }
    return QString();
}

bool MovingBaseWindow::modeFromSetting(
    const QString &value, MovingBaseInputTransport::Mode *mode)
{
    for (MovingBaseInputTransport::Mode candidate : {
             MovingBaseInputTransport::Mode::TcpHost,
             MovingBaseInputTransport::Mode::TcpClient,
             MovingBaseInputTransport::Mode::UdpHost,
             MovingBaseInputTransport::Mode::UdpClient}) {
        if (value == stableModeSetting(candidate)) {
            if (mode) {
                *mode = candidate;
            }
            return true;
        }
    }
    return false;
}

void MovingBaseWindow::closeEvent(QCloseEvent *event)
{
    shutdown();
    QWidget::closeEvent(event);
}
