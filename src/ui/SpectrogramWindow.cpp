#include "SpectrogramWindow.h"

#include <QComboBox>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

#include <exception>
#include <utility>

namespace
{
const char *const kSensors[] = {
    "ACC1", "ACC2", "ACC3", "ACC4", "ACC5",
    "GYR1", "GYR2", "GYR3", "GYR4", "GYR5"
};

constexpr unsigned long kWorkerShutdownWaitMs = 250;

QFrame *makePlotPanel(const QString &axis, const QString &imageObjectName,
                      QLabel **imageOut, QWidget *parent)
{
    auto *panel = new QFrame(parent);
    panel->setObjectName(QStringLiteral("axis%1Panel").arg(axis));
    panel->setFrameShape(QFrame::Box);
    panel->setFrameShadow(QFrame::Plain);
    panel->setLineWidth(1);
    panel->setStyleSheet(QStringLiteral("QFrame { border-color: dimgray; }"));
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *title = new QLabel(
        SpectrogramWindow::tr("%1 — frequency (low → high)").arg(axis),
        panel);
    title->setObjectName(QStringLiteral("axis%1Title").arg(axis));
    title->setContentsMargins(6, 3, 6, 3);
    title->setTextFormat(Qt::PlainText);
    layout->addWidget(title);

    auto *image = new QLabel(panel);
    image->setObjectName(imageObjectName);
    image->setAlignment(Qt::AlignCenter);
    image->setScaledContents(true);
    image->setMinimumHeight(80);
    image->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(image, 1);
    *imageOut = image;
    return panel;
}
}

