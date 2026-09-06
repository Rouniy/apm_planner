/*===================================================================
APM_PLANNER Open Source Ground Control Station

(c) 2014 APM_PLANNER PROJECT <http://www.diydrones.com>

This file is part of the APM_PLANNER project

    APM_PLANNER is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    APM_PLANNER is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with APM_PLANNER. If not, see <http://www.gnu.org/licenses/>.

======================================================================*/

/**
 * @file
 *   @brief LinkManager
 *
 *   @author Michael Carpenter <malcom2073@gmail.com>
 *   @author QGROUNDCONTROL PROJECT - This code has GPLv3+ snippets from QGROUNDCONTROL, (c) 2009, 2010 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 */

#ifndef LINKMANAGER_H
#define LINKMANAGER_H

#include <QObject>
/**
 * @brief The ConnectionManager class
 * This class handles all connections between the GCS and the actual hardware.
 * It will create (on request) serial or UDP links, connect the links to the associated mavlink parsers,
 * and emit signals upwards when mavlink messages come in.
 * This class lives in the UI thread
 * The Serial Link lives in the UI Thread
 * The mavlink decoder lives in its own thread
 * the UAS Class lives in the UI thread
 */
#include "MAVLinkDecoder.h"
#include "MAVLinkProtocol.h"
#include "MAVLinkSigningManager.h"
#include "services/SigningProvisioningTarget.h"
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <memory>

#include "UASInterface.h"
#include "UAS.h"
#include "UASObject.h"
class VehicleTargetManager;
struct SwarmVehicleInstanceLease;
struct VehicleEndpoint;
class SwarmTelemetryRegistry;
class SwarmCommandService;
class SwarmSequenceExecutor;
class SwarmWaypointLeaderExecutor;
class VehicleCommandService;
class GuidedTargetService;
class MovingBasePositionStore;
class MovingBaseService;
class CompassCalibrationService;
class DeveloperVehicleToolService;
class ParameterRecoveryService;
class OfflineMagFitApplyService;
class RemoteDataFlashLogService;
class MavlinkSerialTcpBridgeService;
class ExactLinkTransmitter;
class ExactLogTransferService;
class UDPLink;
class ExactMissionSnapshotService;
class RadioStatusMonitor;
class ParameterService;
class MavlinkComponentRegistry;
class Px4FlowService;
class MavFtpService;
class MavFtpServiceInterface;
class MavAuthKeyService;
class QGCUASParamManager;
class LinkManager : public QObject
{
    friend class LinkManagerFactory;
    Q_OBJECT
public:
    struct ConnectionProfile {
        QString id;
        bool signingRequired = false;
        QString error;
        quint64 revision = 0; // Runtime edit/lifecycle fence; never persisted.
        bool provisioningUnconfirmed = false;
    };
    explicit LinkManager(QObject *parent = nullptr);
    static LinkManager* instance();
    ~LinkManager();

    void shutdown();    // Called when appplication exits
    bool isShuttingDown() const { return m_shuttingDown; }

    void disableTimeouts(int index);
    void enableTimeouts(int index);
    void disableAllTimeouts();
    void enableAllTimeouts();

    MAVLinkProtocol* getProtocol() const;
    VehicleTargetManager *vehicleTargetManager() const;
    SwarmTelemetryRegistry *swarmTelemetryRegistry() const;
    SwarmCommandService *swarmCommandService() const;
    SwarmSequenceExecutor *swarmSequenceExecutor() const;
    SwarmWaypointLeaderExecutor *swarmWaypointLeaderExecutor() const;
    ExactMissionSnapshotService *exactMissionSnapshotService() const;
    ExactLinkTransmitter *exactLinkTransmitter() const;
    ExactLogTransferService *exactLogTransferService() const;
    // Link-scoped SiK RADIO_STATUS / legacy RADIO statistics (MP10 localsnrdb).
    RadioStatusMonitor *radioStatusMonitor() const;
    VehicleCommandService *vehicleCommandService() const;
    GuidedTargetService *guidedTargetService() const;
    MovingBasePositionStore *movingBasePositionStore() const;
    MovingBaseService *movingBaseService() const;
    CompassCalibrationService *compassCalibrationService() const;
    DeveloperVehicleToolService *developerVehicleToolService() const;
    ParameterRecoveryService *parameterRecoveryService() const;
    OfflineMagFitApplyService *offlineMagFitApplyService() const;
    RemoteDataFlashLogService *remoteDataFlashLogService() const;
    MavlinkSerialTcpBridgeService *mavlinkSerialTcpBridgeService() const;
    ParameterService *parameterService() const;
    MavlinkComponentRegistry *componentRegistry() const;
    Px4FlowService *px4FlowService() const;
    bool singleEndpointRouteIsEligible(const VehicleEndpoint &endpoint,
                                      quint64 epoch, QString *error = nullptr) const;
    MavFtpServiceInterface *mavFtpService() const;
    QGCUASParamManager *parameterManager() const;
    Q_INVOKABLE QObject *vehicleTargetManagerObject() const;
    Q_INVOKABLE QObject *vehicleCommandServiceObject() const;
    Q_INVOKABLE QObject *parameterServiceObject() const;
    bool connectLink(int index);
    void disconnectLink(int index);

