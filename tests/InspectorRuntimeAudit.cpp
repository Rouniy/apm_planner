#include "InspectorRuntimeAudit.h"

#include "comm/ExactLinkTransmitter.h"
#include "comm/LinkInterface.h"
#include "comm/LinkManager.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/MAVLinkProtocol.h"
#include "ui/MAVLinkInspectorTrafficSource.h"
#include "ui/MAVLinkInspectorView.h"
#include "ui/MAVLinkInspectorWindow.h"
#include "ui/MainWindow.h"
#include "ui/MainWindowHeader.h"
#include "ui/MavlinkFieldGraphWindow.h"

#include <QAction>
#include <QCheckBox>
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

    void writeBytes(const char *bytes, qint64 size) override
    {
        if (bytes && size > 0) {
            m_writes.append(QByteArray(bytes, static_cast<int>(size)));
        }
    }

    const QVector<QByteArray> &writes() const { return m_writes; }

protected slots:
    void readBytes() override {}

private:
    int m_id = -1;
    bool m_connected = true;
    QVector<QByteArray> m_writes;
};

struct SubmittedMessage
{
    int linkId = -1;
    quint64 epoch = 0;
    mavlink_message_t message{};
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

mavlink_message_t mavlink1NamedValueMessage(
    quint8 systemId, quint8 componentId, qint32 value)
{
    mavlink_message_t message =
        namedValueMessage(systemId, componentId, value);
    mavlink_status_t status{};
    status.flags = MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    mavlink_finalize_message_buffer(
        &message, systemId, componentId, &status,
        MAVLINK_MSG_ID_NAMED_VALUE_INT_MIN_LEN,
        MAVLINK_MSG_ID_NAMED_VALUE_INT_LEN,
        MAVLINK_MSG_ID_NAMED_VALUE_INT_CRC);
    return message;
}

QByteArray wireBytes(const mavlink_message_t &message)
{
    quint8 bytes[MAVLINK_MAX_PACKET_LEN] = {};
    const quint16 size = mavlink_msg_to_send_buffer(bytes, &message);
    return QByteArray(reinterpret_cast<const char *>(bytes), size);
}

bool decodeFrame(const QByteArray &bytes, mavlink_message_t *message)
{
    if (!message) {
        return false;
    }
    MAVLinkFrameParser parser;
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), message);
    }
    return state == MAVLINK_FRAMING_OK;
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

