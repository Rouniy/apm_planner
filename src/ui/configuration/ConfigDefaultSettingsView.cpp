#include "ConfigDefaultSettingsView.h"

#include "ConfigRawParams.h"

#include <QComboBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {

// Mission Planner 10 ConfigDefaultSettingsView.axaml texts.
const char *const kTitleText = QT_TRANSLATE_NOOP("ConfigDefaultSettingsView", "Default Settings");
const char *const kWarningText = QT_TRANSLATE_NOOP(
    "ConfigDefaultSettingsView",
    "Choose an official ArduPilot profile from Tools/Frame_params. The profile "
    "is compared only with the currently selected vehicle. Selected differences "
    "are staged for review and are not sent until you explicitly choose Write "
    "Params and confirm the write.");

} // namespace

ConfigDefaultSettingsView::ConfigDefaultSettingsView(
    FrameDefaultCatalogService *service, const ParameterMetaDataCatalog &catalog,
    QWidget *parent, bool enforceMetadataRanges)
    : QWidget(parent),
      m_service(service),
      m_cacheRoot(FrameDefaultCatalogService::DefaultCacheRoot())
{
    setObjectName(QStringLiteral("ConfigDefaultSettingsView"));
    buildUi(catalog, enforceMetadataRanges);

    if (service) {
        connect(service, &FrameDefaultCatalogService::catalogReady, this,
                &ConfigDefaultSettingsView::catalogReady);
        connect(service, &FrameDefaultCatalogService::catalogFailed, this,
                &ConfigDefaultSettingsView::catalogFailed);
        connect(service, &FrameDefaultCatalogService::downloadFinished, this,
                &ConfigDefaultSettingsView::downloadFinished);
        connect(service, &FrameDefaultCatalogService::downloadFailed, this,
                &ConfigDefaultSettingsView::downloadFailed);
        connect(service, &QObject::destroyed, this, [this]() {
            m_service = nullptr;
            m_waitingCatalogRequest = false;
            m_ownsCatalogRequest = false;
            m_catalogOperationId = FrameDefaultCatalogService::InvalidOperationId;
            m_downloadPath.clear();
            m_downloadOperationId = FrameDefaultCatalogService::InvalidOperationId;
            m_pendingDiscardStatus.clear();
            setBusy(Busy::Idle);
            setStatus(tr("The frame-default catalog service is unavailable."));
        });
    }
    updateControls();
}

ConfigDefaultSettingsView::~ConfigDefaultSettingsView()
{
    // Release the operations this page owns without touching the UI.
    if (m_service && (m_ownsCatalogRequest || !m_downloadPath.isEmpty())) {
        m_service->disconnect(this);
        if (m_ownsCatalogRequest) {
            m_service->cancelCatalog(m_catalogOperationId);
        }
        if (!m_downloadPath.isEmpty()) {
            m_service->cancelDownload(m_downloadOperationId);
        }
    }
}

void ConfigDefaultSettingsView::buildUi(const ParameterMetaDataCatalog &catalog,
                                        bool enforceMetadataRanges)
{
    setStyleSheet(QStringLiteral(
        "ConfigDefaultSettingsView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#DefaultSettingsTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#DefaultSettingsWarning, QLabel#FrameDefaultsLabel,"
        " QLabel#FrameDefaultsStatus { color: #BBC5BF; }"
        "ConfigDefaultSettingsView QPushButton { padding: 5px 12px; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(4);

    auto *header = new QVBoxLayout;
    header->setContentsMargins(8, 4, 8, 0);
    header->setSpacing(4);
    m_title = new QLabel(tr(kTitleText), this);
    m_title->setObjectName(QStringLiteral("DefaultSettingsTitle"));
    header->addWidget(m_title);
    m_warning = new QLabel(tr(kWarningText), this);
    m_warning->setObjectName(QStringLiteral("DefaultSettingsWarning"));
    m_warning->setWordWrap(true);
    m_warning->setMaximumWidth(900);
    m_warning->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    header->addWidget(m_warning, 0, Qt::AlignLeft);
    root->addLayout(header);

    // The "ArduPilot frame defaults" row of MP10 RawParamsView.axaml.
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 8);
    row->setSpacing(6);
    m_rowLabel = new QLabel(tr("ArduPilot frame defaults"), this);
    m_rowLabel->setObjectName(QStringLiteral("FrameDefaultsLabel"));
    row->addWidget(m_rowLabel, 0, Qt::AlignVCenter);
    m_combo = new QComboBox(this);
    m_combo->setObjectName(QStringLiteral("FrameDefaults"));
    m_combo->setFixedWidth(360);
    m_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    row->addWidget(m_combo, 0, Qt::AlignVCenter);
    m_compareButton = new QPushButton(tr("Compare / Stage…"), this);
    m_compareButton->setObjectName(QStringLiteral("LoadFrameDefaultsBtn"));
    row->addWidget(m_compareButton, 0, Qt::AlignVCenter);
    m_refreshButton = new QPushButton(tr("Load / refresh list"), this);
    m_refreshButton->setObjectName(QStringLiteral("RefreshFrameDefaultsBtn"));
    row->addWidget(m_refreshButton, 0, Qt::AlignVCenter);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("FrameDefaultsStatus"));
    m_statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_statusLabel->setTextFormat(Qt::PlainText);
    row->addWidget(m_statusLabel, 1, Qt::AlignVCenter);
    root->addLayout(row);

    m_rawParams = new ConfigRawParams(catalog, this, enforceMetadataRanges);
    root->addWidget(m_rawParams, 1);

    connect(m_compareButton, &QPushButton::clicked, this,
            &ConfigDefaultSettingsView::compareAndStage);
    connect(m_refreshButton, &QPushButton::clicked, this,
            &ConfigDefaultSettingsView::refreshFrameDefaults);
    connect(m_rawParams, &ConfigRawParams::refreshRequested, this,
            &ConfigDefaultSettingsView::refreshRequested);
    // Only ConfigRawParams' own Write Params action reaches this signal.
    connect(m_rawParams, &ConfigRawParams::writeRequested, this,
            &ConfigDefaultSettingsView::writeRequested);
}

