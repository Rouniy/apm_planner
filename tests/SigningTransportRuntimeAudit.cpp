#include "SigningTransportRuntimeAudit.h"

#include "comm/ExactLinkTransmitter.h"
#include "comm/LinkInterface.h"
#include "comm/LinkManager.h"
#include "comm/LinkManagerFactory.h"
#include "comm/MAVLinkDecoder.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/MAVLinkProtocol.h"
#include "comm/MAVLinkSigningClock.h"
#include "comm/MAVLinkSigningManager.h"
#include "comm/RadioStatusMonitor.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QSignalBlocker>
#include <QThread>
#include <QVector>

#include <algorithm>
#include <cstring>
#include <functional>

namespace {

constexpr int FirstAuditLinkId = 910001;
constexpr int SecondAuditLinkId = 910002;
constexpr int WaitTimeoutMs = 1000;

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
        return QStringLiteral("Signing Transport Audit %1").arg(m_id);
    }
    QString getShortName() const override
    {
        return QStringLiteral("Signing Audit %1").arg(m_id);
    }
    QString getDetail() const override
    {
        return QStringLiteral("in-process disconnected transport");
    }
    void requestReset() override { ++m_resetRequests; }
    bool isConnected() const override { return m_connected; }
    qint64 getConnectionSpeed() const override { return 0; }
    qint64 bytesAvailable() override { return 0; }
    LinkType getLinkType() override { return UNKNOWN_LINK; }

    bool connect() override
    {
        if (m_connected) {
            return true;
        }
        m_connected = true;
        emit connected();
        emit connected(this);
        emit connected(true);
        return true;
    }

    bool disconnect() override
    {
        if (!m_connected) {
            return true;
        }
        m_connected = false;
        emit disconnected();
        emit disconnected(this);
        emit connected(false);
        return true;
    }

    void writeBytes(const char *bytes, qint64 size) override
    {
        if (bytes && size > 0) {
            m_writes.append(QByteArray(bytes, static_cast<int>(size)));
        }
    }

    void inject(const QByteArray &bytes)
    {
        if (m_connected && !bytes.isEmpty()) {
            emit bytesReceived(this, bytes);
        }
    }

    const QVector<QByteArray> &writes() const { return m_writes; }
    int resetRequests() const { return m_resetRequests; }

protected slots:
    void readBytes() override {}

private:
    const int m_id;
    bool m_connected = false;
    int m_resetRequests = 0;
    QVector<QByteArray> m_writes;
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
            << QStringLiteral("MAVLink signing transport runtime audit: %1")
                   .arg(message);
    }

    int exitCode() const { return m_failures == 0 ? 0 : 1; }

private:
    int m_failures = 0;
};

bool waitUntil(const std::function<bool()> &predicate,
               int timeoutMs = WaitTimeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

void processFor(int durationMs = 20)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    } while (timer.elapsed() < durationMs);
}

QByteArray signingKey()
{
    QByteArray key;
    key.reserve(32);
    for (int index = 0; index < 32; ++index) {
        key.append(static_cast<char>(0x31 + index));
    }
    return key;
}

QByteArray wireBytes(const mavlink_message_t &message)
{
    quint8 bytes[MAVLINK_MAX_PACKET_LEN]{};
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
    return state == MAVLINK_FRAMING_OK && parser.lastFrame() == bytes;
}

bool isSignedMavlink2(const QByteArray &frame)
{
    if (frame.size() < MAVLINK_NUM_HEADER_BYTES + 2
                           + MAVLINK_SIGNATURE_BLOCK_LEN
        || static_cast<quint8>(frame.at(0)) != MAVLINK_STX
        || !(static_cast<quint8>(frame.at(2)) & MAVLINK_IFLAG_SIGNED)) {
        return false;
    }
    const int payloadLength = static_cast<quint8>(frame.at(1));
    return frame.size() == MAVLINK_NUM_HEADER_BYTES + payloadLength + 2
        + MAVLINK_SIGNATURE_BLOCK_LEN;
}

