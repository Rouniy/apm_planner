#include "logging.h"
#include "MainWindow.h"
#include "QGCMAVLinkLogPlayer.h"
#include "QGCMAVLinkInspector.h"
#include "QGC.h"
#include "ui_QGCMAVLinkLogPlayer.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QFileInfo>

QGCMAVLinkLogPlayer::QGCMAVLinkLogPlayer(QWidget *parent):
    QWidget(parent),
    m_sliderDown(false),
    m_isPlaying(false),
    ui(new Ui::QGCMAVLinkLogPlayer),
    m_logLink(NULL),
    m_logLoaded(false),
    m_shuttingDown(false),
    m_mavlinkDecoder(NULL),
    m_inspectorRelay(),
    m_replaySource()
{
    ui->setupUi(this);
    ui->horizontalLayout->setAlignment(Qt::AlignTop);

    connect(ui->selectFileButton, SIGNAL(clicked()), this, SLOT(loadLogButtonClicked()));
    connect(ui->playButton, SIGNAL(clicked()), this, SLOT(playButtonClicked()));
    connect(ui->positionSlider,SIGNAL(sliderReleased()),this,SLOT(positionSliderReleased()));
    connect(ui->positionSlider,SIGNAL(sliderPressed()),this,SLOT(positionSliderPressed()));

    connect(ui->speedButton75,SIGNAL(clicked()),this,SLOT(speed75Clicked()));
    connect(ui->speedButton100,SIGNAL(clicked()),this,SLOT(speed100Clicked()));
    connect(ui->speedButton150,SIGNAL(clicked()),this,SLOT(speed150Clicked()));
    connect(ui->speedButton200,SIGNAL(clicked()),this,SLOT(speed200Clicked()));
    connect(ui->speedButton500,SIGNAL(clicked()),this,SLOT(speed500Clicked()));
    connect(ui->speedButton1000,SIGNAL(clicked()),this,SLOT(speed1000Clicked()));

    setSpeedControlsEnabled(false);

    connect(&m_replaySource,
            &MAVLinkReplaySource::replayMessageObserved,
            this, &QGCMAVLinkLogPlayer::replayMessageObserved);
    connect(&m_replaySource,
            &MAVLinkReplaySource::replaySourceEnded,
            this, &QGCMAVLinkLogPlayer::replaySourceEnded);
}
void QGCMAVLinkLogPlayer::speed75Clicked()
{
    m_logLink->setSpeed(75);
    ui->speedButton100->setChecked(false);
    ui->speedButton150->setChecked(false);
    ui->speedButton200->setChecked(false);
    ui->speedButton500->setChecked(false);
    ui->speedButton1000->setChecked(false);
}

void QGCMAVLinkLogPlayer::speed100Clicked()
{
    ui->speedButton75->setChecked(false);
    m_logLink->setSpeed(100);
    ui->speedButton150->setChecked(false);
    ui->speedButton200->setChecked(false);
    ui->speedButton500->setChecked(false);
    ui->speedButton1000->setChecked(false);
}

void QGCMAVLinkLogPlayer::speed150Clicked()
{
    ui->speedButton75->setChecked(false);
    ui->speedButton100->setChecked(false);
    m_logLink->setSpeed(150);
    ui->speedButton200->setChecked(false);
    ui->speedButton500->setChecked(false);
    ui->speedButton1000->setChecked(false);
}

void QGCMAVLinkLogPlayer::speed200Clicked()
{
    ui->speedButton75->setChecked(false);
    ui->speedButton100->setChecked(false);
    ui->speedButton150->setChecked(false);
    m_logLink->setSpeed(200);
    ui->speedButton500->setChecked(false);
    ui->speedButton1000->setChecked(false);
}

void QGCMAVLinkLogPlayer::speed500Clicked()
{
    ui->speedButton75->setChecked(false);
    ui->speedButton100->setChecked(false);
    ui->speedButton150->setChecked(false);
    ui->speedButton200->setChecked(false);
    m_logLink->setSpeed(500);
    ui->speedButton1000->setChecked(false);
}
void QGCMAVLinkLogPlayer::speed1000Clicked()
{
    ui->speedButton75->setChecked(false);
    ui->speedButton100->setChecked(false);
    ui->speedButton150->setChecked(false);
    ui->speedButton200->setChecked(false);
    ui->speedButton500->setChecked(false);
    m_logLink->setSpeed(1000);
}

void QGCMAVLinkLogPlayer::positionSliderReleased()
{
    m_sliderDown = false;
    if (m_logLink)
    {
        m_logLink->setPosition(ui->positionSlider->value());
    }
}