void ConfigDefaultSettingsView::setCatalog(const ParameterMetaDataCatalog &catalog,
                                           bool enforceMetadataRanges)
{
    m_rawParams->setCatalog(catalog, enforceMetadataRanges);
}

void ConfigDefaultSettingsView::setConnected(bool connected)
{
    if (m_connected != connected) {
        m_connected = connected;
        if (!connected) {
            ++m_targetRevision;
            discardDownload(tr("Frame-default download discarded because the "
                               "vehicle disconnected."));
        }
    }
    m_rawParams->setConnected(connected);
    updateControls();
}

void ConfigDefaultSettingsView::setParameterSnapshot(
    const QList<ParameterRecord> &records, int preferredComponent)
{
    // A new committed snapshot invalidates a download captured against the
    // previous one before ConfigRawParams learns about it.
    ++m_targetRevision;
    discardDownload(tr("Vehicle changed while the profile was downloading; the "
                       "old result was discarded."));
    m_rawParams->setParameterSnapshot(records, preferredComponent);
}

void ConfigDefaultSettingsView::setCacheRoot(const QString &root)
{
    const QString trimmed = root.trimmed();
    m_cacheRoot = trimmed.isEmpty() ? FrameDefaultCatalogService::DefaultCacheRoot()
                                    : trimmed;
}

QString ConfigDefaultSettingsView::selectedFrameDefaultPath() const
{
    const int index = m_combo->currentIndex();
    if (index < 0) {
        return QString();
    }
    return m_combo->itemData(index).toString();
}

void ConfigDefaultSettingsView::activate()
{
    m_active = true;
    if (m_items.isEmpty() && m_busy == Busy::Idle) {
        requestCatalog(false);
    }
    updateControls();
}

void ConfigDefaultSettingsView::deactivate()
{
    m_active = false;
    if (!m_downloadPath.isEmpty()) {
        discardDownload(tr("Frame-default download cancelled."));
    }
    if (m_waitingCatalogRequest && m_ownsCatalogRequest && m_service) {
        m_service->cancelCatalog(m_catalogOperationId);
    }
    if (m_waitingCatalogRequest) {
        // A joined request belongs to another consumer and must stay alive;
        // detach this hidden page locally. Also settles a vanished service.
        m_waitingCatalogRequest = false;
        m_ownsCatalogRequest = false;
        m_catalogOperationId = FrameDefaultCatalogService::InvalidOperationId;
        setBusy(Busy::Idle);
        setStatus(tr("Frame-default list request cancelled."));
    }
    updateControls();
}

void ConfigDefaultSettingsView::parameterTargetChanged()
{
    ++m_targetRevision;
    discardDownload(tr("Vehicle changed during download; the old profile was "
                       "discarded."));
    m_rawParams->parameterTargetChanged();
    updateControls();
}

void ConfigDefaultSettingsView::refreshFrameDefaults()
{
    requestCatalog(true);
}

