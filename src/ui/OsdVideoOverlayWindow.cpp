#include "OsdVideoOverlayWindow.h"

#include "flightdata/HudControl.h"
#include "tools/MjpegAviWriter.h"
#include "tools/OsdVideoDecoder.h"

#include <QBuffer>
#include <QCheckBox>
#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
constexpr qint64 kMaximumGapSeconds = 30;

QString absolutePath(const QString &path)
{
    return path.trimmed().isEmpty()
        ? QString() : QFileInfo(path).absoluteFilePath();
}

QLabel *pathLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}
}

QPointer<OsdVideoOverlayWindow> OsdVideoOverlayWindow::s_current;

OsdVideoOverlayWindow::OsdVideoOverlayWindow(QWidget *owner)
    : OsdVideoOverlayWindow(DefaultDependencies(), owner)
{}

OsdVideoOverlayWindow::OsdVideoOverlayWindow(
    Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_dependencies(std::move(dependencies))
    , m_decoder(new OsdVideoDecoder(this))
    , m_hud(new HudControl(this, nullptr, false))
{
    buildUi(owner);
    m_hud->hide();
    // The export renderer must obey the decoded frame size, including small
    // diagnostic videos. HudControl's interactive minimum is irrelevant for
    // an offscreen render target and would otherwise crop frames below
    // 240 x 180.
    m_hud->setMinimumSize(0, 0);
    m_hud->setOverlayEnabled(true);
    m_hud->setShowIcons(true);
    m_hud->setDisplayConnection(true);

    connect(m_decoder, &OsdVideoDecoder::ready,
            this, &OsdVideoOverlayWindow::decoderReady);
    connect(m_decoder, &OsdVideoDecoder::frameAvailable,
            this, &OsdVideoOverlayWindow::consumeNextFrame);
    connect(m_decoder, &OsdVideoDecoder::ended,
            this, &OsdVideoOverlayWindow::decoderEnded,
            Qt::QueuedConnection);
    connect(m_decoder, &OsdVideoDecoder::failed, this,
            [this](OsdVideoDecoder::FailureReason, const QString &message) {
        if (m_busy) {
            finishExport(false, tr("OSD video export failed: %1")
                                    .arg(message));
        }
    });
}

OsdVideoOverlayWindow::~OsdVideoOverlayWindow()
{
    ++m_generation;
    if (m_cancelRequested) {
        m_cancelRequested->store(true);
    }
    if (m_decoder) {
        m_decoder->cancel();
    }
    m_writer.reset();
}

OsdVideoOverlayWindow *OsdVideoOverlayWindow::OpenWindow(QWidget *owner)
{
    if (s_current) {
        s_current->show();
        s_current->raise();
        s_current->activateWindow();
        return s_current;
    }
    s_current = new OsdVideoOverlayWindow(owner);
    s_current->show();
    s_current->raise();
    s_current->activateWindow();
    return s_current;
}

OsdVideoOverlayWindow::Dependencies
OsdVideoOverlayWindow::DefaultDependencies()
{
    Dependencies dependencies;
    dependencies.confirm = [](QWidget *owner, const QString &title,
                              const QString &message,
                              const QString &acceptText) {
        QMessageBox box(QMessageBox::Warning, title, message,
                        QMessageBox::NoButton, owner);
        QPushButton *accept = box.addButton(
            acceptText, QMessageBox::AcceptRole);
        QPushButton *cancel = box.addButton(
            QObject::tr("Cancel"), QMessageBox::RejectRole);
        box.setDefaultButton(cancel);
        box.setEscapeButton(cancel);
        box.exec();
        return box.clickedButton() == accept;
    };
    dependencies.chooseVideo = [](QWidget *owner) {
        return QFileDialog::getOpenFileName(
            owner, QObject::tr("Select video for OSD overlay"), QString(),
            QObject::tr("Video files (*.avi *.mpe *.mpeg *.mpg *.mp4 *.mov *.mkv);;All files (*)"));
    };
    dependencies.chooseTlog = [](QWidget *owner) {
        return QFileDialog::getOpenFileName(
            owner, QObject::tr("Select synchronized telemetry log"),
            QString(), QObject::tr("Telemetry log (*.tlog);;All files (*)"));
    };
    dependencies.chooseOutput = [](QWidget *owner,
                                   const QString &suggested) {
        return QFileDialog::getSaveFileName(
            owner, QObject::tr("Save synchronized OSD video"), suggested,
            QObject::tr("MJPEG AVI (*.avi)"));
    };
    return dependencies;
}

void OsdVideoOverlayWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("OsdVideoOverlayWindow"));
    setWindowTitle(tr("OSD Video — Telemetry Overlay"));
    setAttribute(Qt::WA_DeleteOnClose, true);
    setWindowModality(Qt::NonModal);
    resize(1120, 720);
    setMinimumSize(820, 600);
    setStyleSheet(QStringLiteral(
        "QWidget#OsdVideoOverlayWindow { background: #303233; color: #dddddd; }"
        "QLabel#osdVideoDescription { color: #bbbbbb; }"
        "QLabel#osdVideoPreview { background: black; border: 1px solid #606060; }"
        "QLabel#osdVideoStatus { color: #9cdcfe; }"));
    if (owner) {
        move(owner->frameGeometry().center() - QPoint(width() / 2,
                                                       height() / 2));
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *title = new QLabel(tr("OSD Video — Telemetry Overlay"), this);
    title->setObjectName(QStringLiteral("osdVideoTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto *description = new QLabel(tr(
        "Synchronizes a camera video with a .tlog and renders the Mission "
        "Planner HUD into a new silent MJPEG AVI."), this);
    description->setObjectName(QStringLiteral("osdVideoDescription"));
    description->setWordWrap(true);
    root->addWidget(description);

    auto *paths = new QGridLayout;
    paths->setColumnStretch(1, 1);
    paths->setHorizontalSpacing(8);
    paths->setVerticalSpacing(8);
    m_videoPath = new QLineEdit(this);
    m_videoPath->setObjectName(QStringLiteral("osdVideoSourcePath"));
    m_videoPath->setReadOnly(true);
    m_videoPath->setPlaceholderText(tr("AVI, MPEG, MP4, MOV or MKV"));
    m_videoBrowse = new QPushButton(tr("Browse…"), this);
    m_videoBrowse->setObjectName(QStringLiteral("osdVideoBrowseSource"));
    paths->addWidget(pathLabel(tr("Source video"), this), 0, 0);
    paths->addWidget(m_videoPath, 0, 1);
    paths->addWidget(m_videoBrowse, 0, 2);

    m_tlogPath = new QLineEdit(this);
    m_tlogPath->setObjectName(QStringLiteral("osdVideoTlogPath"));
    m_tlogPath->setReadOnly(true);
    m_tlogPath->setPlaceholderText(tr("Matching .tlog"));
    m_tlogBrowse = new QPushButton(tr("Browse…"), this);
    m_tlogBrowse->setObjectName(QStringLiteral("osdVideoBrowseTlog"));
    paths->addWidget(pathLabel(tr("Telemetry log"), this), 1, 0);
    paths->addWidget(m_tlogPath, 1, 1);
    paths->addWidget(m_tlogBrowse, 1, 2);
    root->addLayout(paths);

    auto *options = new QHBoxLayout;
    options->setSpacing(10);
    options->addWidget(new QLabel(tr("Time offset (seconds)"), this));
    m_offset = new QSpinBox(this);
    m_offset->setObjectName(QStringLiteral("osdVideoTimeOffset"));
    m_offset->setRange(OsdVideoOverlayCore::MinimumOffsetSeconds,
                       OsdVideoOverlayCore::MaximumOffsetSeconds);
    options->addWidget(m_offset);
    m_fullResolution = new QCheckBox(tr("Source resolution"), this);
    m_fullResolution->setObjectName(
        QStringLiteral("osdVideoFullResolution"));
    options->addWidget(m_fullResolution);
    auto *offsetHelp = new QLabel(tr(
        "Positive offset selects later telemetry; default output is at most "
        "960 px wide."), this);
    offsetHelp->setWordWrap(true);
    options->addWidget(offsetHelp, 1);
    m_frameCount = new QLabel(tr("Frames: 0"), this);
    m_frameCount->setObjectName(QStringLiteral("osdVideoFrameCount"));
    options->addWidget(m_frameCount);
    root->addLayout(options);

    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("osdVideoPreview"));
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumHeight(260);
    m_preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    root->addWidget(m_preview, 1);

    auto *output = new QGridLayout;
    output->setColumnStretch(1, 1);
    output->addWidget(pathLabel(tr("Output"), this), 0, 0);
    m_outputPath = new QLineEdit(this);
    m_outputPath->setObjectName(QStringLiteral("osdVideoOutputPath"));
    m_outputPath->setReadOnly(true);
    m_outputPath->setPlaceholderText(tr("Unused .avi path"));
    output->addWidget(m_outputPath, 0, 1);
    m_outputBrowse = new QPushButton(tr("Choose…"), this);
    m_outputBrowse->setObjectName(QStringLiteral("osdVideoBrowseOutput"));
    output->addWidget(m_outputBrowse, 0, 2);
    root->addLayout(output);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("osdVideoProgress"));
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    root->addWidget(m_progress);

    auto *bottom = new QHBoxLayout;
    m_status = new QLabel(tr(
        "Choose a source video and its synchronized .tlog. The output is a "
        "silent MJPEG AVI."), this);
    m_status->setObjectName(QStringLiteral("osdVideoStatus"));
    m_status->setWordWrap(true);
    bottom->addWidget(m_status, 1);
    m_start = new QPushButton(tr("Start"), this);
    m_start->setObjectName(QStringLiteral("osdVideoStart"));
    m_start->setMinimumWidth(90);
    bottom->addWidget(m_start);
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("osdVideoCancel"));
    m_cancel->setMinimumWidth(90);
    m_cancel->setEnabled(false);
    bottom->addWidget(m_cancel);
    m_close = new QPushButton(tr("Close"), this);
    m_close->setObjectName(QStringLiteral("osdVideoClose"));
    m_close->setMinimumWidth(90);
    bottom->addWidget(m_close);
    root->addLayout(bottom);

    connect(m_videoBrowse, &QPushButton::clicked,
            this, &OsdVideoOverlayWindow::browseVideo);
    connect(m_tlogBrowse, &QPushButton::clicked,
            this, &OsdVideoOverlayWindow::browseTlog);
    connect(m_outputBrowse, &QPushButton::clicked,
            this, &OsdVideoOverlayWindow::browseOutput);
    connect(m_start, &QPushButton::clicked,
            this, &OsdVideoOverlayWindow::startExport);
    connect(m_cancel, &QPushButton::clicked,
            this, &OsdVideoOverlayWindow::cancelExport);
    connect(m_close, &QPushButton::clicked, this, &QWidget::close);
}

