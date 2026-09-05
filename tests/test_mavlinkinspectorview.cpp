#include "ui/MAVLinkInspectorView.h"
#include "ui/MAVLinkInspectorTrafficSource.h"
#include "ui/MAVLinkInspectorWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QTimer>
#include <QtTest>

namespace {
mavlink_message_t valueMessage(quint8 component, qint32 value)
{
    mavlink_message_t message{};
    const char name[10] = {'t', 'e', 's', 't'};
    mavlink_msg_named_value_int_pack(1, component, &message, 100, name, value);
    return message;
}

QTreeWidgetItem *fieldNamed(QTreeWidgetItem *message, const QString &name)
{
    for (int i = 0; i < message->childCount(); ++i)
        if (message->child(i)->text(0) == name) return message->child(i);
    return nullptr;
}
}

class MAVLinkInspectorViewTest final : public QObject
{
    Q_OBJECT
private slots:
    void completeOfflineSurfaceAndTrafficDisclosure();
    void componentTreeValuesAndStableSelection();
    void outboundTrafficCheckboxAndPauseGate();
    void replayDisablesSeparateOutboundTraffic();
    void graphSelectionAndSessionTokenGuard();
    void filterRemovesOldNodesAndRestoresCachedValues();
    void pauseClearAndSessionReset();
    void independentSourceWindowsAndLifetime();
    void ownedSourceTeardownDisconnectsUiCallbacks();
    void projectionRemainsBoundedAndSuppressesIntermediateSignals();
    void wireSizesIncludeVersionAndSignature();
};

void MAVLinkInspectorViewTest::completeOfflineSurfaceAndTrafficDisclosure()
{
    MAVLinkInspectorView view;
    QCOMPARE(view.objectName(), QStringLiteral("MAVLinkInspectorView"));
    QVERIFY(view.findChild<QPushButton *>("PauseButton")->isEnabled());
    QVERIFY(view.findChild<QPushButton *>("ClearButton")->isEnabled());
    QVERIFY(!view.findChild<QPushButton *>("GraphButton")->isEnabled());
    auto *traffic =
        view.findChild<QCheckBox *>("ShowGcsTrafficCheckBox");
    QVERIFY(!traffic->isEnabled());
    QVERIFY(!traffic->isChecked());
    QVERIFY(traffic->toolTip().contains(QStringLiteral("No MAVLink source")));
    const QString disclosure =
        view.findChild<QLabel *>("InspectorLimitations")->text();
    QVERIFY(disclosure.contains(QStringLiteral("incoming and outgoing")));
    QVERIFY(disclosure.contains(QStringLiteral("not delivery")));
    QVERIFY(view.findChild<QLabel *>("InspectorStatus")->text().contains("No MAVLink"));
    QCOMPARE(view.findChild<QTreeWidget *>("MessageTree")->topLevelItemCount(), 0);
}