void ConfigDefaultSettingsView::compareAndStage()
{
    if (m_busy != Busy::Idle) {
        return;
    }
    const QString path = selectedFrameDefaultPath();
    if (path.isEmpty()) {
        setStatus(tr("Select a frame-default file first."));
        return;
    }
    if (!m_connected) {
        setStatus(tr("Connect a vehicle before comparing a default profile."));
        return;
    }
    if (!m_service) {
        setStatus(tr("Frame-default download failed: the catalog service is "
                     "unavailable."));
        return;
    }

    QString error;
    FrameDefaultCatalogService::OperationId operationId =
        FrameDefaultCatalogService::InvalidOperationId;
    if (!m_service->download(path, &error, &operationId)) {
        setStatus(tr("Frame-default download failed: %1").arg(error));
        return;
    }
    m_downloadPath = path;
    m_downloadOperationId = operationId;
    m_downloadRevision = m_targetRevision;
    m_pendingDiscardStatus.clear();
    setBusy(Busy::Downloading);
    setStatus(tr("Downloading %1…").arg(displayNameFor(path)));
}

void ConfigDefaultSettingsView::requestCatalog(bool forceRefresh)
{
    if (m_busy != Busy::Idle) {
        return;
    }
    if (!m_service) {
        setStatus(tr("Frame-default list failed: the catalog service is "
                     "unavailable."));
        return;
    }
    m_waitingCatalogRequest = true;
    m_ownsCatalogRequest = false;
    m_catalogOperationId = FrameDefaultCatalogService::InvalidOperationId;
    setBusy(Busy::LoadingList);
    setStatus(tr("Loading the ArduPilot frame-default list…"));
    QString error;
    // A memoised catalog is delivered synchronously inside this call.
    bool started = false;
    FrameDefaultCatalogService::OperationId operationId =
        FrameDefaultCatalogService::InvalidOperationId;
    if (!m_service->requestCatalog(
            forceRefresh, &error, &started, &operationId)) {
        m_waitingCatalogRequest = false;
        m_ownsCatalogRequest = false;
        m_catalogOperationId = FrameDefaultCatalogService::InvalidOperationId;
        setBusy(Busy::Idle);
        setStatus(tr("Frame-default list failed: %1").arg(error));
    } else if (m_waitingCatalogRequest) {
        // Cached results are emitted synchronously and already clear the wait.
        m_ownsCatalogRequest = started;
        m_catalogOperationId = operationId;
    }
}

void ConfigDefaultSettingsView::catalogReady(
    const QVector<FrameDefaultCatalogItem> &items, bool fromCache)
{
    Q_UNUSED(fromCache)
    const bool awaited = m_waitingCatalogRequest;
    m_waitingCatalogRequest = false;
    m_ownsCatalogRequest = false;
    m_catalogOperationId = FrameDefaultCatalogService::InvalidOperationId;
    if (!awaited && !m_active) {
        return; // a foreign refresh while hidden must not touch the page
    }

    m_items = items;
    {
        const QSignalBlocker blocker(m_combo);
        m_combo->clear();
        for (const FrameDefaultCatalogItem &item : items) {
            m_combo->addItem(item.displayName(), item.path);
        }
        m_combo->setCurrentIndex(items.isEmpty() ? -1 : 0);
    }
    if (awaited && m_busy == Busy::LoadingList) {
        setBusy(Busy::Idle);
    }
    setStatus(items.isEmpty()
                  ? tr("No ArduPilot frame-default files were found.")
                  : tr("Loaded %1 ArduPilot frame-default file choices.")
                        .arg(items.size()));
    updateControls();
}

void ConfigDefaultSettingsView::catalogFailed(const QString &error, bool cancelled)
{
    if (!m_waitingCatalogRequest) {
        return; // somebody else's walk
    }
    m_waitingCatalogRequest = false;
    m_ownsCatalogRequest = false;
    m_catalogOperationId = FrameDefaultCatalogService::InvalidOperationId;
    if (m_busy == Busy::LoadingList) {
        setBusy(Busy::Idle);
    }
    setStatus(cancelled ? tr("Frame-default list request cancelled.")
                        : tr("Frame-default list failed: %1").arg(error));
    updateControls();
}

