#include "InspectorRuntimeAudit.h"

#include "comm/LinkInterface.h"
#include "comm/LinkManager.h"
#include "comm/MAVLinkProtocol.h"
#include "ui/MAVLinkInspectorTrafficSource.h"
#include "ui/MAVLinkInspectorView.h"
#include "ui/MAVLinkInspectorWindow.h"
#include "ui/MainWindow.h"
#include "ui/MainWindowHeader.h"

#include <QAction>
#include <QComboBox>
#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QSignalBlocker>

#include <cstring>

namespace {

constexpr int FirstAuditLinkId = 900001;
constexpr int SecondAuditLinkId = 900002;

class AuditLink final : public LinkInterface
{
public:
    explicit AuditLink(int id)
        : m_id(id)
    {
    }

    void disableTimeouts() override {}
    void enableTimeouts() override {}
    int getId() const override { return m_id; }
    QString getName() const override
    {
        return QStringLiteral("Inspector Audit %1").arg(m_id);
    }
    QString getShortName() const override
    {
        return QStringLiteral("Audit %1").arg(m_id);
    }
    QString getDetail() const override
    {
        return QStringLiteral("in-process, no transport");
    }
    void requestReset() override {}
    bool isConnected() const override { return m_connected; }
    qint64 getConnectionSpeed() const override { return 0; }
    qint64 bytesAvailable() override { return 0; }
    LinkType getLinkType() override { return UNKNOWN_LINK; }

    bool connect() override
    {
        m_connected = true;
        return true;
    }

    bool disconnect() override
    {
        m_connected = false;
        return true;
    }

    void writeBytes(const char *, qint64) override {}

protected slots:
    void readBytes() override {}

private:
    int m_id = -1;
    bool m_connected = true;
};

class AuditResult final
{
public:
    void expect(bool condition, const QString &message)
    {
        if (condition) {
            return;
        }
        ++m_failures;
        qCritical().noquote()
            << QStringLiteral("MAVLink Inspector runtime audit: %1")
                   .arg(message);
    }

    int exitCode() const { return m_failures == 0 ? 0 : 1; }
    int failures() const { return m_failures; }

private:
    int m_failures = 0;
};

mavlink_message_t namedValueMessage(quint8 systemId, quint8 componentId,
                                    qint32 value)
{
    mavlink_message_t message{};
    char name[10] = {};
    std::memcpy(name, "audit", 5);
    mavlink_msg_named_value_int_pack(systemId, componentId, &message,
                                     100, name, value);
    return message;
}

QByteArray wireBytes(const mavlink_message_t &message)
{
    quint8 bytes[MAVLINK_MAX_PACKET_LEN] = {};
    const quint16 size = mavlink_msg_to_send_buffer(bytes, &message);
    return QByteArray(reinterpret_cast<const char *>(bytes), size);
}

MAVLinkInspectorWindow *newInspectorWindow(
    MainWindow *mainWindow,
    const QSet<MAVLinkInspectorWindow *> &existingWindows)
{
    const QList<MAVLinkInspectorWindow *> windows =
        mainWindow->findChildren<MAVLinkInspectorWindow *>();
    for (MAVLinkInspectorWindow *window : windows) {
        if (!existingWindows.contains(window)) {
            return window;
        }
    }
    return nullptr;
}

} // namespace