void MAVLinkInspectorViewTest::outboundTrafficCheckboxAndPauseGate()
{
    QObject physicalLink;
    MAVLinkInspectorView view;
    auto *source = new MAVLinkInspectorTrafficSource;
    view.attachSource(source);
    auto *traffic =
        view.findChild<QCheckBox *>("ShowGcsTrafficCheckBox");
    QVERIFY(!traffic->isEnabled());
    QVERIFY(source->bindLive(&physicalLink, 8, 0,
                             QStringLiteral("Live")));
    QVERIFY(source->supportsOutboundTraffic());
    QVERIFY(traffic->isEnabled());
    QCOMPARE(source->activeToken(), quint64(0));
    source->beginLiveSession(&physicalLink, 8, 20);

    source->observeOutbound(&physicalLink, 8, 20, valueMessage(40, 1));
    QCOMPARE(view.packetStore().size(), 0);
    source->observeLive(&physicalLink, 8, 20, valueMessage(1, 2));
    QCOMPARE(view.packetStore().size(), 1);

    traffic->setChecked(true);
    source->observeOutbound(&physicalLink, 8, 20, valueMessage(40, 3));
    QCOMPARE(view.packetStore().size(), 2);

    traffic->setChecked(false);
    QCOMPARE(view.packetStore().size(), 2);
    source->observeOutbound(&physicalLink, 8, 20, valueMessage(41, 4));
    QCOMPARE(view.packetStore().size(), 2);

    traffic->setChecked(true);
    view.setPaused(true);
    source->observeOutbound(&physicalLink, 8, 20, valueMessage(41, 5));
    QCOMPARE(view.packetStore().size(), 2);
    view.setPaused(false);
    source->observeLive(&physicalLink, 8, 20, valueMessage(2, 6));
    QCOMPARE(view.packetStore().size(), 3);

    source->endLiveSession(8, 20);
    QVERIFY(traffic->isEnabled());
    QVERIFY(traffic->isChecked());
    source->removeLiveLink(8);
    QVERIFY(!traffic->isEnabled());
    QVERIFY(traffic->isChecked());
    QCOMPARE(view.packetStore().size(), 3);
}

void MAVLinkInspectorViewTest::replayDisablesSeparateOutboundTraffic()
{
    MAVLinkInspectorView view;
    auto *source = new MAVLinkInspectorTrafficSource;
    view.attachSource(source);
    auto *traffic =
        view.findChild<QCheckBox *>("ShowGcsTrafficCheckBox");
    QVERIFY(!traffic->isEnabled());

    QVERIFY(source->bindReplay(44, QStringLiteral("Replay")));
    QVERIFY(!source->supportsOutboundTraffic());
    QVERIFY(!traffic->isEnabled());
    QVERIFY(traffic->toolTip().contains(
        QStringLiteral("no separate outbound GCS traffic")));
}

