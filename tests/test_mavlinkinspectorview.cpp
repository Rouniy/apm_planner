#include "ui/MAVLinkInspectorView.h"
#include "ui/MAVLinkInspectorTrafficSource.h"
#include "ui/MAVLinkInspectorWindow.h"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
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
    void completeOfflineSurfaceAndExplicitLimitations();
    void componentTreeValuesAndStableSelection();
    void filterRemovesOldNodesAndRestoresCachedValues();
    void pauseClearAndSessionReset();
    void independentSourceWindowsAndLifetime();
    void projectionRemainsBoundedAndSuppressesIntermediateSignals();
    void wireSizesIncludeVersionAndSignature();
};

void MAVLinkInspectorViewTest::completeOfflineSurfaceAndExplicitLimitations()
{
    MAVLinkInspectorView view;
    QCOMPARE(view.objectName(), QStringLiteral("MAVLinkInspectorView"));
    QVERIFY(view.findChild<QPushButton *>("PauseButton")->isEnabled());
    QVERIFY(view.findChild<QPushButton *>("ClearButton")->isEnabled());
    QVERIFY(!view.findChild<QPushButton *>("GraphButton")->isEnabled());
    QVERIFY(!view.findChild<QCheckBox *>("ShowGcsTrafficCheckBox")->isEnabled());
    QVERIFY(view.findChild<QLabel *>("InspectorLimitations")->text().contains("not yet"));
    QVERIFY(view.findChild<QLabel *>("InspectorStatus")->text().contains("No MAVLink"));
    QCOMPARE(view.findChild<QTreeWidget *>("MessageTree")->topLevelItemCount(), 0);
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
