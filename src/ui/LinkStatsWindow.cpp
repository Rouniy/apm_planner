#include "LinkStatsWindow.h"

#include <QGridLayout>
#include <QLabel>
#include <QTimer>
#include <QVariant>

#include <cmath>
#include <utility>

namespace {

QLabel *valueLabel(const QString &objectName, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setProperty("statValue", true);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter); // MP10 TextBlock.val right aligned
    label->setTextFormat(Qt::PlainText);
    return label;
}

QLabel *captionLabel(const QString &text, const QString &objectName, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setProperty("statLabel", true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

} // namespace

// --- texts / formatting ------------------------------------------------------------------

QString LinkStatsWindow::Title() { return tr("Link Stats"); }
QString LinkStatsWindow::ConnectedText() { return tr("Connected"); }
QString LinkStatsWindow::DisconnectedText() { return tr("Disconnected"); }
QString LinkStatsWindow::NoLinkText()
{
    return tr("No current link: select a vehicle target or connect a link.");
}
QString LinkStatsWindow::EmDash() { return QString(QChar(0x2014)); }

qint64 LinkStatsWindow::BytesPerSecondFromBits(qint64 bitsPerSecond)
{
    if (bitsPerSecond <= 0) {
        return 0;
    }
    return (bitsPerSecond + 4) / 8; // round to the nearest byte
}

QString LinkStatsWindow::FormatRate(qint64 bytesPerSecond)
{
    // MP10 LinkStatsViewModel.Format: 1024-based, one decimal above 1 KB/s.
    if (bytesPerSecond < 0) {
        bytesPerSecond = 0;
    }
    if (bytesPerSecond >= 1024 * 1024) {
        return QStringLiteral("%1 MB/s").arg(bytesPerSecond / 1024.0 / 1024.0, 0, 'f', 1);
    }
    if (bytesPerSecond >= 1024) {
        return QStringLiteral("%1 KB/s").arg(bytesPerSecond / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 B/s").arg(bytesPerSecond);
}

QString LinkStatsWindow::FormatLinkQuality(quint64 received, quint64 lost)
{
    const quint64 total = received + lost;
    if (total == 0) {
        return EmDash(); // MP10 "—" until the first packet
    }
    return QStringLiteral("%1 %").arg(100.0 * static_cast<double>(received)
                                          / static_cast<double>(total), 0, 'f', 1);
}

QString LinkStatsWindow::FormatElapsed(qint64 milliseconds)
{
    if (milliseconds < 0) {
        milliseconds = 0;
    }
    const qint64 seconds = milliseconds / 1000;
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 remainder = seconds % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(remainder, 2, 10, QLatin1Char('0'));
}

// --- lifecycle ---------------------------------------------------------------------------

LinkStatsWindow::LinkStatsWindow(SampleSource source, QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_source(std::move(source))
    , m_timer(new QTimer(this))
{
    buildUi(owner);
    m_elapsed.start();
    connect(m_timer, &QTimer::timeout, this, &LinkStatsWindow::refresh);
    m_timer->start(RefreshIntervalMs);
    refresh(); // MP10 LinkStatsViewModel ctor: Tick() immediately
}

LinkStatsWindow::~LinkStatsWindow() = default;

LinkStatsWindow *LinkStatsWindow::OpenWindow(QWidget *owner, SampleSource source)
{
    auto *window = new LinkStatsWindow(std::move(source), owner);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

void LinkStatsWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("LinkStatsWindow"));
    setWindowTitle(Title());
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setFixedSize(WindowWidth, WindowHeight); // MP10 CanResize="False"
    setStyleSheet(QStringLiteral(
        "LinkStatsWindow { background: #1A201D; }"
        "LinkStatsWindow QLabel[statLabel=\"true\"] { color: #BBC5BF; }"
        "LinkStatsWindow QLabel[statValue=\"true\"] { color: #F5F5F5; font-family: 'Courier New', monospace; }"
        "LinkStatsWindow QLabel#linkStatsStatus { color: #99AADD; }"));
    if (owner) {
        // MP10 WindowStartupLocation="CenterOwner"
        move(owner->frameGeometry().center() - rect().center());
    }

    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(14, 14, 14, 14); // Grid Margin="14"
    grid->setHorizontalSpacing(12);           // ColumnSpacing="12"
    grid->setVerticalSpacing(8);              // TextBlock.val Margin="0,4" top+bottom
    grid->setColumnStretch(1, 1);

    struct Row
    {
        const char *caption;
        const char *captionName;
        const char *valueName;
        QLabel **target;
    };
    const Row rows[] = {
        {"Bytes received", "linkStatsRxRateLabel", "linkStatsRxRate", &m_rxRate},
        {"Bytes sent", "linkStatsTxRateLabel", "linkStatsTxRate", &m_txRate},
        {"Packets", "linkStatsPacketsLabel", "linkStatsPackets", &m_packetCount},
        {"Packets lost", "linkStatsPacketsLostLabel", "linkStatsPacketsLost", &m_packetsLost},
        {"Link quality", "linkStatsLinkQualityLabel", "linkStatsLinkQuality", &m_linkQuality},
        {"Time connected", "linkStatsTimeConnectedLabel", "linkStatsTimeConnected", &m_timeConnected},
    };
    int row = 0;
    for (const Row &entry : rows) {
        grid->addWidget(captionLabel(tr(entry.caption), QLatin1String(entry.captionName), this),
                        row, 0);
        *entry.target = valueLabel(QLatin1String(entry.valueName), this);
        grid->addWidget(*entry.target, row, 1);
        ++row;
    }
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("linkStatsStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setContentsMargins(0, 10, 0, 0); // Margin="0,10,0,0"
    grid->addWidget(m_status, row, 0, 1, 2);
    grid->setRowStretch(row + 1, 1);            // RowDefinitions "...,*"
}

void LinkStatsWindow::setRefreshIntervalMs(int milliseconds)
{
    m_timer->start(qMax(1, milliseconds));
}

void LinkStatsWindow::refresh()
{
    const LinkStatsSample sample = m_source ? m_source() : LinkStatsSample();
    applySample(sample);
}

void LinkStatsWindow::applySample(const LinkStatsSample &sample)
{
    m_lastSample = sample;
    ++m_refreshCount;
    // Without a link every field is a truthful zero / em dash, never stale data.
    const LinkStatsSample shown = sample.hasLink ? sample : LinkStatsSample();
    m_rxRate->setText(FormatRate(BytesPerSecondFromBits(shown.rxBitsPerSecond)));
    m_txRate->setText(FormatRate(BytesPerSecondFromBits(shown.txBitsPerSecond)));
    m_packetCount->setText(QString::number(shown.packetsReceived));
    m_packetsLost->setText(QString::number(shown.packetsLost));
    m_linkQuality->setText(FormatLinkQuality(shown.packetsReceived, shown.packetsLost));
    m_timeConnected->setText(FormatElapsed(m_elapsed.elapsed()));
    if (!sample.hasLink) {
        m_status->setText(NoLinkText());
    } else {
        m_status->setText(sample.connected ? ConnectedText() : DisconnectedText());
    }
}

QString LinkStatsWindow::rxRateText() const { return m_rxRate->text(); }
QString LinkStatsWindow::txRateText() const { return m_txRate->text(); }
QString LinkStatsWindow::packetCountText() const { return m_packetCount->text(); }
QString LinkStatsWindow::packetsLostText() const { return m_packetsLost->text(); }
QString LinkStatsWindow::linkQualityText() const { return m_linkQuality->text(); }
QString LinkStatsWindow::timeConnectedText() const { return m_timeConnected->text(); }
QString LinkStatsWindow::statusText() const { return m_status->text(); }
