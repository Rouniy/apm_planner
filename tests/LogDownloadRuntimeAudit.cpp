#include "LogDownloadRuntimeAudit.h"

#include "comm/ExactLogTransferService.h"
#include "comm/LinkManager.h"
#include "comm/LinkManagerFactory.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/UDPLink.h"
#include "uas/UASInterface.h"
#include "uas/UASManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QPointer>
#include <QThread>
#include <QUdpSocket>

#include <cstring>
#include <functional>

namespace {

constexpr int WaitTimeoutMs = 3000;

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
            << QStringLiteral("Download Logs UDP runtime audit: %1")
                   .arg(message);
    }

    int exitCode() const { return m_failures == 0 ? 0 : 1; }
    int failures() const { return m_failures; }

private:
    int m_failures = 0;
};

bool waitUntil(const std::function<bool()> &predicate,
               int timeoutMs = WaitTimeoutMs, bool processEvents = true)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        if (processEvents) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        QThread::msleep(2);
    }
    if (processEvents) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return predicate();
}

void processFor(int durationMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < durationMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
}

quint16 reserveLocalPort()
{
    QUdpSocket probe;
    if (!probe.bind(QHostAddress(QHostAddress::LocalHost), quint16(0))) {
        return 0;
    }
    return probe.localPort();
}

QByteArray wireBytes(const mavlink_message_t &message)
{
    quint8 bytes[MAVLINK_MAX_PACKET_LEN]{};
    const quint16 size = mavlink_msg_to_send_buffer(bytes, &message);
    return QByteArray(reinterpret_cast<const char *>(bytes), size);
}

void forceMavlink2(mavlink_message_t *message, quint8 systemId,
                   quint8 componentId, quint8 minimumLength,
                   quint8 maximumLength, quint8 crcExtra)
{
    mavlink_status_t status{};
    mavlink_finalize_message_buffer(message, systemId, componentId, &status,
                                    minimumLength, maximumLength, crcExtra);
}

mavlink_message_t namedValueMessage(quint8 systemId, quint8 componentId,
                                    qint32 value)
{
    mavlink_message_t message{};
    char name[10]{};
    std::memcpy(name, "udp-audit", 9);
    mavlink_msg_named_value_int_pack(systemId, componentId, &message,
                                     100, name, value);
    forceMavlink2(&message, systemId, componentId,
                  MAVLINK_MSG_ID_NAMED_VALUE_INT_MIN_LEN,
                  MAVLINK_MSG_ID_NAMED_VALUE_INT_LEN,
                  MAVLINK_MSG_ID_NAMED_VALUE_INT_CRC);
    return message;
}

mavlink_message_t heartbeatMessage(quint8 systemId, quint8 componentId)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, componentId, &message, MAV_TYPE_QUADROTOR,
        MAV_AUTOPILOT_ARDUPILOTMEGA, MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
        0, MAV_STATE_STANDBY);
    forceMavlink2(&message, systemId, componentId,
                  MAVLINK_MSG_ID_HEARTBEAT_MIN_LEN,
                  MAVLINK_MSG_ID_HEARTBEAT_LEN,
                  MAVLINK_MSG_ID_HEARTBEAT_CRC);
    return message;
}