    UASInterface* getUas(int id);
    UASInterface* createUAS(MAVLinkProtocol* mavlink, LinkInterface* link, int sysid, mavlink_heartbeat_t* heartbeat, QObject* parent = nullptr);

    void addLink(LinkInterface *link, const ConnectionProfile &profile);
    void addLink(LinkInterface *link) { addLink(link, ConnectionProfile{}); }
    ConnectionProfile connectionProfile(int linkId) const;
    bool signingRequired(int linkId) const;
    bool signingReady(int linkId) const;
    MavAuthKeyService *mavAuthKeyService();
    QList<int> getLinks() const;

    LinkInterface* getLink(int linkId) const;
    // Physical connection epoch, available before any vehicle heartbeat.
    quint64 currentPhysicalLinkSession(int linkId) const;
    // Datagram peer changes are connection boundaries, including a port
    // change on the same host. Reject bytes queued before that boundary.
    bool isCurrentPhysicalIngress(LinkInterface *link) const;
    void receiveUdpDatagram(UDPLink *link, const QByteArray &bytes,
                            quint64 peerRevision);
    /** Raw passthrough is refused on every signing-protected link. */
    bool writeRawBytes(int linkId, const QByteArray &bytes);
    /** Legacy payloads share the exact per-link sequencer and signing boundary. */
    bool writeMavlinkMessage(LinkInterface *link, mavlink_message_t message);
    // Local protection for an already provisioned vehicle; requires a physically
    // disconnected link. This never sends SETUP_SIGNING or changes vehicle keys.
    bool configureSigning(int linkId, const QString &connectionProfileId,
                          const QString &keyName, const QByteArray &key,
                          QString *error = nullptr);
    bool prepareSigningProvisioning(int linkId, SigningProvisioningTarget *target,
                                    QString *error = nullptr) const;
    // Caller must obtain explicit trusted-private-channel consent for this
    // exact snapshot. True means submitted, NEVER a vehicle acknowledgement.
    bool provisionSigning(const SigningProvisioningTarget &target,
                          const QString &keyName, const QByteArray &key,
                          QString *error = nullptr);
    const MAVLinkSigningManager *signingManager() const;
    MAVLinkSigningManager::Verification verifyIncomingFrame(
        int linkId, quint64 epoch, const QByteArray &frame);
    bool isUdpPortInUse(quint16 port) const;
    // Remove a link based on instance
    void removeLink(LinkInterface *link);
    // Remove a link based on unique id
    void removeLink(int linkId);

    LinkInterface::LinkType getLinkType(int linkid);
    bool getLinkConnected(int linkid);

    QString getSerialLinkPort(int linkid); // [TODO] remove
    QString getLinkName(int linkid); // [TODO] remove
    QString getLinkShortName(int linkid); // [TODO] remove
    QString getLinkDetail(int linkid); // [TODO] remove
    int getSerialLinkBaud(int linkid); // [TODO] remove

    QStringList getCurrentPorts();
    void stopLogging();
    void startLogging();
    void setLogSubDirectory(const QString& dir);
    bool loggingEnabled() const;
    UASObject *getUasObject(int uasid);
    QMap<int,UASObject*> m_uasObjectMap; // [TODO] make private

    void addSimObject(uint8_t sysid,UASObject *obj); // [TODO] remove
    void removeSimObject(uint8_t sysid); // [TODO] remove

signals:
    //void newLink(LinkInterface* link);
    void newLink(int linkid);
    void linkRemoved(int linkid);
    void protocolStatusMessage(QString title,QString text);
    void linkChanged(int linkid);

    /** @brief aggregated signal for when link status changes */
    void linkChanged(LinkInterface *link);