int OsdVideoOverlayWindow::writtenFrames() const
{
    return m_writer ? m_writer->frameCount() : 0;
}

QString OsdVideoOverlayWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

void OsdVideoOverlayWindow::setVideoPath(const QString &path)
{
    const QString resolved = absolutePath(path);
    m_videoPath->setText(resolved);
    if (!resolved.isEmpty()) {
        m_outputPath->setText(OsdVideoOverlayCore::DefaultOutputPath(resolved));
    }
}

void OsdVideoOverlayWindow::setTlogPath(const QString &path)
{
    m_tlogPath->setText(absolutePath(path));
}

void OsdVideoOverlayWindow::setOutputPath(const QString &path)
{
    m_outputPath->setText(absolutePath(path));
}

void OsdVideoOverlayWindow::browseVideo()
{
    if (m_busy || !m_dependencies.chooseVideo) {
        return;
    }
    const QString selected = m_dependencies.chooseVideo(this);
    if (!selected.isEmpty()) {
        setVideoPath(selected);
        m_status->setText(tr(
            "Select the matching telemetry log and adjust the time offset "
            "if needed."));
    }
}

void OsdVideoOverlayWindow::browseTlog()
{
    if (m_busy || !m_dependencies.chooseTlog) {
        return;
    }
    const QString selected = m_dependencies.chooseTlog(this);
    if (!selected.isEmpty()) {
        setTlogPath(selected);
        m_status->setText(tr(
            "Ready after reviewing the output name and synchronization "
            "offset."));
    }
}