mavlink_message_t logEntryMessage(quint8 systemId, quint8 componentId)
{
    mavlink_message_t message{};
    mavlink_msg_log_entry_pack(systemId, componentId, &message,
                               7, 1, 7, 0, 1234);
    forceMavlink2(&message, systemId, componentId,
                  MAVLINK_MSG_ID_LOG_ENTRY_MIN_LEN,
                  MAVLINK_MSG_ID_LOG_ENTRY_LEN,
                  MAVLINK_MSG_ID_LOG_ENTRY_CRC);
    return message;
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

bool sendFrame(QUdpSocket *sender, quint16 destinationPort,
               const mavlink_message_t &message)
{
    if (!sender || destinationPort == 0) {
        return false;
    }
    const QByteArray bytes = wireBytes(message);
    return sender->writeDatagram(bytes, QHostAddress::LocalHost,
                                 destinationPort) == bytes.size();
}

void drainSocket(QUdpSocket *socket)
{
    if (!socket) {
        return;
    }
    while (socket->hasPendingDatagrams()) {
        QByteArray bytes(static_cast<int>(socket->pendingDatagramSize()), 0);
        socket->readDatagram(bytes.data(), bytes.size());
    }
}

bool takeMessage(QUdpSocket *socket, quint32 messageId,
                 mavlink_message_t *message,
                 int timeoutMs = WaitTimeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        while (socket && socket->hasPendingDatagrams()) {
            QByteArray bytes(
                static_cast<int>(socket->pendingDatagramSize()), 0);
            socket->readDatagram(bytes.data(), bytes.size());
            mavlink_message_t decoded{};
            if (decodeFrame(bytes, &decoded) && decoded.msgid == messageId) {
                if (message) {
                    *message = decoded;
                }
                return true;
            }
        }
        QThread::msleep(2);
    }
    return false;
}

struct ObservedMessage
{
    quint32 messageId = 0;
    quint8 systemId = 0;
    quint8 componentId = 0;
    qint32 value = 0;
    quint64 epoch = 0;
};

int countValue(const QList<ObservedMessage> &messages, qint32 value)
{
    int count = 0;
    for (const ObservedMessage &message : messages) {
        if (message.messageId == MAVLINK_MSG_ID_NAMED_VALUE_INT
            && message.value == value) {
            ++count;
        }
    }
    return count;
}

} // namespace