void QGCMAVLinkLogPlayer::positionSliderPressed()
{
    //Deactivate signals here
    m_sliderDown = true;

}

QGCMAVLinkLogPlayer::~QGCMAVLinkLogPlayer()
{
    storeSettings();
    shutdown();
    delete ui;
}

void QGCMAVLinkLogPlayer::shutdown()
{
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    unloadReplayLink();
}
void QGCMAVLinkLogPlayer::storeSettings()
{
    // Nothing to store
}


void QGCMAVLinkLogPlayer::loadLogButtonClicked()
{
    if (m_shuttingDown) {
        return;
    }

    if (m_logLoaded || m_logLink || m_replaySource.activeLease().isValid())
    {
        unloadReplayLink();
        return;
    }


    QFileDialog *dialog = new QFileDialog(this,tr("Specify MAVLink log file name to replay"), QGC::MAVLinkLogDirectory(), tr("MAVLink Telemetry log (*.tlog)"));
    dialog->setFileMode(QFileDialog::ExistingFile);
    connect(dialog,SIGNAL(accepted()),this,SLOT(loadLogDialogAccepted()));
    dialog->show();
}
void QGCMAVLinkLogPlayer::loadLogDialogAccepted()
{
    if (m_shuttingDown) {
        return;
    }

    QFileDialog *dialog = qobject_cast<QFileDialog*>(sender());
    if (!dialog)
    {
        return;
    }
    if (dialog->selectedFiles().size() == 0)
    {
        //No file selected/cancel clicked
        return;
    }
    const QString fileName = dialog->selectedFiles().at(0);
    TLogReplayLink *const link = new TLogReplayLink(this);
    link->setLog(fileName);

    const MAVLinkReplayLease lease = m_replaySource.beginSource(
        QFileInfo(fileName).fileName());
    if (!lease.isValid()) {
        delete link;
        return;
    }

    m_logLink = link;
    //m_logLink->setMavlinkDecoder(m_mavlinkDecoder);
    connect(link, &TLogReplayLink::inspectorMessage,
            this,
            [this, generation = lease.generation](
                LinkInterface *, mavlink_message_t message) {
                // The worker-provided link pointer is intentionally ignored:
                // it may be stale by the time this queued callback runs.
                const QPointer<QGCMAVLinkLogPlayer> self(this);
                if (!m_replaySource.publish(generation, message) || !self) {
                    return;
                }
                if (m_replaySource.isActive(generation)) {
                    m_inspectorRelay.publish(nullptr, message);
                }
            },
            Qt::QueuedConnection);
    connect(link, &TLogReplayLink::logProgress,
            this,
            [this, generation = lease.generation](qint64 pos,
                                                   qint64 total) {
                if (m_replaySource.isActive(generation)) {
                    logProgress(pos, total);
                }
            },
            Qt::QueuedConnection);

    const QPointer<TLogReplayLink> guardedLink(link);
    connect(link, &QThread::finished,
            this,
            [this, guardedLink, generation = lease.generation]() {
                handleLogLinkTerminated(guardedLink, generation);
            },
            Qt::QueuedConnection);

    m_logLoaded = true;
    m_isPlaying = true;
    ui->logStatsLabel->setText(lease.displayName);
    ui->playButton->setIcon(QIcon(":/files/images/actions/media-playback-stop.svg"));
    setSpeedControlsEnabled(true);

    link->connect();
    emit logLoaded();
}
void QGCMAVLinkLogPlayer::logProgress(qint64 pos,qint64 total)
{
    //ui->positionSlider->setValue(((double)pos / (double)total) * 100);
    if (!m_sliderDown)
    {
        //ui->positionProgressBar->setValue(((double)pos / (double)total) * 100);
        ui->positionLabel->setText(QString::number(pos) + "/" + QString::number(total));
        ui->positionSlider->setValue(((double)pos / (double)total) * 100);
    }
}
void QGCMAVLinkLogPlayer::setMavlinkDecoder(MAVLinkDecoder *decoder)
{
    m_mavlinkDecoder = decoder;
}
void QGCMAVLinkLogPlayer::addMavlinkInspector(
    QGCMAVLinkInspector *inspector)
{
    if (!inspector) {
        return;
    }

    const QPointer<QGCMAVLinkInspector> guardedInspector(inspector);
    m_inspectorRelay.subscribe(
        inspector,
        [guardedInspector](LinkInterface *link,
                           const mavlink_message_t &message) {
            if (guardedInspector) {
                guardedInspector->receiveMessage(link, message);
            }
        });
}

