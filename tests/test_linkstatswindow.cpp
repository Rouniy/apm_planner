#include <QtTest>

#include "ui/LinkStatsWindow.h"

#include <QDialog>
#include <QLabel>
#include <QPointer>

class LinkStatsWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void formatsRatesLikeMp10();
    void formatsLinkQualityAndElapsed();
    void windowsAreIndependentModelessTopLevels();
    void refreshShowsLinkFieldsAndStatus();
    void noTargetPresentsTruthfulZerosAndExplanation();
    void timerRefreshesAndStopsWithTheWindow();
};

namespace {

LinkStatsSample connectedSample()
{
    LinkStatsSample sample;
    sample.hasLink = true;
    sample.connected = true;
    sample.linkName = QStringLiteral("UDP Link");
    sample.rxBitsPerSecond = 8 * 1536;   // 1536 B/s -> "1.5 KB/s"
    sample.txBitsPerSecond = 8 * 200;    // 200 B/s
    sample.packetsReceived = 990;
    sample.packetsLost = 10;
    return sample;
}

} // namespace

void LinkStatsWindowTest::formatsRatesLikeMp10()
{
    QCOMPARE(LinkStatsWindow::BytesPerSecondFromBits(0), qint64(0));
    QCOMPARE(LinkStatsWindow::BytesPerSecondFromBits(-8), qint64(0));
    QCOMPARE(LinkStatsWindow::BytesPerSecondFromBits(8), qint64(1));
    QCOMPARE(LinkStatsWindow::BytesPerSecondFromBits(12), qint64(2));  // rounds to nearest
    QCOMPARE(LinkStatsWindow::BytesPerSecondFromBits(11), qint64(1));
    QCOMPARE(LinkStatsWindow::BytesPerSecondFromBits(57600), qint64(7200));

    QCOMPARE(LinkStatsWindow::FormatRate(0), QStringLiteral("0 B/s"));
    QCOMPARE(LinkStatsWindow::FormatRate(1023), QStringLiteral("1023 B/s"));
    QCOMPARE(LinkStatsWindow::FormatRate(1024), QStringLiteral("1.0 KB/s"));
    QCOMPARE(LinkStatsWindow::FormatRate(1536), QStringLiteral("1.5 KB/s"));
    QCOMPARE(LinkStatsWindow::FormatRate(1024 * 1024 - 1), QStringLiteral("1024.0 KB/s"));
    QCOMPARE(LinkStatsWindow::FormatRate(1024 * 1024), QStringLiteral("1.0 MB/s"));
    QCOMPARE(LinkStatsWindow::FormatRate(3 * 1024 * 1024 + 512 * 1024), QStringLiteral("3.5 MB/s"));
    QCOMPARE(LinkStatsWindow::FormatRate(-5), QStringLiteral("0 B/s"));
}

void LinkStatsWindowTest::formatsLinkQualityAndElapsed()
{
    QCOMPARE(LinkStatsWindow::FormatLinkQuality(0, 0), LinkStatsWindow::EmDash());
    QCOMPARE(LinkStatsWindow::FormatLinkQuality(990, 10), QStringLiteral("99.0 %"));
    QCOMPARE(LinkStatsWindow::FormatLinkQuality(1, 0), QStringLiteral("100.0 %"));
    QCOMPARE(LinkStatsWindow::FormatLinkQuality(0, 7), QStringLiteral("0.0 %"));
    QCOMPARE(LinkStatsWindow::FormatLinkQuality(2, 1), QStringLiteral("66.7 %"));

    QCOMPARE(LinkStatsWindow::FormatElapsed(0), QStringLiteral("00:00:00"));
    QCOMPARE(LinkStatsWindow::FormatElapsed(999), QStringLiteral("00:00:00"));
    QCOMPARE(LinkStatsWindow::FormatElapsed(1000), QStringLiteral("00:00:01"));
    QCOMPARE(LinkStatsWindow::FormatElapsed(61 * 1000), QStringLiteral("00:01:01"));
    QCOMPARE(LinkStatsWindow::FormatElapsed((3 * 3600 + 25 * 60 + 7) * 1000LL), QStringLiteral("03:25:07"));
    QCOMPARE(LinkStatsWindow::FormatElapsed(100 * 3600 * 1000LL), QStringLiteral("100:00:00"));
    QCOMPARE(LinkStatsWindow::FormatElapsed(-5), QStringLiteral("00:00:00"));
}

void LinkStatsWindowTest::windowsAreIndependentModelessTopLevels()
{
    QWidget owner;
    owner.resize(800, 600);
    int calls = 0;
    const auto source = [&calls]() { ++calls; return connectedSample(); };

    LinkStatsWindow *first = LinkStatsWindow::OpenWindow(&owner, source);
    LinkStatsWindow *second = LinkStatsWindow::OpenWindow(&owner, source);
    QPointer<LinkStatsWindow> guardedFirst(first);
    QVERIFY(first != second);                    // MP10 OpenWindow: a new window each time
    QCOMPARE(calls, 2);                          // both refreshed immediately
    for (LinkStatsWindow *window : {first, second}) {
        QCOMPARE(window->objectName(), QStringLiteral("LinkStatsWindow"));
        QCOMPARE(window->windowTitle(), QStringLiteral("Link Stats"));
        QCOMPARE(window->windowType(), Qt::Window);
        QVERIFY(window->isWindow());
        QCOMPARE(window->windowModality(), Qt::NonModal);
        QVERIFY(!window->isModal());
        QVERIFY(qobject_cast<QDialog *>(window) == nullptr); // no dialog Enter/Escape semantics
        QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
        QCOMPARE(window->parentWidget(), &owner);
        QCOMPARE(window->size(), QSize(300, 250));
        QCOMPARE(window->minimumSize(), QSize(300, 250));  // MP10 CanResize="False"
        QCOMPARE(window->maximumSize(), QSize(300, 250));
        QVERIFY(window->isVisible());
    }

    first->close();
    QTRY_VERIFY(guardedFirst.isNull());          // WA_DeleteOnClose
    QVERIFY(second->isVisible());                // independent lifetime
    QCOMPARE(second->statusText(), QStringLiteral("Connected"));
    delete second;
}

