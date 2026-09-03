#ifndef LINKSTATSWINDOW_H
#define LINKSTATSWINDOW_H

#include <QElapsedTimer>
#include <QString>
#include <QWidget>

#include <functional>

class QLabel;
class QTimer;

/*
 * Mission Planner 10 TOOLS > "Link Statistics" (Views/LinkStatsWindow.axaml,
 * ViewModels/LinkStatsViewModel.cs): a 300x250 fixed, modeless, independent
 * top-level window titled "Link Stats" that refreshes once per second and shows
 * Bytes received / Bytes sent (per-second rates), Packets, Packets lost, Link
 * quality (100 * received / (received + lost), or an em dash), Time connected
 * (elapsed since the window opened, hh:mm:ss) and a status line.
 *
 * The window itself is pure presentation over an injected SampleSource so it is
 * unit-testable without LinkManager. The application source, defined in
 * LinkStatsSampleSource.cpp, resolves the exact current VehicleTargetManager
 * lease to its physical link id on EVERY tick (no LinkInterface pointer is
 * retained across ticks, so link removal cannot leave a dangling reference),
 * converts LinkInterface's bits/s rates to B/s and reads MAVLinkProtocol's
 * per-link message counters. Without a valid target or link the fields stay at
 * truthful zeros / em dash and the status says so.
 */
struct LinkStatsSample
{
    bool hasLink = false;          // the current target resolves to an existing link
    bool connected = false;        // LinkInterface::isConnected()
    QString linkName;              // LinkInterface::getShortName(), informational
    qint64 rxBitsPerSecond = 0;    // LinkInterface::getCurrentInDataRate()
    qint64 txBitsPerSecond = 0;    // LinkInterface::getCurrentOutDataRate()
    quint64 packetsReceived = 0;   // MAVLinkProtocol::getTotalMessagesReceived(linkId)
    quint64 packetsLost = 0;       // MAVLinkProtocol::getTotalMessagesLost(linkId)
};

class LinkStatsWindow final : public QWidget
{
    Q_OBJECT

public:
    using SampleSource = std::function<LinkStatsSample()>;

    static constexpr int WindowWidth = 300;     // MP10 Width="300"
    static constexpr int WindowHeight = 250;    // MP10 Height="250"
    static constexpr int RefreshIntervalMs = 1000;

    // Pure window over any source; owner may be null. Refreshes immediately.
    explicit LinkStatsWindow(SampleSource source, QWidget *owner = nullptr);
    ~LinkStatsWindow() override;

    // MP10 OpenWindow(): a NEW independent modeless window on every call,
    // shown and raised. The overload without a source uses the application
    // source (LinkStatsSampleSource.cpp) and is not available to pure tests.
    static LinkStatsWindow *OpenWindow(QWidget *owner, SampleSource source);
    static LinkStatsWindow *OpenWindow(QWidget *owner);
    // Application source: current VehicleTargetManager lease -> link id ->
    // LinkManager::getLink() + MAVLinkProtocol counters, re-fetched per call.
    static SampleSource ApplicationSampleSource();

    // --- MP10 texts / formatting (pure) ---
    static QString Title();                     // "Link Stats"
    static QString ConnectedText();             // "Connected"
    static QString DisconnectedText();          // "Disconnected"
    static QString NoLinkText();                // explains that no current link exists
    static QString EmDash();                    // "—"
    static qint64 BytesPerSecondFromBits(qint64 bitsPerSecond);   // rounds to nearest byte
    static QString FormatRate(qint64 bytesPerSecond);             // "N B/s", "x.x KB/s", "x.x MB/s"
    static QString FormatLinkQuality(quint64 received, quint64 lost); // "x.x %" or em dash
    static QString FormatElapsed(qint64 milliseconds);            // hh:mm:ss

    // --- state for tests ---
    LinkStatsSample lastSample() const { return m_lastSample; }
    QString rxRateText() const;
    QString txRateText() const;
    QString packetCountText() const;
    QString packetsLostText() const;
    QString linkQualityText() const;
    QString timeConnectedText() const;
    QString statusText() const;
    int refreshCount() const { return m_refreshCount; }
    void setRefreshIntervalMs(int milliseconds);
    qint64 elapsedMs() const { return m_elapsed.elapsed(); }

public slots:
    // Pulls one sample and repaints every field; also driven by the timer.
    void refresh();

private:
    void buildUi(QWidget *owner);
    void applySample(const LinkStatsSample &sample);

    SampleSource m_source;
    QTimer *m_timer = nullptr;
    QElapsedTimer m_elapsed;
    LinkStatsSample m_lastSample;
    int m_refreshCount = 0;
    QLabel *m_rxRate = nullptr;
    QLabel *m_txRate = nullptr;
    QLabel *m_packetCount = nullptr;
    QLabel *m_packetsLost = nullptr;
    QLabel *m_linkQuality = nullptr;
    QLabel *m_timeConnected = nullptr;
    QLabel *m_status = nullptr;
};

#endif // LINKSTATSWINDOW_H