void MAVLinkInspectorViewTest::graphSelectionAndSessionTokenGuard()
{
    QObject physicalLink;
    MAVLinkInspectorView view;
    auto *source = new MAVLinkInspectorTrafficSource;
    view.attachSource(source);
    QVERIFY(source->bindLive(&physicalLink, 9, 30,
                             QStringLiteral("Graph source")));
    source->observeLive(&physicalLink, 9, 30, valueMessage(1, 123));
    view.refreshView();

    auto *tree = view.findChild<QTreeWidget *>("MessageTree");
    auto *graph = view.findChild<QPushButton *>("GraphButton");
    QTreeWidgetItem *const message =
        tree->topLevelItem(0)->child(0)->child(0);
    QTreeWidgetItem *const textField = fieldNamed(message, "name");
    QTreeWidgetItem *const numericField = fieldNamed(message, "value");
    QVERIFY(textField && numericField);
    tree->setCurrentItem(message);
    QVERIFY(!graph->isEnabled());
    tree->setCurrentItem(textField);
    QVERIFY(!graph->isEnabled());
    tree->setCurrentItem(numericField);
    QVERIFY(graph->isEnabled());
    // Graph identity belongs to the item's typed roles, never to its
    // presentation text or tree path.
    numericField->setText(0, QStringLiteral("display text is not identity"));

    int requests = 0;
    int requestedHistory = 0;
    quint64 requestedToken = 0;
    MavlinkGraphSelection requestedSelection;
    connect(&view, &MAVLinkInspectorView::graphRequested, &view,
            [&](const MavlinkGraphSelection &selection, int history,
                quint64 token) {
        ++requests;
        requestedSelection = selection;
        requestedHistory = history;
        requestedToken = token;
    });

    bool promptVerified = false;
    QTimer::singleShot(0, &view, [&promptVerified]() {
        auto *dialog = qobject_cast<QInputDialog *>(
            QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        promptVerified = dialog->windowTitle() == QStringLiteral("MAVLink Graph")
            && dialog->labelText()
                == QStringLiteral("Points of history (10..100000)")
            && dialog->intValue() == 500 && dialog->intMinimum() == 10
            && dialog->intMaximum() == 100000;
        dialog->setIntValue(750);
        dialog->accept();
    });
    graph->click();
    QVERIFY(promptVerified);
    QCOMPARE(requests, 1);
    QCOMPARE(requestedHistory, 750);
    QCOMPARE(requestedToken, quint64(30));
    QCOMPARE(requestedSelection.systemId, quint8(1));
    QCOMPARE(requestedSelection.componentId, quint8(1));
    QCOMPARE(requestedSelection.messageId,
             quint32(MAVLINK_MSG_ID_NAMED_VALUE_INT));
    QCOMPARE(requestedSelection.messageName,
             QStringLiteral("NAMED_VALUE_INT"));
    QCOMPARE(requestedSelection.fieldName, QStringLiteral("value"));

    auto *filter = view.findChild<QLineEdit *>("MessageFilter");
    filter->setText(QStringLiteral("HEARTBEAT"));
    QVERIFY(!graph->isEnabled());
    filter->clear();
    QVERIFY(graph->isEnabled());

    QTimer::singleShot(0, &view, [&]() {
        source->endLiveSession(9, 30);
        source->beginLiveSession(&physicalLink, 9, 31);
        auto *dialog = qobject_cast<QInputDialog *>(
            QApplication::activeModalWidget());
        if (dialog) {
            dialog->accept();
        }
    });
    graph->click();
    QCOMPARE(requests, 1);
    QCOMPARE(source->activeToken(), quint64(31));
    QCOMPARE(tree->topLevelItemCount(), 0);
    QVERIFY(!graph->isEnabled());

    source->observeLive(&physicalLink, 9, 31, valueMessage(1, 456));
    view.refreshView();
    tree->setCurrentItem(fieldNamed(
        tree->topLevelItem(0)->child(0)->child(0), "value"));
    QVERIFY(graph->isEnabled());
    view.clearView();
    QVERIFY(!graph->isEnabled());
}

void MAVLinkInspectorViewTest::componentTreeValuesAndStableSelection()
{
    MAVLinkInspectorView view;
    view.receiveMessage(valueMessage(1, 2147483647));
    view.receiveMessage(valueMessage(42, -123));
    view.refreshView();
    auto *tree = view.findChild<QTreeWidget *>("MessageTree");
    QCOMPARE(tree->topLevelItemCount(), 1);
    auto *vehicle = tree->topLevelItem(0);
    QCOMPARE(vehicle->text(0), QStringLiteral("Vehicle 1"));
    QCOMPARE(vehicle->childCount(), 2);
    auto *message = vehicle->child(0)->child(0);
    QVERIFY(message->text(0).contains("NAMED_VALUE_INT"));
    QVERIFY(message->text(0).contains("Hz, #252"));
    auto *field = fieldNamed(message, QStringLiteral("value"));
    QVERIFY(field);
    QCOMPARE(field->text(1), QStringLiteral("2147483647"));
    QCOMPARE(fieldNamed(vehicle->child(1)->child(0), "value")->text(1),
             QStringLiteral("-123"));
    message->setExpanded(true);
    tree->setCurrentItem(field);
    view.receiveMessage(valueMessage(1, 1234567890));
    view.refreshView();
    QCOMPARE(tree->currentItem(), field);
    QVERIFY(message->isExpanded());
    QCOMPARE(field->text(1), QStringLiteral("1234567890"));
    QCOMPARE(fieldNamed(vehicle->child(1)->child(0), "value")->text(1),
             QStringLiteral("-123"));
}

void MAVLinkInspectorViewTest::filterRemovesOldNodesAndRestoresCachedValues()
{
    MAVLinkInspectorView view;
    view.receiveMessage(valueMessage(1, 5));
    mavlink_message_t heartbeat{};
    mavlink_msg_heartbeat_pack(2, 1, &heartbeat, MAV_TYPE_QUADROTOR,
                              MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, MAV_STATE_STANDBY);
    view.receiveMessage(heartbeat);
    view.refreshView();
    auto *tree = view.findChild<QTreeWidget *>("MessageTree");
    auto *filter = view.findChild<QLineEdit *>("MessageFilter");
    QCOMPARE(tree->topLevelItemCount(), 2);
    filter->setText(QStringLiteral("  nAmEd  "));
    QVERIFY(!tree->topLevelItem(0)->isHidden());
    QVERIFY(tree->topLevelItem(1)->isHidden());
    QCOMPARE(view.packetStore().size(), 2);
    filter->setText(QStringLiteral("not_a_message"));
    QVERIFY(tree->topLevelItem(0)->isHidden());
    QVERIFY(tree->topLevelItem(1)->isHidden());
    filter->clear();
    QVERIFY(!tree->topLevelItem(0)->isHidden());
    QVERIFY(!tree->topLevelItem(1)->isHidden());
    // Filter before ingest must also restore the hidden cached packet.
    view.clearView();
    filter->setText(QStringLiteral("HEARTBEAT"));
    view.receiveMessage(valueMessage(1, 6));
    view.refreshView();
    QCOMPARE(tree->topLevelItemCount(), 0);
    filter->clear();
    QCOMPARE(tree->topLevelItemCount(), 1);
}

void MAVLinkInspectorViewTest::pauseClearAndSessionReset()
{
    MAVLinkInspectorView view;
    QObject physicalLink;
    auto *source = new MAVLinkInspectorTrafficSource;
    view.attachSource(source);
    QVERIFY(source->bindLive(&physicalLink, 7, 10, QStringLiteral("USB")));
    source->observeLive(&physicalLink, 7, 10, valueMessage(1, 1));
    view.refreshView();
    auto *tree = view.findChild<QTreeWidget *>("MessageTree");
    auto *pause = view.findChild<QPushButton *>("PauseButton");
    pause->click();
    QVERIFY(view.isPaused());
    QCOMPARE(pause->text(), QStringLiteral("Resume"));
    source->observeLive(&physicalLink, 7, 10, valueMessage(42, 2));
    QCOMPARE(view.packetStore().size(), 1);
    source->endLiveSession(7, 10);
    QCOMPARE(tree->topLevelItemCount(), 1); // Freeze on disconnect.
    source->beginLiveSession(&physicalLink, 7, 11);
    QCOMPARE(tree->topLevelItemCount(), 0); // Never mix connection epochs.
    QVERIFY(view.isPaused());
    source->observeLive(&physicalLink, 7, 11, valueMessage(1, 3));
    QCOMPARE(view.packetStore().size(), 0);
    pause->click();
    source->observeLive(&physicalLink, 7, 11, valueMessage(1, 4));
    QCOMPARE(view.packetStore().size(), 1);
    view.findChild<QPushButton *>("ClearButton")->click();
    QCOMPARE(view.packetStore().size(), 0);
    QVERIFY(!view.isPaused());
    source->observeLive(&physicalLink, 7, 10, valueMessage(1, 5));
    QCOMPARE(view.packetStore().size(), 0);
}

void MAVLinkInspectorViewTest::independentSourceWindowsAndLifetime()
{
    QObject firstLink, secondLink;
    auto *first = new MAVLinkInspectorView;
    auto *second = new MAVLinkInspectorView;
    auto *firstSource = new MAVLinkInspectorTrafficSource;
    auto *secondSource = new MAVLinkInspectorTrafficSource;
    first->attachSource(firstSource);
    second->attachSource(secondSource);
    QVERIFY(firstSource->bindLive(&firstLink, 1, 1, "first"));
    QVERIFY(secondSource->bindLive(&secondLink, 2, 2, "second"));
    auto *firstWindow = new MAVLinkInspectorWindow(first);
    auto *secondWindow = new MAVLinkInspectorWindow(second);
    firstSource->observeLive(&secondLink, 2, 2, valueMessage(1, 7));
    secondSource->observeLive(&secondLink, 2, 2, valueMessage(1, 7));
    QCOMPARE(first->packetStore().size(), 0);
    QCOMPARE(second->packetStore().size(), 1);
    QPointer<MAVLinkInspectorTrafficSource> guarded(firstSource);
    delete firstWindow;
    QVERIFY(guarded.isNull());
    secondSource->observeLive(&secondLink, 2, 2, valueMessage(42, 8));
    QCOMPARE(second->packetStore().size(), 2);
    delete secondWindow;
}

void MAVLinkInspectorViewTest::ownedSourceTeardownDisconnectsUiCallbacks()
{
    QObject physicalLink;
    auto *view = new MAVLinkInspectorView;
    auto *source = new MAVLinkInspectorTrafficSource;
    view->attachSource(source);
    QVERIFY(source->bindLive(&physicalLink, 22, 3,
                             QStringLiteral("Owned source")));

    QPointer<MAVLinkInspectorTrafficSource> guardedSource(source);
    QPointer<QPushButton> guardedGraph(
        view->findChild<QPushButton *>("GraphButton"));
    QPointer<QCheckBox> guardedTraffic(
        view->findChild<QCheckBox *>("ShowGcsTrafficCheckBox"));
    QVERIFY(guardedGraph && guardedTraffic);

    delete view;
    QVERIFY(guardedSource.isNull());
    QVERIFY(guardedGraph.isNull());
    QVERIFY(guardedTraffic.isNull());
}

void MAVLinkInspectorViewTest::projectionRemainsBoundedAndSuppressesIntermediateSignals()
{
    MAVLinkInspectorView view;
    view.receiveMessage(valueMessage(1, 1));
    view.refreshView();
    auto *tree = view.findChild<QTreeWidget *>("MessageTree");
    auto *original = tree->topLevelItem(0)->child(0)->child(0);
    QSignalSpy changes(tree, &QTreeWidget::itemChanged);
    // Evict and reinsert the same key between two refreshes. Its per-entry
    // count may again equal one, so payload identity owns value refresh.
    for (quint32 i = 0; i < 4100; ++i) {
        mavlink_message_t message{};
        message.sysid = 1;
        message.compid = 1;
        message.msgid = 400000 + i;
        view.receiveMessage(message);
    }
    view.receiveMessage(valueMessage(1, 2));
    view.refreshView();
    QCOMPARE(view.packetStore().size(), 4096);
    QCOMPARE(tree->topLevelItem(0)->child(0)->childCount(), 4096);
    QCOMPARE(fieldNamed(original, "value")->text(1), QStringLiteral("2"));
    QCOMPARE(changes.count(), 0);
    view.clearView();
    QCOMPARE(tree->topLevelItemCount(), 0);
    QCOMPARE(changes.count(), 0);
}

void MAVLinkInspectorViewTest::wireSizesIncludeVersionAndSignature()
{
    MAVLinkInspectorView view;
    mavlink_message_t message{};
    message.sysid = 1;
    message.compid = 1;
    message.msgid = 0;
    message.len = 9;
    message.magic = MAVLINK_STX_MAVLINK1;
    view.receiveMessage(message);
    QCOMPARE(view.packetStore().snapshots(100000).first().latestWireBytes, quint32(17));
    message.magic = MAVLINK_STX;
    message.len = 3; // The wire size uses actual trimmed payload length.
    view.receiveMessage(message);
    QCOMPARE(view.packetStore().snapshots(100000).first().latestWireBytes, quint32(15));
    message.incompat_flags = MAVLINK_IFLAG_SIGNED;
    view.receiveMessage(message);
    QCOMPARE(view.packetStore().snapshots(100000).first().latestWireBytes, quint32(28));
}

QTEST_MAIN(MAVLinkInspectorViewTest)
#include "test_mavlinkinspectorview.moc"