int RunInspectorRuntimeAudit()
{
    AuditResult result;
    LinkManager *const links = LinkManager::instance();
    MAVLinkProtocol *const protocol = links ? links->getProtocol() : nullptr;
    MainWindow *const mainWindow = MainWindow::instance();
    result.expect(links != nullptr, QStringLiteral("LinkManager is missing"));
    result.expect(protocol != nullptr,
                  QStringLiteral("production MAVLinkProtocol is missing"));
    result.expect(mainWindow != nullptr,
                  QStringLiteral("production MainWindow is missing"));
    if (!links || !protocol || !mainWindow) {
        return result.exitCode();
    }

    result.expect(links->getLink(FirstAuditLinkId) == nullptr,
                  QStringLiteral("first reserved audit link id is occupied"));
    result.expect(links->getLink(SecondAuditLinkId) == nullptr,
                  QStringLiteral("second reserved audit link id is occupied"));
    if (links->getLink(FirstAuditLinkId)
        || links->getLink(SecondAuditLinkId)) {
        return result.exitCode();
    }

    quint8 systemId = 250;
    while (systemId > 1 && links->getUas(systemId)) {
        --systemId;
    }
    result.expect(links->getUas(systemId) == nullptr,
                  QStringLiteral("no unused MAVLink system id is available"));

    MainWindowHeader &header = mainWindow->toolBar();
    QComboBox *const portCombo =
        header.findChild<QComboBox *>(QStringLiteral("portCombo"));
    const int previousSelectedLinkId = header.selectedLinkId();
    result.expect(portCombo != nullptr,
                  QStringLiteral("production header port selector is missing"));

    QPointer<AuditLink> firstLink(new AuditLink(FirstAuditLinkId));
    QPointer<AuditLink> secondLink(new AuditLink(SecondAuditLinkId));
    {
        // MainWindow's legacy Network-menu consumer of newLink is queued. Do
        // not leave callbacks which would create configuration widgets for
        // these already-deleted audit IDs after this function returns.
        const QSignalBlocker linkSignals(links);
        links->addLink(firstLink.data());
        links->addLink(secondLink.data());
    }
    const bool headerRefreshed = QMetaObject::invokeMethod(
        &header, "refreshLinks", Qt::DirectConnection);
    result.expect(headerRefreshed,
                  QStringLiteral("production header refresh could not run"));
    const quint64 firstEpoch =
        links->currentPhysicalLinkSession(FirstAuditLinkId);
    const quint64 secondEpoch =
        links->currentPhysicalLinkSession(SecondAuditLinkId);
    result.expect(firstEpoch != 0 && secondEpoch != 0
                      && firstEpoch != secondEpoch,
                  QStringLiteral("fake links did not receive distinct live epochs"));

    if (portCombo) {
        const int firstIndex = portCombo->findData(FirstAuditLinkId);
        result.expect(firstIndex >= 0,
                      QStringLiteral("first fake link is absent from header"));
        if (firstIndex >= 0) {
            portCombo->setCurrentIndex(firstIndex);
        }
        result.expect(header.selectedLinkId() == FirstAuditLinkId,
                      QStringLiteral("header did not select the pinned link"));
    }

    QObject connectionScope;
    int rawPacketCount = 0;
    int observedPacketCount = 0;
    int legacyPacketCount = 0;
    bool removeFirstLinkOnPacket = false;
    QObject::connect(
        protocol, &MAVLinkProtocol::packetReceived, &connectionScope,
        [&](LinkInterface *link, mavlink_message_t) {
            ++rawPacketCount;
            if (removeFirstLinkOnPacket && link
                && link->getId() == FirstAuditLinkId) {
                removeFirstLinkOnPacket = false;
                links->removeLink(FirstAuditLinkId);
            }
        });
    QObject::connect(
        links, &LinkManager::mavlinkMessageObserved, &connectionScope,
        [&](int, qulonglong, mavlink_message_t) {
            ++observedPacketCount;
        });
    QObject::connect(
        links, &LinkManager::messageReceived, &connectionScope,
        [&](LinkInterface *, mavlink_message_t) {
            ++legacyPacketCount;
        });

    QSet<MAVLinkInspectorWindow *> existingWindows;
    const QList<MAVLinkInspectorWindow *> before =
        mainWindow->findChildren<MAVLinkInspectorWindow *>();
    for (MAVLinkInspectorWindow *window : before) {
        existingWindows.insert(window);
    }
    QAction *const inspectorAction = mainWindow->findChild<QAction *>(
        QStringLiteral("actionMavlinkInspector"));
    result.expect(inspectorAction && inspectorAction->isEnabled(),
                  QStringLiteral("TOOLS MAVLink Inspector action is unavailable"));
    if (inspectorAction && inspectorAction->isEnabled()) {
        inspectorAction->trigger();
    }

    QPointer<MAVLinkInspectorWindow> inspectorWindow(
        newInspectorWindow(mainWindow, existingWindows));
    MAVLinkInspectorView *const inspector = inspectorWindow
        ? qobject_cast<MAVLinkInspectorView *>(
              inspectorWindow->inspectorView())
        : nullptr;
    MAVLinkInspectorTrafficSource *const source = inspector
        ? inspector->findChild<MAVLinkInspectorTrafficSource *>() : nullptr;
    result.expect(inspectorWindow && inspector,
                  QStringLiteral("TOOLS action did not create the production view"));
    result.expect(source != nullptr,
                  QStringLiteral("production view has no pinned traffic source"));
    if (source) {
        result.expect(source->status().contains(
                          QString::number(FirstAuditLinkId)),
                      QStringLiteral("TOOLS route did not bind the selected link"));
    }

    if (firstLink && inspector) {
        protocol->receiveBytes(
            firstLink.data(),
            wireBytes(namedValueMessage(systemId, 1, 101)));
        protocol->receiveBytes(
            firstLink.data(),
            wireBytes(namedValueMessage(systemId, 42, 142)));

        const MAVLinkInspectorPacketStore &store = inspector->packetStore();
        result.expect(store.contains(
                          {systemId, 1, MAVLINK_MSG_ID_NAMED_VALUE_INT}),
                      QStringLiteral("component 1 pre-heartbeat packet was lost"));
        result.expect(store.contains(
                          {systemId, 42, MAVLINK_MSG_ID_NAMED_VALUE_INT}),
                      QStringLiteral("component 42 pre-heartbeat packet was lost"));
        result.expect(store.size() == 2,
                      QStringLiteral("exact component keys were not preserved"));
    }
    result.expect(links->getUas(systemId) == nullptr,
                  QStringLiteral("non-heartbeat diagnostics created a UAS"));
    result.expect(rawPacketCount == 2 && observedPacketCount == 2,
                  QStringLiteral("pre-heartbeat packets missed raw/epoch ingress"));
    result.expect(legacyPacketCount == 0,
                  QStringLiteral("legacy UAS-gated signal leaked pre-heartbeat packets"));

    if (secondLink && inspector) {
        protocol->receiveBytes(
            secondLink.data(),
            wireBytes(namedValueMessage(systemId, 77, 177)));
        result.expect(!inspector->packetStore().contains(
                          {systemId, 77, MAVLINK_MSG_ID_NAMED_VALUE_INT}),
                      QStringLiteral("packet from the wrong physical link leaked in"));
        result.expect(inspector->packetStore().size() == 2,
                      QStringLiteral("wrong-link packet changed the pinned store"));
    }
    result.expect(rawPacketCount == 3 && observedPacketCount == 3,
                  QStringLiteral("wrong-link packet did not traverse shared ingress"));

    if (firstLink && inspector) {
        const QByteArray staleBatch =
            wireBytes(namedValueMessage(systemId, 100, 200))
            + wireBytes(namedValueMessage(systemId, 101, 201));
        removeFirstLinkOnPacket = true;
        protocol->receiveBytes(firstLink.data(), staleBatch);
        result.expect(!removeFirstLinkOnPacket,
                      QStringLiteral("reentrant removal hook did not run"));
        result.expect(firstLink.isNull()
                          && links->getLink(FirstAuditLinkId) == nullptr,
                      QStringLiteral("reentrant link removal did not complete"));
        result.expect(
            links->currentPhysicalLinkSession(FirstAuditLinkId) == 0,
            QStringLiteral("removed link retained a physical session"));
        result.expect(inspector->packetStore().contains(
                          {systemId, 100, MAVLINK_MSG_ID_NAMED_VALUE_INT}),
                      QStringLiteral("first packet before removal was not delivered"));
        result.expect(!inspector->packetStore().contains(
                          {systemId, 101, MAVLINK_MSG_ID_NAMED_VALUE_INT}),
                      QStringLiteral("stale second packet survived session removal"));
        result.expect(inspector->packetStore().size() == 3,
                      QStringLiteral("stale batch changed the store unexpectedly"));
    }
    result.expect(rawPacketCount == 4 && observedPacketCount == 4,
                  QStringLiteral("parser did not stop at the removed-link boundary"));
    result.expect(legacyPacketCount == 0,
                  QStringLiteral("stale batch escaped through the legacy signal"));
    if (portCombo) {
        result.expect(header.selectedLinkId() != FirstAuditLinkId,
                      QStringLiteral("header retained the removed link id"));
    }

    delete inspectorWindow.data();
    inspectorWindow.clear();
    if (links->getLink(FirstAuditLinkId)) {
        links->removeLink(FirstAuditLinkId);
    }
    if (links->getLink(SecondAuditLinkId)) {
        links->removeLink(SecondAuditLinkId);
    }
    if (portCombo) {
        const int previousIndex = portCombo->findData(previousSelectedLinkId);
        portCombo->setCurrentIndex(previousIndex);
    }

    result.expect(links->getLink(FirstAuditLinkId) == nullptr
                      && links->getLink(SecondAuditLinkId) == nullptr,
                  QStringLiteral("audit links survived cleanup"));
    if (result.failures() == 0) {
        qInfo().noquote()
            << QStringLiteral("MAVLink Inspector runtime audit: production "
                              "pre-heartbeat exact-link path passed");
    }
    return result.exitCode();
}