quint64 frameSigningTimestamp(const QByteArray &frame)
{
    if (!isSignedMavlink2(frame)) {
        return 0;
    }
    const int signatureOffset = frame.size() - MAVLINK_SIGNATURE_BLOCK_LEN;
    quint64 timestamp = 0;
    for (int index = 0; index < 6; ++index) {
        timestamp |= quint64(static_cast<quint8>(
                         frame.at(signatureOffset + 1 + index)))
            << (index * 8);
    }
    return timestamp;
}

quint8 frameSigningLinkId(const QByteArray &frame)
{
    return isSignedMavlink2(frame)
        ? static_cast<quint8>(
              frame.at(frame.size() - MAVLINK_SIGNATURE_BLOCK_LEN))
        : 0;
}

quint8 frameSequence(const QByteArray &frame)
{
    return frame.size() > 4 ? static_cast<quint8>(frame.at(4)) : 0;
}

mavlink_message_t namedValueMessage(quint8 systemId, quint8 componentId,
                                    qint32 value)
{
    mavlink_message_t message{};
    char name[10]{};
    std::memcpy(name, "sign-audit", 10);
    mavlink_msg_named_value_int_pack(
        systemId, componentId, &message, 100, name, value);
    return message;
}

mavlink_message_t heartbeatMessage(quint8 systemId, quint8 componentId)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, componentId, &message, MAV_TYPE_ONBOARD_CONTROLLER,
        MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
    return message;
}

mavlink_message_t radioStatusMessage()
{
    mavlink_message_t message{};
    mavlink_msg_radio_status_pack(
        static_cast<quint8>('3'), static_cast<quint8>('D'), &message,
        180, 170, 90, 80, 75, 4, 3);
    return message;
}

mavlink_message_t setupSigningMessage(
    quint8 sourceSystem, quint8 sourceComponent,
    quint8 targetSystem, quint8 targetComponent,
    const QByteArray &key, quint64 initialTimestamp)
{
    mavlink_message_t message{};
    if (key.size() != 32) {
        return message;
    }
    mavlink_msg_setup_signing_pack(
        sourceSystem, sourceComponent, &message,
        targetSystem, targetComponent,
        reinterpret_cast<const quint8 *>(key.constData()),
        initialTimestamp);
    return message;
}

QByteArray nativeFrame(mavlink_message_t message, quint8 systemId,
                       quint8 componentId, quint8 sequence,
                       bool mavlink1, const QByteArray &key = {},
                       quint8 signingLinkId = 0,
                       quint64 signingTimestamp = 0)
{
    const mavlink_msg_entry_t *const entry =
        mavlink_get_msg_entry(message.msgid);
    if (!entry || (!key.isEmpty() && key.size() != 32)) {
        return {};
    }
    mavlink_status_t status{};
    status.current_tx_seq = sequence;
    if (mavlink1) {
        status.flags = MAVLINK_STATUS_FLAG_OUT_MAVLINK1;
    }
    mavlink_signing_t signing{};
    if (!key.isEmpty()) {
        signing.flags = MAVLINK_SIGNING_FLAG_SIGN_OUTGOING;
        signing.link_id = signingLinkId;
        signing.timestamp = signingTimestamp;
        std::memcpy(signing.secret_key, key.constData(), 32);
        status.signing = &signing;
    }
    mavlink_finalize_message_buffer(
        &message, systemId, componentId, &status,
        entry->min_msg_len, entry->max_msg_len, entry->crc_extra);
    return wireBytes(message);
}

bool nativeVerifierAccepts(const QVector<QByteArray> &frames,
                           const QByteArray &key)
{
    if (key.size() != 32) {
        return false;
    }
    mavlink_signing_t signing{};
    mavlink_signing_streams_t streams{};
    std::memcpy(signing.secret_key, key.constData(), 32);
    MAVLinkFrameParser parser;
    parser.status().signing = &signing;
    parser.status().signing_streams = &streams;
    for (const QByteArray &frame : frames) {
        mavlink_message_t decoded{};
        unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
        for (const char byte : frame) {
            state = parser.parseByte(static_cast<quint8>(byte), &decoded);
        }
        if (state != MAVLINK_FRAMING_OK || parser.lastFrame() != frame) {
            return false;
        }
    }
    return true;
}