int RunLogDownloadRuntimeAudit()
{
    AuditResult result;
    LinkManager *const links = LinkManager::instance();
    result.expect(links != nullptr, QStringLiteral("LinkManager is missing"));
    if (!links) {
        return result.exitCode();
    }

    QUdpSocket firstPeer;
    QUdpSocket stalePeer;
    QUdpSocket replacementPeer;
    result.expect(firstPeer.bind(QHostAddress(QHostAddress::LocalHost),
                                 quint16(0)),
                  QStringLiteral("first sender could not bind"));
    result.expect(stalePeer.bind(QHostAddress(QHostAddress::LocalHost),
                                 quint16(0)),
                  QStringLiteral("stale sender could not bind"));
    result.expect(replacementPeer.bind(QHostAddress(QHostAddress::LocalHost),
                                       quint16(0)),
                  QStringLiteral("replacement sender could not bind"));
    const quint16 listenPort = reserveLocalPort();
    result.expect(listenPort != 0,
                  QStringLiteral("no local listener port is available"));
    if (!firstPeer.localPort() || !stalePeer.localPort()
        || !replacementPeer.localPort() || !listenPort) {
        return result.exitCode();
    }

    const int linkId = LinkManagerFactory::addUdpConnection(
        QHostAddress::Any, listenPort, false);
    QPointer<UDPLink> link(
        qobject_cast<UDPLink *>(links->getLink(linkId)));
    result.expect(linkId >= 0 && link,
                  QStringLiteral("production UDP link was not created"));
    if (!link) {
        return result.exitCode();
    }

    QObject connectionScope;
    QList<ObservedMessage> observed;
    QList<quint64> beganEpochs;
    QList<quint64> endedEpochs;
    QList<ExactLogTransferResult> logResults;
    bool replacePeerReentrantly = false;

    QObject::connect(
        links, &LinkManager::mavlinkMessageObserved, &connectionScope,
        [&](int observedLinkId, qulonglong epoch,
            mavlink_message_t message) {
            if (observedLinkId != linkId) {
                return;
            }
            ObservedMessage item;
            item.messageId = message.msgid;
            item.systemId = message.sysid;
            item.componentId = message.compid;
            item.epoch = epoch;
            if (message.msgid == MAVLINK_MSG_ID_NAMED_VALUE_INT) {
                mavlink_named_value_int_t payload{};
                mavlink_msg_named_value_int_decode(&message, &payload);
                item.value = payload.value;
            }
            observed.append(item);
            if (replacePeerReentrantly
                && message.msgid == MAVLINK_MSG_ID_NAMED_VALUE_INT
                && item.value == 303 && link) {
                replacePeerReentrantly = false;
                link->addHost(QStringLiteral("127.0.0.1:%1")
                                  .arg(firstPeer.localPort()));
            }
        });
    QObject::connect(
        links, &LinkManager::physicalLinkSessionBegan, &connectionScope,
        [&](int observedLinkId, qulonglong epoch) {
            if (observedLinkId == linkId) {
                beganEpochs.append(epoch);
            }
        });
    QObject::connect(
        links, &LinkManager::physicalLinkSessionEnded, &connectionScope,
        [&](int observedLinkId, qulonglong epoch) {
            if (observedLinkId == linkId) {
                endedEpochs.append(epoch);
            }
        });

    ExactLogTransferService *const logService =
        links->exactLogTransferService();
    result.expect(logService != nullptr,
                  QStringLiteral("production exact log service is missing"));
    if (logService) {
        QObject::connect(
            logService, &ExactLogTransferService::transferFinished,
            &connectionScope,
            [&](const ExactLogTransferResult &finished) {
                if (finished.vehicle.endpoint.linkId == linkId) {
                    logResults.append(finished);
                }
            });
    }

    result.expect(waitUntil([&]() { return !link || link->isConnected(); }),
                  QStringLiteral("UDP listener did not bind"));
    result.expect(link && links->currentPhysicalLinkSession(linkId) == 0,
                  QStringLiteral("UDP bind created an epoch before a peer"));
    result.expect(beganEpochs.isEmpty(),
                  QStringLiteral("UDP bind emitted a spurious session begin"));

    const quint8 systemId = 249;
    const quint8 componentId = 1;
    result.expect(sendFrame(&firstPeer, listenPort,
                            namedValueMessage(systemId, 11, 101)),
                  QStringLiteral("first stamped packet could not be sent"));
    result.expect(waitUntil([&]() { return countValue(observed, 101) == 1; }),
                  QStringLiteral("first stamped packet was not observed"));
    const quint64 firstEpoch = links->currentPhysicalLinkSession(linkId);
    result.expect(firstEpoch != 0 && beganEpochs.size() == 1
                      && beganEpochs.first() == firstEpoch,
                  QStringLiteral("first peer did not establish one epoch"));
    const UDPLink::PeerSnapshot firstSnapshot = link->peerSnapshot();
    result.expect(firstSnapshot.revision != 0
                      && firstSnapshot.hosts.size() == 1
                      && firstSnapshot.ports.size() == 1
                      && firstSnapshot.ports.first() == firstPeer.localPort()
                      && links->isCurrentPhysicalIngress(link),
                  QStringLiteral("first peer snapshot is not current/atomic"));

    drainSocket(&firstPeer);
    const int submittedBaseline = observed.size();
    result.expect(links->writeMavlinkMessage(
                      link, namedValueMessage(250, 190, 202)),
                  QStringLiteral("strict current-peer write was rejected"));
    mavlink_message_t outbound{};
    result.expect(takeMessage(&firstPeer,
                              MAVLINK_MSG_ID_NAMED_VALUE_INT, &outbound),
                  QStringLiteral("strict write did not reach the current peer"));
    result.expect(outbound.sysid == 250 && outbound.compid == 190,
                  QStringLiteral("strict write changed the MAVLink identity"));
    result.expect(observed.size() == submittedBaseline,
                  QStringLiteral("outbound frame leaked into inbound observation"));

    // Queue two worker-to-main signals under different peer revisions without
    // draining the main event queue. Only the final current revision may parse.
    result.expect(sendFrame(&stalePeer, listenPort,
                            namedValueMessage(systemId, 12, 212)),
                  QStringLiteral("stale-boundary packet could not be sent"));
    result.expect(waitUntil(
                      [&]() {
                          return link
                              && link->peerSnapshot().ports.value(0)
                                  == stalePeer.localPort();
                      }, WaitTimeoutMs, false),
                  QStringLiteral("worker did not learn the stale peer"));
    result.expect(sendFrame(&replacementPeer, listenPort,
                            namedValueMessage(systemId, 13, 213)),
                  QStringLiteral("replacement packet could not be sent"));
    result.expect(waitUntil(
                      [&]() {
                          return link
                              && link->peerSnapshot().ports.value(0)
                                  == replacementPeer.localPort();
                      }, WaitTimeoutMs, false),
                  QStringLiteral("worker did not learn the replacement peer"));
    drainSocket(&replacementPeer);
    result.expect(!links->writeRawBytes(
                      linkId, QByteArrayLiteral("stale-route-must-fail")),
                  QStringLiteral("old epoch wrote after peer replacement"));
    result.expect(!replacementPeer.waitForReadyRead(100),
                  QStringLiteral("rejected old-epoch bytes reached new peer"));

    result.expect(waitUntil([&]() { return countValue(observed, 213) == 1; }),
                  QStringLiteral("replacement peer packet was not observed"));
    result.expect(countValue(observed, 212) == 0,
                  QStringLiteral("stale queued ingress crossed revision"));
    const quint64 replacementEpoch =
        links->currentPhysicalLinkSession(linkId);
    result.expect(replacementEpoch != 0 && replacementEpoch != firstEpoch
                      && endedEpochs.contains(firstEpoch),
                  QStringLiteral("peer replacement did not rotate epoch"));

    // Mutating the peer from the first packet observer must stop parsing the
    // remainder of the same datagram batch.
    replacePeerReentrantly = true;
    const QByteArray reentrantBatch =
        wireBytes(namedValueMessage(systemId, 20, 303))
        + wireBytes(namedValueMessage(systemId, 21, 304));
    result.expect(replacementPeer.writeDatagram(
                      reentrantBatch, QHostAddress::LocalHost, listenPort)
                      == reentrantBatch.size(),
                  QStringLiteral("reentrant two-frame datagram could not be sent"));
    result.expect(waitUntil([&]() { return countValue(observed, 303) == 1; }),
                  QStringLiteral("first reentrant frame was not observed"));
    processFor(100);
    result.expect(countValue(observed, 304) == 0,
                  QStringLiteral("second frame survived reentrant peer change"));
    result.expect(!links->isCurrentPhysicalIngress(link),
                  QStringLiteral("old epoch remained current after reentrant change"));
    result.expect(!links->writeRawBytes(
                      linkId, QByteArrayLiteral("reentrant-stale-write")),
                  QStringLiteral("reentrant stale route accepted a write"));

    // A fresh stamped heartbeat establishes the new revision and a production
    // exact vehicle lease for Download Logs.
    const mavlink_message_t heartbeat =
        heartbeatMessage(systemId, componentId);
    SwarmTelemetryRegistry *const registry =
        links->swarmTelemetryRegistry();
    result.expect(registry != nullptr,
                  QStringLiteral("production telemetry registry is missing"));
    SwarmVehicleInstanceLease lease;
    for (int attempt = 0; attempt < 10 && !lease.isValid(); ++attempt) {
        sendFrame(&firstPeer, listenPort, heartbeat);
        waitUntil([&]() {
            if (!registry || !link) {
                return true;
            }
            VehicleEndpoint endpoint;
            endpoint.linkId = linkId;
            endpoint.systemId = systemId;
            endpoint.componentId = componentId;
            lease = registry->acquireVehicle(endpoint);
            return lease.isValid();
        }, 300);
    }
    result.expect(lease.isValid(),
                  QStringLiteral("fresh UDP heartbeat produced no exact lease"));
    result.expect(lease.linkSessionEpoch
                      == links->currentPhysicalLinkSession(linkId),
                  QStringLiteral("lease did not use the fresh peer epoch"));

    QObject operationOwner;
    ExactLogTransferToken firstListToken;
    QString listError;
    drainSocket(&firstPeer);
    if (logService && lease.isValid()) {
        result.expect(
            logService->requestList(
                &operationOwner, lease, &firstListToken, &listError)
                == ExactLogTransferService::StartResult::Started,
            QStringLiteral("production requestList failed: %1").arg(listError));
    }
    mavlink_message_t listRequest{};
    result.expect(firstListToken.isValid()
                      && takeMessage(&firstPeer,
                                     MAVLINK_MSG_ID_LOG_REQUEST_LIST,
                                     &listRequest),
                  QStringLiteral("LOG_REQUEST_LIST did not reach exact UDP peer"));

    result.expect(sendFrame(&firstPeer, listenPort,
                            logEntryMessage(systemId, componentId)),
                  QStringLiteral("fake LOG_ENTRY could not be sent"));
    result.expect(waitUntil([&]() {
                      for (const ExactLogTransferResult &finished : logResults) {
                          if (finished.token == firstListToken) {
                              return true;
                          }
                      }
                      return false;
                  }),
                  QStringLiteral("fake LOG_ENTRY did not finish list request"));
    for (const ExactLogTransferResult &finished : logResults) {
        if (finished.token == firstListToken) {
            result.expect(
                finished.outcome
                        == ExactLogTransferService::Outcome::Completed
                    && finished.entries.size() == 1
                    && finished.entries.first().id == 7,
                QStringLiteral("completed list result lost the LOG_ENTRY"));
        }
    }

    // Start another real operation, then let a different source port retire
    // its exact lease before the replacement heartbeat is admitted.
    processFor(50);
    drainSocket(&firstPeer);
    ExactLogTransferToken retiringToken;
    listError.clear();
    if (logService && lease.isValid()) {
        result.expect(
            logService->requestList(
                &operationOwner, lease, &retiringToken, &listError)
                == ExactLogTransferService::StartResult::Started,
            QStringLiteral("second requestList failed: %1").arg(listError));
    }
    result.expect(retiringToken.isValid()
                      && takeMessage(&firstPeer,
                                     MAVLINK_MSG_ID_LOG_REQUEST_LIST, nullptr),
                  QStringLiteral("second list request was not submitted"));
    result.expect(sendFrame(&replacementPeer, listenPort, heartbeat),
                  QStringLiteral("replacement heartbeat could not be sent"));
    result.expect(waitUntil([&]() {
                      for (const ExactLogTransferResult &finished : logResults) {
                          if (finished.token == retiringToken) {
                              return true;
                          }
                      }
                      return false;
                  }),
                  QStringLiteral("peer change did not retire active list"));
    for (const ExactLogTransferResult &finished : logResults) {
        if (finished.token == retiringToken) {
            result.expect(
                finished.outcome
                    == ExactLogTransferService::Outcome::LeaseRetired,
                QStringLiteral("peer change used the wrong terminal outcome"));
        }
    }

    // Remove the audit UAS before its physical link so no retained legacy UAS
    // can hold the soon-to-be-deleted transport pointer.
    if (UASInterface *auditUas = links->getUas(systemId)) {
        UASManager::instance()->removeUAS(auditUas);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    if (links->getLink(linkId)) {
        links->removeLink(linkId);
    }
    link.clear();
    processFor(20);
    result.expect(links->getLink(linkId) == nullptr,
                  QStringLiteral("audit UDP link survived cleanup"));
    result.expect(!logService || !logService->busy(),
                  QStringLiteral("log service remained busy after peer retirement"));

    if (result.failures() == 0) {
        qInfo().noquote()
            << QStringLiteral("Download Logs UDP runtime audit: production "
                              "stamped peer/session path passed");
    }
    return result.exitCode();
}
