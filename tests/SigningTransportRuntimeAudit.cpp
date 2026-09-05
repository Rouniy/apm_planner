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
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/TCPLink.h"
#include "comm/UASObject.h"
#include "comm/VehicleTargetManager.h"
#include "services/MavlinkSigningProfiles.h"
#include "services/SigningProvisioningTarget.h"
#include "ui/configuration/PlannerStartupUdpOptions.h"
#include "uas/UASInterface.h"
#include "uas/UASManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QEvent>
#include <QHostAddress>
#include <QPointer>
#include <QSettings>
#include <QSignalBlocker>
#include <QTcpServer>
#include <QThread>
#include <QUdpSocket>
#include <QVector>

#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>

namespace {

constexpr int FirstAuditLinkId = 910001;
constexpr int SecondAuditLinkId = 910002;
constexpr int MissingPolicyLinkId = 910003;
constexpr int CorruptPolicyLinkId = 910004;
constexpr int DuplicatePolicyFirstLinkId = 910005;
constexpr int DuplicatePolicySecondLinkId = 910006;
constexpr int SignedProvisioningAuditLinkId = 910007;
constexpr int ProvisioningAuditLinkId = 910008;
constexpr int InterruptedProvisioningAuditLinkId = 910009;
constexpr int RetiredUasAuditLinkId = 910010;
constexpr int RediscoveredUasAuditLinkId = 910011;
constexpr int WaitTimeoutMs = 1000;

const QString PrimaryProfileId =
    QStringLiteral("75a4a7e8-5ee0-4fd3-a47d-f464e0d151a1");
const QString SecondaryProfileId =
    QStringLiteral("8c37a8ab-8e06-4e0c-8c5a-3e70ba924127");
const QString MissingProfileId =
    QStringLiteral("d7b7390d-d6ea-47a3-8b24-b42b6bbaf014");
const QString CorruptProfileId =
    QStringLiteral("e95852ee-809c-403e-bfe2-2aacfc64248b");
const QString DuplicateProfileId =
    QStringLiteral("a79e88c1-f4e2-41d0-a58e-a8153af23043");
const QString TcpServerProfileId =
    QStringLiteral("11c9098e-2156-456c-b7b2-5d69609b2623");
const QString RestartUdpProfileId =
    QStringLiteral("5f038b27-c17f-4be9-8a02-d388cb975a48");
const QString DuplicateRestoreProfileId =
    QStringLiteral("b00a099c-f77e-48fd-8413-754f87f73128");
const QString CollisionManualProfileId =
    QStringLiteral("bb442861-3ece-4b70-803c-b7772756e5a2");
const QString SignedProvisioningProfileId =
    QStringLiteral("4d94df9a-d341-4aa6-b566-8aeea1288c58");
const QString ProvisioningProfileId =
    QStringLiteral("fac50c45-9ca9-418c-a3af-a06ec37c46cb");
const QString InterruptedProvisioningProfileId =
    QStringLiteral("6890197f-e034-4a6f-b2b6-7f105707c6f9");
const QString RetiredUasProfileId =
    QStringLiteral("ab772fee-c647-48f4-9eae-dd89859f60ef");
const QString RediscoveredUasProfileId =
    QStringLiteral("3eb5b624-45e7-432e-ae6a-3b7d2eb26eb5");

struct StoredUdpDefinition
{
    quint16 port = 0;
    QString profileId;
    bool includeIdentity = true;
    bool signingRequired = false;
    bool includeRequirement = true;
};

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

// A TCPLink subclass keeps LinkManager::saveSettings() on its production
// downcast path while replacing every socket operation with deterministic
// in-process transport behavior. No network endpoint or SITL is touched.
class ProvisioningAuditLink final : public TCPLink
{
public:
    using WriteObserver = std::function<void(const QByteArray &)>;

    explicit ProvisioningAuditLink(int id)
        : TCPLink(QHostAddress::LocalHost,
                  QStringLiteral("Signing Provisioning Audit"), 5760, false)
        , m_id(id)
    {
    }