quint64 currentSigningTimestamp()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now < MAVLinkSigningClock::EpochUnixMs) {
        return 1;
    }
    const quint64 timestamp =
        quint64(now - MAVLinkSigningClock::EpochUnixMs) * 100ULL;
    return std::min(timestamp,
                    MAVLinkSigningClock::MaxTimestamp - 4ULL);
}

struct SubmittedFrame
{
    int linkId = -1;
    quint64 epoch = 0;
    quint32 messageId = 0;
    QByteArray frame;
};

struct IngressSnapshot
{
    int packets = 0;
    int frames = 0;
    int observed = 0;
    int legacy = 0;

    bool operator==(const IngressSnapshot &other) const
    {
        return packets == other.packets && frames == other.frames
            && observed == other.observed && legacy == other.legacy;
    }
};

} // namespace

int RunSigningTransportRuntimeAudit()
{
    AuditResult result;
    LinkManager *const links = LinkManager::instance();
    MAVLinkProtocol *const protocol = links ? links->getProtocol() : nullptr;
    ExactLinkTransmitter *const transmitter =
        links ? links->exactLinkTransmitter() : nullptr;
    RadioStatusMonitor *const radioMonitor =
        links ? links->radioStatusMonitor() : nullptr;
    MAVLinkDecoder *const decoder =
        links ? links->findChild<MAVLinkDecoder *>() : nullptr;
    result.expect(links != nullptr, QStringLiteral("LinkManager is missing"));
    result.expect(protocol != nullptr,
                  QStringLiteral("production MAVLinkProtocol is missing"));
    result.expect(transmitter != nullptr,
                  QStringLiteral("production ExactLinkTransmitter is missing"));
    result.expect(radioMonitor != nullptr,
                  QStringLiteral("production RadioStatusMonitor is missing"));
    result.expect(decoder != nullptr,
                  QStringLiteral("production MAVLinkDecoder is missing"));
    if (!links || !protocol || !transmitter || !radioMonitor || !decoder) {
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

    QPointer<AuditLink> firstLink(new AuditLink(FirstAuditLinkId));
    QPointer<AuditLink> secondLink(new AuditLink(SecondAuditLinkId));
    // This runtime-only function is a narrow friend of LinkManagerFactory so
    // the fake exercises the same queued ingress and typed lifecycle wiring as
    // every production transport without widening the public factory API.
    LinkManagerFactory::connectLinkSignals(firstLink.data(), links);
    LinkManagerFactory::connectLinkSignals(secondLink.data(), links);
    {
        const QSignalBlocker blockManagerSignals(links);
        links->addLink(firstLink.data());
        links->addLink(secondLink.data());
    }
    result.expect(links->getLink(FirstAuditLinkId) == firstLink.data()
                      && links->getLink(SecondAuditLinkId) == secondLink.data(),
                  QStringLiteral("disconnected fake links were not registered"));
    result.expect(links->currentPhysicalLinkSession(FirstAuditLinkId) == 0
                      && links->currentPhysicalLinkSession(SecondAuditLinkId) == 0,
                  QStringLiteral("offline fake links acquired physical epochs"));

    const QByteArray key = signingKey();
    QString signingError;
    const bool firstConfigured = links->configureSigning(
        FirstAuditLinkId,
        QStringLiteral("apm-runtime-audit-signing-primary-v1"),
        QStringLiteral("Runtime Audit Shared Key"), key, &signingError);
    result.expect(firstConfigured,
                  QStringLiteral("first offline signing configuration failed: %1")
                      .arg(signingError));
    signingError.clear();
    const bool secondConfigured = links->configureSigning(
        SecondAuditLinkId,
        QStringLiteral("apm-runtime-audit-signing-secondary-v1"),
        QStringLiteral("Runtime Audit Shared Key Alias"), key, &signingError);
    result.expect(secondConfigured,
                  QStringLiteral("second offline signing configuration failed: %1")
                      .arg(signingError));

    const auto cleanup = [&]() {
        const QSignalBlocker blockManagerSignals(links);
        if (links->getLink(FirstAuditLinkId)) {
            links->removeLink(FirstAuditLinkId);
        }
        if (links->getLink(SecondAuditLinkId)) {
            links->removeLink(SecondAuditLinkId);
        }
    };

    if (!firstConfigured || !secondConfigured || !firstLink || !secondLink) {
        cleanup();
        result.expect(firstLink.isNull() && secondLink.isNull(),
                      QStringLiteral("failed-setup fake links were not deleted"));
        return result.exitCode();
    }

    result.expect(!links->writeRawBytes(
                      FirstAuditLinkId, QByteArrayLiteral("offline raw secret"))
                      && firstLink->writes().isEmpty(),
                  QStringLiteral("protected offline link admitted a raw write"));
    transmitter->setOutboundVersion(FirstAuditLinkId, 1);
    result.expect(transmitter->outboundVersion(FirstAuditLinkId) == 2U,
                  QStringLiteral("protected offline link downgraded to MAVLink 1"));

    result.expect(links->connectLink(FirstAuditLinkId)
                      && links->connectLink(SecondAuditLinkId),
                  QStringLiteral("fake links did not connect through LinkManager"));
    const quint64 firstEpoch =
        links->currentPhysicalLinkSession(FirstAuditLinkId);
    const quint64 secondEpoch =
        links->currentPhysicalLinkSession(SecondAuditLinkId);
    result.expect(firstEpoch != 0 && secondEpoch != 0
                      && firstEpoch != secondEpoch,
                  QStringLiteral("connected fake links lack distinct physical epochs"));

    const MAVLinkSigningManager::LinkStatus firstStatus =
        links->signingManager()->status(FirstAuditLinkId);
    const MAVLinkSigningManager::LinkStatus secondStatus =
        links->signingManager()->status(SecondAuditLinkId);
    result.expect(firstStatus.protectedLink && secondStatus.protectedLink
                      && firstStatus.activeEpoch == firstEpoch
                      && secondStatus.activeEpoch == secondEpoch,
                  QStringLiteral("signing policies did not bind to live epochs"));
    result.expect(!firstStatus.keyFingerprint.isEmpty()
                      && firstStatus.keyFingerprint == secondStatus.keyFingerprint
                      && firstStatus.signingLinkId != secondStatus.signingLinkId,
                  QStringLiteral("same key aliases did not share a key domain with distinct profile IDs"));

    signingError.clear();
    result.expect(!links->configureSigning(
                      FirstAuditLinkId,
                      QStringLiteral("apm-runtime-audit-signing-primary-v1"),
                      QStringLiteral("Runtime Audit Shared Key"), key,
                      &signingError)
                      && !signingError.isEmpty(),
                  QStringLiteral("live link admitted signing reprovisioning"));
    const int rawWriteBaseline = firstLink->writes().size();
    result.expect(!links->writeRawBytes(
                      FirstAuditLinkId, QByteArrayLiteral("online raw secret"))
                      && firstLink->writes().size() == rawWriteBaseline,
                  QStringLiteral("protected live link admitted a raw write"));
    transmitter->setOutboundVersion(FirstAuditLinkId, 1);
    result.expect(transmitter->outboundVersion(FirstAuditLinkId) == 2U,
                  QStringLiteral("protected live link downgraded to MAVLink 1"));

    QObject signalScope;
    QVector<SubmittedFrame> submittedFrames;
    int exactSubmittedCount = 0;
    int packetCount = 0;
    QVector<QByteArray> receivedFrames;
    int observedCount = 0;
    int legacyCount = 0;
    int decoderValueCount = 0;
    int radioSampleCount = 0;
    RadioStatusSample lastRadioSample;

    QObject::connect(
        links, &LinkManager::mavlinkMessageSubmitted, &signalScope,
        [&](int linkId, qulonglong epoch, mavlink_message_t message) {
            if (linkId == FirstAuditLinkId || linkId == SecondAuditLinkId) {
                submittedFrames.append(
                    {linkId, epoch, message.msgid, wireBytes(message)});
            }
        });
    QObject::connect(
        transmitter, &ExactLinkTransmitter::messageSubmitted, &signalScope,
        [&](int linkId, quint64, mavlink_message_t) {
            if (linkId == FirstAuditLinkId || linkId == SecondAuditLinkId) {
                ++exactSubmittedCount;
            }
        });
    QObject::connect(
        protocol, &MAVLinkProtocol::packetReceived, &signalScope,
        [&](LinkInterface *link, mavlink_message_t) {
            if (link && (link->getId() == FirstAuditLinkId
                         || link->getId() == SecondAuditLinkId)) {
                ++packetCount;
            }
        });
    QObject::connect(
        protocol, &MAVLinkProtocol::frameReceived, &signalScope,
        [&](int linkId, const QByteArray &frame) {
            if (linkId == FirstAuditLinkId || linkId == SecondAuditLinkId) {
                receivedFrames.append(frame);
            }
        });
    QObject::connect(
        links, &LinkManager::mavlinkMessageObserved, &signalScope,
        [&](int linkId, qulonglong, mavlink_message_t) {
            if (linkId == FirstAuditLinkId || linkId == SecondAuditLinkId) {
                ++observedCount;
            }
        });
    QObject::connect(
        links, &LinkManager::messageReceived, &signalScope,
        [&](LinkInterface *link, mavlink_message_t) {
            if (link && (link->getId() == FirstAuditLinkId
                         || link->getId() == SecondAuditLinkId)) {
                ++legacyCount;
            }
        });
    QObject::connect(
        decoder, &MAVLinkDecoder::valueChanged, &signalScope,
        [&](int, const QString &, const QString &, const QVariant &, quint64) {
            ++decoderValueCount;
        });
    QObject::connect(
        radioMonitor, &RadioStatusMonitor::sampleReceived, &signalScope,
        [&](int linkId, const RadioStatusSample &sample) {
            if (linkId == FirstAuditLinkId || linkId == SecondAuditLinkId) {
                ++radioSampleCount;
                lastRadioSample = sample;
            }
        });

    const quint8 localSystemId = 210;
    const quint8 localComponentId = 191;
    const int firstWriteBaseline = firstLink->writes().size();
    const auto exactResult = transmitter->sendMessage(
        FirstAuditLinkId, localSystemId, localComponentId,
        namedValueMessage(1, 1, 101));
    result.expect(exactResult == ExactLinkTransmitter::SendResult::Sent,
                  QStringLiteral("exact transmitter rejected protected link"));
    result.expect(links->writeMavlinkMessage(
                      firstLink.data(),
                      namedValueMessage(localSystemId, localComponentId, 102)),
                  QStringLiteral("legacy typed write rejected protected link"));
    result.expect(firstLink->writes().size() == firstWriteBaseline + 2,
                  QStringLiteral("typed sends did not produce exactly two wire frames"));
    result.expect(submittedFrames.size() == 2 && exactSubmittedCount == 2,
                  QStringLiteral("typed sends were not observed exactly once"));

    QVector<QByteArray> outboundFrames;
    if (firstLink->writes().size() >= firstWriteBaseline + 2) {
        outboundFrames = {
            firstLink->writes().at(firstWriteBaseline),
            firstLink->writes().at(firstWriteBaseline + 1)};
        result.expect(isSignedMavlink2(outboundFrames.at(0))
                          && isSignedMavlink2(outboundFrames.at(1)),
                      QStringLiteral("typed transport frames were not MAVLink 2 signed"));
        result.expect(frameSequence(outboundFrames.at(1))
                          == static_cast<quint8>(
                              frameSequence(outboundFrames.at(0)) + 1),
                      QStringLiteral("exact and legacy paths did not share one link sequence"));
        result.expect(frameSigningTimestamp(outboundFrames.at(1))
                          > frameSigningTimestamp(outboundFrames.at(0))
                          && frameSigningLinkId(outboundFrames.at(0))
                              == static_cast<quint8>(firstStatus.signingLinkId)
                          && frameSigningLinkId(outboundFrames.at(1))
                              == static_cast<quint8>(firstStatus.signingLinkId),
                      QStringLiteral("typed paths did not share the persistent signing stream"));
        result.expect(nativeVerifierAccepts(outboundFrames, key),
                      QStringLiteral("native MAVLink verifier rejected typed wire frames"));
    }
    for (int index = 0; index < submittedFrames.size(); ++index) {
        if (index >= outboundFrames.size()) {
            break;
        }
        result.expect(submittedFrames.at(index).linkId == FirstAuditLinkId
                          && submittedFrames.at(index).epoch == firstEpoch
                          && submittedFrames.at(index).frame
                              == outboundFrames.at(index),
                      QStringLiteral("LinkManager submitted event %1 did not preserve signed wire bytes")
                          .arg(index));
    }

    // SETUP_SIGNING carries the key itself. It may reach an explicitly chosen
    // transport, but neither the exact nor public submitted streams may mirror
    // it into inspectors, logs or generic observers.
    const mavlink_message_t outboundSetup = setupSigningMessage(
        localSystemId, localComponentId, 42, 1, key,
        currentSigningTimestamp());
    const int setupWriteBaseline = firstLink->writes().size();
    const int managerSubmissionBaseline = submittedFrames.size();
    const int exactSubmissionBaseline = exactSubmittedCount;
    result.expect(links->writeMavlinkMessage(firstLink.data(), outboundSetup),
                  QStringLiteral("typed SETUP_SIGNING transport write failed"));
    result.expect(firstLink->writes().size() == setupWriteBaseline + 1
                      && submittedFrames.size() == managerSubmissionBaseline
                      && exactSubmittedCount == exactSubmissionBaseline,
                  QStringLiteral("SETUP_SIGNING did not stay transport-only"));
    if (firstLink->writes().size() == setupWriteBaseline + 1) {
        const QByteArray setupWire = firstLink->writes().at(setupWriteBaseline);
        mavlink_message_t decoded{};
        result.expect(isSignedMavlink2(setupWire) && decodeFrame(setupWire, &decoded)
                          && decoded.msgid == MAVLINK_MSG_ID_SETUP_SIGNING,
                      QStringLiteral("SETUP_SIGNING wire frame was not exact/signed"));
        QVector<QByteArray> verifierFrames = outboundFrames;
        verifierFrames.append(setupWire);
        result.expect(nativeVerifierAccepts(verifierFrames, key),
                      QStringLiteral("native verifier rejected SETUP_SIGNING wire frame"));
    }

    const auto ingressSnapshot = [&]() {
        return IngressSnapshot{packetCount, receivedFrames.size(),
                               observedCount, legacyCount};
    };
    const auto expectNoGenericIngress =
        [&](const IngressSnapshot &before, const QString &label) {
            processFor();
            result.expect(ingressSnapshot() == before,
                          QStringLiteral("%1 escaped the authentication gate")
                              .arg(label));
        };

    // Protected links must not treat rejected/non-MAVLink bytes as a baud-rate
    // failure and reset a transport which may simply be under attack.
    IngressSnapshot before = ingressSnapshot();
    firstLink->inject(QByteArray(1999, char(0x55)));
    expectNoGenericIngress(before, QStringLiteral("bounded garbage"));
    result.expect(firstLink->resetRequests() == 0,
                  QStringLiteral("rejected garbage requested a physical reset"));

    quint8 foreignSystemId = 239;
    while (foreignSystemId > 1 && links->getUas(foreignSystemId)) {
        --foreignSystemId;
    }
    result.expect(links->getUas(foreignSystemId) == nullptr,
                  QStringLiteral("no unused foreign system id is available"));
    const quint8 foreignComponentId = 154;
    const quint8 incomingSigningLinkId = 203;
    const quint64 incomingTimestamp = currentSigningTimestamp();
    const QByteArray validIncoming = nativeFrame(
        namedValueMessage(foreignSystemId, foreignComponentId, 303),
        foreignSystemId, foreignComponentId, 77, false, key,
        incomingSigningLinkId, incomingTimestamp);
    result.expect(isSignedMavlink2(validIncoming),
                  QStringLiteral("native signed ingress fixture is invalid"));

    QByteArray badMac = validIncoming;
    if (!badMac.isEmpty()) {
        badMac[badMac.size() - 1] = static_cast<char>(
            static_cast<quint8>(badMac.at(badMac.size() - 1)) ^ 0x01U);
    }
    before = ingressSnapshot();
    firstLink->inject(badMac);
    expectNoGenericIngress(before, QStringLiteral("bad-MAC frame"));

    const QByteArray unsignedHeartbeat = nativeFrame(
        heartbeatMessage(foreignSystemId, 1), foreignSystemId, 1,
        78, false);
    before = ingressSnapshot();
    firstLink->inject(unsignedHeartbeat);
    expectNoGenericIngress(before, QStringLiteral("unsigned HEARTBEAT"));
    result.expect(links->getUas(foreignSystemId) == nullptr,
                  QStringLiteral("rejected HEARTBEAT created a vehicle"));

    const mavlink_message_t incomingSetupMessage = setupSigningMessage(
        foreignSystemId, foreignComponentId, localSystemId,
        localComponentId, key, incomingTimestamp);
    const QByteArray unsignedSetup = nativeFrame(
        incomingSetupMessage, foreignSystemId, foreignComponentId,
        79, false);
    before = ingressSnapshot();
    firstLink->inject(unsignedSetup);
    expectNoGenericIngress(before, QStringLiteral("unsigned SETUP_SIGNING"));

    const int decoderBaseline = decoderValueCount;
    decoder->receiveMessage(firstLink.data(), incomingSetupMessage);
    processFor(1);
    result.expect(decoderValueCount == decoderBaseline,
                  QStringLiteral("MAVLinkDecoder exposed SETUP_SIGNING key fields"));

    before = ingressSnapshot();
    firstLink->inject(validIncoming);
    result.expect(waitUntil([&]() {
                      const IngressSnapshot now = ingressSnapshot();
                      return now.packets == before.packets + 1
                          && now.frames == before.frames + 1
                          && now.observed == before.observed + 1;
                  }),
                  QStringLiteral("valid signed frame was not accepted"));
    result.expect(legacyCount == before.legacy,
                  QStringLiteral("pre-heartbeat signed frame entered UAS-only stream"));
    result.expect(!receivedFrames.isEmpty()
                      && receivedFrames.constLast() == validIncoming,
                  QStringLiteral("accepted signed frame bytes were reconstructed or trimmed"));
    result.expect(links->getUas(foreignSystemId) == nullptr,
                  QStringLiteral("signed peripheral frame created a vehicle"));

    before = ingressSnapshot();
    firstLink->inject(validIncoming);
    expectNoGenericIngress(before, QStringLiteral("same-link replay"));

    before = ingressSnapshot();
    secondLink->inject(validIncoming);
    expectNoGenericIngress(before, QStringLiteral("same-key cross-link replay"));

    const QByteArray signedSetup = nativeFrame(
        incomingSetupMessage, foreignSystemId, foreignComponentId,
        80, false, key, incomingSigningLinkId,
        incomingTimestamp + 1);
    const quint64 acceptedBeforeSetup =
        links->signingManager()->status(FirstAuditLinkId)
            .counters.signedAccepted;
    before = ingressSnapshot();
    firstLink->inject(signedSetup);
    expectNoGenericIngress(before, QStringLiteral("signed SETUP_SIGNING"));
    result.expect(
        links->signingManager()->status(FirstAuditLinkId)
                .counters.signedAccepted
            == acceptedBeforeSetup + 1,
        QStringLiteral("signed SETUP_SIGNING was not authenticated before suppression"));

    const QByteArray unsignedRadio = nativeFrame(
        radioStatusMessage(), static_cast<quint8>('3'),
        static_cast<quint8>('D'), 81, true);
    const int radioBaseline = radioSampleCount;
    before = ingressSnapshot();
    firstLink->inject(unsignedRadio);
    result.expect(waitUntil([&]() {
                      return radioSampleCount == radioBaseline + 1;
                  }),
                  QStringLiteral("unsigned RADIO_STATUS exception was not monitored"));
    result.expect(ingressSnapshot() == before,
                  QStringLiteral("RADIO_STATUS exception entered generic observers"));
    result.expect(lastRadioSample.isValid()
                      && lastRadioSample.systemId == static_cast<quint8>('3')
                      && lastRadioSample.componentId == static_cast<quint8>('D')
                      && lastRadioSample.rssi == 180
                      && lastRadioSample.remrssi == 170,
                  QStringLiteral("radio-only monitor did not preserve raw sample"));
    result.expect(transmitter->outboundVersion(FirstAuditLinkId) == 2U,
                  QStringLiteral("unsigned MAVLink 1 radio downgraded protected link"));

    links->disconnectLink(FirstAuditLinkId);
    result.expect(firstLink && !firstLink->isConnected()
                      && links->currentPhysicalLinkSession(FirstAuditLinkId) == 0,
                  QStringLiteral("disconnect did not invalidate physical epoch"));
    const int offlineWriteBaseline = firstLink ? firstLink->writes().size() : 0;
    result.expect(firstLink && !links->writeRawBytes(
                                  FirstAuditLinkId,
                                  QByteArrayLiteral("disconnected protected raw"))
                      && firstLink->writes().size() == offlineWriteBaseline,
                  QStringLiteral("disconnected protected link admitted raw bytes"));
    result.expect(links->connectLink(FirstAuditLinkId),
                  QStringLiteral("same physical link did not reconnect"));
    const quint64 reconnectedEpoch =
        links->currentPhysicalLinkSession(FirstAuditLinkId);
    result.expect(reconnectedEpoch != 0 && reconnectedEpoch != firstEpoch,
                  QStringLiteral("reconnect did not rotate physical epoch"));
    before = ingressSnapshot();
    firstLink->inject(validIncoming);
    expectNoGenericIngress(before, QStringLiteral("post-reconnect replay"));

    const MAVLinkSigningManager::LinkStatus sharedFirstStatus =
        links->signingManager()->status(FirstAuditLinkId);
    const MAVLinkSigningManager::LinkStatus sharedSecondStatus =
        links->signingManager()->status(SecondAuditLinkId);
    result.expect(sharedFirstStatus.keyFingerprint
                      == sharedSecondStatus.keyFingerprint
                      && sharedFirstStatus.counters.signedAccepted
                          == sharedSecondStatus.counters.signedAccepted
                      && sharedFirstStatus.counters.rejected
                          == sharedSecondStatus.counters.rejected
                      && sharedFirstStatus.counters.unsignedRadioAccepted
                          == sharedSecondStatus.counters.unsignedRadioAccepted,
                  QStringLiteral("duplicate-key links did not retain one replay/counter context"));
    result.expect(sharedFirstStatus.counters.signedAccepted >= 2
                      && sharedFirstStatus.counters.rejected >= 5
                      && sharedFirstStatus.counters.unsignedRadioAccepted >= 1,
                  QStringLiteral("production signing counters missed transport outcomes"));

    cleanup();
    result.expect(firstLink.isNull() && secondLink.isNull()
                      && links->getLink(FirstAuditLinkId) == nullptr
                      && links->getLink(SecondAuditLinkId) == nullptr,
                  QStringLiteral("LinkManager did not safely delete fake links"));
    processFor(1);
    return result.exitCode();
}