SpectrogramWindow::SpectrogramWindow(Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_dependencies(std::move(dependencies))
{
    buildUi(owner);
}

SpectrogramWindow::~SpectrogramWindow()
{
    stopWorker();
}

void SpectrogramWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("SpectrogramWindow"));
    setWindowTitle(tr("DataFlash Spectrogram"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(WindowWidth, WindowHeight);
    setMinimumSize(MinimumWindowWidth, MinimumWindowHeight);
    if (owner) {
        move(owner->frameGeometry().center() - rect().center());
    }

    auto *root = new QGridLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setHorizontalSpacing(0);
    root->setVerticalSpacing(0);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(8);

    m_openButton = new QPushButton(tr("Open log…"), this);
    m_openButton->setObjectName(QStringLiteral("openLogButton"));
    toolbar->addWidget(m_openButton);

    auto *sensorLabel = new QLabel(tr("Sensor"), this);
    sensorLabel->setObjectName(QStringLiteral("sensorLabel"));
    toolbar->addWidget(sensorLabel);
    m_sensorCombo = new QComboBox(this);
    m_sensorCombo->setObjectName(QStringLiteral("sensorCombo"));
    for (const char *sensor : kSensors) {
        m_sensorCombo->addItem(QLatin1String(sensor));
    }
    m_sensorCombo->setCurrentIndex(0);
    m_sensorCombo->setFixedWidth(100);
    toolbar->addWidget(m_sensorCombo);

    auto *minimumLabel = new QLabel(tr("Min dB"), this);
    minimumLabel->setObjectName(QStringLiteral("minimumDbLabel"));
    toolbar->addWidget(minimumLabel);
    m_minimumDbSpin = new QSpinBox(this);
    m_minimumDbSpin->setObjectName(QStringLiteral("minimumDbSpin"));
    m_minimumDbSpin->setRange(-1000, 1000);
    m_minimumDbSpin->setValue(-80);
    m_minimumDbSpin->setFixedWidth(85);
    toolbar->addWidget(m_minimumDbSpin);

    auto *maximumLabel = new QLabel(tr("Max dB"), this);
    maximumLabel->setObjectName(QStringLiteral("maximumDbLabel"));
    toolbar->addWidget(maximumLabel);
    m_maximumDbSpin = new QSpinBox(this);
    m_maximumDbSpin->setObjectName(QStringLiteral("maximumDbSpin"));
    m_maximumDbSpin->setRange(-1000, 1000);
    m_maximumDbSpin->setValue(-20);
    m_maximumDbSpin->setFixedWidth(85);
    toolbar->addWidget(m_maximumDbSpin);

    m_redrawButton = new QPushButton(tr("Redraw"), this);
    m_redrawButton->setObjectName(QStringLiteral("redrawButton"));
    toolbar->addWidget(m_redrawButton);
    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setObjectName(QStringLiteral("cancelButton"));
    m_cancelButton->setToolTip(
        tr("Cancel the current parsing and spectrogram calculation."));
    toolbar->addWidget(m_cancelButton);
    toolbar->addStretch(1);
    root->addLayout(toolbar, 0, 0);

    m_status = new QLabel(
        tr("Open a DataFlash .bin or .log file."), this);
    m_status->setObjectName(QStringLiteral("spectrogramStatus"));
    m_status->setContentsMargins(0, 8, 0, 8);
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    root->addWidget(m_status, 1, 0);

    auto *plots = new QVBoxLayout;
    plots->setContentsMargins(0, 0, 0, 0);
    plots->setSpacing(4);
    plots->addWidget(makePlotPanel(
        QStringLiteral("X"), QStringLiteral("axisXImage"),
        &m_imageLabels[0], this), 1);
    plots->addWidget(makePlotPanel(
        QStringLiteral("Y"), QStringLiteral("axisYImage"),
        &m_imageLabels[1], this), 1);
    plots->addWidget(makePlotPanel(
        QStringLiteral("Z"), QStringLiteral("axisZImage"),
        &m_imageLabels[2], this), 1);
    root->addLayout(plots, 2, 0);
    root->setRowStretch(2, 1);

    connect(m_openButton, &QPushButton::clicked,
            this, &SpectrogramWindow::pickLog);
    connect(m_redrawButton, &QPushButton::clicked,
            this, &SpectrogramWindow::beginRender);
    connect(m_cancelButton, &QPushButton::clicked,
            this, &SpectrogramWindow::cancelRender);
    setBusy(false);
}

void SpectrogramWindow::setLogPath(const QString &path,
                                   bool renderImmediately)
{
    if (m_busy) {
        return;
    }

    const QFileInfo file(path);
    const QString suffix = file.suffix();
    const bool validSuffix = suffix.compare(QStringLiteral("bin"),
                                             Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("log"), Qt::CaseInsensitive) == 0;
    const bool valid = file.exists() && file.isFile() && validSuffix;
    m_logPath = valid ? file.absoluteFilePath() : QString();
    clearImages();
    if (!valid) {
        m_status->setText(
            path.isEmpty()
                ? tr("Open a DataFlash .bin or .log file.")
                : tr("Select an existing DataFlash .bin or .log file."));
        setBusy(false);
        return;
    }

    m_status->setText(tr("Ready to compute %1 from %2.")
                          .arg(m_sensorCombo->currentText(), file.fileName()));
    setBusy(false);
    if (renderImmediately) {
        beginRender();
    }
}

QString SpectrogramWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

void SpectrogramWindow::pickLog()
{
    if (m_busy) {
        return;
    }
    if (!m_dependencies.chooseLog) {
        m_status->setText(tr("The DataFlash log picker is unavailable."));
        return;
    }
    const QString path = m_dependencies.chooseLog(this);
    if (!path.isEmpty()) {
        setLogPath(path, true);
    }
}

void SpectrogramWindow::beginRender()
{
    if (m_busy) {
        return;
    }
    clearImages();
    if (m_logPath.isEmpty()) {
        m_status->setText(
            tr("Open a DataFlash .bin or .log file first."));
        return;
    }
    const int minimumDb = m_minimumDbSpin->value();
    const int maximumDb = m_maximumDbSpin->value();
    if (minimumDb >= maximumDb) {
        m_status->setText(tr("Min dB must be smaller than Max dB."));
        return;
    }
    if (!m_dependencies.generate) {
        m_status->setText(tr("The spectrogram generator is unavailable."));
        return;
    }

    m_cancelFlag = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancelFlag = m_cancelFlag;
    const QString path = m_logPath;
    const QString sensor = m_sensorCombo->currentText();
    const auto generator = m_dependencies.generate;
    const auto result = std::make_shared<RenderResult>();

    setBusy(true);
    m_status->setText(tr("Computing %1 spectrogram…").arg(sensor));
    QThread *thread = QThread::create(
        [result, generator, path, sensor, minimumDb, maximumDb,
         cancelFlag]() {
            try {
                *result = generator(
                    path, sensor, minimumDb, maximumDb,
                    [cancelFlag]() { return cancelFlag->load(); });
            } catch (const std::exception &exception) {
                result->success = false;
                result->error = QString::fromUtf8(exception.what());
            } catch (...) {
                result->success = false;
                result->error = QStringLiteral(
                    "Unknown spectrogram generator exception.");
            }
        });
    m_thread = thread;
    connect(thread, &QThread::finished, this,
            [this, thread, result, path, sensor]() {
        if (m_thread == thread) {
            m_thread = nullptr;
        }
        finishRender(*result, QFileInfo(path).fileName(), sensor);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void SpectrogramWindow::finishRender(const RenderResult &result,
                                     const QString &fileName,
                                     const QString &sensor)
{
    setBusy(false);
    m_cancelFlag.reset();
    if (result.cancelled) {
        m_status->setText(tr("Spectrogram calculation cancelled."));
        return;
    }
    if (!result.success) {
        clearImages();
        m_status->setText(
            tr("Unable to generate spectrogram: %1")
                .arg(result.error.isEmpty()
                         ? tr("Unknown calculation error.") : result.error));
        return;
    }

    for (int axis = 0; axis < 3; ++axis) {
        if (result.images[axis].isNull()) {
            clearImages();
            m_status->setText(tr(
                "Unable to generate spectrogram: an axis image is empty."));
            return;
        }
        m_imageLabels[axis]->setPixmap(QPixmap::fromImage(result.images[axis]));
    }
    QString status =
        tr("%1 — %2; %3–%4 s, 0–%5 Hz.")
            .arg(fileName, sensor)
            .arg(result.startSeconds, 0, 'f', 3)
            .arg(result.endSeconds, 0, 'f', 3)
            .arg(result.maximumFrequency, 0, 'f', 1);
    if (!result.warning.isEmpty()) {
        status += tr(" Warning: %1").arg(result.warning);
    }
    m_status->setText(status);
}

void SpectrogramWindow::clearImages()
{
    for (QLabel *label : m_imageLabels) {
        if (label) {
            label->clear();
        }
    }
}

void SpectrogramWindow::cancelRender()
{
    if (!m_busy || !m_cancelFlag) {
        return;
    }
    m_cancelFlag->store(true);
    m_cancelButton->setEnabled(false);
    m_status->setText(tr("Cancelling spectrogram calculation…"));
}

void SpectrogramWindow::setBusy(bool busy)
{
    m_busy = busy;
    m_openButton->setEnabled(!busy);
    m_sensorCombo->setEnabled(!busy);
    m_minimumDbSpin->setEnabled(!busy);
    m_maximumDbSpin->setEnabled(!busy);
    m_redrawButton->setEnabled(!busy && !m_logPath.isEmpty());
    m_cancelButton->setEnabled(busy);
}

void SpectrogramWindow::stopWorker()
{
    if (m_cancelFlag) {
        m_cancelFlag->store(true);
    }
    QThread *thread = m_thread.data();
    m_thread = nullptr;
    if (!thread) {
        return;
    }
    disconnect(thread, nullptr, this, nullptr);
    thread->requestInterruption();
    if (thread->isRunning() && !thread->wait(kWorkerShutdownWaitMs)) {
        // The worker captures no window state. Leave its canonical
        // finished->deleteLater connection intact rather than freezing GUI
        // teardown on an uncooperative dependency or blocked filesystem.
        return;
    }
    disconnect(thread, &QThread::finished, thread, &QObject::deleteLater);
    delete thread;
}
