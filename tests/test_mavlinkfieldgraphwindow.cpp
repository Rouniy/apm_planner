#include <QtTest>

#include "ui/MavlinkFieldGraphWindow.h"
#include "ui/qcustomplot.h"

#include <QLabel>
#include <QPointer>
#include <QTimer>

namespace {

mavlink_message_t heartbeat(quint8 systemId,
                            quint8 componentId,
                            quint32 customMode)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, componentId, &message,
        MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
        0, customMode, MAV_STATE_ACTIVE);
    return message;
}

} // namespace

class MavlinkFieldGraphWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void isIndependentModelessWindowWithExactLabels();
    void plotsOnlyMatchingTrafficWithBoundedHistory();
    void usesMissionPlannerPaletteForArraySeries();
};

void MavlinkFieldGraphWindowTest::
isIndependentModelessWindowWithExactLabels()
{
    QWidget owner;
    owner.setGeometry(100, 80, 1000, 700);
    const MavlinkGraphSelection selection{
        42, 7, MAVLINK_MSG_ID_HEARTBEAT,
        QStringLiteral("HEARTBEAT"), QStringLiteral("custom_mode")};
    auto *window = new MavlinkFieldGraphWindow(selection, 500, &owner);
    QPointer<MavlinkFieldGraphWindow> guardedWindow(window);

    QCOMPARE(window->objectName(),
             QStringLiteral("mavlinkFieldGraphWindow"));
    QCOMPARE(window->windowTitle(),
             QStringLiteral("MAVLink Graph \u2014 HEARTBEAT.custom_mode"));
    QCOMPARE(window->windowType(), Qt::Window);
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QCOMPARE(window->size(), QSize(800, 500));
    QCOMPARE(window->geometry().center(), owner.frameGeometry().center());

    QCustomPlot *const plot = window->findChild<QCustomPlot *>(
        QStringLiteral("mavlinkFieldGraphPlot"));
    QVERIFY(plot);
    QCOMPARE(plot->xAxis->label(),
             QStringLiteral("Time since graph opened (s)"));
    QCOMPARE(plot->yAxis->label(), QStringLiteral("Value"));
    QVERIFY(plot->interactions().testFlag(QCP::iRangeDrag));
    QVERIFY(plot->interactions().testFlag(QCP::iRangeZoom));
    QVERIFY(plot->legend->visible());

    auto *vehicleTitle = qobject_cast<QCPTextElement *>(
        plot->plotLayout()->element(0, 0));
    QVERIFY(vehicleTitle);
    QCOMPARE(vehicleTitle->text(),
             QStringLiteral("Vehicle 42, component 7"));

    QTimer *const timer = window->findChild<QTimer *>(
        QStringLiteral("mavlinkFieldGraphRefreshTimer"));
    QVERIFY(timer);
    QCOMPARE(timer->interval(), 100);
    QVERIFY(timer->isActive());

    QLabel *const status = window->findChild<QLabel *>(
        QStringLiteral("mavlinkFieldGraphSourceStatus"));
    QVERIFY(status);
    window->setSourceStatus(QStringLiteral("Live link 3"));
    QCOMPARE(status->text(), QStringLiteral("Live link 3"));

    window->show();
    QCoreApplication::processEvents();
    window->close();
    QTRY_VERIFY(guardedWindow.isNull());
}

void MavlinkFieldGraphWindowTest::
plotsOnlyMatchingTrafficWithBoundedHistory()
{
    QWidget owner;
    const MavlinkGraphSelection selection{
        9, 3, MAVLINK_MSG_ID_HEARTBEAT,
        QStringLiteral("HEARTBEAT"), QStringLiteral("custom_mode")};
    MavlinkFieldGraphWindow window(selection, 10, &owner);
    QCustomPlot *const plot = window.findChild<QCustomPlot *>(
        QStringLiteral("mavlinkFieldGraphPlot"));
    QVERIFY(plot);

    window.receiveMessage(heartbeat(8, 3, 99));
    window.receiveMessage(heartbeat(9, 2, 99));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "refreshPlot", Qt::DirectConnection));
    QCOMPARE(plot->graphCount(), 0);

    for (quint32 value = 0; value < 12; ++value) {
        window.receiveMessage(heartbeat(9, 3, value));
    }
    QVERIFY(QMetaObject::invokeMethod(
        &window, "refreshPlot", Qt::DirectConnection));
    QCOMPARE(plot->graphCount(), 1);
    QCOMPARE(plot->graph(0)->name(),
             QStringLiteral("HEARTBEAT.custom_mode"));
    QCOMPARE(plot->graph(0)->pen().color(),
             QColor(QStringLiteral("red")));
    QCOMPARE(plot->graph(0)->dataCount(), 10);
    QCOMPARE(plot->graph(0)->data()->constEnd()[-1].value, 11.0);

    window.clearSamples();
    QCOMPARE(plot->graph(0)->dataCount(), 0);
    QCOMPARE(plot->xAxis->range(), QCPRange(0.0, 10.0));
    QCOMPARE(plot->yAxis->range(), QCPRange(-1.0, 1.0));
}

void MavlinkFieldGraphWindowTest::
usesMissionPlannerPaletteForArraySeries()
{
    QWidget owner;
    const MavlinkGraphSelection selection{
        4, 5, MAVLINK_MSG_ID_GPS_RTCM_DATA,
        QStringLiteral("GPS_RTCM_DATA"), QStringLiteral("data")};
    MavlinkFieldGraphWindow window(selection, 10, &owner);
    QCustomPlot *const plot = window.findChild<QCustomPlot *>(
        QStringLiteral("mavlinkFieldGraphPlot"));
    QVERIFY(plot);

    quint8 data[180]{};
    for (quint8 index = 0; index < 6; ++index) {
        data[index] = index + 1;
    }
    mavlink_message_t message{};
    mavlink_msg_gps_rtcm_data_pack(4, 5, &message, 0, 6, data);
    window.receiveMessage(message);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "refreshPlot", Qt::DirectConnection));

    const QVector<QColor> palette{
        QColor(QStringLiteral("red")),
        QColor(QStringLiteral("green")),
        QColor(QStringLiteral("blue")),
        QColor(QStringLiteral("violet")),
        QColor(QStringLiteral("orange")),
        QColor(QStringLiteral("cyan")),
    };
    QVERIFY(plot->graphCount() >= palette.size());
    for (int index = 0; index < palette.size(); ++index) {
        QCOMPARE(plot->graph(index)->name(),
                 QStringLiteral("GPS_RTCM_DATA.data[%1]").arg(index));
        QCOMPARE(plot->graph(index)->pen().color(), palette.at(index));
    }
}

QTEST_MAIN(MavlinkFieldGraphWindowTest)
#include "test_mavlinkfieldgraphwindow.moc"
