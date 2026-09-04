#include "ExternalGuidedWindow.h"

#include "comm/VehicleTargetManager.h"

#include <QCloseEvent>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <exception>
#include <memory>
#include <utility>

namespace
{
constexpr unsigned long kWorkerShutdownWaitMs = 250;

bool sameLease(const VehicleTargetLease &left,
               const VehicleTargetLease &right)
{
    return left.isValid() && right.isValid()
        && left.generation == right.generation
        && left.endpoint == right.endpoint;
}
}

ExternalGuidedWindow::ExternalGuidedWindow(
    Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_targetManager(dependencies.targetManager)
    , m_guidedService(dependencies.guidedService)
    , m_readFile(std::move(dependencies.readFile))
    , m_chooseFile(std::move(dependencies.chooseFile))
    , m_confirmStart(std::move(dependencies.confirmStart))
    , m_updateIntervalMs(qMax(1, dependencies.updateIntervalMs))
{
    if (!m_readFile) {
        m_readFile = [](const QString &path) {
            return ExternalGuidedFile::read(path);
        };
    }
    // An omitted confirmation dependency must fail closed. Production supplies
    // the explicit default-Cancel QMessageBox in the integration unit.
    if (!m_confirmStart) {
        m_confirmStart = [](QWidget *, const QString &, const QString &) {
            return false;
        };
    }

    m_updateTimer = new QTimer(this);
    m_updateTimer->setSingleShot(true);
    m_updateTimer->setInterval(m_updateIntervalMs);
    buildUi(owner);
    connectDependencies();
    refreshTargetDescription();
    syncUi();
}

ExternalGuidedWindow::~ExternalGuidedWindow()
{
    shutdown();
}

QString ExternalGuidedWindow::filePath() const
{
    return m_filePath ? m_filePath->text() : QString();
}

QString ExternalGuidedWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

QString ExternalGuidedWindow::lastAcceptedText() const
{
    return m_lastAccepted ? m_lastAccepted->text() : QString();
}

void ExternalGuidedWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("ExternalGuidedWindow"));
    setWindowTitle(tr("External Guided"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(WindowWidth, WindowHeight);
    setMinimumSize(MinimumWindowWidth, MinimumWindowHeight);
    setStyleSheet(QStringLiteral(
        "QWidget#ExternalGuidedWindow { background: #434445; color: #dddddd; }"
        "QLabel#externalGuidedDescription, QLabel#externalGuidedExample, "
        "QLabel#externalGuidedLastAcceptedCaption { color: #aaaaaa; }"
        "QLabel#externalGuidedTargetDescription, "
        "QLabel#externalGuidedStatus { color: #34d399; }"
        "QFrame#externalGuidedStatusPanel { background: #303132; "
        "border: 1px solid #555657; border-radius: 4px; }"));
    if (owner) {
        move(owner->frameGeometry().center()
             - QPoint(WindowWidth / 2, WindowHeight / 2));
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto *header = new QLabel(tr("External Guided"), this);
    header->setObjectName(QStringLiteral("externalGuidedHeader"));
    QFont headerFont = header->font();
    headerFont.setPointSize(16);
    header->setFont(headerFont);
    root->addWidget(header);

    auto *description = new QLabel(tr(
        "Native port of Mission Planner's ExtGuided plugin. It rereads a "
        "latitude,longitude,relative-altitude-m file once per second and "
        "sends the target only to the exact selected modem, system and "
        "component. A target switch or disconnect stops the session."), this);
    description->setObjectName(QStringLiteral("externalGuidedDescription"));
    description->setWordWrap(true);
    description->setTextFormat(Qt::PlainText);
    root->addWidget(description);

    m_target = new QLabel(this);
    m_target->setObjectName(
        QStringLiteral("externalGuidedTargetDescription"));
    m_target->setWordWrap(true);
    m_target->setTextFormat(Qt::PlainText);
    root->addWidget(m_target);

    auto *fileRow = new QGridLayout;
    fileRow->setHorizontalSpacing(8);
    auto *fileLabel = new QLabel(tr("Target file"), this);
    fileLabel->setObjectName(QStringLiteral("externalGuidedFileLabel"));
    fileRow->addWidget(fileLabel, 0, 0);
    m_filePath = new QLineEdit(this);
    m_filePath->setObjectName(QStringLiteral("externalGuidedFilePath"));
    m_filePath->setPlaceholderText(
        QStringLiteral("/path/to/guided-target.txt"));
    m_filePath->setMaxLength(MaximumPathCharacters);
    fileRow->addWidget(m_filePath, 0, 1);
    m_browse = new QPushButton(tr("Browse…"), this);
    m_browse->setObjectName(QStringLiteral("externalGuidedBrowse"));
    fileRow->addWidget(m_browse, 0, 2);
    fileRow->setColumnStretch(1, 1);
    root->addLayout(fileRow);

    auto *example = new QLabel(
        tr("Example: 34.1234567,33.1234567,50"), this);
    example->setObjectName(QStringLiteral("externalGuidedExample"));
    example->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    example->setTextFormat(Qt::PlainText);
    root->addWidget(example);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    m_toggle = new QPushButton(tr("Start"), this);
    m_toggle->setObjectName(QStringLiteral("ToggleExternalGuidedButton"));
    m_toggle->setMinimumWidth(90);
    buttons->addWidget(m_toggle);
    buttons->addStretch(1);
    root->addLayout(buttons);

    auto *statusPanel = new QFrame(this);
    statusPanel->setObjectName(QStringLiteral("externalGuidedStatusPanel"));
    statusPanel->setFrameShape(QFrame::StyledPanel);
    auto *statusLayout = new QVBoxLayout(statusPanel);
    statusLayout->setContentsMargins(10, 10, 10, 10);
    statusLayout->setSpacing(4);
    m_status = new QLabel(tr("Stopped."), statusPanel);
    m_status->setObjectName(QStringLiteral("externalGuidedStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    statusLayout->addWidget(m_status);
    auto *lastCaption = new QLabel(tr("Last accepted target:"), statusPanel);
    lastCaption->setObjectName(
        QStringLiteral("externalGuidedLastAcceptedCaption"));
    statusLayout->addWidget(lastCaption);
    m_lastAccepted = new QLabel(tr("No file target read."), statusPanel);
    m_lastAccepted->setObjectName(
        QStringLiteral("externalGuidedLocationLabel"));
    m_lastAccepted->setWordWrap(true);
    m_lastAccepted->setTextFormat(Qt::PlainText);
    m_lastAccepted->setFont(
        QFontDatabase::systemFont(QFontDatabase::FixedFont));
    statusLayout->addWidget(m_lastAccepted);
    root->addWidget(statusPanel, 1);

    connect(m_browse, &QPushButton::clicked, this, [this]() {
        if (!m_chooseFile) {
            m_status->setText(
                tr("No application window is available for selecting a target file."));
            return;
        }
        const QString selected = m_chooseFile(this);
        if (!selected.trimmed().isEmpty()) {
            m_filePath->setText(selected);
            m_status->setText(tr(
                "Target file selected. Press Start after verifying the active vehicle."));
        }
    });
    connect(m_toggle, &QPushButton::clicked,
            this, &ExternalGuidedWindow::toggleSession);
    connect(m_updateTimer, &QTimer::timeout, this, [this]() {
        if (isRunning() && !m_stopping && !m_readThread) {
            m_status->setText(tr("Reading the External Guided target file…"));
            beginRead(ReadPurpose::PeriodicUpdate, m_activePath);
        }
    });
}

void ExternalGuidedWindow::connectDependencies()
{
    if (m_targetManager) {
        connect(m_targetManager, &VehicleTargetManager::currentTargetChanged,
                this, [this]() {
            if (m_starting && m_pendingLease.isValid()) {
                const VehicleTargetLease current =
                    m_targetManager->acquireTarget();
                if (!sameLease(current, m_pendingLease)) {
                    abortStart(tr(
                        "The active modem or vehicle changed or disconnected. "
                        "External Guided was not started; verify the selected target."));
                    return;
                }
            }
            if (!isRunning()) {
                refreshTargetDescription();
            }
        });
        connect(m_targetManager, &QObject::destroyed, this, [this]() {
            if (m_starting) {
                abortStart(tr("The vehicle target service is unavailable."));
            } else if (isRunning()) {
                finishLocalSession(
                    tr("External Guided stopped: the vehicle target service is unavailable."));
            } else {
                refreshTargetDescription();
                syncUi();
            }
        });
    }
    if (!m_guidedService) {
        return;
    }

    // Queue callbacks even when a synthetic test transport acknowledges during
    // start(). This lets start() publish the SessionToken before it is consumed.
    connect(m_guidedService, &GuidedTargetService::targetAccepted,
            this,
            [this](GuidedTargetService::SessionToken session,
                   GuidedTargetService::Target target) {
        if (!sessionMatches(session) || m_closing || m_stopping) {
            return;
        }
        m_lastAccepted->setText(waypointText(target));
        m_status->setText(
            tr("External Guided target accepted by %1.")
                .arg(targetDescription(session.target)));
        scheduleNextRead();
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
            description.isEmpty()
                ? tr("External Guided stopped.") : description);
    }, Qt::QueuedConnection);
    connect(m_guidedService, &GuidedTargetService::statusChanged,
            this, [this](const QString &status) {
        if (!m_guidedService || m_closing || status.isEmpty()) {
            return;
        }
        const GuidedTargetService::SessionToken active =
            m_guidedService->activeSession();
        if (sessionMatches(active)) {
            m_status->setText(status);
        }
    }, Qt::QueuedConnection);
    connect(m_guidedService, &QObject::destroyed, this, [this]() {
        if (m_starting) {
            abortStart(tr("The guided target service is unavailable."));
        } else if (isRunning()) {
            finishLocalSession(
                tr("External Guided stopped: the guided target service is unavailable."));
        }
        syncUi();
    });
}

void ExternalGuidedWindow::syncUi()
{
    const bool running = isRunning();
    const bool editable = !m_starting && !running && !m_stopping;
    m_filePath->setEnabled(editable);
    m_browse->setEnabled(editable);
    m_toggle->setText(running ? tr("Stop") : tr("Start"));
    m_toggle->setEnabled(!m_starting && !m_stopping
                         && m_targetManager && m_guidedService);
}

void ExternalGuidedWindow::refreshTargetDescription()
{
    const VehicleTargetLease target = m_targetManager
        ? m_targetManager->acquireTarget() : VehicleTargetLease();
    m_target->setText(
        target.isValid()
            ? tr("Ready for %1.").arg(targetDescription(target))
            : tr("No connected vehicle selected."));
}

void ExternalGuidedWindow::toggleSession()
{
    if (isRunning()) {
        requestStop(tr("Stopped."));
    } else if (!m_starting && !m_stopping) {
        beginStart();
    }
}

void ExternalGuidedWindow::beginStart()
{
    if (!m_targetManager || !m_guidedService) {
        m_status->setText(tr("External Guided services are unavailable."));
        return;
    }
    const QString path = m_filePath->text().trimmed();
    if (path.isEmpty()) {
        m_status->setText(
            tr("Select an existing External Guided target file first."));
        return;
    }

    ++m_operationGeneration;
    m_starting = true;
    m_pendingLease = VehicleTargetLease();
    m_activePath.clear();
    m_status->setText(tr("Validating the External Guided target file…"));
    syncUi();
    beginRead(ReadPurpose::InitialValidation, path);
}

void ExternalGuidedWindow::beginRead(ReadPurpose purpose,
                                     const QString &path)
{
    if (m_closing || m_readThread || !m_readFile) {
        return;
    }
    const quint64 generation = m_operationGeneration;
    const FileReader reader = m_readFile;
    const auto result = std::make_shared<ExternalGuidedFileResult>();
    QThread *thread = QThread::create([reader, path, result]() {
        try {
            *result = reader(path);
        } catch (const std::exception &exception) {
            result->errorCode = ExternalGuidedFileError::ReadFailed;
            result->error = QString::fromUtf8(exception.what());
        } catch (...) {
            result->errorCode = ExternalGuidedFileError::ReadFailed;
            result->error = QStringLiteral(
                "unknown target-file reader failure.");
        }
    });
    m_readThread = thread;
    connect(thread, &QThread::finished, this,
            [this, thread, purpose, result, generation]() {
        if (m_readThread == thread) {
            m_readThread = nullptr;
        }
        finishRead(purpose, *result, generation);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void ExternalGuidedWindow::finishRead(
    ReadPurpose purpose, const ExternalGuidedFileResult &result,
    quint64 generation)
{
    if (m_closing || generation != m_operationGeneration) {
        return;
    }
    switch (purpose) {
    case ReadPurpose::InitialValidation:
        handleInitialRead(result);
        break;
    case ReadPurpose::PostConfirmation:
        handlePostConfirmationRead(result);
        break;
    case ReadPurpose::PeriodicUpdate:
        handlePeriodicRead(result);
        break;
    }
}

void ExternalGuidedWindow::handleInitialRead(
    const ExternalGuidedFileResult &result)
{
    if (!m_starting) {
        return;
    }
    if (!result.isValid()) {
        abortStart(tr("Target file is invalid: %1").arg(result.error));
        return;
    }

    const VehicleTargetLease target = m_targetManager
        ? m_targetManager->acquireTarget() : VehicleTargetLease();
    if (!target.isValid()) {
        abortStart(
            tr("Connect and select a vehicle before starting External Guided."));
        return;
    }
    m_pendingLease = target;
    m_activePath = result.absolutePath.isEmpty()
        ? ExternalGuidedFile::absolutePath(m_filePath->text())
        : result.absolutePath;
    {
        const QSignalBlocker blocker(m_filePath);
        m_filePath->setText(m_activePath);
    }

    const QString title = tr("Start External Guided");
    const QString warning = tr(
        "External Guided will reread '%1' once per second and repeatedly "
        "command %2 to its latitude,longitude,altitude target. The altitude "
        "is a relative GUIDED altitude in metres. Verify the file writer, "
        "selected modem, flight mode and surrounding airspace before continuing.")
        .arg(m_activePath, targetDescription(target));
    const quint64 confirmationGeneration = m_operationGeneration;
    bool confirmed = false;
    try {
        confirmed = m_confirmStart(this, title, warning);
    } catch (...) {
        confirmed = false;
    }
    if (m_closing || !m_starting
        || confirmationGeneration != m_operationGeneration) {
        return;
    }
    if (!confirmed) {
        abortStart(tr("External Guided start cancelled."));
        return;
    }

    // The file is an external command channel and may change while the modal
    // warning is open. Never send the value that was validated before consent.
    m_status->setText(
        tr("Re-reading the target file after confirmation…"));
    beginRead(ReadPurpose::PostConfirmation, m_activePath);
}

void ExternalGuidedWindow::handlePostConfirmationRead(
    const ExternalGuidedFileResult &result)
{
    if (!m_starting) {
        return;
    }
    if (!result.isValid()) {
        abortStart(tr("Target file cannot be used after confirmation: %1")
                       .arg(result.error));
        return;
    }
    if (!m_guidedService) {
        abortStart(tr("The guided target service is unavailable."));
        return;
    }

    GuidedTargetService::SessionToken session;
    const GuidedTargetService::RequestResult started =
        m_guidedService->start(
            this, m_pendingLease, guidedTarget(result.waypoint), &session);
    const bool accepted =
        (started == GuidedTargetService::RequestResult::Started
         || started == GuidedTargetService::RequestResult::Sent
         || started == GuidedTargetService::RequestResult::Queued)
        && session.isValid();
    if (!accepted) {
        abortStart(startFailureText(started));
        return;
    }

    m_session = session;
    m_starting = false;
    m_stopping = false;
    m_target->setText(
        tr("Bound to %1.").arg(targetDescription(session.target)));
    m_status->setText(
        m_guidedService->statusText().isEmpty()
            ? tr("External Guided target sent; awaiting acknowledgement.")
            : m_guidedService->statusText());
    syncUi();
}

void ExternalGuidedWindow::handlePeriodicRead(
    const ExternalGuidedFileResult &result)
{
    if (!isRunning() || m_stopping) {
        return;
    }
    if (!result.isValid()) {
        m_status->setText(
            tr("Target file is invalid; GUIDED update withheld: %1")
                .arg(result.error));
        scheduleNextRead();
        return;
    }
    if (!m_guidedService) {
        finishLocalSession(
            tr("External Guided stopped: the guided target service is unavailable."));
        return;
    }

    const GuidedTargetService::RequestResult submitted =
        m_guidedService->submit(m_session, guidedTarget(result.waypoint));
    if (submitted == GuidedTargetService::RequestResult::Sent
        || submitted == GuidedTargetService::RequestResult::Queued) {
        m_status->setText(
            tr("External Guided target sent; awaiting acknowledgement."));
        return;
    }

    const QString error = tr("External Guided stopped: %1")
        .arg(GuidedTargetService::resultDescription(submitted));
    requestStop(error);
}

void ExternalGuidedWindow::scheduleNextRead()
{
    if (isRunning() && !m_stopping && !m_closing && !m_readThread) {
        m_updateTimer->start(m_updateIntervalMs);
    }
}

void ExternalGuidedWindow::requestStop(const QString &status)
{
    ++m_operationGeneration;
    m_updateTimer->stop();
    cancelRead();
    m_starting = false;
    m_pendingLease = VehicleTargetLease();

    if (!m_session.isValid() || !m_guidedService) {
        finishLocalSession(status);
        return;
    }

    m_stopping = true;
    const GuidedTargetService::RequestResult stopped =
        m_guidedService->stop(m_session);
    if (stopped == GuidedTargetService::RequestResult::OutcomeUncertain) {
        finishLocalSession(
            m_guidedService->statusText().isEmpty()
                ? startFailureText(stopped) : m_guidedService->statusText());
        return;
    }
    finishLocalSession(status);
}

void ExternalGuidedWindow::finishLocalSession(const QString &status)
{
    ++m_operationGeneration;
    m_updateTimer->stop();
    cancelRead();
    m_starting = false;
    m_stopping = false;
    m_pendingLease = VehicleTargetLease();
    m_session = GuidedTargetService::SessionToken();
    m_activePath.clear();
    if (!m_closing && m_status) {
        m_status->setText(status.isEmpty() ? tr("Stopped.") : status);
        refreshTargetDescription();
        syncUi();
    }
}

void ExternalGuidedWindow::abortStart(const QString &status)
{
    ++m_operationGeneration;
    m_updateTimer->stop();
    cancelRead();
    m_starting = false;
    m_stopping = false;
    m_pendingLease = VehicleTargetLease();
    m_activePath.clear();
    if (!m_closing) {
        m_status->setText(status);
        refreshTargetDescription();
        syncUi();
    }
}

void ExternalGuidedWindow::cancelRead()
{
    QThread *thread = m_readThread.data();
    m_readThread = nullptr;
    if (!thread) {
        return;
    }
    disconnect(thread, nullptr, this, nullptr);
    thread->requestInterruption();
    if (thread->isRunning() && !thread->wait(kWorkerShutdownWaitMs)) {
        // The job owns only a copied reader/path/result. It can safely finish
        // after this window is gone, without a late submit or UI callback.
        return;
    }
    disconnect(thread, &QThread::finished, thread, &QObject::deleteLater);
    delete thread;
}

void ExternalGuidedWindow::shutdown()
{
    if (m_closing) {
        return;
    }
    m_closing = true;
    ++m_operationGeneration;
    if (m_updateTimer) {
        m_updateTimer->stop();
    }
    cancelRead();
    if (m_guidedService && m_session.isValid()) {
        m_guidedService->stop(m_session);
    }
    m_session = GuidedTargetService::SessionToken();
    m_pendingLease = VehicleTargetLease();
    m_starting = false;
    m_stopping = false;
}

bool ExternalGuidedWindow::sessionMatches(
    const GuidedTargetService::SessionToken &session) const
{
    return m_session.isValid() && session.isValid()
        && session.owner.data() == this
        && session.generation == m_session.generation
        && sameLease(session.target, m_session.target);
}

QString ExternalGuidedWindow::targetDescription(
    const VehicleTargetLease &target) const
{
    return target.isValid()
        ? tr("vehicle %1:%2 on the selected modem")
              .arg(target.endpoint.systemId)
              .arg(target.endpoint.componentId)
        : tr("no connected vehicle");
}

QString ExternalGuidedWindow::startFailureText(
    GuidedTargetService::RequestResult result) const
{
    if (result == GuidedTargetService::RequestResult::Busy) {
        return tr("External Guided is already active in another window. "
                  "Stop that session before starting this one.");
    }
    return tr("External Guided could not start: %1")
        .arg(GuidedTargetService::resultDescription(result));
}

GuidedTargetService::Target ExternalGuidedWindow::guidedTarget(
    const ExternalGuidedWaypoint &waypoint)
{
    GuidedTargetService::Target target;
    target.latitude = waypoint.latitude;
    target.longitude = waypoint.longitude;
    target.relativeAltitudeM = waypoint.relativeAltitudeM;
    return target;
}

QString ExternalGuidedWindow::waypointText(
    const GuidedTargetService::Target &target)
{
    QString altitude = QString::number(target.relativeAltitudeM, 'f', 2);
    while (altitude.contains(QLatin1Char('.'))
           && altitude.endsWith(QLatin1Char('0'))) {
        altitude.chop(1);
    }
    if (altitude.endsWith(QLatin1Char('.'))) {
        altitude.chop(1);
    }
    return QStringLiteral("%1, %2, %3 m relative")
        .arg(target.latitude, 0, 'f', 7)
        .arg(target.longitude, 0, 'f', 7)
        .arg(altitude);
}

void ExternalGuidedWindow::closeEvent(QCloseEvent *event)
{
    shutdown();
    QWidget::closeEvent(event);
}
