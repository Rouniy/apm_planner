#include <QtTest>

#include "ui/MAVLinkInspectorMessageRelay.h"
#include "ui/MAVLinkInspectorWindow.h"

#include <QDialog>
#include <QPointer>
#include <QWidget>

class MAVLinkInspectorWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void windowOwnsIndependentModelessView();
    void relayTracksMultipleInspectorLifetimes();
};

void MAVLinkInspectorWindowTest::windowOwnsIndependentModelessView()
{
    auto *firstView = new QWidget;
    auto *secondView = new QWidget;
    firstView->setObjectName(QStringLiteral("FirstInspectorView"));
    secondView->setObjectName(QStringLiteral("SecondInspectorView"));

    auto *firstWindow = new MAVLinkInspectorWindow(firstView);
    auto *secondWindow = new MAVLinkInspectorWindow(secondView);
    QPointer<MAVLinkInspectorWindow> guardedFirst(firstWindow);

    QCOMPARE(firstWindow->objectName(),
             QStringLiteral("MAVLinkInspectorWindow"));
    QCOMPARE(firstWindow->windowTitle(), QStringLiteral("Mavlink Inspector"));
    QCOMPARE(firstWindow->windowType(), Qt::Window);
    QCOMPARE(firstWindow->windowModality(), Qt::NonModal);
    QVERIFY(qobject_cast<QDialog *>(firstWindow) == nullptr);
    QVERIFY(firstWindow->testAttribute(Qt::WA_DeleteOnClose));
    QCOMPARE(firstWindow->inspectorView(), firstView);
    QCOMPARE(secondWindow->inspectorView(), secondView);
    QCOMPARE(firstView->parentWidget(), firstWindow);
    QCOMPARE(secondView->parentWidget(), secondWindow);
    QVERIFY(firstWindow != secondWindow);

    firstWindow->show();
    secondWindow->show();
    QCoreApplication::processEvents();
    QVERIFY(firstWindow->isWindow());
    QVERIFY(secondWindow->isWindow());
    QVERIFY(firstWindow->isVisible());
    QVERIFY(secondWindow->isVisible());

    firstWindow->close();
    QTRY_VERIFY(guardedFirst.isNull());
    QVERIFY(secondWindow->isVisible());
    delete secondWindow;
}

void MAVLinkInspectorWindowTest::relayTracksMultipleInspectorLifetimes()
{
    MAVLinkInspectorMessageRelay relay;
    QObject first;
    auto *second = new QObject;
    int firstCalls = 0;
    int secondCalls = 0;
    quint32 lastMessageId = 0;

    relay.subscribe(
        &first,
        [&firstCalls, &lastMessageId](LinkInterface *,
                                      const mavlink_message_t &message) {
            ++firstCalls;
            lastMessageId = message.msgid;
        });
    relay.subscribe(
        second,
        [&secondCalls](LinkInterface *, const mavlink_message_t &) {
            ++secondCalls;
        });
    QCOMPARE(relay.subscriberCount(), 2);

    mavlink_message_t message{};
    message.msgid = 330;
    relay.publish(nullptr, message);
    QCOMPARE(firstCalls, 1);
    QCOMPARE(secondCalls, 1);
    QCOMPARE(lastMessageId, quint32(330));

    // Re-subscribing the same receiver replaces its callback instead of
    // duplicating deliveries.
    relay.subscribe(
        &first,
        [&firstCalls](LinkInterface *, const mavlink_message_t &) {
            firstCalls += 10;
        });
    QCOMPARE(relay.subscriberCount(), 2);
    relay.publish(nullptr, message);
    QCOMPARE(firstCalls, 11);
    QCOMPARE(secondCalls, 2);

    delete second;
    QCoreApplication::processEvents();
    QCOMPARE(relay.subscriberCount(), 1);
    relay.publish(nullptr, message);
    QCOMPARE(firstCalls, 21);
    QCOMPARE(secondCalls, 2);

    // A callback may close another inspector while a packet is being
    // multicast. The guarded snapshot must not call the destroyed receiver.
    auto *closing = new QObject;
    int closingCalls = 0;
    relay.subscribe(
        closing,
        [&closingCalls](LinkInterface *, const mavlink_message_t &) {
            ++closingCalls;
        });
    relay.subscribe(
        &first,
        [&firstCalls, &closing](LinkInterface *,
                                const mavlink_message_t &) {
            ++firstCalls;
            delete closing;
            closing = nullptr;
        });
    relay.publish(nullptr, message);
    QCOMPARE(firstCalls, 22);
    QCOMPARE(closingCalls, 0);
    QCOMPARE(relay.subscriberCount(), 1);

    relay.unsubscribe(&first);
    QCOMPARE(relay.subscriberCount(), 0);
}

QTEST_MAIN(MAVLinkInspectorWindowTest)
#include "test_mavlinkinspectorwindow.moc"