    int getId() const override { return m_id; }
    QString getName() const override
    {
        return QStringLiteral("Signing Provisioning Audit %1").arg(m_id);
    }
    QString getShortName() const override
    {
        return QStringLiteral("Provisioning Audit %1").arg(m_id);
    }
    QString getDetail() const override
    {
        return QStringLiteral("in-process private TCP client");
    }
    bool isConnected() const override { return m_connected; }
    bool connect() override
    {
        if (!m_connected) {
            m_connected = true;
            emit connected();
            emit connected(this);
            emit connected(true);
        }
        return true;
    }
    bool disconnect() override
    {
        if (m_connected) {
            m_connected = false;
            emit disconnected();
            emit disconnected(this);
            emit connected(false);
        }
        return true;
    }
    qint64 bytesAvailable() override { return 0; }
    void writeBytes(const char *bytes, qint64 size) override
    {
        if (m_connected && bytes && size > 0) {
            const QByteArray frame(bytes, int(size));
            const WriteObserver observer = m_writeObserver;
            m_writes.append(frame);
            // The observer may synchronously remove and delete this link.
            // Keep everything needed after the append on the stack.
            if (observer) observer(frame);
        }
    }
    void inject(const QByteArray &bytes)
    {
        if (m_connected && !bytes.isEmpty()) emit bytesReceived(this, bytes);
    }
    const QVector<QByteArray> &writes() const { return m_writes; }
    void setWriteObserver(WriteObserver observer)
    {
        m_writeObserver = std::move(observer);
    }

protected slots:
    void readBytes() override {}

private:
    const int m_id;
    bool m_connected = false;
    QVector<QByteArray> m_writes;
    WriteObserver m_writeObserver;
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

QByteArray alternateSigningKey()
{
    QByteArray key;
    key.reserve(32);
    for (int index = 0; index < 32; ++index) {
        key.append(static_cast<char>(0x71 + index));
    }
    return key;
}

QByteArray keyFingerprint(const QByteArray &key)
{
    return QCryptographicHash::hash(key, QCryptographicHash::Sha256);
}

LinkManager::ConnectionProfile requiredProfile(const QString &id)
{
    LinkManager::ConnectionProfile profile;
    profile.id = id;
    profile.signingRequired = true;
    return profile;
}

bool saveRequiredProfile(const QString &id, const QByteArray &fingerprint,
                         QString *error = nullptr)
{
    QSettings settings;
    return MavlinkSigningProfiles::saveRequired(
        settings, id, fingerprint, error);
}

quint16 unusedUdpPort()
{
    QUdpSocket socket;
    if (!socket.bind(QHostAddress::AnyIPv4, 0,
                     QUdpSocket::DontShareAddress)) {
        return 0;
    }
    return socket.localPort();
}

quint16 unusedTcpPort()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    return server.serverPort();
}

void replaceStoredUdpLinks(const QList<StoredUdpDefinition> &definitions)
{
    QSettings settings;
    settings.remove(QStringLiteral("LINKMANAGER"));
    settings.remove(QLatin1String(PlannerStartupUdpOptions::EnabledSettingKey));
    settings.remove(QLatin1String(PlannerStartupUdpOptions::PrimaryPortSettingKey));
    settings.remove(QLatin1String(PlannerStartupUdpOptions::AlternatePortSettingKey));
    settings.remove(QStringLiteral("MAVLinkSigning/StartupRequiredPorts"));
    settings.beginGroup(QStringLiteral("LINKMANAGER"));
    settings.beginWriteArray(QStringLiteral("LINKS"));
    for (int index = 0; index < definitions.size(); ++index) {
        const StoredUdpDefinition &definition = definitions.at(index);
        settings.setArrayIndex(index);
        settings.setValue(QStringLiteral("type"),
                          QStringLiteral("UDP_LINK"));
        settings.setValue(QStringLiteral("port"), definition.port);
        if (definition.includeIdentity) {
            settings.setValue(QStringLiteral("profileId"),
                              definition.profileId);
        }
        if (definition.includeRequirement) {
            settings.setValue(QStringLiteral("signingRequired"),
                              definition.signingRequired);
        }
    }
    settings.endArray();
    settings.endGroup();
    settings.sync();
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

mavlink_message_t autopilotHeartbeatMessage(quint8 systemId, bool armed,
                                             quint8 autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA)
{
    mavlink_message_t message{};
    mavlink_msg_heartbeat_pack(
        systemId, MAV_COMP_ID_AUTOPILOT1, &message, MAV_TYPE_QUADROTOR,
        autopilot,
        armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
        0, MAV_STATE_ACTIVE);
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

struct NewLinkSnapshot
{
    int id = -1;
    LinkInterface::LinkType type = LinkInterface::UNKNOWN_LINK;
    LinkManager::ConnectionProfile profile;
    bool connected = false;
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

    const QByteArray key = signingKey();
    const QByteArray fingerprint = keyFingerprint(key);
    QString profileError;
    result.expect(saveRequiredProfile(PrimaryProfileId, fingerprint,
                                      &profileError),
                  QStringLiteral("primary required profile could not be staged: %1")
                      .arg(profileError));
    profileError.clear();
    result.expect(saveRequiredProfile(SecondaryProfileId, fingerprint,
                                      &profileError),
                  QStringLiteral("secondary required profile could not be staged: %1")
                      .arg(profileError));

    QPointer<AuditLink> firstLink(new AuditLink(FirstAuditLinkId));
    // This runtime-only function is a narrow friend of LinkManagerFactory so
    // the fake exercises the same queued ingress and typed lifecycle wiring as
    // every production transport without widening the public factory API.
    LinkManagerFactory::connectLinkSignals(firstLink.data(), links);
    {
        const QSignalBlocker blockManagerSignals(links);
        links->addLink(firstLink.data(), requiredProfile(PrimaryProfileId));
    }
    LinkManager::ConnectionProfile firstProfile =
        links->connectionProfile(FirstAuditLinkId);
    result.expect(links->getLink(FirstAuditLinkId) == firstLink.data(),
                  QStringLiteral("required fake link was not registered"));
    result.expect(firstProfile.id == PrimaryProfileId
                      && firstProfile.signingRequired
                      && firstProfile.error.isEmpty()
                      && links->signingRequired(FirstAuditLinkId)
                      && !links->signingReady(FirstAuditLinkId),
                  QStringLiteral("persisted requirement was not restored locked"));
    const quint64 firstAddedRevision = firstProfile.revision;
    result.expect(firstAddedRevision != 0,
                  QStringLiteral("new connection profile has a zero runtime revision"));

    // Profile identity is persistent, while revision is deliberately a
    // process-local fence for asynchronous key selection. Even an offline
    // endpoint edit must invalidate a callback that captured the old shape.
    links->linkUpdated(firstLink.data());
    firstProfile = links->connectionProfile(FirstAuditLinkId);
    result.expect(firstProfile.id == PrimaryProfileId
                      && firstProfile.revision != 0
                      && firstProfile.revision != firstAddedRevision,
                  QStringLiteral("offline link edit changed identity or retained its runtime revision"));
    result.expect(links->currentPhysicalLinkSession(FirstAuditLinkId) == 0,
                  QStringLiteral("locked fake link acquired a physical epoch"));
    result.expect(!links->connectLink(FirstAuditLinkId)
                      && firstLink && !firstLink->isConnected(),
                  QStringLiteral("locked required link opened before key activation"));
    result.expect(firstLink && !links->writeRawBytes(
                                  FirstAuditLinkId,
                                  QByteArrayLiteral("locked raw secret"))
                      && firstLink->writes().isEmpty(),
                  QStringLiteral("locked required link admitted a raw write"));

    QString initialActivationError;
    result.expect(links->configureSigning(
                      FirstAuditLinkId, firstProfile.id,
                      QStringLiteral("Runtime Audit Pre-removal Key"), key,
                      &initialActivationError)
                      && links->signingReady(FirstAuditLinkId),
                  QStringLiteral("initial offline profile activation failed: %1")
                      .arg(initialActivationError));

    quint64 revisionBeforeRemoval = firstProfile.revision;
    if (links->signingReady(FirstAuditLinkId) && firstLink) {
        const bool connected = links->connectLink(FirstAuditLinkId);
        LinkManager::ConnectionProfile lifecycleProfile =
            links->connectionProfile(FirstAuditLinkId);
        result.expect(connected && firstLink->isConnected()
                          && lifecycleProfile.id == PrimaryProfileId
                          && lifecycleProfile.revision != revisionBeforeRemoval,
                      QStringLiteral("connect did not preserve identity and advance runtime revision"));
        const quint64 connectedRevision = lifecycleProfile.revision;

        links->disconnectLink(FirstAuditLinkId);
        lifecycleProfile = links->connectionProfile(FirstAuditLinkId);
        result.expect(!firstLink->isConnected()
                          && lifecycleProfile.id == PrimaryProfileId
                          && lifecycleProfile.revision != connectedRevision,
                      QStringLiteral("disconnect did not preserve identity and advance runtime revision"));
        const quint64 disconnectedRevision = lifecycleProfile.revision;

        const bool reconnected = links->connectLink(FirstAuditLinkId);
        lifecycleProfile = links->connectionProfile(FirstAuditLinkId);
        result.expect(reconnected && firstLink->isConnected()
                          && lifecycleProfile.id == PrimaryProfileId
                          && lifecycleProfile.revision != disconnectedRevision,
                      QStringLiteral("reconnect did not preserve identity and advance runtime revision"));

        // Leave the fixture offline for the pre-existing removal/restore
        // policy audit. This final disconnect is itself another revision
        // boundary and is the value against which re-addition is checked.
        links->disconnectLink(FirstAuditLinkId);
        lifecycleProfile = links->connectionProfile(FirstAuditLinkId);
        revisionBeforeRemoval = lifecycleProfile.revision;
    }

    // Removing a transient link must not erase its stable policy or retain an
    // active key implicitly. Re-adding the same connection profile is locked
    // again until an operator supplies the matching key while it is offline.
    {
        const QSignalBlocker blockManagerSignals(links);
        links->removeLink(FirstAuditLinkId);
    }
    result.expect(firstLink.isNull(),
                  QStringLiteral("first locked fake link was not deleted"));
    firstLink = new AuditLink(FirstAuditLinkId);
    LinkManagerFactory::connectLinkSignals(firstLink.data(), links);
    {
        const QSignalBlocker blockManagerSignals(links);
        links->addLink(firstLink.data(), requiredProfile(PrimaryProfileId));
    }
    firstProfile = links->connectionProfile(FirstAuditLinkId);
    result.expect(firstLink && firstProfile.id == PrimaryProfileId
                      && firstProfile.revision != 0
                      && firstProfile.revision != revisionBeforeRemoval
                      && firstProfile.signingRequired
                      && firstProfile.error.isEmpty()
                      && links->signingRequired(FirstAuditLinkId)
                      && !links->signingReady(FirstAuditLinkId),
                  QStringLiteral("re-added persisted profile reused a key without activation"));

    QObject lockedIngressScope;
    int lockedPackets = 0;
    int lockedFrames = 0;
    int lockedObserved = 0;
    QObject::connect(
        protocol, &MAVLinkProtocol::packetReceived, &lockedIngressScope,
        [&](LinkInterface *link, mavlink_message_t) {
            if (link && link->getId() == FirstAuditLinkId) ++lockedPackets;
        });
    QObject::connect(
        protocol, &MAVLinkProtocol::frameReceived, &lockedIngressScope,
        [&](int linkId, const QByteArray &) {
            if (linkId == FirstAuditLinkId) ++lockedFrames;
        });
    QObject::connect(
        links, &LinkManager::mavlinkMessageObserved, &lockedIngressScope,
        [&](int linkId, qulonglong, mavlink_message_t) {
            if (linkId == FirstAuditLinkId) ++lockedObserved;
        });
    // Exercise defense in depth against a transport reconnect which bypasses
    // connectLink(). It must be closed before an epoch or parser fan-out can
    // arise; otherwise a transport-owned retry could evade the startup gate.
    firstLink->connect();
    processFor(1);
    if (firstLink) {
        const QByteArray lockedFrame = nativeFrame(
            namedValueMessage(241, 154, 7), 241, 154, 17, false, key, 71,
            currentSigningTimestamp());
        firstLink->inject(lockedFrame);
    }
    processFor();
    result.expect(firstLink && !firstLink->isConnected()
                      && links->currentPhysicalLinkSession(FirstAuditLinkId) == 0,
                  QStringLiteral("direct reconnect bypass left a locked transport open"));
    result.expect(lockedPackets == 0 && lockedFrames == 0
                      && lockedObserved == 0,
                  QStringLiteral("locked restored profile admitted inbound MAVLink"));

    QString signingError;
    const QByteArray wrongKey = alternateSigningKey();
    result.expect(!links->configureSigning(
                      FirstAuditLinkId, firstProfile.id,
                      QStringLiteral("Runtime Audit Wrong Key"), wrongKey,
                      &signingError)
                      && !signingError.isEmpty()
                      && links->signingRequired(FirstAuditLinkId)
                      && !links->signingReady(FirstAuditLinkId)
                      && !links->connectLink(FirstAuditLinkId),
                  QStringLiteral("wrong key did not leave the restored profile locked"));
    signingError.clear();
    const bool firstConfigured = links->configureSigning(
        FirstAuditLinkId, firstProfile.id,
        QStringLiteral("Runtime Audit Shared Key"), key, &signingError);
    result.expect(firstConfigured,
                  QStringLiteral("first offline signing configuration failed: %1")
                      .arg(signingError));

    QPointer<AuditLink> secondLink(new AuditLink(SecondAuditLinkId));
    LinkManagerFactory::connectLinkSignals(secondLink.data(), links);
    {
        const QSignalBlocker blockManagerSignals(links);
        links->addLink(secondLink.data(), requiredProfile(SecondaryProfileId));
    }
    const LinkManager::ConnectionProfile secondProfile =
        links->connectionProfile(SecondAuditLinkId);
    result.expect(secondProfile.id == SecondaryProfileId
                      && secondProfile.signingRequired
                      && secondProfile.error.isEmpty()
                      && links->signingRequired(SecondAuditLinkId)
                      && !links->signingReady(SecondAuditLinkId),
                  QStringLiteral("second persisted requirement was not restored locked"));
    signingError.clear();
    const bool secondConfigured = links->configureSigning(
        SecondAuditLinkId, secondProfile.id,
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
                      FirstAuditLinkId, firstProfile.id,
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

    // SETUP_SIGNING carries the key itself. Generic typed paths are not an
    // authorization boundary: both must reject it before sequence allocation,
    // signing, transport, or observer publication. Only LinkManager's checked
    // provisioning operation may invoke the private secret transport path.
    const mavlink_message_t outboundSetup = setupSigningMessage(
        localSystemId, localComponentId, 42, 1, key,
        currentSigningTimestamp());
    const int setupWriteBaseline = firstLink->writes().size();
    const int managerSubmissionBaseline = submittedFrames.size();
    const int exactSubmissionBaseline = exactSubmittedCount;
    bool setupWriterInvoked = true;
    result.expect(transmitter->sendMessage(
                      FirstAuditLinkId, localSystemId, localComponentId,
                      outboundSetup, &setupWriterInvoked)
                      == ExactLinkTransmitter::SendResult::RestrictedMessage
                      && !setupWriterInvoked
                      && !links->writeMavlinkMessage(
                          firstLink.data(), outboundSetup),
                  QStringLiteral("a generic typed path admitted SETUP_SIGNING"));
    result.expect(firstLink->writes().size() == setupWriteBaseline
                      && submittedFrames.size() == managerSubmissionBaseline
                      && exactSubmittedCount == exactSubmissionBaseline,
                  QStringLiteral("rejected SETUP_SIGNING reached transport or observers"));

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

    // Removing the last link retires a UAS synchronously from UASManager but
    // destroys it later. A new heartbeat in that precise window must create a
    // fresh registered UAS, rather than attaching the successor link to the
    // old object which is already queued for deletion.
    quint8 rediscoverySystemId = 238;
    while (rediscoverySystemId > 1 && links->getUas(rediscoverySystemId)) {
        --rediscoverySystemId;
    }
    UASManager *const uasManager = UASManager::instance();
    QPointer<AuditLink> retiredLink(new AuditLink(RetiredUasAuditLinkId));
    LinkManagerFactory::connectLinkSignals(retiredLink.data(), links);
    LinkManager::ConnectionProfile retiredProfile;
    retiredProfile.id = RetiredUasProfileId;
    {
        const QSignalBlocker blockManagerSignals(links);
        links->addLink(retiredLink.data(), retiredProfile);
    }
    result.expect(links->connectLink(RetiredUasAuditLinkId),
                  QStringLiteral("retired-UAS audit link did not connect"));
    retiredLink->inject(nativeFrame(
        autopilotHeartbeatMessage(rediscoverySystemId, false),
        rediscoverySystemId, MAV_COMP_ID_AUTOPILOT1, 31, false));
    result.expect(waitUntil([&]() {
                      return links->getUas(rediscoverySystemId) != nullptr;
                  }),
                  QStringLiteral("first lifecycle heartbeat did not create a UAS"));
    QPointer<UASInterface> retiredUas(links->getUas(rediscoverySystemId));
    QPointer<UASObject> retiredObject(
        links->getUasObject(rediscoverySystemId));
    result.expect(retiredUas && retiredObject
                      && uasManager->getUASList().contains(retiredUas.data()),
                  QStringLiteral("first lifecycle UAS was not fully registered"));

    {
        const QSignalBlocker blockManagerSignals(links);
        links->removeLink(RetiredUasAuditLinkId);
    }
    result.expect(retiredLink.isNull() && retiredUas
                      && !uasManager->getUASList().contains(retiredUas.data())
                      && links->getUas(rediscoverySystemId) == nullptr
                      && retiredObject.isNull()
                      && links->getUasObject(rediscoverySystemId) == nullptr,
                  QStringLiteral("logical UAS retirement waited for deferred destruction"));

    QPointer<AuditLink> rediscoveredLink(
        new AuditLink(RediscoveredUasAuditLinkId));
    LinkManagerFactory::connectLinkSignals(rediscoveredLink.data(), links);
    LinkManager::ConnectionProfile rediscoveredProfile;
    rediscoveredProfile.id = RediscoveredUasProfileId;
    {
        const QSignalBlocker blockManagerSignals(links);
        links->addLink(rediscoveredLink.data(), rediscoveredProfile);
    }
    result.expect(links->connectLink(RediscoveredUasAuditLinkId),
                  QStringLiteral("rediscovery audit link did not connect"));
    rediscoveredLink->inject(nativeFrame(
        autopilotHeartbeatMessage(rediscoverySystemId, false),
        rediscoverySystemId, MAV_COMP_ID_AUTOPILOT1, 32, false));
    // Factory ingress is deliberately queued. Deliver that exact receiver's
    // metacalls, but leave the old UAS DeferredDelete pending for this check.
    QCoreApplication::sendPostedEvents(links, QEvent::MetaCall);
    UASInterface *const rediscoveredRaw = links->getUas(rediscoverySystemId);
    QPointer<UASInterface> rediscoveredUas(rediscoveredRaw);
    QPointer<UASObject> rediscoveredObject(
        links->getUasObject(rediscoverySystemId));
    result.expect(retiredUas && rediscoveredUas
                      && rediscoveredRaw != retiredUas.data()
                      && rediscoveredObject
                      && uasManager->getUASList().contains(rediscoveredRaw)
                      && !uasManager->getUASList().contains(retiredUas.data()),
                  QStringLiteral("immediate heartbeat reused the deferred-delete UAS"));

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    result.expect(retiredUas.isNull() && rediscoveredUas
                      && links->getUas(rediscoverySystemId)
                          == rediscoveredUas.data()
                      && links->getUasObject(rediscoverySystemId)
                          == rediscoveredObject.data(),
                  QStringLiteral("old deferred destruction erased its replacement"));
    {
        const QSignalBlocker blockManagerSignals(links);
        links->removeLink(RediscoveredUasAuditLinkId);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    result.expect(rediscoveredLink.isNull() && rediscoveredUas.isNull()
                      && rediscoveredObject.isNull()
                      && links->getUas(rediscoverySystemId) == nullptr
                      && !uasManager->getUASList().contains(rediscoveredRaw),
                  QStringLiteral("rediscovered lifecycle fixture did not clean up"));

    VehicleTargetManager *const targetManager = links->vehicleTargetManager();
    SwarmTelemetryRegistry *const swarmRegistry =
        links->swarmTelemetryRegistry();
    result.expect(targetManager != nullptr && swarmRegistry != nullptr,
                  QStringLiteral("signing provisioning target services are missing"));

    quint8 provisioningSystemId = 226;
    while (provisioningSystemId > 2
           && links->getUas(provisioningSystemId)) {
        --provisioningSystemId;
    }
    quint8 signedCandidateSystemId = quint8(provisioningSystemId - 1);
    while (signedCandidateSystemId > 1
           && links->getUas(signedCandidateSystemId)) {
        --signedCandidateSystemId;
    }

    // A valid signed heartbeat on an otherwise unprotected live link is
    // positive evidence that the vehicle may already hold a key. Initial
    // provisioning must refuse it instead of overwriting an unknown key.
    QPointer<ProvisioningAuditLink> signedCandidate(
        new ProvisioningAuditLink(SignedProvisioningAuditLinkId));
    LinkManager::ConnectionProfile signedCandidateProfile;
    signedCandidateProfile.id = SignedProvisioningProfileId;
    LinkManagerFactory::connectLinkSignals(signedCandidate.data(), links);
    links->addLink(signedCandidate.data(), signedCandidateProfile);
    result.expect(links->connectLink(SignedProvisioningAuditLinkId),
                  QStringLiteral("signed provisioning-refusal fixture did not connect"));
    const quint64 signedCandidateEpoch =
        links->currentPhysicalLinkSession(SignedProvisioningAuditLinkId);
    if (signedCandidate) {
        signedCandidate->inject(nativeFrame(
            autopilotHeartbeatMessage(signedCandidateSystemId, false),
            signedCandidateSystemId, MAV_COMP_ID_AUTOPILOT1, 89, false));
    }
    SigningProvisioningTarget unsignedCandidateTarget;
    QString provisioningError;
    provisioningError.clear();
    result.expect(waitUntil([&]() {
                      provisioningError.clear();
                      return links->prepareSigningProvisioning(
                          SignedProvisioningAuditLinkId,
                          &unsignedCandidateTarget, &provisioningError);
                  }) && unsignedCandidateTarget.isValid(),
                  QStringLiteral("fresh unsigned candidate was not initially eligible: %1")
                      .arg(provisioningError));

    QObject signedCandidateObservationScope;
    bool signedCandidateObserved = false;
    bool signedCandidateRefusedInsideObserver = false;
    QString signedCandidateObserverError;
    QObject::connect(
        links, &LinkManager::mavlinkMessageObserved,
        &signedCandidateObservationScope,
        [&](int linkId, qulonglong, mavlink_message_t message) {
            if (linkId != SignedProvisioningAuditLinkId
                || message.msgid != MAVLINK_MSG_ID_HEARTBEAT
                || !(message.incompat_flags & MAVLINK_IFLAG_SIGNED)) {
                return;
            }
            signedCandidateObserved = true;
            SigningProvisioningTarget nestedTarget;
            signedCandidateRefusedInsideObserver =
                !links->prepareSigningProvisioning(
                    SignedProvisioningAuditLinkId, &nestedTarget,
                    &signedCandidateObserverError)
                && !nestedTarget.isValid()
                && !signedCandidateObserverError.isEmpty();
        });
    const QByteArray signedCandidateHeartbeat = nativeFrame(
        autopilotHeartbeatMessage(signedCandidateSystemId, false),
        signedCandidateSystemId, MAV_COMP_ID_AUTOPILOT1, 90, false,
        alternateSigningKey(), 27, currentSigningTimestamp());
    if (signedCandidate) signedCandidate->inject(signedCandidateHeartbeat);
    result.expect(waitUntil([&]() {
                      return signedCandidateObserved;
                  }),
                  QStringLiteral("signed provisioning-refusal heartbeat was not observed"));
    SigningProvisioningTarget signedCandidateTarget;
    const int signedCandidateWriteBaseline =
        signedCandidate ? signedCandidate->writes().size() : 0;
    provisioningError.clear();
    result.expect(signedCandidateEpoch != 0
                      && signedCandidateRefusedInsideObserver
                      && !links->prepareSigningProvisioning(
                          SignedProvisioningAuditLinkId,
                          &signedCandidateTarget, &provisioningError)
                      && !signedCandidateTarget.isValid()
                      && !provisioningError.isEmpty()
                      && signedCandidate
                      && signedCandidate->writes().size()
                          == signedCandidateWriteBaseline,
                  QStringLiteral("observed signed heartbeat did not block initial provisioning before public fan-out"));
    links->removeLink(SignedProvisioningAuditLinkId);
    result.expect(signedCandidate.isNull()
                      && links->getLink(SignedProvisioningAuditLinkId) == nullptr,
                  QStringLiteral("signed provisioning-refusal fixture was not removed"));

    // Exercise the only secret-bearing production path with an actual
    // TCPLink subtype. Socket I/O is overridden, so this remains a fully
    // in-process audit while LinkManager's persistence downcasts stay valid.
    QPointer<ProvisioningAuditLink> provisioningLink(
        new ProvisioningAuditLink(ProvisioningAuditLinkId));
    LinkManager::ConnectionProfile provisioningProfile;
    provisioningProfile.id = ProvisioningProfileId;
    LinkManagerFactory::connectLinkSignals(provisioningLink.data(), links);
    links->addLink(provisioningLink.data(), provisioningProfile);
    result.expect(links->connectLink(ProvisioningAuditLinkId),
                  QStringLiteral("initial provisioning fixture did not connect"));
    quint64 provisioningEpoch =
        links->currentPhysicalLinkSession(ProvisioningAuditLinkId);
    result.expect(provisioningEpoch != 0,
                  QStringLiteral("initial provisioning fixture lacks a physical epoch"));

    const auto injectProvisioningHeartbeat =
        [&](quint8 systemId, bool armed, quint8 sequence,
            quint8 autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA) {
            if (!provisioningLink) return;
            provisioningLink->inject(nativeFrame(
                autopilotHeartbeatMessage(systemId, armed, autopilot),
                systemId, MAV_COMP_ID_AUTOPILOT1, sequence, false));
        };
    injectProvisioningHeartbeat(provisioningSystemId, false, 91);
    result.expect(waitUntil([&]() {
                      const VehicleTargetLease selected =
                          targetManager->acquireTarget();
                      const VehicleEndpoint endpoint{
                          ProvisioningAuditLinkId, provisioningSystemId,
                          MAV_COMP_ID_AUTOPILOT1, {}, {}};
                      return selected.isValid()
                          && selected.endpoint.sameIdentity(endpoint)
                          && swarmRegistry->acquireVehicle(endpoint, 3000)
                              .isValid();
                  }),
                  QStringLiteral("initial provisioning vehicle was not discovered"));

    SigningProvisioningTarget preRadioTarget;
    provisioningError.clear();
    result.expect(links->prepareSigningProvisioning(
                      ProvisioningAuditLinkId, &preRadioTarget,
                      &provisioningError)
                      && preRadioTarget.isValid(),
                  QStringLiteral("fresh private route was not eligible before radio evidence: %1")
                      .arg(provisioningError));
    if (provisioningLink) {
        provisioningLink->inject(nativeFrame(
            radioStatusMessage(), static_cast<quint8>('3'),
            static_cast<quint8>('D'), 90, true));
    }
    processFor();
    SigningProvisioningTarget radioTarget;
    const int radioProvisionWriteBaseline =
        provisioningLink ? provisioningLink->writes().size() : 0;
    provisioningError.clear();
    result.expect(!links->prepareSigningProvisioning(
                      ProvisioningAuditLinkId, &radioTarget,
                      &provisioningError)
                      && !radioTarget.isValid()
                      && !provisioningError.isEmpty()
                      && provisioningLink
                      && provisioningLink->writes().size()
                          == radioProvisionWriteBaseline,
                  QStringLiteral("observed radio/router traffic did not block cleartext provisioning"));
    links->disconnectLink(ProvisioningAuditLinkId);
    result.expect(links->connectLink(ProvisioningAuditLinkId),
                  QStringLiteral("radio-refusal fixture did not reconnect for a clean epoch"));
    const quint64 postRadioEpoch =
        links->currentPhysicalLinkSession(ProvisioningAuditLinkId);
    result.expect(postRadioEpoch != 0 && postRadioEpoch != provisioningEpoch,
                  QStringLiteral("radio evidence survived without a physical epoch boundary"));
    provisioningEpoch = postRadioEpoch;
    injectProvisioningHeartbeat(provisioningSystemId, false, 91);
    result.expect(waitUntil([&]() {
                      const VehicleTargetLease selected =
                          targetManager->acquireTarget();
                      return selected.isValid()
                          && selected.endpoint.linkId
                              == ProvisioningAuditLinkId
                          && selected.endpoint.systemId
                              == provisioningSystemId;
                  }),
                  QStringLiteral("provisioning target did not return after radio epoch retirement"));

    SigningProvisioningTarget staleTarget;
    processFor(3100);
    // Routine GCS heartbeat/mission timers may write while time is advanced;
    // only the preparation call itself must be side-effect-free.
    const int staleWriteBaseline =
        provisioningLink ? provisioningLink->writes().size() : 0;
    provisioningError.clear();
    result.expect(!links->prepareSigningProvisioning(
                      ProvisioningAuditLinkId, &staleTarget,
                      &provisioningError)
                      && !staleTarget.isValid()
                      && !provisioningError.isEmpty()
                      && provisioningLink
                      && provisioningLink->writes().size()
                          == staleWriteBaseline,
                  QStringLiteral("stale heartbeat authorized signing provisioning"));

    injectProvisioningHeartbeat(provisioningSystemId, true, 92);
    processFor();
    SigningProvisioningTarget armedTarget;
    const int armedWriteBaseline =
        provisioningLink ? provisioningLink->writes().size() : 0;
    provisioningError.clear();
    result.expect(!links->prepareSigningProvisioning(
                      ProvisioningAuditLinkId, &armedTarget,
                      &provisioningError)
                      && !armedTarget.isValid()
                      && !provisioningError.isEmpty()
                      && provisioningLink
                      && provisioningLink->writes().size()
                          == armedWriteBaseline,
                  QStringLiteral("armed heartbeat authorized signing provisioning"));

    injectProvisioningHeartbeat(provisioningSystemId, false, 93);
    processFor();
    SigningProvisioningTarget selectionFence;
    provisioningError.clear();
    result.expect(links->prepareSigningProvisioning(
                      ProvisioningAuditLinkId, &selectionFence,
                      &provisioningError)
                      && selectionFence.isValid(),
                  QStringLiteral("fresh disarmed exact target was not provisionable: %1")
                      .arg(provisioningError));
    targetManager->clearTarget();
    const int targetFenceWriteBaseline =
        provisioningLink ? provisioningLink->writes().size() : 0;
    provisioningError.clear();
    result.expect(!links->provisionSigning(
                      selectionFence, QStringLiteral("Stale consent"),
                      alternateSigningKey(), &provisioningError)
                      && !provisioningError.isEmpty()
                      && provisioningLink
                      && provisioningLink->writes().size()
                          == targetFenceWriteBaseline,
                  QStringLiteral("stale target-generation consent sent a key"));
    result.expect(targetManager->selectTarget(
                      ProvisioningAuditLinkId, provisioningSystemId,
                      MAV_COMP_ID_AUTOPILOT1),
                  QStringLiteral("provisioning exact target could not be reselected"));

    // A second command-capable autopilot on the same physical link makes the
    // secret route ambiguous even though the UI still has one selected row.
    quint8 duplicateSystemId = quint8(provisioningSystemId + 1);
    if (duplicateSystemId == 0 || duplicateSystemId == 255)
        duplicateSystemId = quint8(provisioningSystemId - 2);
    injectProvisioningHeartbeat(duplicateSystemId, false, 94);
    processFor();
    SigningProvisioningTarget duplicateTarget;
    const int duplicateWriteBaseline =
        provisioningLink ? provisioningLink->writes().size() : 0;
    provisioningError.clear();
    result.expect(!links->prepareSigningProvisioning(
                      ProvisioningAuditLinkId, &duplicateTarget,
                      &provisioningError)
                      && !duplicateTarget.isValid()
                      && !provisioningError.isEmpty()
                      && provisioningLink
                      && provisioningLink->writes().size()
                          == duplicateWriteBaseline,
                  QStringLiteral("multi-autopilot route authorized a secret write"));
    injectProvisioningHeartbeat(duplicateSystemId, false, 95,
                                MAV_AUTOPILOT_INVALID);
    result.expect(waitUntil([&]() {
                      const VehicleEndpoint duplicateEndpoint{
                          ProvisioningAuditLinkId, duplicateSystemId,
                          MAV_COMP_ID_AUTOPILOT1, {}, {}};
                      return !swarmRegistry->acquireVehicle(
                                  duplicateEndpoint, 3000)
                                  .isValid();
                  }),
                  QStringLiteral("non-command-capable duplicate was not retired"));
    injectProvisioningHeartbeat(provisioningSystemId, false, 96);
    processFor();

    SigningProvisioningTarget revisionFence;
    provisioningError.clear();
    result.expect(links->prepareSigningProvisioning(
                      ProvisioningAuditLinkId, &revisionFence,
                      &provisioningError)
                      && revisionFence.isValid(),
                  QStringLiteral("provisioning target could not be captured before edit: %1")
                      .arg(provisioningError));
    links->linkUpdated(provisioningLink.data());
    const int revisionWriteBaseline =
        provisioningLink ? provisioningLink->writes().size() : 0;
    provisioningError.clear();
    result.expect(!links->provisionSigning(
                      revisionFence, QStringLiteral("Stale revision"),
                      alternateSigningKey(), &provisioningError)
                      && !provisioningError.isEmpty()
                      && provisioningLink
                      && provisioningLink->writes().size()
                          == revisionWriteBaseline,
                  QStringLiteral("stale connection-revision consent sent a key"));

    SigningProvisioningTarget provisionTarget;
    provisioningError.clear();
    result.expect(links->prepareSigningProvisioning(
                      ProvisioningAuditLinkId, &provisionTarget,
                      &provisioningError)
                      && provisionTarget.isValid()
                      && provisionTarget.linkSessionEpoch == provisioningEpoch
                      && provisionTarget.systemId == provisioningSystemId
                      && provisionTarget.componentId
                          == MAV_COMP_ID_AUTOPILOT1,
                  QStringLiteral("final signing provisioning target was invalid: %1")
                      .arg(provisioningError));

    QObject provisioningSignalScope;
    int provisionManagerSubmissions = 0;
    int provisionExactSubmissions = 0;
    int provisionPackets = 0;
    int provisionFrames = 0;
    int provisionObserved = 0;
    QObject::connect(
        links, &LinkManager::mavlinkMessageSubmitted,
        &provisioningSignalScope,
        [&](int linkId, qulonglong, mavlink_message_t) {
            if (linkId == ProvisioningAuditLinkId)
                ++provisionManagerSubmissions;
        });
    QObject::connect(
        transmitter, &ExactLinkTransmitter::messageSubmitted,
        &provisioningSignalScope,
        [&](int linkId, quint64, mavlink_message_t) {
            if (linkId == ProvisioningAuditLinkId)
                ++provisionExactSubmissions;
        });
    QObject::connect(
        protocol, &MAVLinkProtocol::packetReceived,
        &provisioningSignalScope,
        [&](LinkInterface *link, mavlink_message_t) {
            if (link && link->getId() == ProvisioningAuditLinkId)
                ++provisionPackets;
        });
    QObject::connect(
        protocol, &MAVLinkProtocol::frameReceived,
        &provisioningSignalScope,
        [&](int linkId, const QByteArray &) {
            if (linkId == ProvisioningAuditLinkId) ++provisionFrames;
        });
    QObject::connect(
        links, &LinkManager::mavlinkMessageObserved,
        &provisioningSignalScope,
        [&](int linkId, qulonglong, mavlink_message_t) {
            if (linkId == ProvisioningAuditLinkId) ++provisionObserved;
        });

    const QByteArray provisionedKey = alternateSigningKey();
    const int provisionWriteBaseline =
        provisioningLink ? provisioningLink->writes().size() : 0;
    provisioningError.clear();
    const bool provisionSubmitted = links->provisionSigning(
        provisionTarget, QStringLiteral("Runtime Audit Provisioned Key"),
        provisionedKey, &provisioningError);
    result.expect(provisionSubmitted && !provisioningError.isEmpty(),
                  QStringLiteral("valid initial provisioning was not submitted-unconfirmed: %1")
                      .arg(provisioningError));
    result.expect(provisioningLink
                      && provisioningLink->writes().size()
                          == provisionWriteBaseline + 1,
                  QStringLiteral("initial provisioning did not write exactly one frame"));
    result.expect(provisionManagerSubmissions == 0
                      && provisionExactSubmissions == 0,
                  QStringLiteral("secret provisioning frame escaped through outbound observers"));

    QByteArray provisionWire;
    if (provisioningLink
        && provisioningLink->writes().size() == provisionWriteBaseline + 1) {
        provisionWire = provisioningLink->writes().at(provisionWriteBaseline);
    }
    mavlink_message_t decodedProvision{};
    mavlink_setup_signing_t decodedSetup{};
    const bool provisionWireDecoded = decodeFrame(
        provisionWire, &decodedProvision);
    if (provisionWireDecoded
        && decodedProvision.msgid == MAVLINK_MSG_ID_SETUP_SIGNING) {
        mavlink_msg_setup_signing_decode(&decodedProvision, &decodedSetup);
    }
    result.expect(provisionWireDecoded && isSignedMavlink2(provisionWire)
                      && decodedProvision.msgid
                          == MAVLINK_MSG_ID_SETUP_SIGNING
                      && decodedSetup.target_system == provisioningSystemId
                      && decodedSetup.target_component
                          == MAV_COMP_ID_AUTOPILOT1
                      && decodedSetup.initial_timestamp != 0
                      && decodedSetup.initial_timestamp
                          <= frameSigningTimestamp(provisionWire)
                      && std::memcmp(decodedSetup.secret_key,
                                     provisionedKey.constData(), 32) == 0
                      && nativeVerifierAccepts({provisionWire}, provisionedKey),
                  QStringLiteral("submitted provisioning wire was not the exact signed target/key frame"));

    const auto provisionStatus =
        links->signingManager()->status(ProvisioningAuditLinkId);
    const auto provisionProfile =
        links->connectionProfile(ProvisioningAuditLinkId);
    QSettings provisionSettings;
    const auto persistedProvision = MavlinkSigningProfiles::load(
        provisionSettings, ProvisioningProfileId, true);
    result.expect(provisionProfile.id == ProvisioningProfileId
                      && provisionProfile.signingRequired
                      && provisionProfile.provisioningUnconfirmed
                      && provisionProfile.error.isEmpty()
                      && links->signingRequired(ProvisioningAuditLinkId)
                      && links->signingReady(ProvisioningAuditLinkId)
                      && provisionStatus.protectedLink
                      && provisionStatus.keyAvailable
                      && provisionStatus.activeEpoch == provisioningEpoch
                      && provisionStatus.connectionProfileId
                          == ProvisioningProfileId
                      && provisionStatus.keyFingerprint
                          == QString::fromLatin1(
                              keyFingerprint(provisionedKey).toHex())
                      && persistedProvision.required
                      && persistedProvision.provisioningUnconfirmed
                      && persistedProvision.error.isEmpty()
                      && persistedProvision.fingerprint
                          == keyFingerprint(provisionedKey),
                  QStringLiteral("submitted-unconfirmed provisioning state was not fail-closed and persisted"));

    const auto protectedIngress = [&]() {
        return IngressSnapshot{provisionPackets, provisionFrames,
                               provisionObserved, 0};
    };
    IngressSnapshot provisionBefore = protectedIngress();
    const auto provisionCountersBeforeUnsigned = provisionStatus.counters;
    injectProvisioningHeartbeat(provisioningSystemId, false, 97);
    processFor();
    result.expect(protectedIngress() == provisionBefore
                      && links->signingManager()
                                 ->status(ProvisioningAuditLinkId)
                                 .counters.rejected
                          > provisionCountersBeforeUnsigned.rejected,
                  QStringLiteral("unsigned traffic was accepted after provisioning submission"));

    const QByteArray authenticatedProvisionHeartbeat = nativeFrame(
        autopilotHeartbeatMessage(provisioningSystemId, false),
        provisioningSystemId, MAV_COMP_ID_AUTOPILOT1, 98, false,
        provisionedKey, 41,
        decodedSetup.initial_timestamp + 6000000ULL);
    if (provisioningLink)
        provisioningLink->inject(authenticatedProvisionHeartbeat);
    result.expect(waitUntil([&]() {
                      const IngressSnapshot now = protectedIngress();
                      return now.packets == provisionBefore.packets + 1
                          && now.frames == provisionBefore.frames + 1
                          && now.observed == provisionBefore.observed + 1;
                  }),
                  QStringLiteral("newly provisioned key did not authenticate vehicle traffic"));

    SigningProvisioningTarget repeatTarget;
    const int repeatWriteBaseline =
        provisioningLink ? provisioningLink->writes().size() : 0;
    provisioningError.clear();
    result.expect(!links->prepareSigningProvisioning(
                      ProvisioningAuditLinkId, &repeatTarget,
                      &provisioningError)
                      && !repeatTarget.isValid()
                      && !links->provisionSigning(
                          provisionTarget,
                          QStringLiteral("Runtime Audit Provisioned Key"),
                          provisionedKey, nullptr)
                      && provisioningLink
                      && provisioningLink->writes().size()
                          == repeatWriteBaseline,
                  QStringLiteral("initial provisioning admitted a second attempt"));

    links->removeLink(ProvisioningAuditLinkId);
    result.expect(provisioningLink.isNull(),
                  QStringLiteral("provisioned fixture was not removed"));
    QPointer<ProvisioningAuditLink> restoredProvisioningLink(
        new ProvisioningAuditLink(ProvisioningAuditLinkId));
    LinkManagerFactory::connectLinkSignals(
        restoredProvisioningLink.data(), links);
    LinkManager::ConnectionProfile restoredProvisioningProfile;
    restoredProvisioningProfile.id = ProvisioningProfileId;
    restoredProvisioningProfile.signingRequired = true;
    links->addLink(restoredProvisioningLink.data(),
                   restoredProvisioningProfile);
    const auto restoredProvision =
        links->connectionProfile(ProvisioningAuditLinkId);
    result.expect(restoredProvision.id == ProvisioningProfileId
                      && restoredProvision.signingRequired
                      && restoredProvision.provisioningUnconfirmed
                      && restoredProvision.error.isEmpty()
                      && !links->signingReady(ProvisioningAuditLinkId)
                      && !links->connectLink(ProvisioningAuditLinkId)
                      && restoredProvisioningLink
                      && !restoredProvisioningLink->isConnected()
                      && restoredProvisioningLink->writes().isEmpty(),
                  QStringLiteral("unconfirmed provisioning policy did not restore locked"));
    links->removeLink(ProvisioningAuditLinkId);
    result.expect(restoredProvisioningLink.isNull(),
                  QStringLiteral("restored provisioning fixture was not removed"));

    // A transport can disappear synchronously from its write callback. Once
    // the private writer has been entered the vehicle outcome is unknowable:
    // report failure, retain the pending required policy, and never retry or
    // publish the key through typed observer signals.
    const quint8 interruptedSystemId = quint8(provisioningSystemId - 2);
    QPointer<ProvisioningAuditLink> interruptedLink(
        new ProvisioningAuditLink(InterruptedProvisioningAuditLinkId));
    LinkManager::ConnectionProfile interruptedProfile;
    interruptedProfile.id = InterruptedProvisioningProfileId;
    LinkManagerFactory::connectLinkSignals(interruptedLink.data(), links);
    links->addLink(interruptedLink.data(), interruptedProfile);
    result.expect(links->connectLink(InterruptedProvisioningAuditLinkId),
                  QStringLiteral("interrupted provisioning fixture did not connect"));
    if (interruptedLink) {
        interruptedLink->inject(nativeFrame(
            autopilotHeartbeatMessage(interruptedSystemId, false),
            interruptedSystemId, MAV_COMP_ID_AUTOPILOT1, 99, false));
    }
    SigningProvisioningTarget interruptedTarget;
    provisioningError.clear();
    result.expect(waitUntil([&]() {
                      provisioningError.clear();
                      const auto selected = targetManager->acquireTarget();
                      if (targetManager->contains(InterruptedProvisioningAuditLinkId,
                              interruptedSystemId, MAV_COMP_ID_AUTOPILOT1)
                          && (!selected.isValid()
                              || selected.endpoint.linkId != InterruptedProvisioningAuditLinkId))
                          targetManager->selectTarget(InterruptedProvisioningAuditLinkId,
                              interruptedSystemId, MAV_COMP_ID_AUTOPILOT1);
                      return links->prepareSigningProvisioning(
                          InterruptedProvisioningAuditLinkId,
                          &interruptedTarget, &provisioningError);
                  }) && interruptedTarget.isValid(),
                  QStringLiteral("interrupted provisioning target was not eligible: %1")
                      .arg(provisioningError));

    QObject interruptedSignalScope;
    int interruptedManagerSubmissions = 0;
    int interruptedExactSubmissions = 0;
    QObject::connect(
        links, &LinkManager::mavlinkMessageSubmitted,
        &interruptedSignalScope,
        [&](int linkId, qulonglong, mavlink_message_t) {
            if (linkId == InterruptedProvisioningAuditLinkId)
                ++interruptedManagerSubmissions;
        });
    QObject::connect(
        transmitter, &ExactLinkTransmitter::messageSubmitted,
        &interruptedSignalScope,
        [&](int linkId, quint64, mavlink_message_t) {
            if (linkId == InterruptedProvisioningAuditLinkId)
                ++interruptedExactSubmissions;
        });
    QVector<QByteArray> interruptedWire;
    if (interruptedLink) {
        interruptedLink->setWriteObserver(
            [&](const QByteArray &frame) {
                interruptedWire.append(frame);
                if (links->getLink(InterruptedProvisioningAuditLinkId))
                    links->removeLink(InterruptedProvisioningAuditLinkId);
            });
    }
    const QByteArray interruptedKey = signingKey();
    provisioningError.clear();
    result.expect(!links->provisionSigning(
                      interruptedTarget,
                      QStringLiteral("Runtime Audit Interrupted Key"),
                      interruptedKey, &provisioningError)
                      && provisioningError.contains(
                          QStringLiteral("unconfirmed"),
                          Qt::CaseInsensitive)
                      && interruptedLink.isNull()
                      && links->getLink(InterruptedProvisioningAuditLinkId)
                          == nullptr
                      && interruptedWire.size() == 1
                      && interruptedManagerSubmissions == 0
                      && interruptedExactSubmissions == 0,
                  QStringLiteral("synchronous link removal did not produce one unpublished, unconfirmed attempt"));
    result.expect(interruptedWire.size() == 1
                      && isSignedMavlink2(interruptedWire.value(0))
                      && nativeVerifierAccepts(interruptedWire,
                                               interruptedKey),
                  QStringLiteral("interrupted provisioning attempt was not signed exactly once"));
    QSettings interruptedSettings;
    const auto interruptedPolicy = MavlinkSigningProfiles::load(
        interruptedSettings, InterruptedProvisioningProfileId, true);
    result.expect(interruptedPolicy.required
                      && interruptedPolicy.provisioningUnconfirmed
                      && interruptedPolicy.error.isEmpty()
                      && interruptedPolicy.fingerprint
                          == keyFingerprint(interruptedKey),
                  QStringLiteral("uncertain transport outcome did not retain its fail-closed policy"));
    const int interruptedAttemptCount = interruptedWire.size();
    result.expect(!links->provisionSigning(
                      interruptedTarget,
                      QStringLiteral("Runtime Audit Interrupted Key"),
                      interruptedKey, nullptr)
                      && interruptedWire.size() == interruptedAttemptCount,
                  QStringLiteral("uncertain provisioning outcome was retried"));

    QPointer<ProvisioningAuditLink> restoredInterruptedLink(
        new ProvisioningAuditLink(InterruptedProvisioningAuditLinkId));
    LinkManagerFactory::connectLinkSignals(restoredInterruptedLink.data(),
                                           links);
    LinkManager::ConnectionProfile restoredInterruptedProfile;
    restoredInterruptedProfile.id = InterruptedProvisioningProfileId;
    restoredInterruptedProfile.signingRequired = true;
    links->addLink(restoredInterruptedLink.data(),
                   restoredInterruptedProfile);
    const auto restoredInterrupted =
        links->connectionProfile(InterruptedProvisioningAuditLinkId);
    result.expect(restoredInterrupted.signingRequired
                      && restoredInterrupted.provisioningUnconfirmed
                      && restoredInterrupted.error.isEmpty()
                      && !links->signingReady(
                          InterruptedProvisioningAuditLinkId)
                      && !links->connectLink(
                          InterruptedProvisioningAuditLinkId)
                      && restoredInterruptedLink
                      && !restoredInterruptedLink->isConnected(),
                  QStringLiteral("uncertain provisioning outcome restored unsigned"));
    links->removeLink(InterruptedProvisioningAuditLinkId);
    result.expect(restoredInterruptedLink.isNull(),
                  QStringLiteral("interrupted provisioning restore fixture was not removed"));

    // A required hint is fail-closed when its separate secret-free policy is
    // missing or malformed. The diagnostic belongs to the connection profile;
    // it must not be normalized into an unsigned definition on registration.
    {
        QSettings settings;
        settings.setValue(
            QStringLiteral("MAVLinkSigning/Profiles/%1/fingerprint")
                .arg(CorruptProfileId),
            QStringLiteral("NOT-a-canonical-sha256-fingerprint"));
        settings.sync();
    }
    QPointer<AuditLink> missingLink(new AuditLink(MissingPolicyLinkId));
    QPointer<AuditLink> corruptLink(new AuditLink(CorruptPolicyLinkId));
    LinkManagerFactory::connectLinkSignals(missingLink.data(), links);
    LinkManagerFactory::connectLinkSignals(corruptLink.data(), links);
    {
        const QSignalBlocker blockManagerSignals(links);
        links->addLink(missingLink.data(), requiredProfile(MissingProfileId));
        links->addLink(corruptLink.data(), requiredProfile(CorruptProfileId));
    }
    const LinkManager::ConnectionProfile missingProfile =
        links->connectionProfile(MissingPolicyLinkId);
    const LinkManager::ConnectionProfile corruptProfile =
        links->connectionProfile(CorruptPolicyLinkId);
    result.expect(missingProfile.id == MissingProfileId
                      && missingProfile.signingRequired
                      && !missingProfile.error.isEmpty()
                      && links->signingRequired(MissingPolicyLinkId)
                      && !links->signingReady(MissingPolicyLinkId)
                      && !links->connectLink(MissingPolicyLinkId),
                  QStringLiteral("missing required fingerprint did not fail closed"));
    result.expect(corruptProfile.id == CorruptProfileId
                      && corruptProfile.signingRequired
                      && !corruptProfile.error.isEmpty()
                      && links->signingRequired(CorruptPolicyLinkId)
                      && !links->signingReady(CorruptPolicyLinkId)
                      && !links->connectLink(CorruptPolicyLinkId),
                  QStringLiteral("corrupt required fingerprint did not fail closed"));
    transmitter->setOutboundVersion(MissingPolicyLinkId, 1);
    transmitter->setOutboundVersion(CorruptPolicyLinkId, 1);
    result.expect(transmitter->outboundVersion(MissingPolicyLinkId) == 2U
                      && transmitter->outboundVersion(CorruptPolicyLinkId) == 2U,
                  QStringLiteral("invalid required profiles allowed MAVLink 1 downgrade"));
    {
        const QSignalBlocker blockManagerSignals(links);
        links->removeLink(MissingPolicyLinkId);
        links->removeLink(CorruptPolicyLinkId);
    }
    result.expect(missingLink.isNull() && corruptLink.isNull(),
                  QStringLiteral("invalid-policy fake links were not deleted"));

    profileError.clear();
    result.expect(saveRequiredProfile(DuplicateProfileId, fingerprint,
                                      &profileError),
                  QStringLiteral("duplicate-profile fixture could not be staged: %1")
                      .arg(profileError));
    QPointer<AuditLink> duplicateFirst(
        new AuditLink(DuplicatePolicyFirstLinkId));
    QPointer<AuditLink> duplicateSecond(
        new AuditLink(DuplicatePolicySecondLinkId));
    LinkManagerFactory::connectLinkSignals(duplicateFirst.data(), links);
    LinkManagerFactory::connectLinkSignals(duplicateSecond.data(), links);
    {
        const QSignalBlocker blockManagerSignals(links);
        links->addLink(duplicateFirst.data(),
                       requiredProfile(DuplicateProfileId));
        links->addLink(duplicateSecond.data(),
                       requiredProfile(DuplicateProfileId));
    }
    const LinkManager::ConnectionProfile duplicateFirstProfile =
        links->connectionProfile(DuplicatePolicyFirstLinkId);
    const LinkManager::ConnectionProfile duplicateSecondProfile =
        links->connectionProfile(DuplicatePolicySecondLinkId);
    result.expect(duplicateFirstProfile.signingRequired
                      && duplicateSecondProfile.signingRequired
                      && (!duplicateFirstProfile.error.isEmpty()
                          || !duplicateSecondProfile.error.isEmpty())
                      && !links->signingReady(DuplicatePolicyFirstLinkId)
                      && !links->signingReady(DuplicatePolicySecondLinkId)
                      && !links->connectLink(DuplicatePolicyFirstLinkId)
                      && !links->connectLink(DuplicatePolicySecondLinkId),
                  QStringLiteral("duplicate connection profile did not block every link"));
    {
        const QSignalBlocker blockManagerSignals(links);
        links->removeLink(DuplicatePolicyFirstLinkId);
        links->removeLink(DuplicatePolicySecondLinkId);
    }
    result.expect(duplicateFirst.isNull() && duplicateSecond.isNull(),
                  QStringLiteral("duplicate-profile fake links were not deleted"));

    // Factory auto-connect paths are the critical startup boundary. A locked
    // listener may be represented in the manager, but it must not bind the OS
    // socket before the exact expected key has been activated.
    const quint16 udpPort = unusedUdpPort();
    result.expect(udpPort != 0,
                  QStringLiteral("could not reserve an ephemeral UDP audit port"));
    const QString udpProfileId =
        QStringLiteral("startup-udp-%1").arg(udpPort);
    profileError.clear();
    result.expect(udpPort != 0
                      && saveRequiredProfile(udpProfileId, fingerprint,
                                             &profileError),
                  QStringLiteral("startup UDP profile could not be staged: %1")
                      .arg(profileError));
    const int udpLinkId = udpPort == 0
        ? -1
        : LinkManagerFactory::addUdpConnection(
              QHostAddress::AnyIPv4, udpPort, false,
              requiredProfile(udpProfileId));
    processFor(75);
    QUdpSocket udpProbe;
    const bool udpPortRemainedFree = udpPort != 0
        && udpProbe.bind(QHostAddress::AnyIPv4, udpPort,
                         QUdpSocket::DontShareAddress);
    result.expect(udpLinkId >= 0 && links->getLink(udpLinkId)
                      && !links->getLinkConnected(udpLinkId)
                      && links->signingRequired(udpLinkId)
                      && !links->signingReady(udpLinkId)
                      && !links->connectLink(udpLinkId)
                      && udpPortRemainedFree,
                  QStringLiteral("locked startup UDP factory opened its listener"));
    udpProbe.close();
    if (udpLinkId >= 0 && links->getLink(udpLinkId)) {
        const QSignalBlocker blockManagerSignals(links);
        links->removeLink(udpLinkId);
    }

    const quint16 tcpPort = unusedTcpPort();
    result.expect(tcpPort != 0,
                  QStringLiteral("could not reserve an ephemeral TCP audit port"));
    profileError.clear();
    result.expect(tcpPort != 0
                      && saveRequiredProfile(TcpServerProfileId, fingerprint,
                                             &profileError),
                  QStringLiteral("TCP server profile could not be staged: %1")
                      .arg(profileError));
    const int tcpLinkId = tcpPort == 0
        ? -1
        : LinkManagerFactory::addTcpConnection(
              QHostAddress::LocalHost, QStringLiteral("127.0.0.1"),
              tcpPort, true, requiredProfile(TcpServerProfileId));
    processFor(75);
    QTcpServer tcpProbe;
    const bool tcpPortRemainedFree = tcpPort != 0
        && tcpProbe.listen(QHostAddress::LocalHost, tcpPort);
    result.expect(tcpLinkId >= 0 && links->getLink(tcpLinkId)
                      && !links->getLinkConnected(tcpLinkId)
                      && links->signingRequired(tcpLinkId)
                      && !links->signingReady(tcpLinkId)
                      && !links->connectLink(tcpLinkId)
                      && tcpPortRemainedFree,
                  QStringLiteral("locked TCP server factory opened its listener"));
    tcpProbe.close();
    if (tcpLinkId >= 0 && links->getLink(tcpLinkId)) {
        const QSignalBlocker blockManagerSignals(links);
        links->removeLink(tcpLinkId);
    }

    // QSettings owns only stable policy metadata. Friendly vault aliases and
    // key material must never be copied into the connection/profile records.
    {
        QSettings settings;
        settings.sync();
        const MavlinkSigningProfiles::Policy persistedPrimary =
            MavlinkSigningProfiles::load(settings, PrimaryProfileId, true);
        result.expect(persistedPrimary.required
                          && persistedPrimary.error.isEmpty()
                          && persistedPrimary.fingerprint == fingerprint,
                      QStringLiteral("required profile fingerprint did not round-trip"));
        const QByteArray rawKey = key;
        const QByteArray hexKey = key.toHex();
        bool secretOrAliasStored = false;
        for (const QString &settingKey : settings.allKeys()) {
            const QByteArray stored =
                settings.value(settingKey).toString().toUtf8();
            if (stored == rawKey || stored == hexKey
                || stored.contains("Runtime Audit")) {
                secretOrAliasStored = true;
                break;
            }
        }
        result.expect(!secretOrAliasStored,
                      QStringLiteral("QSettings persisted a signing secret or key alias"));
    }

    // Let the constructor's one-shot reload expire before exercising explicit
    // restart fixtures, then remove every default/runtime link. The audit's
    // QSettings and application-data roots are isolated by main().
    processFor(600);
    const auto removeAllLinks = [&]() {
        const QList<int> ids = links->getLinks();
        const QSignalBlocker blockManagerSignals(links);
        for (int id : ids) {
            if (links->getLink(id)) links->removeLink(id);
        }
    };
    const auto invokeReload = [&]() {
        const bool invoked = QMetaObject::invokeMethod(
            links, "reloadSettings", Qt::DirectConnection);
        result.expect(invoked,
                      QStringLiteral("reloadSettings could not be invoked"));
        return invoked;
    };
    removeAllLinks();

    // Real LINKS restoration: the required profile must be installed before
    // the factory emits newLink or attempts its automatic UDP connection.
    const quint16 restartPort = unusedUdpPort();
    result.expect(restartPort != 0,
                  QStringLiteral("could not reserve restart UDP fixture port"));
    profileError.clear();
    result.expect(restartPort != 0
                      && saveRequiredProfile(RestartUdpProfileId, fingerprint,
                                             &profileError),
                  QStringLiteral("restart UDP profile could not be staged: %1")
                      .arg(profileError));
    replaceStoredUdpLinks({StoredUdpDefinition{
        restartPort, RestartUdpProfileId, true, true, true}});
    {
        QSettings settings;
        settings.setValue(
            QLatin1String(PlannerStartupUdpOptions::EnabledSettingKey), false);
        settings.sync();
    }
    QVector<NewLinkSnapshot> restartObserved;
    QObject restartObserver;
    QObject::connect(links, &LinkManager::newLink, &restartObserver,
                     [&](int id) {
        LinkInterface *const link = links->getLink(id);
        restartObserved.append({id,
            link ? link->getLinkType() : LinkInterface::UNKNOWN_LINK,
            links->connectionProfile(id),
            link && link->isConnected()});
    });
    invokeReload();
    processFor(75);
    NewLinkSnapshot restoredUdp;
    for (const NewLinkSnapshot &snapshot : restartObserved) {
        if (snapshot.profile.id == RestartUdpProfileId) {
            restoredUdp = snapshot;
            break;
        }
    }
    QUdpSocket restartProbe;
    const bool restartPortFree = restartPort != 0
        && restartProbe.bind(QHostAddress::AnyIPv4, restartPort,
                             QUdpSocket::DontShareAddress);
    const LinkManager::ConnectionProfile restoredProfile =
        links->connectionProfile(restoredUdp.id);
    result.expect(restoredUdp.id >= 0
                      && restoredUdp.type == LinkInterface::UDP_LINK
                      && !restoredUdp.connected
                      && restoredProfile.id == RestartUdpProfileId
                      && restoredProfile.signingRequired
                      && restoredProfile.error.isEmpty()
                      && links->signingRequired(restoredUdp.id)
                      && !links->signingReady(restoredUdp.id)
                      && !links->getLinkConnected(restoredUdp.id)
                      && restartPortFree,
                  QStringLiteral("LINKS required UDP policy was not restored before connect"));
    restartProbe.close();
    removeAllLinks();

    // The complete duplicate identity set must be quarantined before the first
    // corresponding newLink signal; a first row must never briefly bind while
    // parsing later rows.
    const quint16 duplicateRestorePort = unusedUdpPort();
    result.expect(duplicateRestorePort != 0,
                  QStringLiteral("could not reserve duplicate restore port"));
    profileError.clear();
    result.expect(saveRequiredProfile(DuplicateRestoreProfileId, fingerprint,
                                      &profileError),
                  QStringLiteral("duplicate restore profile could not be staged: %1")
                      .arg(profileError));
    replaceStoredUdpLinks({
        StoredUdpDefinition{duplicateRestorePort,
                            DuplicateRestoreProfileId, true, true, true},
        StoredUdpDefinition{duplicateRestorePort,
                            DuplicateRestoreProfileId, true, true, true}});
    {
        QSettings settings;
        settings.setValue(
            QLatin1String(PlannerStartupUdpOptions::EnabledSettingKey), false);
        settings.sync();
    }
    QVector<NewLinkSnapshot> duplicateRestoreObserved;
    QObject duplicateRestoreObserver;
    QObject::connect(links, &LinkManager::newLink,
                     &duplicateRestoreObserver, [&](int id) {
        LinkInterface *const link = links->getLink(id);
        const LinkManager::ConnectionProfile profile =
            links->connectionProfile(id);
        if (profile.id == DuplicateRestoreProfileId) {
            duplicateRestoreObserved.append({id,
                link ? link->getLinkType() : LinkInterface::UNKNOWN_LINK,
                profile, link && link->isConnected()});
        }
    });
    invokeReload();
    processFor(75);
    bool duplicateRestoreBlocked = duplicateRestoreObserved.size() == 2;
    for (const NewLinkSnapshot &snapshot : duplicateRestoreObserved) {
        duplicateRestoreBlocked = duplicateRestoreBlocked
            && snapshot.type == LinkInterface::UDP_LINK
            && snapshot.profile.signingRequired
            && !snapshot.profile.error.isEmpty()
            && !snapshot.connected
            && !links->signingReady(snapshot.id)
            && !links->getLinkConnected(snapshot.id);
    }
    QUdpSocket duplicateRestoreProbe;
    const bool duplicateRestorePortFree = duplicateRestorePort != 0
        && duplicateRestoreProbe.bind(
            QHostAddress::AnyIPv4, duplicateRestorePort,
            QUdpSocket::DontShareAddress);
    result.expect(duplicateRestoreBlocked && duplicateRestorePortFree,
                  QStringLiteral("duplicate LINKS identities were not quarantined before notification"));
    duplicateRestoreProbe.close();
    removeAllLinks();

    // A canonical manual UUID on a required startup port is not the startup
    // signing profile. It must not occupy the port and hide the locked listener.
    const quint16 collisionPort = unusedUdpPort();
    result.expect(collisionPort != 0,
                  QStringLiteral("could not reserve startup collision port"));
    const QString collisionStartupProfileId =
        QStringLiteral("startup-udp-%1").arg(collisionPort);
    profileError.clear();
    result.expect(collisionPort != 0
                      && saveRequiredProfile(collisionStartupProfileId,
                                             fingerprint, &profileError),
                  QStringLiteral("startup collision policy could not be staged: %1")
                      .arg(profileError));
    replaceStoredUdpLinks({StoredUdpDefinition{
        collisionPort, CollisionManualProfileId, true, false, true}});
    {
        QSettings settings;
        settings.setValue(
            QLatin1String(PlannerStartupUdpOptions::EnabledSettingKey), true);
        settings.setValue(
            QLatin1String(PlannerStartupUdpOptions::PrimaryPortSettingKey),
            collisionPort);
        settings.setValue(
            QLatin1String(PlannerStartupUdpOptions::AlternatePortSettingKey),
            collisionPort);
        settings.setValue(
            QStringLiteral("MAVLinkSigning/StartupRequiredPorts/%1")
                .arg(collisionPort), true);
        settings.sync();
    }
    QVector<NewLinkSnapshot> collisionObserved;
    QObject collisionObserver;
    QObject::connect(links, &LinkManager::newLink, &collisionObserver,
                     [&](int id) {
        LinkInterface *const link = links->getLink(id);
        const LinkManager::ConnectionProfile profile =
            links->connectionProfile(id);
        if (profile.id == CollisionManualProfileId) {
            collisionObserved.append({id,
                link ? link->getLinkType() : LinkInterface::UNKNOWN_LINK,
                profile, link && link->isConnected()});
        }
    });
    invokeReload();
    processFor(75);
    QUdpSocket collisionProbe;
    const bool collisionPortFree = collisionPort != 0
        && collisionProbe.bind(QHostAddress::AnyIPv4, collisionPort,
                               QUdpSocket::DontShareAddress);
    result.expect(collisionObserved.size() == 1
                      && collisionObserved.first().profile.signingRequired
                      && !collisionObserved.first().profile.error.isEmpty()
                      && !collisionObserved.first().connected
                      && !links->signingReady(collisionObserved.first().id)
                      && collisionPortFree,
                  QStringLiteral("unsigned manual UDP hid a required startup profile"));
    collisionProbe.close();
    removeAllLinks();

    // The sole identity-less legacy default is assigned its deterministic
    // startup identity before the factory can connect. Protect both default
    // ports because no explicit startup setting may exist during this migration.
    profileError.clear();
    result.expect(saveRequiredProfile(QStringLiteral("startup-udp-14550"),
                                      fingerprint, &profileError),
                  QStringLiteral("legacy startup 14550 policy could not be staged: %1")
                      .arg(profileError));
    profileError.clear();
    result.expect(saveRequiredProfile(QStringLiteral("startup-udp-14551"),
                                      fingerprint, &profileError),
                  QStringLiteral("legacy startup 14551 policy could not be staged: %1")
                      .arg(profileError));
    replaceStoredUdpLinks({StoredUdpDefinition{
        14550, {}, false, false, false}});
    {
        QSettings settings;
        settings.setValue(
            QStringLiteral("MAVLinkSigning/StartupRequiredPorts/14550"), true);
        settings.setValue(
            QStringLiteral("MAVLinkSigning/StartupRequiredPorts/14551"), true);
        settings.sync();
    }
    QVector<NewLinkSnapshot> legacyObserved;
    QObject legacyObserver;
    QObject::connect(links, &LinkManager::newLink, &legacyObserver,
                     [&](int id) {
        LinkInterface *const link = links->getLink(id);
        const LinkManager::ConnectionProfile profile =
            links->connectionProfile(id);
        if (profile.id.startsWith(QStringLiteral("startup-udp-"))) {
            legacyObserved.append({id,
                link ? link->getLinkType() : LinkInterface::UNKNOWN_LINK,
                profile, link && link->isConnected()});
        }
    });
    invokeReload();
    processFor(75);
    bool sawLegacy14550 = false;
    bool allLegacyStartupLocked = !legacyObserved.isEmpty();
    for (const NewLinkSnapshot &snapshot : legacyObserved) {
        sawLegacy14550 = sawLegacy14550
            || snapshot.profile.id == QStringLiteral("startup-udp-14550");
        allLegacyStartupLocked = allLegacyStartupLocked
            && snapshot.profile.signingRequired
            && snapshot.profile.error.isEmpty()
            && !snapshot.connected
            && !links->signingReady(snapshot.id)
            && !links->getLinkConnected(snapshot.id);
    }
    result.expect(sawLegacy14550 && allLegacyStartupLocked,
                  QStringLiteral("legacy UDP 14550 was not safely mapped before connect"));
    removeAllLinks();

    // Keep the global restore error fixture last: it intentionally poisons all
    // later automatic connections for this process. Normalization must not turn
    // a malformed port into an unsigned listener on either default port.
    const quint16 malformedAlternatePort = unusedUdpPort();
    result.expect(malformedAlternatePort != 0,
                  QStringLiteral("could not reserve malformed startup fixture port"));
    replaceStoredUdpLinks({});
    {
        QSettings settings;
        settings.setValue(
            QLatin1String(PlannerStartupUdpOptions::EnabledSettingKey), true);
        settings.setValue(
            QLatin1String(PlannerStartupUdpOptions::PrimaryPortSettingKey),
            QStringLiteral("not-a-port"));
        settings.setValue(
            QLatin1String(PlannerStartupUdpOptions::AlternatePortSettingKey),
            malformedAlternatePort);
        settings.sync();
    }
    QVector<NewLinkSnapshot> malformedObserved;
    QObject malformedObserver;
    QObject::connect(links, &LinkManager::newLink, &malformedObserver,
                     [&](int id) {
        LinkInterface *const link = links->getLink(id);
        malformedObserved.append({id,
            link ? link->getLinkType() : LinkInterface::UNKNOWN_LINK,
            links->connectionProfile(id),
            link && link->isConnected()});
    });
    invokeReload();
    processFor(75);
    bool allMalformedBlocked = !malformedObserved.isEmpty();
    for (const NewLinkSnapshot &snapshot : malformedObserved) {
        allMalformedBlocked = allMalformedBlocked
            && snapshot.profile.signingRequired
            && !snapshot.profile.error.isEmpty()
            && !snapshot.connected
            && !links->signingReady(snapshot.id)
            && !links->getLinkConnected(snapshot.id);
    }
    QUdpSocket malformedProbe;
    const bool malformedAlternateFree = malformedAlternatePort != 0
        && malformedProbe.bind(QHostAddress::AnyIPv4,
                               malformedAlternatePort,
                               QUdpSocket::DontShareAddress);
    result.expect(allMalformedBlocked && malformedAlternateFree,
                  QStringLiteral("malformed startup port opened a normalized listener"));
    malformedProbe.close();
    removeAllLinks();

    processFor(1);
    return result.exitCode();
}