void OsdVideoOverlayWindow::browseOutput()
{
    if (m_busy || !m_dependencies.chooseOutput) {
        return;
    }
    QString suggested = m_outputPath->text();
    if (suggested.isEmpty() && !m_videoPath->text().isEmpty()) {
        suggested = OsdVideoOverlayCore::DefaultOutputPath(
            m_videoPath->text());
    }
    const QString selected = m_dependencies.chooseOutput(this, suggested);
    if (!selected.isEmpty()) {
        setOutputPath(selected);
    }
}

OsdVideoExportOptions OsdVideoOverlayWindow::currentOptions() const
{
    OsdVideoExportOptions options;
    options.videoPath = m_videoPath->text();
    options.tlogPath = m_tlogPath->text();
    options.outputPath = m_outputPath->text();
    options.timeOffsetSeconds = m_offset->value();
    options.fullResolution = m_fullResolution->isChecked();
    return options;
}

void OsdVideoOverlayWindow::startExport()
{
    if (m_busy) {
        return;
    }
    if (m_outputPath->text().isEmpty() && !m_videoPath->text().isEmpty()) {
        m_outputPath->setText(OsdVideoOverlayCore::DefaultOutputPath(
            m_videoPath->text()));
    }
    const OsdVideoExportOptions options = currentOptions();
    const QString validation = OsdVideoOverlayCore::Validate(options);
    if (!validation.isEmpty()) {
        m_status->setText(tr("Cannot start OSD video: %1").arg(validation));
        return;
    }
    const QString warning = tr(
        "The output contains every visible source-video frame and HUD "
        "value. The result is a silent MJPEG AVI; audio is not copied.\n\n"
        "Output:\n%1").arg(QFileInfo(options.outputPath).absoluteFilePath());
    if (!m_dependencies.confirm
        || !m_dependencies.confirm(this, tr("Create synchronized OSD video?"),
                                   warning, tr("CREATE OSD VIDEO"))) {
        m_status->setText(tr(
            "OSD video export cancelled before any output was created."));
        return;
    }

    ++m_generation;
    m_cancelRequested = std::make_shared<std::atomic_bool>(false);
    m_timeline = OsdVideoTelemetryTimeline();
    m_timelineReady = false;
    m_writer.reset();
    m_lastJpeg.clear();
    m_lastPreview = QImage();
    m_sourceSize = QSize();
    m_outputSize = QSize();
    m_progress->setValue(0);
    m_frameCount->setText(tr("Frames: 0"));
    m_status->setText(tr("Reading telemetry log…"));
    setBusy(true);
    emit exportStarted();
    startTimelineLoad(options);
}