MavlinkFieldGraphWindow *newGraphWindow(
    MainWindow *mainWindow,
    const QSet<MavlinkFieldGraphWindow *> &existingWindows)
{
    const QList<MavlinkFieldGraphWindow *> windows =
        mainWindow->findChildren<MavlinkFieldGraphWindow *>();
    for (MavlinkFieldGraphWindow *window : windows) {
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
    QVector<SubmittedMessage> submittedMessages;
    bool removeFirstLinkOnPacket = false;
    QObject::connect(
        protocol, &MAVLinkProtocol::packetReceived, &connectionScope,
        [&](LinkInterface *link, mavlink_message_t) {
            if (!link || (link->getId() != FirstAuditLinkId
                          && link->getId() != SecondAuditLinkId)) {
                return;
            }
            ++rawPacketCount;
            if (removeFirstLinkOnPacket && link
                && link->getId() == FirstAuditLinkId) {
                removeFirstLinkOnPacket = false;
                links->removeLink(FirstAuditLinkId);
            }
        });
    QObject::connect(
        links, &LinkManager::mavlinkMessageObserved, &connectionScope,
        [&](int id, qulonglong, mavlink_message_t) {
            if (id == FirstAuditLinkId || id == SecondAuditLinkId) {
                ++observedPacketCount;
            }
        });
    QObject::connect(
        links, &LinkManager::messageReceived, &connectionScope,
        [&](LinkInterface *link, mavlink_message_t) {
            if (link && (link->getId() == FirstAuditLinkId
                         || link->getId() == SecondAuditLinkId)) {
                ++legacyPacketCount;
            }
        });
    QObject::connect(
        links, &LinkManager::mavlinkMessageSubmitted, &connectionScope,
        [&](int id, qulonglong epoch, mavlink_message_t message) {
            if (id == FirstAuditLinkId || id == SecondAuditLinkId) {
                submittedMessages.append({id, epoch, message});
            }
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
        const int writeBaseline = secondLink->writes().size();
        const int submissionBaseline = submittedMessages.size();
        protocol->receiveBytes(
            secondLink.data(),
            wireBytes(mavlink1NamedValueMessage(systemId, 77, 177)));
        result.expect(!inspector->packetStore().contains(
                          {systemId, 77, MAVLINK_MSG_ID_NAMED_VALUE_INT}),
                      QStringLiteral("packet from the wrong physical link leaked in"));
        result.expect(inspector->packetStore().size() == 2,
                      QStringLiteral("wrong-link packet changed the pinned store"));
        result.expect(secondLink->writes().size() == writeBaseline + 2,
                      QStringLiteral("MAVLink 1 negotiation did not submit two requests"));
        result.expect(submittedMessages.size() == submissionBaseline + 2,
                      QStringLiteral("typed negotiation writes were not observed exactly once"));

        const quint16 expectedCommands[] = {
            MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES,
            MAV_CMD_REQUEST_MESSAGE,
        };
        for (int offset = 0; offset < 2; ++offset) {
            if (writeBaseline + offset >= secondLink->writes().size()) {
                continue;
            }
            mavlink_message_t written{};
            const bool decoded = decodeFrame(
                secondLink->writes().at(writeBaseline + offset), &written);
            result.expect(decoded,
                          QStringLiteral("negotiation write %1 is not a MAVLink frame")
                              .arg(offset));
            if (!decoded) {
                continue;
            }
            mavlink_command_long_t command{};
            mavlink_msg_command_long_decode(&written, &command);
            result.expect(
                written.msgid == MAVLINK_MSG_ID_COMMAND_LONG
                    && written.sysid == protocol->systemId()
                    && written.compid == QGC::defaultComponentId,
                QStringLiteral("negotiation request %1 has the wrong local source")
                    .arg(offset));
            result.expect(
                command.target_system == systemId
                    && command.target_component == 77
                    && command.command == expectedCommands[offset],
                QStringLiteral("negotiation request %1 has the wrong target/command")
                    .arg(offset));
            const float expectedParam = offset == 0
                ? 1.0F : static_cast<float>(MAVLINK_MSG_ID_AUTOPILOT_VERSION);
            result.expect(command.param1 == expectedParam,
                          QStringLiteral("negotiation request %1 has the wrong param1")
                              .arg(offset));
            result.expect(
                command.param2 == 0.0F && command.param3 == 0.0F
                    && command.param4 == 0.0F && command.param5 == 0.0F
                    && command.param6 == 0.0F && command.param7 == 0.0F
                    && command.confirmation == 0,
                QStringLiteral("negotiation request %1 retained uninitialized fields")
                    .arg(offset));
            if (submissionBaseline + offset < submittedMessages.size()) {
                const SubmittedMessage &submitted =
                    submittedMessages.at(submissionBaseline + offset);
                result.expect(
                    submitted.linkId == SecondAuditLinkId
                        && submitted.epoch == secondEpoch
                        && submitted.message.msgid == written.msgid
                        && submitted.message.sysid == written.sysid
                        && submitted.message.compid == written.compid
                        && submitted.message.checksum == written.checksum,
                    QStringLiteral("negotiation request %1 was not observed exactly")
                        .arg(offset));
            }
        }
    }
    result.expect(rawPacketCount == 3 && observedPacketCount == 3,
                  QStringLiteral("wrong-link packet did not traverse shared ingress"));

    QPointer<MavlinkFieldGraphWindow> graphWindow;
    if (firstLink && secondLink && inspectorAction) {
        QSet<MAVLinkInspectorWindow *> currentWindows;
        const auto currentInspectorWindows =
            mainWindow->findChildren<MAVLinkInspectorWindow *>();
        for (MAVLinkInspectorWindow *window : currentInspectorWindows) {
            currentWindows.insert(window);
        }
        inspectorAction->trigger();
        QPointer<MAVLinkInspectorWindow> temporaryWindow(
            newInspectorWindow(mainWindow, currentWindows));
        QPointer<MAVLinkInspectorView> temporaryInspector(
            temporaryWindow
                ? qobject_cast<MAVLinkInspectorView *>(
                      temporaryWindow->inspectorView())
                : nullptr);
        MAVLinkInspectorTrafficSource *const temporarySource =
            temporaryInspector
                ? temporaryInspector->findChild<
                      MAVLinkInspectorTrafficSource *>()
                : nullptr;
        QCheckBox *const showGcsTraffic = temporaryInspector
            ? temporaryInspector->findChild<QCheckBox *>(
                  QStringLiteral("ShowGcsTrafficCheckBox"))
            : nullptr;
        result.expect(temporaryWindow && temporaryInspector
                          && temporarySource && showGcsTraffic,
                      QStringLiteral("second production Inspector route is incomplete"));
        if (temporarySource) {
            result.expect(temporarySource->activeToken() == firstEpoch,
                          QStringLiteral("second Inspector did not retain the link epoch"));
        }

        ExactLinkTransmitter *const transmitter =
            links->exactLinkTransmitter();
        result.expect(transmitter != nullptr,
                      QStringLiteral("production exact transmitter is missing"));
        if (transmitter && temporaryInspector) {
            const int writeBaseline = firstLink->writes().size();
            const int submissionBaseline = submittedMessages.size();
            const auto sendResult = transmitter->sendMessage(
                FirstAuditLinkId, 210, 211,
                namedValueMessage(1, 2, 310));
            result.expect(
                sendResult == ExactLinkTransmitter::SendResult::Sent
                    && firstLink->writes().size() == writeBaseline + 1
                    && submittedMessages.size() == submissionBaseline + 1,
                QStringLiteral("exact transmitter was not observed exactly once"));
            result.expect(!temporaryInspector->packetStore().contains(
                              {210, 211,
                               MAVLINK_MSG_ID_NAMED_VALUE_INT}),
                          QStringLiteral("unchecked Show GCS admitted outbound traffic"));
            if (submissionBaseline < submittedMessages.size()) {
                const SubmittedMessage &submitted =
                    submittedMessages.at(submissionBaseline);
                mavlink_message_t written{};
                const bool decoded = writeBaseline < firstLink->writes().size()
                    && decodeFrame(firstLink->writes().at(writeBaseline),
                                   &written);
                result.expect(
                    submitted.linkId == FirstAuditLinkId
                        && submitted.epoch == firstEpoch
                        && submitted.message.sysid == 210
                        && submitted.message.compid == 211
                        && submitted.message.seq == 0
                        && decoded
                        && submitted.message.checksum == written.checksum,
                    QStringLiteral("exact transmitter event was not the finalized frame"));
            }
        }

        if (showGcsTraffic && temporaryInspector) {
            showGcsTraffic->setChecked(true);
            const mavlink_message_t typed =
                namedValueMessage(212, 213, 313);
            const int writeBaseline = firstLink->writes().size();
            const int submissionBaseline = submittedMessages.size();
            result.expect(links->writeMavlinkMessage(firstLink.data(), typed),
                          QStringLiteral("typed MAVLink helper rejected a live link"));
            result.expect(
                firstLink->writes().size() == writeBaseline + 1
                    && submittedMessages.size() == submissionBaseline + 1,
                QStringLiteral("typed MAVLink helper was not observed exactly once"));
            if (submissionBaseline < submittedMessages.size()) {
                const SubmittedMessage &submitted =
                    submittedMessages.at(submissionBaseline);
                mavlink_message_t written{};
                const QByteArray writtenFrame =
                    writeBaseline < firstLink->writes().size()
                    ? firstLink->writes().at(writeBaseline)
                    : QByteArray();
                const bool decoded =
                    decodeFrame(writtenFrame, &written);
                result.expect(
                    submitted.linkId == FirstAuditLinkId
                        && submitted.epoch == firstEpoch
                        && decoded
                        && written.msgid == typed.msgid
                        && written.sysid == typed.sysid
                        && written.compid == typed.compid
                        && submitted.message.seq == written.seq
                        && wireBytes(submitted.message) == writtenFrame,
                    QStringLiteral("typed MAVLink event did not preserve the finalized wire frame"));
            }
            result.expect(temporaryInspector->packetStore().contains(
                              {212, 213,
                               MAVLINK_MSG_ID_NAMED_VALUE_INT}),
                          QStringLiteral("checked Show GCS missed typed outbound traffic"));

            const int rawWriteBaseline = firstLink->writes().size();
            const int rawSubmissionBaseline = submittedMessages.size();
            result.expect(links->writeRawBytes(
                              FirstAuditLinkId,
                              QByteArrayLiteral("inspector-audit-raw")),
                          QStringLiteral("raw write helper rejected a live link"));
            result.expect(
                firstLink->writes().size() == rawWriteBaseline + 1
                    && submittedMessages.size() == rawSubmissionBaseline,
                QStringLiteral("raw bytes leaked into MAVLink submission events"));

            const mavlink_message_t wrongLink =
                namedValueMessage(214, 215, 315);
            const int wrongSubmissionBaseline = submittedMessages.size();
            result.expect(links->writeMavlinkMessage(
                              secondLink.data(), wrongLink),
                          QStringLiteral("wrong-link typed audit write failed"));
            result.expect(submittedMessages.size()
                              == wrongSubmissionBaseline + 1,
                          QStringLiteral("wrong-link submission was not observable"));
            result.expect(!temporaryInspector->packetStore().contains(
                              {214, 215,
                               MAVLINK_MSG_ID_NAMED_VALUE_INT}),
                          QStringLiteral("wrong-link outbound packet leaked into source"));

            showGcsTraffic->setChecked(false);
            const mavlink_message_t hidden =
                namedValueMessage(216, 217, 317);
            result.expect(links->writeMavlinkMessage(
                              firstLink.data(), hidden),
                          QStringLiteral("post-toggle typed audit write failed"));
            result.expect(!temporaryInspector->packetStore().contains(
                              {216, 217,
                               MAVLINK_MSG_ID_NAMED_VALUE_INT}),
                          QStringLiteral("disabled Show GCS retained outbound traffic"));
        }

        if (temporaryInspector && temporarySource) {
            QSet<MavlinkFieldGraphWindow *> currentGraphs;
            const auto graphs =
                mainWindow->findChildren<MavlinkFieldGraphWindow *>();
            for (MavlinkFieldGraphWindow *graph : graphs) {
                currentGraphs.insert(graph);
            }
            const MavlinkGraphSelection selection{
                systemId, 42, MAVLINK_MSG_ID_NAMED_VALUE_INT,
                QStringLiteral("NAMED_VALUE_INT"),
                QStringLiteral("value")};
            emit temporaryInspector->graphRequested(
                selection, 10, temporarySource->activeToken());
            graphWindow = newGraphWindow(mainWindow, currentGraphs);
            result.expect(graphWindow
                              && graphWindow->parentWidget() == mainWindow,
                          QStringLiteral("Graph It did not create a MainWindow-owned graph"));
        }

        delete temporaryWindow.data();
        temporaryWindow.clear();
        result.expect(temporaryInspector.isNull(),
                      QStringLiteral("launching Inspector survived deletion"));
        result.expect(!graphWindow.isNull(),
                      QStringLiteral("graph lifetime depended on Inspector"));

        if (graphWindow && firstLink) {
            protocol->receiveBytes(
                firstLink.data(),
                wireBytes(namedValueMessage(systemId, 42, 4242)));
            result.expect(graphWindow->model().seriesCount() == 1
                              && graphWindow->model().storedPointCount(0) == 1,
                          QStringLiteral("independent graph missed incoming traffic"));
            MavlinkGraphPoint latest;
            result.expect(graphWindow->model().latestValue(0, &latest)
                              && latest.value == 4242.0,
                          QStringLiteral("independent graph decoded incoming value incorrectly"));

            const int submissionBaseline = submittedMessages.size();
            result.expect(links->writeMavlinkMessage(
                              firstLink.data(),
                              namedValueMessage(systemId, 42, 4343)),
                          QStringLiteral("graph outbound audit write failed"));
            result.expect(submittedMessages.size()
                              == submissionBaseline + 1,
                          QStringLiteral("graph outbound write was not observed"));
            result.expect(graphWindow->model().storedPointCount(0) == 2
                              && graphWindow->model().latestValue(0, &latest)
                              && latest.value == 4343.0,
                          QStringLiteral("independent graph missed outbound traffic"));
        }
    }

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
    result.expect(rawPacketCount == 5 && observedPacketCount == 5,
                  QStringLiteral("parser did not stop at the removed-link boundary"));
    result.expect(legacyPacketCount == 0,
                  QStringLiteral("stale batch escaped through the legacy signal"));
    if (portCombo) {
        result.expect(header.selectedLinkId() != FirstAuditLinkId,
                      QStringLiteral("header retained the removed link id"));
    }

    delete inspectorWindow.data();
    inspectorWindow.clear();
    delete graphWindow.data();
    graphWindow.clear();
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