void LinkStatsWindowTest::refreshShowsLinkFieldsAndStatus()
{
    LinkStatsSample sample = connectedSample();
    LinkStatsWindow window([&sample]() { return sample; });
    QCOMPARE(window.refreshCount(), 1);
    QCOMPARE(window.rxRateText(), QStringLiteral("1.5 KB/s"));
    QCOMPARE(window.txRateText(), QStringLiteral("200 B/s"));
    QCOMPARE(window.packetCountText(), QStringLiteral("990"));
    QCOMPARE(window.packetsLostText(), QStringLiteral("10"));
    QCOMPARE(window.linkQualityText(), QStringLiteral("99.0 %"));
    QCOMPARE(window.timeConnectedText(), QStringLiteral("00:00:00"));
    QCOMPARE(window.statusText(), QStringLiteral("Connected"));
    QCOMPARE(window.lastSample().linkName, QStringLiteral("UDP Link"));

    // Labels are real widgets with stable object names (MP10 captions).
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("linkStatsRxRateLabel"))->text(),
             QStringLiteral("Bytes received"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("linkStatsTimeConnectedLabel"))->text(),
             QStringLiteral("Time connected"));
    QCOMPARE(int(window.findChild<QLabel *>(QStringLiteral("linkStatsRxRate"))->alignment()
                 & Qt::AlignHorizontal_Mask),
             int(Qt::AlignRight));

    // A disconnected but existing link keeps its counters and says so.
    sample.connected = false;
    sample.rxBitsPerSecond = 0;
    sample.txBitsPerSecond = 0;
    window.refresh();
    QCOMPARE(window.refreshCount(), 2);
    QCOMPARE(window.statusText(), QStringLiteral("Disconnected"));
    QCOMPARE(window.rxRateText(), QStringLiteral("0 B/s"));
    QCOMPARE(window.packetCountText(), QStringLiteral("990"));
    QCOMPARE(window.linkQualityText(), QStringLiteral("99.0 %"));

    // Counters that grow between ticks are shown as-is (per-link totals).
    sample.connected = true;
    sample.packetsReceived = 2000;
    sample.packetsLost = 0;
    sample.rxBitsPerSecond = 8 * 2 * 1024 * 1024;
    window.refresh();
    QCOMPARE(window.packetCountText(), QStringLiteral("2000"));
    QCOMPARE(window.packetsLostText(), QStringLiteral("0"));
    QCOMPARE(window.linkQualityText(), QStringLiteral("100.0 %"));
    QCOMPARE(window.rxRateText(), QStringLiteral("2.0 MB/s"));
}

void LinkStatsWindowTest::noTargetPresentsTruthfulZerosAndExplanation()
{
    // A source that reports no link, even with stale-looking numbers attached.
    LinkStatsSample stale;
    stale.hasLink = false;
    stale.connected = true;
    stale.rxBitsPerSecond = 8000;
    stale.packetsReceived = 500;
    stale.packetsLost = 5;
    LinkStatsWindow window([stale]() { return stale; });
    QCOMPARE(window.rxRateText(), QStringLiteral("0 B/s"));
    QCOMPARE(window.txRateText(), QStringLiteral("0 B/s"));
    QCOMPARE(window.packetCountText(), QStringLiteral("0"));
    QCOMPARE(window.packetsLostText(), QStringLiteral("0"));
    QCOMPARE(window.linkQualityText(), LinkStatsWindow::EmDash());
    QCOMPARE(window.statusText(), LinkStatsWindow::NoLinkText());
    QVERIFY(window.statusText().contains(QStringLiteral("No current link")));

    // A null source behaves like "no link" instead of crashing.
    LinkStatsWindow empty{LinkStatsWindow::SampleSource()};
    QCOMPARE(empty.statusText(), LinkStatsWindow::NoLinkText());
    QCOMPARE(empty.packetCountText(), QStringLiteral("0"));
}

void LinkStatsWindowTest::timerRefreshesAndStopsWithTheWindow()
{
    int calls = 0;
    auto *window = new LinkStatsWindow([&calls]() { ++calls; return connectedSample(); });
    QCOMPARE(calls, 1);
    window->setRefreshIntervalMs(10);
    QTRY_VERIFY(calls >= 4);
    QVERIFY(window->refreshCount() >= 4);
    QVERIFY(window->elapsedMs() >= 20);

    // Deleting the window stops the timer: no callbacks into a dead source.
    delete window;
    const int after = calls;
    QTest::qWait(60);
    QCOMPARE(calls, after);
}

QTEST_MAIN(LinkStatsWindowTest)
#include "test_linkstatswindow.moc"