void OsdVideoOverlayWindow::startTimelineLoad(
    const OsdVideoExportOptions &options)
{
    const quint64 generation = m_generation;
    const auto cancel = m_cancelRequested;
    QPointer<OsdVideoOverlayWindow> guard(this);
    auto *thread = QThread::create([guard, generation, cancel,
                                    path = options.tlogPath]() mutable {
        QFile file(path);
        OsdVideoTelemetryTimeline::LoadStatus loadStatus =
            OsdVideoTelemetryTimeline::LoadStatus::Error;
        QString error;
        OsdVideoTelemetryTimeline timeline;
        if (!file.open(QIODevice::ReadOnly)) {
            error = QObject::tr("The telemetry log cannot be opened: %1")
                        .arg(file.errorString());
        } else {
            qint64 lastPublished = -1;
            timeline = OsdVideoTelemetryTimeline::Load(
                &file, &loadStatus, &error,
                [cancel]() { return cancel && cancel->load(); },
                [guard, generation, &lastPublished](qint64 done,
                                                     qint64 total) {
                    if (!guard || total <= 0
                        || (lastPublished >= 0
                            && done - lastPublished < qMax<qint64>(
                                512 * 1024, total / 200))) {
                        return;
                    }
                    lastPublished = done;
                    const int value = static_cast<int>(
                        qBound<qint64>(0, done * 200 / total, 200));
                    QMetaObject::invokeMethod(
                        guard, [guard, generation, value]() {
                            if (guard && guard->m_busy
                                && guard->m_generation == generation) {
                                guard->m_progress->setValue(value);
                            }
                        }, Qt::QueuedConnection);
                });
        }
        if (guard) {
            QMetaObject::invokeMethod(
                guard,
                [guard, generation, timeline = std::move(timeline),
                 loadStatus, error]() mutable {
                    if (guard) {
                        guard->timelineLoaded(
                            generation, std::move(timeline), loadStatus,
                            error);
                    }
                }, Qt::QueuedConnection);
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void OsdVideoOverlayWindow::timelineLoaded(
    quint64 generation, OsdVideoTelemetryTimeline timeline,
    OsdVideoTelemetryTimeline::LoadStatus loadStatus,
    const QString &errorMessage)
{
    if (!m_busy || generation != m_generation) {
        return;
    }
    if (loadStatus == OsdVideoTelemetryTimeline::LoadStatus::Cancelled
        || (m_cancelRequested && m_cancelRequested->load())) {
        finishExport(false, tr(
            "OSD video export cancelled. No output frames were written."));
        return;
    }
    if (loadStatus != OsdVideoTelemetryTimeline::LoadStatus::Ok
        || timeline.isEmpty()) {
        finishExport(false, tr("Cannot read telemetry log: %1")
                                .arg(errorMessage.isEmpty()
                                     ? tr("no readable MAVLink state")
                                     : errorMessage));
        return;
    }
    m_timeline = std::move(timeline);
    m_timelineReady = true;
    m_progress->setValue(200);
    m_status->setText(tr("Opening source video…"));
    if (!m_decoder->start(currentOptions().videoPath)) {
        finishExport(false, tr("OSD video export failed: %1")
                                .arg(tr("Qt Multimedia rejected the source video.")));
    }
}

void OsdVideoOverlayWindow::decoderReady(int framesPerSecond,
                                         qint64 durationMs)
{
    if (!m_busy || !m_timelineReady) {
        return;
    }
    m_framesPerSecond = OsdVideoOverlayCore::ClampFramesPerSecond(
        framesPerSecond);
    m_durationMs = qMax<qint64>(0, durationMs);
    m_status->setText(tr("Rendering synchronized HUD overlay…"));
}

bool OsdVideoOverlayWindow::prepareWriter(const QSize &sourceSize)
{
    if (!sourceSize.isValid()
        || sourceSize.width() > 8192 || sourceSize.height() > 8192) {
        m_status->setText(tr("The decoded video dimensions are invalid."));
        return false;
    }
    if (m_writer) {
        return sourceSize == m_sourceSize;
    }
    const OsdVideoExportOptions options = currentOptions();
    m_sourceSize = sourceSize;
    m_outputSize = OsdVideoOverlayCore::OutputSize(
        sourceSize.width(), sourceSize.height(), options.fullResolution,
        options.previewWidth);
    if (!m_outputSize.isValid()) {
        m_status->setText(tr("The output video dimensions are invalid."));
        return false;
    }
    m_writer.reset(new MjpegAviWriter(
        options.outputPath, m_outputSize.width(), m_outputSize.height(),
        m_framesPerSecond));
    if (!m_writer->isOpen()) {
        m_status->setText(tr("The output AVI cannot be created: %1")
                              .arg(m_writer->errorString()));
        m_writer.reset();
        return false;
    }
    m_hud->resize(m_outputSize);
    return true;
}

void OsdVideoOverlayWindow::consumeNextFrame()
{
    if (!m_busy || !m_timelineReady || !m_decoder->hasPendingFrame()) {
        return;
    }
    const OsdVideoDecoder::Frame frame = m_decoder->takeNextFrame();
    if (!frame.isValid() || !prepareWriter(frame.image.size())) {
        finishExport(false, tr("OSD video export failed: %1")
                                .arg(m_status->text()));
        return;
    }
    qint64 positionUsec = frame.startTimeUsec;
    if (positionUsec < 0) {
        positionUsec = 0;
    }
    if (m_durationMs > 0) {
        const qint64 durationUsec = m_durationMs
                > std::numeric_limits<qint64>::max() / 1000
            ? std::numeric_limits<qint64>::max() : m_durationMs * 1000;
        positionUsec = qMin(positionUsec, durationUsec);
    }
    const qint64 targetFrame = OsdVideoOverlayCore::FrameIndexForTimestamp(
        positionUsec, m_framesPerSecond);
    if (!appendRepeatedFrames(targetFrame)) {
        finishExport(false, tr("OSD video export failed: %1")
                                .arg(m_status->text()));
        return;
    }
    if (m_writer->frameCount() > targetFrame) {
        return;
    }

    const OsdVideoTelemetrySample sample = m_timeline.sampleAt(
        positionUsec,
        static_cast<qint64>(m_offset->value()) * 1000000);
    QImage preview;
    const QByteArray jpeg = renderFrame(frame.image, sample, &preview);
    if (jpeg.isEmpty() || !m_writer->writeJpeg(jpeg)) {
        m_status->setText(m_writer->errorString().isEmpty()
            ? tr("The HUD frame could not be encoded as JPEG.")
            : m_writer->errorString());
        finishExport(false, tr("OSD video export failed: %1")
                                .arg(m_status->text()));
        return;
    }
    m_lastJpeg = jpeg;
    m_lastPreview = preview;
    updatePreview();
    if (m_writer->frameCount() % m_framesPerSecond == 0
        && !m_writer->checkpoint()) {
        m_status->setText(m_writer->errorString());
        finishExport(false, tr("OSD video export failed: %1")
                                .arg(m_status->text()));
        return;
    }
    m_frameCount->setText(tr("Frames: %1")
                              .arg(m_writer->frameCount()));
    const int progress = m_durationMs > 0
        ? static_cast<int>(qBound<qint64>(
              200, 200 + (positionUsec / 1000) * 800 / m_durationMs, 1000))
        : 200;
    m_progress->setValue(progress);
}

QByteArray OsdVideoOverlayWindow::renderFrame(
    const QImage &source, const OsdVideoTelemetrySample &sample,
    QImage *preview)
{
    applyTelemetry(sample);
    m_hud->setVideoBackground(source);
    m_hud->snapToValues();
    QImage rendered(m_outputSize, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::black);
    QPainter painter(&rendered);
    m_hud->render(&painter);
    painter.end();
    if (preview) {
        *preview = rendered;
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    if (!buffer.open(QIODevice::WriteOnly)
        || !rendered.save(&buffer, "JPG", currentOptions().jpegQuality)) {
        return QByteArray();
    }
    return jpeg;
}

void OsdVideoOverlayWindow::applyTelemetry(
    const OsdVideoTelemetrySample &sample)
{
    m_hud->setRoll(sample.roll);
    m_hud->setPitch(sample.pitch);
    m_hud->setYaw(sample.yaw);
    m_hud->setAlt(sample.altitude);
    m_hud->setAirSpeed(sample.airSpeed);
    m_hud->setGroundSpeed(sample.groundSpeed);
    m_hud->setVerticalSpeed(sample.verticalSpeed);
    m_hud->setSatCount(sample.satelliteCount);
    m_hud->setArmed(sample.armed);
    m_hud->setPrearmOk(sample.prearmOk);
    m_hud->setGpsFixType(sample.gpsFixType);
    m_hud->setMode(sample.mode);
    m_hud->setBatteryVoltage(sample.batteryVoltage);
    m_hud->setBatteryRemaining(sample.batteryRemaining);
    m_hud->setCurrentAmps(sample.currentAmps);
    m_hud->setNavBearing(sample.navBearing);
    m_hud->setTargetAlt(sample.targetAltitude);
    m_hud->setTargetSpeed(sample.targetSpeed);
    m_hud->setWindDir(sample.windDirection);
    m_hud->setWindVel(sample.windSpeed);
    m_hud->setAoa(sample.aoa);
    m_hud->setSsa(sample.ssa);
    m_hud->setXTrackError(sample.xtrackError);
    m_hud->setTurnRate(sample.turnRate);
    m_hud->setBatteryVoltage2(sample.batteryVoltage2);
    m_hud->setBatteryRemaining2(sample.batteryRemaining2);
    m_hud->setCurrentAmps2(sample.currentAmps2);
    m_hud->setThrottlePercent(sample.throttlePercent);
    m_hud->setFailsafe(sample.failsafe);
    m_hud->setSafetyActive(sample.safetyActive);
    m_hud->setLinkQuality(sample.linkQuality);
    m_hud->setWpDist(sample.waypointDistance);
    m_hud->setWpNo(sample.waypointNumber);
}

bool OsdVideoOverlayWindow::appendRepeatedFrames(qint64 targetFrameCount)
{
    if (!m_writer || m_lastJpeg.isEmpty()) {
        return true;
    }
    targetFrameCount = qMax<qint64>(0, targetFrameCount);
    const qint64 missing = targetFrameCount - m_writer->frameCount();
    if (missing > static_cast<qint64>(m_framesPerSecond)
                      * kMaximumGapSeconds) {
        m_status->setText(tr(
            "The decoder reported a gap longer than the bounded 30-second "
            "fill window."));
        return false;
    }
    while (m_writer->frameCount() < targetFrameCount) {
        if ((m_cancelRequested && m_cancelRequested->load())
            || !m_writer->writeJpeg(m_lastJpeg)) {
            if (!m_writer->errorString().isEmpty()) {
                m_status->setText(m_writer->errorString());
            }
            return false;
        }
    }
    return true;
}

void OsdVideoOverlayWindow::decoderEnded()
{
    if (!m_busy || m_decoder->state() != OsdVideoDecoder::State::Ended) {
        return;
    }
    // Some backends publish EndOfMedia immediately after presenting the last
    // frame. Drain it before finalizing even if a queued frame notification is
    // still pending.
    if (m_decoder->hasPendingFrame()) {
        consumeNextFrame();
        if (!m_busy) {
            return;
        }
    }
    if (!m_writer || m_writer->frameCount() == 0) {
        finishExport(false, tr(
            "The video ended without producing a decodable frame."));
        return;
    }
    const qint64 expected = qMax<qint64>(
        1, (m_durationMs * m_framesPerSecond + 500) / 1000);
    if (!appendRepeatedFrames(expected)) {
        finishExport(false, tr("OSD video export failed: %1")
                                .arg(m_status->text()));
        return;
    }
    const int frames = m_writer->frameCount();
    const QSize size = m_outputSize;
    const QString output = currentOptions().outputPath;
    if (!m_writer->finalize()) {
        finishExport(false, tr("OSD video export failed: %1")
                                .arg(m_writer->errorString()));
        return;
    }
    m_writer.reset();
    finishExport(true,
        tr("Saved %1 frames at %2×%3, %4 fps:\n%5")
            .arg(frames).arg(size.width()).arg(size.height())
            .arg(m_framesPerSecond).arg(output));
}

void OsdVideoOverlayWindow::cancelExport()
{
    if (!m_busy) {
        return;
    }
    if (m_cancelRequested) {
        m_cancelRequested->store(true);
    }
    QString message = tr("OSD video export cancelled.");
    if (m_writer && m_writer->frameCount() > 0) {
        const QString path = m_writer->path();
        if (m_writer->finalize()) {
            message += tr(" A playable partial AVI was retained at:\n%1")
                           .arg(path);
        } else {
            message += tr(" The partial AVI could not be finalized: %1")
                           .arg(m_writer->errorString());
        }
    } else {
        message += tr(" No output frames were written.");
    }
    m_writer.reset();
    finishExport(false, message);
}

void OsdVideoOverlayWindow::finishExport(bool success,
                                         const QString &message)
{
    if (!m_busy) {
        return;
    }
    setBusy(false);
    if (m_decoder) {
        m_decoder->cancel();
    }
    if (m_writer) {
        m_writer->finalize();
        m_writer.reset();
    }
    if (m_cancelRequested) {
        m_cancelRequested->store(true);
    }
    m_timeline = OsdVideoTelemetryTimeline();
    m_timelineReady = false;
    m_status->setText(message);
    if (success) {
        m_progress->setValue(1000);
    }
    emit exportFinished(success, message);
    if (m_closeAfterFinish) {
        m_closeAfterFinish = false;
        QTimer::singleShot(0, this, &QWidget::close);
    }
}

void OsdVideoOverlayWindow::setBusy(bool busy)
{
    m_busy = busy;
    m_videoBrowse->setEnabled(!busy);
    m_tlogBrowse->setEnabled(!busy);
    m_outputBrowse->setEnabled(!busy);
    m_offset->setEnabled(!busy);
    m_fullResolution->setEnabled(!busy);
    m_start->setEnabled(!busy);
    m_cancel->setEnabled(busy);
}

void OsdVideoOverlayWindow::updatePreview()
{
    if (m_lastPreview.isNull() || m_preview->size().isEmpty()) {
        m_preview->clear();
        return;
    }
    m_preview->setPixmap(QPixmap::fromImage(m_lastPreview).scaled(
        m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void OsdVideoOverlayWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updatePreview();
}

void OsdVideoOverlayWindow::closeEvent(QCloseEvent *event)
{
    if (m_busy) {
        if (!m_dependencies.confirm
            || !m_dependencies.confirm(
                this, tr("Cancel OSD video export?"),
                tr("Video rendering is still in progress. Cancel it, "
                   "finalize the playable partial AVI, and close?"),
                tr("CANCEL AND CLOSE"))) {
            event->ignore();
            return;
        }
        event->ignore();
        m_closeAfterFinish = true;
        cancelExport();
        return;
    }
    if (s_current == this) {
        s_current = nullptr;
    }
    QWidget::closeEvent(event);
}