    void linkError(int linkid, QString message);
    void messageReceived(LinkInterface* link,mavlink_message_t message);
    void physicalLinkSessionBegan(int linkId, qulonglong epoch);
    void physicalLinkSessionEnded(int linkId, qulonglong epoch);
    void mavlinkMessageObserved(int linkId, qulonglong epoch,
                               mavlink_message_t message);
    // Submitted to the transport, not a delivery/vehicle acknowledgement.
    void mavlinkMessageSubmitted(int linkId, qulonglong epoch,
                                mavlink_message_t message);

public slots:
    void receiveMessage(LinkInterface* link,mavlink_message_t message);
    void protocolStatusMessageRec(QString title,QString text);
    void enableLogging(bool enabled);
    void reloadSettings();
    void linkUpdated(LinkInterface* link);

private slots:
    void linkConnected(LinkInterface* link);
    void linkDisonnected(LinkInterface* link);
    void linkErrorRec(LinkInterface* link,QString error);
    void linkTimeoutTriggered(LinkInterface*);

private:
    bool writeSequencedFrame(int linkId, const QByteArray &frame);
    bool writeBytesToTransport(int linkId, const QByteArray &bytes);
    void loadSettings();
    bool saveSettings();
    bool activateLinkSession(LinkInterface *link);
    void invalidateLinkSession(int linkId);
    bool exactVehicleRouteIsEligible(
        const SwarmVehicleInstanceLease &lease,
        QString *error = nullptr) const;
    bool singleVehicleParameterRouteIsEligible(
        const SwarmVehicleInstanceLease &lease, QString *error) const;
    void syncActiveUasToTarget();
    void syncTargetToActiveUas(UASInterface *uas);

private:
    QMap<int,LinkInterface*> m_connectionMap;
    QMap<int,QPointer<UASInterface>> m_uasMap;
    QMap<QString,int> m_portToBaudMap;
    // Automatic MP10 startup listeners are runtime policy, not user-created
    // connection definitions. Excluding them from LINKMANAGER/LINKS makes the
    // enable switch effective on the next restart.
    QSet<int> m_startupUdpLinkIds;
    QHash<int, ConnectionProfile> m_connectionProfiles;
    quint64 m_nextConnectionRevision = 0;
    QString m_connectionRestoreError;
    MavAuthKeyService *m_mavAuthKeyService = nullptr;
    QScopedPointer<MAVLinkDecoder> m_mavlinkDecoder;
    QScopedPointer<MAVLinkProtocol> m_mavlinkProtocol;
    std::unique_ptr<MAVLinkSigningManager> m_signingManager;
    bool captureSigningProvisioningTarget(int linkId, SigningProvisioningTarget *target,
                                         bool initialPolicy, QString *error) const;
    bool m_signingProvisioningBusy = false;
    QHash<int, quint64> m_observedSignedHeartbeatEpochs;
    QHash<int, quint64> m_observedRadioEpochs;
    VehicleTargetManager *m_vehicleTargetManager = nullptr;
    SwarmTelemetryRegistry *m_swarmTelemetryRegistry = nullptr;
    SwarmCommandService *m_swarmCommandService = nullptr;
    SwarmSequenceExecutor *m_swarmSequenceExecutor = nullptr;
    SwarmWaypointLeaderExecutor *m_swarmWaypointLeaderExecutor = nullptr;
    ExactMissionSnapshotService *m_exactMissionSnapshotService = nullptr;
    ExactLinkTransmitter *m_exactLinkTransmitter = nullptr;
    ExactLogTransferService *m_exactLogTransferService = nullptr;
    QHash<int, quint64> m_udpIngressRevision;
    RadioStatusMonitor *m_radioStatusMonitor = nullptr;
    VehicleCommandService *m_vehicleCommandService = nullptr;
    GuidedTargetService *m_guidedTargetService = nullptr;
    MovingBasePositionStore *m_movingBasePositionStore = nullptr;
    MovingBaseService *m_movingBaseService = nullptr;
    CompassCalibrationService *m_compassCalibrationService = nullptr;
    DeveloperVehicleToolService *m_developerVehicleToolService = nullptr;
    ParameterRecoveryService *m_parameterRecoveryService = nullptr;
    OfflineMagFitApplyService *m_offlineMagFitApplyService = nullptr;
    RemoteDataFlashLogService *m_remoteDataFlashLogService = nullptr;
    MavlinkSerialTcpBridgeService *m_mavlinkSerialTcpBridgeService = nullptr;
    ParameterService *m_parameterService = nullptr;
    MavlinkComponentRegistry *m_componentRegistry = nullptr;
    Px4FlowService *m_px4FlowService = nullptr;
    MavFtpService *m_mavFtpService = nullptr;
    QGCUASParamManager *m_parameterManager = nullptr;
    QString m_logSubDir;
    bool m_mavlinkLoggingEnabled;
    bool m_shuttingDown = false;
};

#endif // LINKMANAGER_H