void QGCMAVLinkLogPlayer::removeMavlinkInspector(
    QGCMAVLinkInspector *inspector)
{
    m_inspectorRelay.unsubscribe(inspector);
}

int QGCMAVLinkLogPlayer::mavlinkInspectorSubscriberCount() const
{
    return m_inspectorRelay.subscriberCount();
}

MAVLinkReplayLease QGCMAVLinkLogPlayer::activeReplayLease() const
{
    return m_replaySource.activeLease();
}

void QGCMAVLinkLogPlayer::playButtonClicked()
{
    if (m_logLink)
    {
        if (m_logLink->isPaused())
        {
            m_logLink->play();
            m_isPlaying = true;
            ui->playButton->setIcon(QIcon(":/files/images/actions/media-playback-stop.svg"));
        }
        else
        {
            m_logLink->pause();
            m_isPlaying = false;
            ui->playButton->setIcon(QIcon(":/files/images/actions/media-playback-start.svg"));
        }
    }
}
void QGCMAVLinkLogPlayer::logLinkTerminated()
{
    TLogReplayLink *finishedLink = qobject_cast<TLogReplayLink *>(sender());
    if (!finishedLink) {
        finishedLink = m_logLink;
    }
    handleLogLinkTerminated(QPointer<TLogReplayLink>(finishedLink),
                            m_replaySource.activeLease().generation);
}

void QGCMAVLinkLogPlayer::setSpeedControlsEnabled(bool enabled)
{
    ui->speedButton75->setEnabled(enabled);
    ui->speedButton100->setEnabled(enabled);
    ui->speedButton150->setEnabled(enabled);
    ui->speedButton200->setEnabled(enabled);
    ui->speedButton500->setEnabled(enabled);
    ui->speedButton1000->setEnabled(enabled);
}

void QGCMAVLinkLogPlayer::unloadReplayLink()
{
    const MAVLinkReplayLease lease = m_replaySource.activeLease();
    const QPointer<TLogReplayLink> linkToDelete(m_logLink);

    // Detach all player state before replaySourceEnded is emitted. A listener
    // may synchronously start a successor replay, and this cleanup must never
    // clear its pointer or overwrite its controls.
    m_logLink = nullptr;
    m_logLoaded = false;
    m_isPlaying = false;
    setSpeedControlsEnabled(false);

    if (lease.isValid()) {
        // Invalidate first. Already queued callbacks carrying this generation
        // are rejected before the worker is stopped or destroyed.
        m_replaySource.endSource(lease.generation);
    }

    if (linkToDelete) {
        TLogReplayLink *const oldLink = linkToDelete.data();
        oldLink->disconnect();
        if (linkToDelete) {
            delete oldLink;
        }
    }
}

void QGCMAVLinkLogPlayer::handleLogLinkTerminated(
    const QPointer<TLogReplayLink> &finishedLink,
    quint64 generation)
{
    if (!finishedLink || finishedLink.data() != m_logLink
        || !m_replaySource.isActive(generation)
        || !finishedLink->toBeDeleted()) {
        return;
    }

    const QPointer<TLogReplayLink> linkToDelete(finishedLink);

    // Natural completion belongs to this source only when both the guarded
    // worker identity and its captured generation still match. Detach the old
    // UI state before emitting replaySourceEnded because a listener may start
    // the next source synchronously.
    m_logLink = nullptr;
    m_logLoaded = false;
    m_isPlaying = false;
    setSpeedControlsEnabled(false);
    const QPointer<QGCMAVLinkLogPlayer> self(this);
    m_replaySource.endSource(generation);

    if (linkToDelete) {
        linkToDelete->deleteLater();
    }

    // logFinished has no generation parameter. Suppress it if an end observer
    // already installed a successor, otherwise legacy listeners could mistake
    // the old EOF for completion of the new replay.
    if (self && !m_logLink && !m_replaySource.activeLease().isValid()) {
        emit logFinished();
    }
}

void QGCMAVLinkLogPlayer::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    switch (e->type())
    {
    case QEvent::LanguageChange:
        ui->retranslateUi(this);
        break;
    default:
        break;
    }
}
void QGCMAVLinkLogPlayer::speedSliderValueChanged(int value)
{
    if (m_logLink)
    {
        double newval = ((value / 100.0) * 130) + 70;
        m_logLink->setSpeed(newval);
        //ui->speedLabel->setText(QString::number(newval) + "%");
    }
}
