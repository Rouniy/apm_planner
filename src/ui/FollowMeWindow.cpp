#include "FollowMeWindow.h"

#include "comm/VehicleTargetManager.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace
{
bool sameLease(const VehicleTargetLease &left,
               const VehicleTargetLease &right)
{
    return left.isValid() && right.isValid()
        && left.generation == right.generation
        && left.endpoint == right.endpoint;
}

bool supportedRate(double rateHz)
{
    return rateHz == 0.25 || rateHz == 0.5
        || rateHz == 1.0 || rateHz == 2.0;
}
}

FollowMeWindow::FollowMeWindow(Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_targetManager(dependencies.targetManager)
    , m_guidedService(dependencies.guidedService)
    , m_gpsInputFactory(std::move(dependencies.gpsInputFactory))
    , m_enumeratePorts(std::move(dependencies.enumeratePorts))
    , m_validateSerial(std::move(dependencies.validateSerial))
    , m_confirmStart(std::move(dependencies.confirmStart))
    , m_monotonicMs(std::move(dependencies.monotonicMs))
{
    if (!m_enumeratePorts) {
        m_enumeratePorts = []() { return QStringList(); };
    }
    if (!m_validateSerial) {
        m_validateSerial = [](const QString &) { return QString(); };
    }
    // An omitted confirmation must fail closed. Production supplies a warning
    // dialog whose default and escape button are Cancel.
    if (!m_confirmStart) {
        m_confirmStart = [](QWidget *, const QString &, const QString &) {
            return false;
        };
    }
    m_defaultClock.start();
    if (!m_monotonicMs) {
        m_monotonicMs = [this]() { return m_defaultClock.elapsed(); };
    }

    m_updateTimer = new QTimer(this);
    m_updateTimer->setObjectName(QStringLiteral("followMeUpdateTimer"));
    m_updateTimer->setTimerType(Qt::PreciseTimer);
    buildUi(owner);
    connectDependencies();
    refreshPorts();
    refreshTargetDescription();
    syncUi();
}

FollowMeWindow::~FollowMeWindow()
{
    shutdown();
}

QString FollowMeWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

QString FollowMeWindow::locationText() const
{
    return m_location ? m_location->text() : QString();
}

int FollowMeWindow::updateIntervalMs(double rateHz)
{
    if (!supportedRate(rateHz)) {
        return 0;
    }
    return qRound(1000.0 / rateHz);
}

qint64 FollowMeWindow::maximumFixAgeMs(double rateHz)
{
    if (!supportedRate(rateHz)) {
        return 0;
    }
    return qMax<qint64>(5000, qRound64(3000.0 / rateHz));
}

void FollowMeWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("FollowMeWindow"));
    setWindowTitle(tr("Follow Me"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(WindowWidth, WindowHeight);
    setStyleSheet(QStringLiteral(
        "QWidget#FollowMeWindow { background: #434445; color: #dddddd; }"
        "QLabel#followMeDescription, QLabel#followMeLocationCaption { color: #aaaaaa; }"
        "QLabel#followMeTargetDescription, QLabel#followMeStatus { color: #34d399; }"
        "QFrame#followMeStatusPanel { background: #303132; border: 1px solid #555657; "
        "border-radius: 4px; }"));
    if (owner) {
        move(owner->frameGeometry().center()
             - QPoint(WindowWidth / 2, WindowHeight / 2));
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);

    auto *header = new QLabel(tr("Follow Me"), this);
    header->setObjectName(QStringLiteral("followMeHeader"));
    QFont headerFont = header->font();
    headerFont.setPointSize(16);
    header->setFont(headerFont);
    root->addWidget(header);

    auto *description = new QLabel(tr(
        "Streams GUIDED-mode targets so the vehicle follows a moving position. "
        "The session is bound to the exact selected modem, system and component. "
        "A target switch, input failure or stale GPS fix stops or withholds commands."), this);
    description->setObjectName(QStringLiteral("followMeDescription"));
    description->setWordWrap(true);
    description->setTextFormat(Qt::PlainText);
    root->addWidget(description);

    m_target = new QLabel(this);
    m_target->setObjectName(QStringLiteral("followMeTargetDescription"));
    m_target->setWordWrap(true);
    m_target->setTextFormat(Qt::PlainText);
    root->addWidget(m_target);

    m_manualSource = new QRadioButton(tr("Manual target"), this);
    m_manualSource->setObjectName(QStringLiteral("followMeManualSource"));
    m_manualSource->setChecked(true);
    root->addWidget(m_manualSource);

    auto *manualRow = new QGridLayout;
    manualRow->setContentsMargins(18, 0, 0, 0);
    manualRow->setHorizontalSpacing(6);
    auto *coordinates = new QLabel(tr("Lat / Lng"), this);
    coordinates->setObjectName(QStringLiteral("followMeCoordinatesLabel"));
    manualRow->addWidget(coordinates, 0, 0);
    m_manualLatitude = new QDoubleSpinBox(this);
    m_manualLatitude->setObjectName(QStringLiteral("followMeManualLat"));
    m_manualLatitude->setDecimals(7);
    m_manualLatitude->setRange(-90.0, 90.0);
    m_manualLatitude->setSingleStep(0.0001);
    manualRow->addWidget(m_manualLatitude, 0, 1);
    m_manualLongitude = new QDoubleSpinBox(this);
    m_manualLongitude->setObjectName(QStringLiteral("followMeManualLng"));
    m_manualLongitude->setDecimals(7);
    m_manualLongitude->setRange(-180.0, 180.0);
    m_manualLongitude->setSingleStep(0.0001);
    manualRow->addWidget(m_manualLongitude, 0, 2);
    m_useManual = new QPushButton(tr("Use"), this);
    m_useManual->setObjectName(QStringLiteral("followMeUse"));
    manualRow->addWidget(m_useManual, 0, 3);
    manualRow->setColumnStretch(1, 1);
    manualRow->setColumnStretch(2, 1);
    root->addLayout(manualRow);

    m_serialSource = new QRadioButton(tr("Serial NMEA GPS"), this);
    m_serialSource->setObjectName(QStringLiteral("followMeSerialSource"));
    root->addWidget(m_serialSource);

    auto *serialRows = new QGridLayout;
    serialRows->setContentsMargins(18, 0, 0, 0);
    serialRows->setHorizontalSpacing(6);
    serialRows->setVerticalSpacing(5);
    auto *portLabel = new QLabel(tr("Port"), this);
    portLabel->setObjectName(QStringLiteral("followMeSerialPortLabel"));
    serialRows->addWidget(portLabel, 0, 0);
    m_serialPort = new QComboBox(this);
    m_serialPort->setObjectName(QStringLiteral("followMeSerialPort"));
    serialRows->addWidget(m_serialPort, 0, 1);
    m_refreshPorts = new QPushButton(tr("Refresh"), this);
    m_refreshPorts->setObjectName(QStringLiteral("followMeRefreshPorts"));
    serialRows->addWidget(m_refreshPorts, 0, 2);
    auto *baudLabel = new QLabel(tr("Baud"), this);
    baudLabel->setObjectName(QStringLiteral("followMeBaudLabel"));
    serialRows->addWidget(baudLabel, 1, 0);
    m_baud = new QComboBox(this);
    m_baud->setObjectName(QStringLiteral("followMeBaud"));
    const QList<int> bauds = {
        4800, 9600, 14400, 19200, 28800, 38400, 57600, 115200
    };
    for (int baud : bauds) {
        m_baud->addItem(QString::number(baud), baud);
    }
    m_baud->setCurrentIndex(m_baud->findData(DefaultBaud));
    serialRows->addWidget(m_baud, 1, 1);
    serialRows->setColumnStretch(1, 1);
    root->addLayout(serialRows);

    auto *settingsRow = new QGridLayout;
    settingsRow->setHorizontalSpacing(6);
    auto *altitudeLabel = new QLabel(tr("Rel. alt (m)"), this);
    altitudeLabel->setObjectName(QStringLiteral("followMeRelativeAltitudeLabel"));
    settingsRow->addWidget(altitudeLabel, 0, 0);
    m_relativeAltitude = new QDoubleSpinBox(this);
    m_relativeAltitude->setObjectName(QStringLiteral("followMeRelativeAltitude"));
    m_relativeAltitude->setDecimals(1);
    m_relativeAltitude->setRange(0.0, 10000.0);
    m_relativeAltitude->setSingleStep(5.0);
    m_relativeAltitude->setValue(DefaultRelativeAltitudeM);
    settingsRow->addWidget(m_relativeAltitude, 0, 1);
    auto *rateLabel = new QLabel(tr("Rate (Hz)"), this);
    rateLabel->setObjectName(QStringLiteral("followMeRateLabel"));
    settingsRow->addWidget(rateLabel, 0, 2);
    m_rate = new QComboBox(this);
    m_rate->setObjectName(QStringLiteral("followMeRate"));
    for (double rate : {0.25, 0.5, 1.0, 2.0}) {
        m_rate->addItem(QString::number(rate, 'g', 2), rate);
    }
    m_rate->setCurrentIndex(m_rate->findData(DefaultRateHz));
    settingsRow->addWidget(m_rate, 0, 3);
    settingsRow->setColumnStretch(1, 1);
    settingsRow->setColumnStretch(3, 1);
    root->addLayout(settingsRow);

    auto *buttonRow = new QHBoxLayout;
    m_toggle = new QPushButton(tr("Start"), this);
    m_toggle->setObjectName(QStringLiteral("ToggleFollowMeButton"));
    m_toggle->setMinimumWidth(90);
    buttonRow->addWidget(m_toggle);
    buttonRow->addStretch(1);
    root->addLayout(buttonRow);

    auto *statusPanel = new QFrame(this);
    statusPanel->setObjectName(QStringLiteral("followMeStatusPanel"));
    statusPanel->setFrameShape(QFrame::StyledPanel);
    auto *statusLayout = new QVBoxLayout(statusPanel);
    statusLayout->setContentsMargins(10, 8, 10, 8);
    statusLayout->setSpacing(3);
    m_status = new QLabel(tr("Stopped."), statusPanel);
    m_status->setObjectName(QStringLiteral("followMeStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    statusLayout->addWidget(m_status);
    auto *locationCaption = new QLabel(tr("Target:"), statusPanel);
    locationCaption->setObjectName(QStringLiteral("followMeLocationCaption"));
    statusLayout->addWidget(locationCaption);
    m_location = new QLabel(tr("No target position received."), statusPanel);
    m_location->setObjectName(QStringLiteral("followMeLocationLabel"));
    m_location->setWordWrap(true);
    m_location->setTextFormat(Qt::PlainText);
    m_location->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    statusLayout->addWidget(m_location);
    root->addWidget(statusPanel, 1);

    connect(m_manualSource, &QRadioButton::toggled,
            this, &FollowMeWindow::syncUi);
    connect(m_serialSource, &QRadioButton::toggled,
            this, &FollowMeWindow::syncUi);
    connect(m_refreshPorts, &QPushButton::clicked,
            this, &FollowMeWindow::refreshPorts);
    connect(m_toggle, &QPushButton::clicked,
            this, &FollowMeWindow::toggleSession);
    connect(m_useManual, &QPushButton::clicked, this, [this]() {
        m_manualSource->setChecked(true);
        const double latitude = m_manualLatitude->value();
        const double longitude = m_manualLongitude->value();
        const double altitude = m_relativeAltitude->value();
        if (!validCoordinates(latitude, longitude)
            || !std::isfinite(altitude) || altitude <= 0.0
            || altitude > 10000.0) {
            m_status->setText(tr("Enter valid latitude, longitude and relative altitude first."));
            return;
        }
        GuidedTargetService::Target target;
        target.latitude = latitude;
        target.longitude = longitude;
        target.relativeAltitudeM = altitude;
        m_location->setText(targetText(target));
        m_status->setText(tr("Manual target selected. Press Start after verifying the vehicle."));
    });
    connect(m_updateTimer, &QTimer::timeout,
            this, &FollowMeWindow::sendCurrentTarget);
}

void FollowMeWindow::connectDependencies()
{
    if (m_targetManager) {
        connect(m_targetManager,
                &VehicleTargetManager::targetGenerationChanged,
                this, [this](qulonglong generation) {
            if (m_starting && m_pendingLease.isValid()
                && generation != m_pendingLease.generation) {
                abortStart(tr(
                    "The active modem or vehicle changed or disconnected. "
                    "Follow Me was not started; verify the selected target."));
                return;
            }
            if (isRunning()
                && generation != m_session.target.generation) {
                requestStop(tr(
                    "The active modem or vehicle changed or disconnected. "
                    "Follow Me was stopped; verify the selected target."));
                return;
            }
            if (!m_starting && !isRunning()) {
                refreshTargetDescription();
            }
        });
        connect(m_targetManager, &QObject::destroyed, this, [this]() {
            if (m_starting) {
                abortStart(tr("The vehicle target service is unavailable."));
            } else if (isRunning()) {
                requestStop(tr(
                    "Follow Me stopped: the vehicle target service is unavailable."));
            } else {
                refreshTargetDescription();
                syncUi();
            }
        });
    }

    if (!m_guidedService) {
        return;
    }
    connect(m_guidedService, &GuidedTargetService::targetAccepted,
            this,
            [this](GuidedTargetService::SessionToken session,
                   GuidedTargetService::Target target) {
        if (!sessionMatches(session) || m_closing || m_stopping) {
            return;
        }
        m_location->setText(targetText(target));
        m_status->setText(
            tr("Follow Me target accepted by %1.")
                .arg(targetDescription(session.target)));
    }, Qt::QueuedConnection);
    connect(m_guidedService, &GuidedTargetService::sessionEnded,
            this,
            [this](GuidedTargetService::SessionToken session,
                   GuidedTargetService::RequestResult,
                   const QString &description) {
        if (!sessionMatches(session) || m_closing) {
            return;
        }
        finishLocalSession(
            description.isEmpty() ? tr("Follow Me stopped.") : description);
    }, Qt::QueuedConnection);
    connect(m_guidedService, &GuidedTargetService::statusChanged,
            this, [this](const QString &status) {
        if (!m_guidedService || m_closing || status.isEmpty()) {
            return;
        }
        // A reserved serial session has no target yet. Keep the more useful
        // port-specific waiting text instead of replacing it with the shared
        // service's generic reservation status when the queued signal arrives.
        if (m_guidedService->state()
            == GuidedTargetService::State::Reserved) {
            return;
        }
        // Signals are queued to survive synchronous test/SITL ACKs. Drop an
        // older reservation/awaiting text if the service advanced before this
        // callback reached the window.
        if (status == m_guidedService->statusText()
            && sessionMatches(m_guidedService->activeSession())) {
            m_status->setText(status);
        }
    }, Qt::QueuedConnection);
    connect(m_guidedService, &QObject::destroyed, this, [this]() {
        if (m_starting) {
            abortStart(tr("The guided target service is unavailable."));
        } else if (isRunning()) {
            requestStop(tr(
                "Follow Me stopped: the guided target service is unavailable."));
        }
        syncUi();
    });
}

void FollowMeWindow::syncUi()
{
    const bool editable = !m_starting && !isRunning() && !m_stopping;
    m_manualSource->setEnabled(editable);
    m_serialSource->setEnabled(editable);
    m_manualLatitude->setEnabled(editable && m_manualSource->isChecked());
    m_manualLongitude->setEnabled(editable && m_manualSource->isChecked());
    m_useManual->setEnabled(editable && m_manualSource->isChecked());
    m_serialPort->setEnabled(editable && m_serialSource->isChecked());
    m_baud->setEnabled(editable && m_serialSource->isChecked());
    m_refreshPorts->setEnabled(editable && m_serialSource->isChecked());
    m_relativeAltitude->setEnabled(editable);
    m_rate->setEnabled(editable);
    m_toggle->setText(isRunning() ? tr("Stop") : tr("Start"));
    m_toggle->setEnabled(!m_starting && !m_stopping
                         && m_targetManager && m_guidedService);
}

void FollowMeWindow::refreshPorts()
{
    const QString previous = m_serialPort->currentText();
    QStringList ports;
    try {
        ports = m_enumeratePorts ? m_enumeratePorts() : QStringList();
    } catch (...) {
        ports.clear();
    }
    ports.removeAll(QString());
    ports.removeDuplicates();
    std::sort(ports.begin(), ports.end(), [](const QString &left,
                                             const QString &right) {
        return QString::localeAwareCompare(left, right) < 0;
    });
    const QSignalBlocker blocker(m_serialPort);
    m_serialPort->clear();
    m_serialPort->addItems(ports);
    const int priorIndex = m_serialPort->findText(previous);
    if (priorIndex >= 0) {
        m_serialPort->setCurrentIndex(priorIndex);
    }
}

void FollowMeWindow::refreshTargetDescription()
{
    const VehicleTargetLease lease = m_targetManager
        ? m_targetManager->acquireTarget() : VehicleTargetLease();
    m_target->setText(
        lease.isValid()
            ? tr("Ready for %1.").arg(targetDescription(lease))
            : tr("No connected vehicle selected."));
}

void FollowMeWindow::toggleSession()
{
    if (isRunning()) {
        requestStop(tr("Stopped."));
    } else if (!m_starting && !m_stopping) {
        beginStart();
    }
}

void FollowMeWindow::beginStart()
{
    if (!m_targetManager || !m_guidedService) {
        m_status->setText(tr("Follow Me services are unavailable."));
        return;
    }
    const double rateHz = selectedRateHz();
    const double altitude = m_relativeAltitude->value();
    if (!supportedRate(rateHz)) {
        m_status->setText(tr("Select a supported update rate."));
        return;
    }
    if (!std::isfinite(altitude) || altitude <= 0.0
        || altitude > 10000.0) {
        m_status->setText(tr(
            "Relative altitude must be greater than zero and no more than 10000 m."));
        return;
    }
    if (m_manualSource->isChecked()
        && !validCoordinates(m_manualLatitude->value(),
                             m_manualLongitude->value())) {
        m_status->setText(tr(
            "Enter valid latitude and longitude; Null Island (0, 0) is not a safe Follow Me target."));
        return;
    }
    if (m_serialSource->isChecked()) {
        if (m_serialPort->currentText().trimmed().isEmpty()
            || selectedBaud() <= 0) {
            m_status->setText(tr(
                "Select a GPS serial port and supported baud rate first."));
            return;
        }
        const QString conflict = m_validateSerial(
            m_serialPort->currentText().trimmed());
        if (!conflict.isEmpty()) {
            m_status->setText(conflict);
            return;
        }
    }

    const VehicleTargetLease lease = m_targetManager->acquireTarget();
    if (!leaseIsCurrent(lease)) {
        m_status->setText(tr(
            "Connect and select a settled vehicle before starting Follow Me."));
        refreshTargetDescription();
        return;
    }

    ++m_operationGeneration;
    m_starting = true;
    m_pendingLease = lease;
    m_activeRateHz = rateHz;
    m_activeRelativeAltitudeM = altitude;
    syncUi();

    const QString source = m_serialSource->isChecked()
        ? tr("NMEA GPS on %1 at %2 baud")
              .arg(m_serialPort->currentText())
              .arg(selectedBaud())
        : tr("the fixed target %1, %2 at %3 m")
              .arg(m_manualLatitude->value(), 0, 'f', 7)
              .arg(m_manualLongitude->value(), 0, 'f', 7)
              .arg(altitude, 0, 'f', 1);
    const QString warning = tr(
        "Follow Me will repeatedly command %1 to move toward %2. Verify GUIDED "
        "flight, relative altitude, surrounding airspace and the selected modem "
        "before continuing.")
        .arg(targetDescription(lease), source);
    const quint64 confirmationGeneration = m_operationGeneration;
    bool confirmed = false;
    try {
        confirmed = m_confirmStart(
            this, tr("Start Follow Me"), warning);
    } catch (...) {
        confirmed = false;
    }
    if (m_closing || !m_starting
        || confirmationGeneration != m_operationGeneration) {
        return;
    }
    if (!confirmed) {
        abortStart(tr("Follow Me start cancelled."));
        return;
    }
    // The modal confirmation runs a nested event loop. Never reserve or send
    // until the exact target generation has been re-read and verified.
    if (!leaseIsCurrent(lease)
        || !sameLease(m_targetManager->acquireTarget(), lease)) {
        abortStart(tr(
            "The active modem or vehicle changed while confirmation was open. "
            "Follow Me was not started."));
        return;
    }

    GuidedTargetService::SessionToken session;
    const GuidedTargetService::RequestResult reserved =
        m_guidedService->reserve(this, lease, &session);
    if (reserved != GuidedTargetService::RequestResult::Started
        || !session.isValid()) {
        abortStart(startFailureText(reserved));
        return;
    }

    m_session = session;
    m_pendingLease = VehicleTargetLease();
    m_starting = false;
    m_stopping = false;
    m_serialSession = m_serialSource->isChecked();
    m_hasLatestFix = false;
    m_updateTimer->setInterval(updateIntervalMs(m_activeRateHz));
    m_target->setText(
        tr("Bound to %1.").arg(targetDescription(session.target)));
    syncUi();

    if (m_serialSession) {
        startSerialInput();
    } else {
        startManualTarget();
    }
}

void FollowMeWindow::startManualTarget()
{
    if (!isRunning() || m_serialSession) {
        return;
    }
    m_manualTarget.latitude = m_manualLatitude->value();
    m_manualTarget.longitude = m_manualLongitude->value();
    m_manualTarget.relativeAltitudeM = m_activeRelativeAltitudeM;
    m_location->setText(targetText(m_manualTarget));
    sendCurrentTarget();
}

void FollowMeWindow::startSerialInput()
{
    if (!isRunning() || !m_serialSession) {
        return;
    }
    if (!m_gpsInputFactory) {
        requestStop(tr(
            "Follow Me stopped: the shared NMEA GPS input service is unavailable."));
        return;
    }
    try {
        m_gpsInput = m_gpsInputFactory(this);
    } catch (...) {
        m_gpsInput = nullptr;
    }
    if (!m_gpsInput) {
        requestStop(tr(
            "Follow Me stopped: the NMEA GPS input could not be created."));
        return;
    }

    const quint64 generation = m_operationGeneration;
    FollowMeGpsInput *input = m_gpsInput.data();
    if (input->parent() != this) {
        input->setParent(this);
    }
    connect(input, &FollowMeGpsInput::fixReceived, this,
            [this, input, generation](const NmeaGgaFix &fix) {
        if (m_gpsInput == input) {
            handleGpsFix(generation, fix);
        }
    });
    connect(input, &FollowMeGpsInput::noPositionFix, this,
            [this, input, generation](const QString &) {
        if (m_gpsInput == input) {
            handleNoPositionFix(generation);
        }
    });
    connect(input, &FollowMeGpsInput::inputStopped, this,
            [this, input, generation](const QString &error) {
        if (m_gpsInput == input) {
            handleGpsError(generation, error);
        }
    });

    m_status->setText(
        tr("Waiting for a valid GGA fix from %1.")
            .arg(m_serialPort->currentText()));
    QString error;
    bool opened = false;
    try {
        opened = input->open(
            m_serialPort->currentText(), selectedBaud(), &error);
    } catch (const std::exception &exception) {
        error = QString::fromUtf8(exception.what());
    } catch (...) {
        error = tr("unknown serial input failure");
    }
    if (generation != m_operationGeneration || !isRunning()) {
        return;
    }
    if (!opened) {
        requestStop(tr("Follow Me GPS input failed: %1")
                        .arg(error.isEmpty() ? tr("cannot open the selected port")
                                             : error));
    }
}

void FollowMeWindow::handleGpsFix(
    quint64 generation, const NmeaGgaFix &fix)
{
    if (generation != m_operationGeneration || !isRunning()
        || !m_serialSession || m_stopping || m_closing) {
        return;
    }
    if (!validCoordinates(fix.latitude, fix.longitude)) {
        m_status->setText(tr(
            "The NMEA GGA coordinates are invalid; GUIDED updates are withheld."));
        return;
    }

    const bool firstFix = !m_hasLatestFix;
    m_latestFix = fix;
    m_latestFixReceivedMs = m_monotonicMs();
    m_hasLatestFix = true;
    GuidedTargetService::Target target;
    target.latitude = fix.latitude;
    target.longitude = fix.longitude;
    // Follow Me intentionally ignores GGA altitude; the operator-confirmed
    // relative altitude is used for every GLOBAL_RELATIVE_ALT target.
    target.relativeAltitudeM = m_activeRelativeAltitudeM;
    m_location->setText(targetText(target, &fix));
    if (firstFix && !m_updateTimer->isActive()) {
        sendCurrentTarget();
    }
}

void FollowMeWindow::handleNoPositionFix(quint64 generation)
{
    if (generation != m_operationGeneration || !isRunning()
        || !m_serialSession || m_stopping || m_closing) {
        return;
    }
    m_hasLatestFix = false;
    m_location->setText(tr("No target position received."));
    m_status->setText(tr(
        "GPS has no position fix; GUIDED updates are withheld."));
}

void FollowMeWindow::handleGpsError(
    quint64 generation, const QString &error)
{
    if (generation != m_operationGeneration || !isRunning()
        || !m_serialSession || m_stopping || m_closing) {
        return;
    }
    requestStop(tr("Follow Me GPS input stopped: %1")
                    .arg(error.isEmpty() ? tr("input unavailable") : error));
}

void FollowMeWindow::sendCurrentTarget()
{
    if (!isRunning() || m_stopping || m_closing || !m_guidedService) {
        return;
    }

    GuidedTargetService::Target target = m_manualTarget;
    if (m_serialSession) {
        if (!m_hasLatestFix) {
            m_status->setText(tr(
                "Waiting for a valid target position; no GUIDED update was sent."));
            return;
        }
        const qint64 ageMs = m_monotonicMs() - m_latestFixReceivedMs;
        if (ageMs < 0 || ageMs > maximumFixAgeMs(m_activeRateHz)) {
            m_status->setText(tr(
                "The NMEA fix is stale; GUIDED updates are withheld until a fresh fix arrives."));
            return;
        }
        target.latitude = m_latestFix.latitude;
        target.longitude = m_latestFix.longitude;
        target.relativeAltitudeM = m_activeRelativeAltitudeM;
    }

    const GuidedTargetService::RequestResult result =
        m_guidedService->submit(m_session, target);
    if (result == GuidedTargetService::RequestResult::Sent
        || result == GuidedTargetService::RequestResult::Queued) {
        if (!m_updateTimer->isActive()) {
            m_updateTimer->start(updateIntervalMs(m_activeRateHz));
        }
        m_status->setText(
            result == GuidedTargetService::RequestResult::Queued
                ? tr("The newest Follow Me target is queued behind the pending command.")
                : tr("Follow Me target sent; awaiting vehicle acknowledgement."));
        return;
    }

    requestStop(tr("Follow Me stopped: %1")
                    .arg(GuidedTargetService::resultDescription(result)));
}

void FollowMeWindow::requestStop(const QString &status)
{
    ++m_operationGeneration;
    m_starting = false;
    m_stopping = true;
    m_pendingLease = VehicleTargetLease();
    if (m_updateTimer) {
        m_updateTimer->stop();
    }
    releaseGpsInput();
    const GuidedTargetService::SessionToken session = m_session;
    if (m_guidedService && session.isValid()) {
        m_guidedService->stop(session);
    }
    finishLocalSession(status);
}

void FollowMeWindow::finishLocalSession(const QString &status)
{
    ++m_operationGeneration;
    if (m_updateTimer) {
        m_updateTimer->stop();
    }
    releaseGpsInput();
    m_starting = false;
    m_stopping = false;
    m_serialSession = false;
    m_hasLatestFix = false;
    m_latestFix = NmeaGgaFix();
    m_manualTarget = GuidedTargetService::Target();
    m_pendingLease = VehicleTargetLease();
    m_session = GuidedTargetService::SessionToken();
    if (!m_closing && m_status) {
        m_status->setText(status.isEmpty() ? tr("Stopped.") : status);
        refreshTargetDescription();
        syncUi();
    }
}

void FollowMeWindow::abortStart(const QString &status)
{
    ++m_operationGeneration;
    if (m_updateTimer) {
        m_updateTimer->stop();
    }
    releaseGpsInput();
    if (m_guidedService && m_session.isValid()) {
        m_guidedService->stop(m_session);
    }
    m_session = GuidedTargetService::SessionToken();
    m_pendingLease = VehicleTargetLease();
    m_starting = false;
    m_stopping = false;
    m_serialSession = false;
    m_hasLatestFix = false;
    if (!m_closing) {
        m_status->setText(status);
        refreshTargetDescription();
        syncUi();
    }
}

void FollowMeWindow::releaseGpsInput()
{
    if (!m_gpsInput) {
        return;
    }
    FollowMeGpsInput *input = m_gpsInput.data();
    m_gpsInput = nullptr;
    disconnect(input, nullptr, this, nullptr);
    try {
        input->close();
    } catch (...) {
    }
    input->deleteLater();
}

void FollowMeWindow::shutdown()
{
    if (m_closing) {
        return;
    }
    m_closing = true;
    ++m_operationGeneration;
    if (m_updateTimer) {
        m_updateTimer->stop();
    }
    releaseGpsInput();
    if (m_guidedService && m_session.isValid()) {
        m_guidedService->stop(m_session);
    }
    m_session = GuidedTargetService::SessionToken();
    m_pendingLease = VehicleTargetLease();
    m_starting = false;
    m_stopping = false;
}

bool FollowMeWindow::sessionMatches(
    const GuidedTargetService::SessionToken &session) const
{
    return m_session.isValid() && session.isValid()
        && session.owner.data() == this
        && session.generation == m_session.generation
        && sameLease(session.target, m_session.target);
}

bool FollowMeWindow::leaseIsCurrent(
    const VehicleTargetLease &lease) const
{
    return m_targetManager && lease.isValid()
        && m_targetManager->isTargetGenerationSettled()
        && m_targetManager->isCurrentTarget(
            lease.endpoint.linkId, lease.endpoint.systemId,
            lease.endpoint.componentId, lease.generation);
}

double FollowMeWindow::selectedRateHz() const
{
    return m_rate ? m_rate->currentData().toDouble() : 0.0;
}

int FollowMeWindow::selectedBaud() const
{
    return m_baud ? m_baud->currentData().toInt() : 0;
}

QString FollowMeWindow::targetDescription(
    const VehicleTargetLease &target) const
{
    return target.isValid()
        ? tr("vehicle %1:%2 on the selected modem")
              .arg(target.endpoint.systemId)
              .arg(target.endpoint.componentId)
        : tr("no connected vehicle");
}

QString FollowMeWindow::startFailureText(
    GuidedTargetService::RequestResult result) const
{
    if (result == GuidedTargetService::RequestResult::Busy) {
        return tr("Another window owns the shared guided command channel. "
                  "Stop that session before starting Follow Me.");
    }
    return tr("Follow Me could not start: %1")
        .arg(GuidedTargetService::resultDescription(result));
}

bool FollowMeWindow::validCoordinates(double latitude, double longitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0
        && (latitude != 0.0 || longitude != 0.0);
}

QString FollowMeWindow::targetText(
    const GuidedTargetService::Target &target, const NmeaGgaFix *fix)
{
    QString text = QStringLiteral("%1 %2 %3 m relative")
        .arg(target.latitude, 0, 'f', 7)
        .arg(target.longitude, 0, 'f', 7)
        .arg(target.relativeAltitudeM, 0, 'f', 1);
    if (fix) {
        text += QStringLiteral("; sats %1, HDOP %2")
            .arg(fix->satellites)
            .arg(fix->hdop, 0, 'f', 2);
    }
    return text;
}

void FollowMeWindow::closeEvent(QCloseEvent *event)
{
    shutdown();
    QWidget::closeEvent(event);
}