void ConfigDefaultSettingsView::downloadFinished(const QString &catalogPath,
                                                 const QByteArray &bytes)
{
    if (m_downloadPath.isEmpty() || catalogPath != m_downloadPath) {
        return; // not the download this page owns
    }
    const QString path = m_downloadPath;
    m_downloadPath.clear();
    m_downloadOperationId = FrameDefaultCatalogService::InvalidOperationId;
    m_pendingDiscardStatus.clear();
    setBusy(Busy::Idle);

    if (m_downloadRevision != m_targetRevision || !m_connected) {
        setStatus(tr("Vehicle changed while the profile was downloading; the "
                     "old result was discarded."));
        updateControls();
        return;
    }
    if (!m_active) {
        setStatus(tr("Frame-default download cancelled."));
        updateControls();
        return;
    }

    QString error;
    const QString cachePath = FrameDefaultCatalogService::WriteCacheFile(
        m_cacheRoot, path, bytes, &error);
    if (cachePath.isEmpty()) {
        setStatus(tr("Frame-default download failed: %1").arg(error));
        updateControls();
        return;
    }

    const QString display = displayNameFor(path);
    setStatus(tr("%1: choose which differences to stage.").arg(display));
    updateControls();

    // Opens ConfigRawParams' modal Compare Params dialog; selected rows are
    // only staged. Writing remains its separate Write Params confirmation.
    const QPointer<ConfigDefaultSettingsView> guard(this);
    const quint64 reviewRevision = m_targetRevision;
    m_rawParams->reviewAndStageParameterFile(cachePath);
    if (!guard) {
        return;
    }
    // ConfigRawParams guards its own staging revision while the nested modal
    // event loop runs. Mirror that guard here so a target switch, disconnect,
    // or page change cannot overwrite the newer lifecycle status afterwards.
    if (reviewRevision != m_targetRevision || !m_connected || !m_active) {
        setStatus(tr("Vehicle or page changed while the profile was being "
                     "reviewed; no profile values were staged."));
        return;
    }
    setStatus(tr("%1: compared with the current vehicle; staged values are "
                 "written only through Write Params.").arg(display));
}

void ConfigDefaultSettingsView::downloadFailed(const QString &catalogPath,
                                               const QString &error, bool cancelled)
{
    if (m_downloadPath.isEmpty() || catalogPath != m_downloadPath) {
        return; // not the download this page owns
    }
    m_downloadPath.clear();
    m_downloadOperationId = FrameDefaultCatalogService::InvalidOperationId;
    setBusy(Busy::Idle);
    const QString discardStatus = m_pendingDiscardStatus;
    m_pendingDiscardStatus.clear();
    if (!discardStatus.isEmpty()) {
        setStatus(discardStatus);
    } else if (cancelled) {
        setStatus(tr("Frame-default download cancelled."));
    } else {
        setStatus(tr("Frame-default download failed: %1").arg(error));
    }
    updateControls();
}

void ConfigDefaultSettingsView::discardDownload(const QString &status)
{
    if (m_downloadPath.isEmpty()) {
        return;
    }
    m_pendingDiscardStatus = status;
    if (m_service) {
        m_service->cancelDownload(m_downloadOperationId);
    }
    if (!m_downloadPath.isEmpty()) {
        // The service is gone or did not report back: settle the UI here.
        m_downloadPath.clear();
        m_downloadOperationId = FrameDefaultCatalogService::InvalidOperationId;
        m_pendingDiscardStatus.clear();
        setBusy(Busy::Idle);
        setStatus(status);
        updateControls();
    }
}

QString ConfigDefaultSettingsView::displayNameFor(const QString &catalogPath) const
{
    for (const FrameDefaultCatalogItem &item : m_items) {
        if (item.path == catalogPath) {
            return item.displayName();
        }
    }
    return FrameDefaultCatalogService::DisplayName(
        catalogPath.section(QLatin1Char('/'), -1), catalogPath);
}

void ConfigDefaultSettingsView::setBusy(Busy busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    updateControls();
}

void ConfigDefaultSettingsView::updateControls()
{
    // MP10: every control of the row is enabled while !LoadingFrameDefaults.
    const bool idle = m_busy == Busy::Idle;
    m_combo->setEnabled(idle);
    m_compareButton->setEnabled(idle);
    m_refreshButton->setEnabled(idle);
}

void ConfigDefaultSettingsView::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    updateStatusLabel();
    emit statusChanged(status);
}

void ConfigDefaultSettingsView::updateStatusLabel()
{
    m_statusLabel->setToolTip(m_status);
    const int width = qMax(0, m_statusLabel->width() - 2);
    m_statusLabel->setText(width > 0
                               ? m_statusLabel->fontMetrics().elidedText(
                                     m_status, Qt::ElideRight, width)
                               : m_status);
}

void ConfigDefaultSettingsView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateStatusLabel();
}
