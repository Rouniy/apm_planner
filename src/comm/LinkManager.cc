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

#include "LinkManagerFactory.h"
#include "LinkManager.h"
#include "AppPaths.h"
#include "RadioStatusMonitor.h"
#include "SwarmCommandService.h"
#include "SwarmTelemetryRegistry.h"
#include "services/SwarmSequenceExecutor.h"
#include "services/SwarmWaypointLeaderExecutor.h"
#include "PxQuadMAV.h"
#include "SlugsMAV.h"
#include "ArduPilotMegaMAV.h"
#include "UASManager.h"
#include "serialconnection.h"
#include "UDPLink.h"
#include "UDPClientLink.h"
#include "TCPLink.h"
#include "UASObject.h"
#include "CompassCalibrationService.h"
#include "ExactLinkTransmitter.h"
#include "ExactLogTransferService.h"
#include "ExactMissionSnapshotService.h"
#include "GuidedTargetService.h"
#include "MavFtpService.h"
#include "MovingBasePositionStore.h"
#include "MovingBaseService.h"
#include "ParameterService.h"
#include "MavlinkComponentRegistry.h"
#include "Px4FlowService.h"
#include "QGCUASParamManager.h"
#include "VehicleCommandService.h"
#include "VehicleEndpoint.h"
#include "VehicleTargetManager.h"
#include "services/MavlinkSigningProfiles.h"
#include "services/MavAuthKeyService.h"
#include <QCryptographicHash>
#include "ui/configuration/PlannerStartupUdpOptions.h"
#include <QApplication>
#include <QPointer>
#include <QSettings>
#include <QtSerialPort/qserialportinfo.h>
#include <QTimer>
#include <QDateTime>
#include <QDir>
#include <QThread>
#include <algorithm>

namespace
{

QString mavTypeName(quint8 type)
{
    static const char *const names[] = {
        "GENERIC", "FIXED WING", "QUADROTOR", "COAXIAL", "HELICOPTER",
        "ANTENNA TRACKER", "GCS", "AIRSHIP", "FREE BALLOON", "ROCKET",
        "GROUND ROVER", "SURFACE BOAT", "SUBMARINE", "HEXAROTOR",
        "OCTOROTOR", "TRICOPTER", "FLAPPING WING", "KITE",
        "ONBOARD CONTROLLER", "VTOL DUOROTOR", "VTOL QUADROTOR",
        "VTOL TILTROTOR", "VTOL RESERVED2", "VTOL RESERVED3",
        "VTOL RESERVED4", "VTOL RESERVED5", "GIMBAL", "ADSB", "PARAFOIL",
        "DODECAROTOR", "CAMERA", "CHARGING STATION", "FLARM", "SERVO",
        "ODID", "DECAROTOR", "BATTERY", "PARACHUTE", "LOG", "OSD",
        "IMU", "GPS", "WINCH"
    };
    return type < sizeof(names) / sizeof(names[0])
        ? QString::fromLatin1(names[type]) : QString::number(type);
}

QString componentNameFromDiscoveryMessage(const mavlink_message_t &message)
{
    if (message.compid != MAV_COMP_ID_AUTOPILOT1) {
        return VehicleEndpoint::defaultComponentName(message.compid);
    }
    if (message.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        mavlink_heartbeat_t heartbeat = {};
        mavlink_msg_heartbeat_decode(&message, &heartbeat);
        return mavTypeName(heartbeat.type);
    }
    if (message.msgid == MAVLINK_MSG_ID_HIGH_LATENCY2) {
        mavlink_high_latency2_t highLatency = {};
        mavlink_msg_high_latency2_decode(&message, &highLatency);
        return mavTypeName(highLatency.type);
    }
    return VehicleEndpoint::defaultComponentName(message.compid);
}

} // namespace


LinkManager* LinkManager::instance()
{
    static LinkManager _instance;
    return &_instance;
}

LinkManager::LinkManager(QObject *parent) :
    QObject(parent),
    m_mavlinkLoggingEnabled(true)
{
    m_signingManager = std::make_unique<MAVLinkSigningManager>(
        QDir(AppPaths::writableDataDirectory()).filePath(QStringLiteral("mavlink-signing")));
    m_vehicleTargetManager = new VehicleTargetManager(this);
    m_swarmTelemetryRegistry = new SwarmTelemetryRegistry(this);
    connect(m_swarmTelemetryRegistry, &SwarmTelemetryRegistry::linkSessionEnded,
            this, [this](int id, qulonglong epoch) {
        m_signingManager->endEpoch(id, epoch);
        m_exactLinkTransmitter->setLinkSessionEpoch(id, 0);
        emit physicalLinkSessionEnded(id, epoch);
    });
    m_radioStatusMonitor = new RadioStatusMonitor(this);
    m_exactLinkTransmitter = new ExactLinkTransmitter(
        [this](int linkId, const QByteArray &frame) {
            return writeSequencedFrame(linkId, frame);
        }, this);
    m_exactLinkTransmitter->setFrameSigner(
        [this](int linkId, const QByteArray &frame, QByteArray *output) {
            return !m_shuttingDown && signingReady(linkId) && m_signingManager->signFrame(
                linkId, currentPhysicalLinkSession(linkId), frame,
                QDateTime::currentMSecsSinceEpoch(), output);
        });
    connect(m_swarmTelemetryRegistry, &SwarmTelemetryRegistry::linkSessionBegan,
            this, [this](int id, qulonglong epoch) {
        if (!signingReady(id) || !m_signingManager->beginEpoch(id, epoch)) {
            invalidateLinkSession(id);
            disconnectLink(id);
            return;
        }
        m_exactLinkTransmitter->setSigningRequired(id, signingRequired(id));
        m_exactLinkTransmitter->setLinkSessionEpoch(id, epoch);
        emit physicalLinkSessionBegan(id, epoch);
    });
    m_exactLogTransferService = new ExactLogTransferService(
        m_swarmTelemetryRegistry, m_exactLinkTransmitter,
        [this](const SwarmVehicleInstanceLease &lease, QString *error) {
            LinkInterface *const link = getLink(lease.endpoint.linkId);
            if (!link || !link->isConnected()
                || !isCurrentPhysicalIngress(link)) {
                if (error) *error = tr("The selected physical link is unavailable or its peer changed.");
                return false;
            }
            if (auto *udp = qobject_cast<UDPLink *>(link)) {
                const auto peers = udp->peerSnapshot();
                if (peers.hosts.size() != 1 || peers.ports.size() != 1) {
                    if (error) *error = tr("Download Logs requires one UDP peer. Use a separate connection for each vehicle.");
                    return false;
                }
            }
            return true;
        }, this);
    m_exactLogTransferService->setLocalIdentity(
        QGC::MavlinkID(), QGC::ComponentID());
    m_exactLogTransferService->setRouteIdentityProvider(
        [this](const SwarmVehicleInstanceLease &lease) -> QString {
            LinkInterface *const link = getLink(lease.endpoint.linkId);
            if (!link || !link->isConnected()
                || !isCurrentPhysicalIngress(link)) return {};
            if (auto *udp = qobject_cast<UDPLink *>(link)) {
                const auto peers = udp->peerSnapshot();
                if (peers.revision != m_udpIngressRevision.value(link->getId())
                    || peers.hosts.size() != 1 || peers.ports.size() != 1) return {};
                return QStringLiteral("udp:%1:%2:%3")
                    .arg(peers.revision).arg(peers.hosts.first().toString())
                    .arg(peers.ports.first());
            }
            return QStringLiteral("link:%1:%2")
                .arg(lease.endpoint.linkId).arg(lease.linkSessionEpoch);
        });
    m_exactMissionSnapshotService = new ExactMissionSnapshotService(
        m_swarmTelemetryRegistry, m_exactLinkTransmitter,
        [this](const SwarmVehicleInstanceLease &lease, QString *error) {
            return exactVehicleRouteIsEligible(lease, error);
        },
        [this](const SwarmVehicleInstanceLease &lease) {
            UAS *const uas = dynamic_cast<UAS *>(
                m_uasMap.value(lease.endpoint.systemId, nullptr).data());
            if (!uas
                || !uas->getLinkIdList().contains(lease.endpoint.linkId)
                || uas->primaryComponentId()
                    != lease.endpoint.componentId) {
                return static_cast<MissionProtocolCoordinator *>(nullptr);
            }
            return uas->missionProtocolCoordinator();
        },
        1500, 3, this);
    m_exactMissionSnapshotService->setLocalIdentity(
        QGC::MavlinkID(), QGC::ComponentID());
    m_swarmCommandService = new SwarmCommandService(
        m_swarmTelemetryRegistry, m_exactLinkTransmitter,
        [this](const SwarmVehicleInstanceLease &lease, QString *error) {
            return exactVehicleRouteIsEligible(lease, error);
        }, this);
    m_swarmCommandService->setLocalIdentity(
        QGC::MavlinkID(), QGC::ComponentID());
    m_vehicleCommandService = new VehicleCommandService(
        m_vehicleTargetManager, m_exactLinkTransmitter, this);
    m_vehicleCommandService->setLocalIdentity(
        QGC::MavlinkID(), QGC::ComponentID());
    const bool exactCommandsConfigured =
        m_vehicleCommandService->configureExactTransactions(
            [registry = QPointer<SwarmTelemetryRegistry>(
                 m_swarmTelemetryRegistry)](
                const SwarmVehicleInstanceLease &lease) {
                return registry && registry->validateLease(lease);
            },
            [this](const SwarmVehicleInstanceLease &lease, QString *error) {
                return exactVehicleRouteIsEligible(lease, error);
            });
    Q_ASSERT(exactCommandsConfigured);
    connect(m_swarmTelemetryRegistry,
            &SwarmTelemetryRegistry::endpointRetired,
            m_vehicleCommandService,
            [service = QPointer<VehicleCommandService>(
                 m_vehicleCommandService)](
                const SwarmVehicleInstanceLease &lease,
                SwarmTelemetryRegistry::RetirementReason) {
                if (service) {
                    service->retireExactVehicle(lease);
                }
            });
    m_guidedTargetService = new GuidedTargetService(
        m_vehicleTargetManager, m_vehicleCommandService, this);
    m_guidedTargetService->setLocalIdentity(
        QGC::MavlinkID(), QGC::ComponentID());
    m_movingBasePositionStore = new MovingBasePositionStore(
        m_vehicleTargetManager, this);
    m_movingBaseService = new MovingBaseService(
        m_vehicleTargetManager, m_movingBasePositionStore, this);
    m_compassCalibrationService = new CompassCalibrationService(
        m_vehicleTargetManager, m_vehicleCommandService,
        m_exactLinkTransmitter, this);
    m_compassCalibrationService->setLocalIdentity(
        QGC::MavlinkID(), QGC::ComponentID());
    m_parameterService = new ParameterService(
        m_vehicleTargetManager, m_exactLinkTransmitter, this);
    m_parameterService->setLocalIdentity(
        QGC::MavlinkID(), QGC::ComponentID());
    const bool exactParametersConfigured =
        m_parameterService->configureExactTransactions(
            [registry = QPointer<SwarmTelemetryRegistry>(
                 m_swarmTelemetryRegistry)](
                const SwarmVehicleInstanceLease &lease) {
                return registry && registry->validateLease(lease);
            },
            [this](const SwarmVehicleInstanceLease &lease, QString *error) {
                return exactVehicleRouteIsEligible(lease, error);
            });
    Q_ASSERT(exactParametersConfigured);
    const bool singleVehicleParametersConfigured =
        m_parameterService->configureSingleVehicleExactRoute(
            [this](const SwarmVehicleInstanceLease &lease, QString *error) {
                return singleVehicleParameterRouteIsEligible(lease, error);
            });
    Q_ASSERT(singleVehicleParametersConfigured);
    connect(m_swarmTelemetryRegistry,
            &SwarmTelemetryRegistry::endpointRetired,
            m_parameterService,
            [service = QPointer<ParameterService>(m_parameterService)](
                const SwarmVehicleInstanceLease &lease,
                SwarmTelemetryRegistry::RetirementReason) {
                if (service) {
                    service->retireExactVehicle(lease);
                }
            });
    m_componentRegistry = new MavlinkComponentRegistry(this);
    connect(this, &LinkManager::physicalLinkSessionBegan,
            m_componentRegistry, &MavlinkComponentRegistry::beginLinkSession);
    connect(this, &LinkManager::physicalLinkSessionEnded,
            m_componentRegistry, &MavlinkComponentRegistry::endLinkSession);
    const bool componentParametersConfigured = m_parameterService->configureComponentExactTransactions(
        [this](const MavlinkComponentInstanceLease &lease) {
            return m_componentRegistry->validateLease(lease);
        },
        [this](const MavlinkComponentInstanceLease &lease, QString *error) {
            return singleEndpointRouteIsEligible(lease.endpoint, lease.linkSessionEpoch, error);
        });
    Q_ASSERT(componentParametersConfigured);
    connect(m_componentRegistry, &MavlinkComponentRegistry::componentRetired,
            m_parameterService, &ParameterService::retireComponent);
    m_px4FlowService = new Px4FlowService(m_componentRegistry, m_parameterService, this);
    m_swarmWaypointLeaderExecutor = new SwarmWaypointLeaderExecutor(
        m_swarmTelemetryRegistry,
        m_exactMissionSnapshotService,
        m_swarmCommandService,
        m_vehicleCommandService,
        m_parameterService,
        this);
    m_swarmSequenceExecutor = new SwarmSequenceExecutor(
        m_swarmTelemetryRegistry,
        m_swarmCommandService,
        m_vehicleCommandService,
        this);
    m_mavFtpService = new MavFtpService(
        m_vehicleTargetManager, m_exactLinkTransmitter, this);
    m_mavFtpService->setLocalIdentity(
        QGC::MavlinkID(), QGC::ComponentID());
    m_parameterManager = new QGCUASParamManager(
        m_parameterService, m_vehicleTargetManager, this);
    connect(m_vehicleTargetManager,
            &VehicleTargetManager::currentTargetChanged,
            this, &LinkManager::syncActiveUasToTarget);
    connect(UASManager::instance(),
            QOverload<UASInterface *>::of(&UASManager::activeUASSet),
            this, &LinkManager::syncTargetToActiveUas);
    m_mavlinkDecoder.reset(new MAVLinkDecoder(this));
    m_mavlinkProtocol.reset(new MAVLinkProtocol());
    m_mavlinkProtocol->setConnectionManager(this);
    connect(m_mavlinkProtocol.data(), &MAVLinkProtocol::packetReceived,
            this, [this](LinkInterface *link, mavlink_message_t message) {
        if (m_shuttingDown || !link) return;
        const int id = link->getId();
        if (!isCurrentPhysicalIngress(link)) return;
        const quint64 epoch = currentPhysicalLinkSession(id);
        if (epoch == 0) return;
        // Peripherals such as PX4Flow do not necessarily create a legacy UAS.
        // Keep discovery and their exact ACK owner ahead of that compatibility
        // gate. Revalidate after every observer (it can remove/rebind a link).
        QPointer<LinkInterface> pinned(link);
        const auto current = [this, pinned, id, epoch]() {
            return !m_shuttingDown && pinned && getLink(id) == pinned.data()
                && currentPhysicalLinkSession(id) == epoch
                && isCurrentPhysicalIngress(pinned.data());
        };
        m_componentRegistry->observeMessage(id, epoch, message);
        if (!current()) return;
        m_parameterService->observePhysicalMessage(id, epoch, message);
        if (!current()) return;
        m_px4FlowService->observeMessage(id, epoch, message);
        if (!current()) return;
        emit mavlinkMessageObserved(id, epoch, message);
    });
    connect(m_mavlinkProtocol.data(),SIGNAL(messageReceived(LinkInterface*,mavlink_message_t)),m_mavlinkDecoder.data(),SLOT(receiveMessage(LinkInterface*,mavlink_message_t)));
    connect(m_mavlinkProtocol.data(),SIGNAL(messageReceived(LinkInterface*,mavlink_message_t)),this,SLOT(receiveMessage(LinkInterface*,mavlink_message_t)));
    connect(m_mavlinkProtocol.data(),SIGNAL(protocolStatusMessage(QString,QString)),this,SLOT(protocolStatusMessageRec(QString,QString)));
    connect(m_mavlinkProtocol.data(), &MAVLinkProtocol::outboundVersionChanged,
            m_exactLinkTransmitter,
            &ExactLinkTransmitter::setOutboundVersion);

    QTimer::singleShot(500, this, SLOT(reloadSettings()));
}

bool LinkManager::singleVehicleParameterRouteIsEligible(
    const SwarmVehicleInstanceLease &lease, QString *error) const
{
    return lease.isValid()
        && singleEndpointRouteIsEligible(lease.endpoint, lease.linkSessionEpoch, error);
}

bool LinkManager::singleEndpointRouteIsEligible(
    const VehicleEndpoint &endpoint, quint64 epoch, QString *error) const
{
    if (error) error->clear();
    LinkInterface *const link = getLink(endpoint.linkId);
    if (m_shuttingDown || !endpoint.isValid() || epoch == 0 || !link || !link->isConnected()
        || currentPhysicalLinkSession(link->getId()) != epoch
        || !isCurrentPhysicalIngress(link) || !signingReady(endpoint.linkId)) {
        if (error) *error = tr("The selected physical link is unavailable or its peer changed.");
        return false;
    }
    switch (link->getLinkType()) {
    case LinkInterface::SERIAL_LINK:
    case LinkInterface::TCP_LINK:
    case LinkInterface::UDP_CLIENT_LINK:
        return true;
    case LinkInterface::UDP_LINK: {
        auto *udp = qobject_cast<UDPLink *>(link);
        if (!udp) return false;
        const auto peers = udp->peerSnapshot();
        if (peers.revision == m_udpIngressRevision.value(link->getId())
            && peers.hosts.size() == 1 && peers.ports.size() == 1) return true;
        if (error) *error = tr("Exact parameters require one UDP peer; use a separate connection for each device.");
        return false;
    }
    default:
        if (error) *error = tr("This transport is not a supported single-vehicle parameter route.");
        return false;
    }
}

bool LinkManager::exactVehicleRouteIsEligible(
    const SwarmVehicleInstanceLease &lease, QString *error) const
{
    if (error) {
        error->clear();
    }
    const int linkId = lease.endpoint.linkId;
    QPointer<LinkInterface> link(getLink(linkId));
    if (!lease.isValid() || !link || !link->isConnected()) {
        if (error) {
            *error = QStringLiteral(
                "The exact physical link is unavailable.");
        }
        return false;
    }
    switch (link->getLinkType()) {
    case LinkInterface::TCP_LINK:
    case LinkInterface::UDP_CLIENT_LINK:
        return true;
    case LinkInterface::SERIAL_LINK:
        if (error) {
            *error = QStringLiteral(
                "Serial/radio swarm control requires an explicit dedicated-link approval that is not yet available.");
        }
        return false;
    case LinkInterface::UDP_LINK:
        if (error) {
            *error = QStringLiteral(
                "Listening UDP broadcasts queued frames to every learned peer; use TCP or UDP Client for exact swarm control.");
        }
        return false;
    case LinkInterface::SIM_LINK:
    case LinkInterface::UNKNOWN_LINK:
        if (error) {
            *error = QStringLiteral(
                "Simulation or unknown transports are not exact swarm command routes.");
        }
        return false;
    }
    if (error) {
        *error = QStringLiteral("The exact physical link type is unsupported.");
    }
    return false;
}

void LinkManager::reloadSettings()
{
    if (m_shuttingDown) {
        return;
    }
    loadSettings();
    //Check to see if we have a single serial and single UDP connection, since they are the defaults

    bool foundserial = false;
    bool migratedLegacyStartupUdp = false;
    QSet<int> existingUdpPorts;
    QList<int> legacyStartupUdpCandidates;
    QSettings settings;
    const bool hasExplicitStartupUdp =
        PlannerStartupUdpOptions::hasExplicitConfiguration(settings);

    for (QMap<int,LinkInterface*>::const_iterator i= m_connectionMap.constBegin();i!=m_connectionMap.constEnd();i++)
    {
        if (i.value()->getLinkType() == LinkInterface::SERIAL_LINK)
        {
            foundserial = true;
        }
        else if (i.value()->getLinkType() == LinkInterface::UDP_LINK)
        {
            if (const auto *udp = qobject_cast<UDPLink *>(i.value())) {
                existingUdpPorts.insert(udp->getPort());
                // Older APM Planner builds persisted the implicit default
                // 14550 listener as an ordinary link. Adopt it once so the
                // new Startup UDP switch can actually disable it later.
                if (!hasExplicitStartupUdp
                    && udp->getPort()
                        == PlannerStartupUdpOptions::DefaultPrimaryPort
                    && udp->getHosts().isEmpty() && !signingRequired(i.key())
                    && connectionProfile(i.key()).id.startsWith(QStringLiteral("startup-udp-"))) {
                    legacyStartupUdpCandidates.append(i.key());
                }
            }
        }
    }
    if (legacyStartupUdpCandidates.size() == 1) {
        m_startupUdpLinkIds.insert(legacyStartupUdpCandidates.first());
        migratedLegacyStartupUdp = true;
    }
    if (!foundserial)
    {
        LinkManagerFactory::addSerialConnection();
    }
    const QList<int> startupPorts =
        PlannerStartupUdpOptions::load(settings).orderedPorts();
    for (int port : startupPorts) {
        if (existingUdpPorts.contains(port)) {
            continue;
        }
        const int linkId = LinkManagerFactory::addUdpConnection(
            QHostAddress::Any, port, false,
            ConnectionProfile{QStringLiteral("startup-udp-%1").arg(port),
                settings.contains(QStringLiteral("MAVLinkSigning/StartupRequiredPorts/%1").arg(port)), {}});
        if (linkId >= 0) {
            m_startupUdpLinkIds.insert(linkId);
            existingUdpPorts.insert(port);
        }
    }
    if (migratedLegacyStartupUdp || m_connectionRestoreError.isEmpty()) {
        // Rewrite the manual-link array immediately. A later settings change
        // or abnormal exit must not resurrect the adopted legacy listener.
        saveSettings();
    }
}

void LinkManager::syncActiveUasToTarget()
{
    if (m_shuttingDown) {
        return;
    }
    const VehicleTargetLease target =
        m_vehicleTargetManager->acquireTarget();
    if (!target.isValid()) {
        return;
    }
    UASInterface *const uas = getUas(target.endpoint.systemId);
    UASManager *const manager = UASManager::instance();
    if (uas && manager->getActiveUAS() != uas) {
        manager->setActiveUAS(uas);
    }
}

void LinkManager::syncTargetToActiveUas(UASInterface *uas)
{
    if (m_shuttingDown || !uas) {
        return;
    }
    const int systemId = uas->getUASID();
    const VehicleTargetLease current =
        m_vehicleTargetManager->acquireTarget();
    if (current.isValid() && current.endpoint.systemId == systemId) {
        return;
    }

    VehicleEndpoint selected;
    for (const VehicleEndpoint &endpoint
         : m_vehicleTargetManager->endpoints()) {
        if (endpoint.systemId != systemId
            || endpoint.componentId == MAV_COMP_ID_MISSIONPLANNER) {
            continue;
        }
        if (!selected.isValid()
            || (endpoint.componentId == MAV_COMP_ID_AUTOPILOT1
                && selected.componentId != MAV_COMP_ID_AUTOPILOT1)) {
            selected = endpoint;
        }
    }
    if (selected.isValid()) {
        m_vehicleTargetManager->selectTarget(
            selected.linkId, selected.systemId, selected.componentId);
    } else {
        m_vehicleTargetManager->clearTarget();
    }
}

void LinkManager::stopLogging()
{
    if (!m_mavlinkLoggingEnabled || !m_mavlinkProtocol)
    {
        return;
    }
    m_mavlinkProtocol->stopLogging();
}

LinkManager::~LinkManager()
{
    shutdown();
}

void LinkManager::shutdown()
{
    if (m_shuttingDown) {
        return;
    }
    m_shuttingDown = true;
    if (m_mavAuthKeyService) m_mavAuthKeyService->shutdown();

    // Stop exact multi-vehicle dispatch while every reservation, endpoint
    // session and physical route is still available for orderly cancellation.
    if (m_swarmWaypointLeaderExecutor) {
        m_swarmWaypointLeaderExecutor->cancelActiveRun(
            QStringLiteral("Application is shutting down."));
    }
    if (m_swarmSequenceExecutor) {
        m_swarmSequenceExecutor->cancelActiveOperation(
            QStringLiteral("Application is shutting down."));
    }

    // Persist configured connections before taking them out of the live map.
    saveSettings();

    QList<QPointer<UASInterface>> knownVehicles;
    for (const QPointer<UASInterface> &uas : m_uasMap) {
        if (!uas) {
            continue;
        }
        bool alreadyKnown = false;
        for (const QPointer<UASInterface> &known : knownVehicles) {
            if (known == uas) {
                alreadyKnown = true;
                break;
            }
        }
        if (!alreadyKnown) {
            knownVehicles.append(uas);
        }
    }

    // Flush any open MAVFTP session while exact-link lookup and the physical
    // transport are still available. No terminal signal is needed during
    // application shutdown.
    m_compassCalibrationService->shutdown();
    m_mavFtpService->shutdown();
    m_exactLogTransferService->shutdown();
    m_px4FlowService->shutdown();

    // Make every outbound lookup fail and detach ingress. Links remain live
    // while UASManager quiesces DroneCAN and other vehicle-owned transports.
    const QMap<int, LinkInterface *> links = m_connectionMap;
    m_connectionMap.clear();
    m_udpIngressRevision.clear();
    m_startupUdpLinkIds.clear();
    for (auto it = links.constBegin(); it != links.constEnd(); ++it) {
        const int linkId = it.key();
        LinkInterface *link = it.value();
        const quint64 swarmSession =
            m_swarmTelemetryRegistry->currentLinkSessionEpoch(linkId);
        if (swarmSession != 0) {
            // Retire exact instances while the shared transmitter still has
            // its link state, so any active swarm owner is cancelled first.
            m_swarmTelemetryRegistry->endLinkSession(linkId, swarmSession);
        }
        m_compassCalibrationService->forgetLink(linkId);
        m_guidedTargetService->forgetLink(linkId);
        m_movingBaseService->forgetLink(linkId);
        m_vehicleTargetManager->removeLink(linkId);
        m_vehicleCommandService->forgetLink(linkId);
        m_parameterService->forgetLink(linkId);
        m_mavFtpService->forgetLink(linkId);
        m_exactLinkTransmitter->forgetLink(linkId);
        m_radioStatusMonitor->forgetLink(linkId);
        if (m_mavlinkProtocol) {
            m_mavlinkProtocol->forgetLink(linkId);
            if (link) {
                disconnect(link,
                           SIGNAL(bytesReceived(LinkInterface*,QByteArray)),
                           m_mavlinkProtocol.data(),
                           SLOT(receiveBytes(LinkInterface*,QByteArray)));
            }
        }
    }

    // Ingress is detached, but direct exact-link writes still work here. Send
    // transport stop commands first, then cooperatively join every worker.
    UASManager *uasManager = UASManager::instance();
    uasManager->quiesceTransports();
    for (LinkInterface *link : links) {
        if (!link) {
            continue;
        }
        link->disconnect();
        link->requestInterruption();
        link->quit();
        if (link->isRunning() && !link->wait(5000)) {
            // Never terminate a QThread: it can leave locks and native socket
            // state corrupted. Built-in links all observe disconnect or
            // interruption; wait for a slow final iteration if necessary.
            QLOG_ERROR() << "Link required an extended shutdown wait:"
                         << link->getId();
            link->wait();
        }
    }

    // No communication worker can now access vehicle or protocol objects.
    uasManager->shutdown();
    for (const QPointer<UASInterface> &known : knownVehicles) {
        if (known) {
            delete known.data();
        }
    }
    m_uasMap.clear();
    qDeleteAll(m_uasObjectMap);
    m_uasObjectMap.clear();

    for (LinkInterface *link : links) {
        delete link;
    }

    m_vehicleTargetManager->clear();
    m_radioStatusMonitor->clear();
    m_mavlinkDecoder.reset();
    m_mavlinkProtocol.reset();
}

void LinkManager::loadSettings()
{
    struct StoredLink {
        QString type;
        QVariantMap values;
        QList<QPair<QString, int>> hosts;
        ConnectionProfile profile;
        bool legacyIdentity = false;
    };
    QVector<StoredLink> stored;
    QSettings settings;
    // Never let normalization redirect a corrupt startup listener to a new,
    // unsigned default profile. Validate before ANY saved listener can open.
    for (const char *key : {PlannerStartupUdpOptions::PrimaryPortSettingKey,
                            PlannerStartupUdpOptions::AlternatePortSettingKey}) {
        if (!settings.contains(QLatin1String(key))) continue;
        bool valid = false;
        const int port = settings.value(QLatin1String(key)).toInt(&valid);
        if (!valid || port < 1 || port > 65535)
            m_connectionRestoreError = tr("Stored startup UDP port is invalid; automatic connections are blocked.");
    }
    if (settings.contains(QLatin1String(PlannerStartupUdpOptions::EnabledSettingKey))) {
        const QString enabled = settings.value(QLatin1String(PlannerStartupUdpOptions::EnabledSettingKey))
            .toString().trimmed().toLower();
        if (enabled != "true" && enabled != "false" && enabled != "1" && enabled != "0")
            m_connectionRestoreError = tr("Stored startup UDP switch is invalid; automatic connections are blocked.");
    }
    settings.beginGroup("LINKMANAGER");
    m_mavlinkLoggingEnabled = settings.value("LOGGING", true).toBool();
    const int count = settings.beginReadArray("LINKS");
    if (count < 0 || count > 256)
        m_connectionRestoreError = tr("Connection settings exceed the supported profile limit.");
    for (int i = 0; i < qBound(0, count, 256); ++i) {
        settings.setArrayIndex(i);
        StoredLink item;
        item.type = settings.value("type").toString();
        if (item.type != "SERIAL_LINK" && item.type != "UDP_LINK"
            && item.type != "TCP_LINK" && item.type != "UDP_CLIENT_LINK")
            m_connectionRestoreError = tr("Unsupported stored connection profile type; settings were not rewritten.");
        for (const QString &key : {QString("port"), QString("baud"), QString("host"),
                                  QString("hostname"), QString("asServer")})
            item.values.insert(key, settings.value(key));
        item.profile.id = settings.value("profileId").toString();
        item.legacyIdentity = !settings.contains("profileId") && !settings.contains("signingRequired");
        const QString required = settings.value("signingRequired", false).toString();
        item.profile.signingRequired = required == QStringLiteral("true");
        if (required != QStringLiteral("true") && required != QStringLiteral("false"))
            item.profile.error = tr("Malformed signing requirement in connection settings.");
        if (item.profile.id.isEmpty()) {
            if (settings.contains("profileId") || item.profile.signingRequired)
                item.profile.error = tr("Stored signing profile identity is missing.");
            else
                item.profile.id = MavlinkSigningProfiles::newProfileId();
        }
        if (!MavlinkSigningProfiles::validProfileId(item.profile.id))
            item.profile.error = tr("Stored connection profile identity is invalid.");
        if (item.type == "UDP_LINK") {
            const int hostCount = settings.beginReadArray("HOSTS");
            if (hostCount < 0 || hostCount > 256)
                item.profile.error = tr("Stored UDP peer list exceeds its limit.");
            for (int j = 0; j < qBound(0, hostCount, 256); ++j) {
                settings.setArrayIndex(j);
                item.hosts.append({settings.value("host").toString(), settings.value("port").toInt()});
            }
            settings.endArray();
        }
        stored.append(item);
    }
    settings.endArray();
    const int portCount = settings.beginReadArray("PORTBAUDPAIRS");
    for (int i = 0; i < qBound(0, portCount, 256); ++i) {
        settings.setArrayIndex(i);
        m_portToBaudMap[settings.value("port").toString()] = settings.value("baud").toInt();
    }
    settings.endArray();
    settings.endGroup();
    if (settings.status() != QSettings::NoError)
        m_connectionRestoreError = tr("Connection settings are unreadable or malformed.");

    // Adopt only the old implicit, identity-less default. Never discard a
    // canonical manual UUID merely because it happens to use port 14550.
    QList<int> legacyDefaults;
    if (!PlannerStartupUdpOptions::hasExplicitConfiguration(settings)) {
        for (int i = 0; i < stored.size(); ++i) {
            const auto &item = stored.at(i);
            if (item.legacyIdentity && item.profile.error.isEmpty()
                && item.type == "UDP_LINK" && item.values.value("port").toInt() == 14550
                && item.hosts.isEmpty()) legacyDefaults.append(i);
        }
    }
    if (legacyDefaults.size() == 1)
        stored[legacyDefaults.first()].profile.id = QStringLiteral("startup-udp-14550");

    // A saved manual listener must not hide a required startup-port policy by
    // occupying its port before the startup pass gets to inspect that policy.
    for (auto &item : stored) {
        if (item.type != "UDP_LINK") continue;
        const int port = item.values.value("port").toInt();
        const QString startupId = QStringLiteral("startup-udp-%1").arg(port);
        const auto startupPolicy = MavlinkSigningProfiles::load(settings, startupId,
            settings.contains(QStringLiteral("MAVLinkSigning/StartupRequiredPorts/%1").arg(port)));
        if (startupPolicy.required && item.profile.id != startupId)
            item.profile.error = tr("Manual UDP listener conflicts with a required startup signing profile.");
    }
    // Validate the complete identity set before a factory may start a listener.
    QHash<QString, int> identities;
    for (const auto &item : stored) ++identities[item.profile.id];
    for (auto &item : stored) {
        if (identities.value(item.profile.id) > 1)
            item.profile.error = tr("Duplicate stored connection profile identity.");
        if (!m_connectionRestoreError.isEmpty()) item.profile.error = m_connectionRestoreError;
    }
    for (const auto &item : stored) {
        const int port = item.values.value("port").toInt();
        if (item.type == "SERIAL_LINK") {
            int baud = item.values.value("baud").toInt();
            if (baud < 0 || baud > 12500000) baud = 115200;
            LinkManagerFactory::addSerialConnection(item.values.value("port").toString(), baud, item.profile);
        } else if (item.type == "UDP_LINK") {
            const int id = LinkManagerFactory::addUdpConnection(QHostAddress::Any, port, true, item.profile);
            if (auto *udp = qobject_cast<UDPLink *>(getLink(id))) {
                for (const auto &host : item.hosts)
                    udp->addHost(QStringLiteral("%1:%2").arg(host.first).arg(host.second));
            }
        } else if (item.type == "TCP_LINK") {
            LinkManagerFactory::addTcpConnection(QHostAddress(item.values.value("host").toString()),
                item.values.value("hostname").toString(), port,
                item.values.value("asServer").toBool(), item.profile);
        } else if (item.type == "UDP_CLIENT_LINK") {
            LinkManagerFactory::addUdpClientConnection(QHostAddress(item.values.value("host").toString()), port, item.profile);
        }
    }
}

bool LinkManager::saveSettings()
{
    if (!m_connectionRestoreError.isEmpty()) return false;
    for (const auto &profile : m_connectionProfiles)
        if (!profile.error.isEmpty()) return false; // Preserve quarantined input verbatim.
    QSet<QString> knownHosts;

    QSettings settings;
    settings.beginGroup("LINKMANAGER");
    settings.setValue("LOGGING",m_mavlinkLoggingEnabled);
    settings.remove("LINKS"); // Do not retain stale type/endpoint fields in reused slots.
    settings.beginWriteArray("LINKS");
    int index = 0;
    for (QMap<int,LinkInterface*>::const_iterator i= m_connectionMap.constBegin();i!=m_connectionMap.constEnd();i++)
    {
        if (m_startupUdpLinkIds.contains(i.key())
            || i.value()->getLinkType() == LinkInterface::UNKNOWN_LINK
            || i.value()->getLinkType() == LinkInterface::SIM_LINK) {
            continue;
        }
        settings.setArrayIndex(index++);
        settings.setValue("linkid",i.value()->getId());
        const auto profile = connectionProfile(i.key());
        settings.setValue("profileId", profile.id);
        settings.setValue("signingRequired", signingRequired(i.key()));
        if (i.value()->getLinkType() == LinkInterface::SERIAL_LINK)
        {
            SerialConnection *link = qobject_cast<SerialConnection*>(i.value());
            settings.setValue("type","SERIAL_LINK");
            settings.setValue("port",link->getPortName());
            settings.setValue("baud",link->getBaudRate());
        }
        else if (i.value()->getLinkType() == LinkInterface::UDP_LINK)
        {
            UDPLink *link = qobject_cast<UDPLink*>(i.value());
            settings.setValue("type","UDP_LINK");
            settings.beginWriteArray("HOSTS");
            int storageCount = 0;
            const auto peers = link->peerSnapshot();
            for (int j=0;j<peers.hosts.size();j++)
            {
                QString hostName = peers.hosts.at(j).toString();
                if(hostName != "10.1.1.1")  // never store 10.1.1.1 which are created by Solo see issue #1121
                {
                    hostName.append(':');
                    hostName.append(QString::number(peers.ports.at(j)));
                    if(!knownHosts.contains(hostName))  // store all adresses only once
                    {
                        knownHosts.insert(hostName);
                        settings.setArrayIndex(storageCount++);
                        settings.setValue("host",peers.hosts.at(j).toString());
                        settings.setValue("port",peers.ports.at(j));
                    }
                }
            }
            settings.endArray();
            settings.setValue("port",link->getPort());
        }
        else if (i.value()->getLinkType() == LinkInterface::UDP_CLIENT_LINK)
        {
            UDPClientLink *link = qobject_cast<UDPClientLink*>(i.value());
            settings.setValue("type","UDP_CLIENT_LINK");
            settings.setValue("host",link->getHostAddress().toString());
            settings.setValue("port",link->getPort());
        }
        else if (i.value()->getLinkType() == LinkInterface::TCP_LINK)
        {
            TCPLink *link = qobject_cast<TCPLink*>(i.value());
            settings.setValue("type","TCP_LINK");
            settings.setValue("host",link->getHostAddress().toString());
            settings.setValue("hostname",link->getName());
            settings.setValue("port",link->getPort());
            settings.setValue("asServer",link->isServer());
        }
    }
    settings.endArray(); // LINKS
    settings.beginWriteArray("PORTBAUDPAIRS");
    index = 0;
    for (QMap<QString,int>::const_iterator i=m_portToBaudMap.constBegin();i!=m_portToBaudMap.constEnd();i++)
    {
        settings.setArrayIndex(index++);
        settings.setValue("port",i.key());
        settings.setValue("baud",i.value());
    }
    settings.endArray(); // PORTBAUDPAIRS
    settings.endGroup();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

void LinkManager::setLogSubDirectory(const QString& dir)
{
    m_logSubDir = dir;
    if (!m_logSubDir.startsWith(QChar('/')))
    {
        m_logSubDir.prepend('/');
    }
    if (!m_logSubDir.endsWith(QChar('/')))
    {
        m_logSubDir += QChar('/');
    }

    QDir logdir(QGC::MAVLinkLogDirectory());
    if (!logdir.cd(m_logSubDir.mid(1)))
    {
        logdir.mkdir(m_logSubDir.mid(1));
    }
}

void LinkManager::enableLogging(bool enabled)
{

    if (enabled)
    {
        m_mavlinkLoggingEnabled = enabled;
        startLogging();
    }
    else
    {
        stopLogging();
        m_mavlinkLoggingEnabled = enabled;
    }
}

bool LinkManager::loggingEnabled() const
{
    return m_mavlinkLoggingEnabled;
}

void LinkManager::startLogging()
{
    if (!m_mavlinkLoggingEnabled || !m_mavlinkProtocol)
    {
        return;
    }
    QString logFileName = QGC::MAVLinkLogDirectory() + m_logSubDir + QGC::fileNameAsTime();
    QLOG_DEBUG() << "LinkManger::startLogging()" << logFileName;
    m_mavlinkProtocol->startLogging(logFileName);
}


MAVLinkProtocol* LinkManager::getProtocol() const
{
    return m_mavlinkProtocol.data();
}

VehicleTargetManager *LinkManager::vehicleTargetManager() const
{
    return m_vehicleTargetManager;
}

SwarmTelemetryRegistry *LinkManager::swarmTelemetryRegistry() const
{
    return m_swarmTelemetryRegistry;
}

SwarmCommandService *LinkManager::swarmCommandService() const
{
    return m_swarmCommandService;
}

SwarmSequenceExecutor *LinkManager::swarmSequenceExecutor() const
{
    return m_swarmSequenceExecutor;
}

SwarmWaypointLeaderExecutor *LinkManager::swarmWaypointLeaderExecutor() const
{
    return m_swarmWaypointLeaderExecutor;
}

ExactMissionSnapshotService *LinkManager::exactMissionSnapshotService() const
{
    return m_exactMissionSnapshotService;
}

ExactLinkTransmitter *LinkManager::exactLinkTransmitter() const
{
    return m_exactLinkTransmitter;
}

ExactLogTransferService *LinkManager::exactLogTransferService() const
{
    return m_exactLogTransferService;
}

RadioStatusMonitor *LinkManager::radioStatusMonitor() const
{
    return m_radioStatusMonitor;
}

VehicleCommandService *LinkManager::vehicleCommandService() const
{
    return m_vehicleCommandService;
}

GuidedTargetService *LinkManager::guidedTargetService() const
{
    return m_guidedTargetService;
}

MovingBasePositionStore *LinkManager::movingBasePositionStore() const
{
    return m_movingBasePositionStore;
}

MovingBaseService *LinkManager::movingBaseService() const
{
    return m_movingBaseService;
}

CompassCalibrationService *LinkManager::compassCalibrationService() const
{
    return m_compassCalibrationService;
}

ParameterService *LinkManager::parameterService() const
{
    return m_parameterService;
}

MavlinkComponentRegistry *LinkManager::componentRegistry() const
{ return m_componentRegistry; }

Px4FlowService *LinkManager::px4FlowService() const
{ return m_px4FlowService; }

MavFtpServiceInterface *LinkManager::mavFtpService() const
{
    return m_mavFtpService;
}

QGCUASParamManager *LinkManager::parameterManager() const
{
    return m_parameterManager;
}

QObject *LinkManager::vehicleTargetManagerObject() const
{
    return m_vehicleTargetManager;
}

QObject *LinkManager::vehicleCommandServiceObject() const
{
    return m_vehicleCommandService;
}

QObject *LinkManager::parameterServiceObject() const
{
    return m_parameterService;
}

LinkInterface::LinkType LinkManager::getLinkType(int linkid)
{
    if (!m_connectionMap.contains(linkid))
    {
        return LinkInterface::UNKNOWN_LINK;
    }
    return m_connectionMap.value(linkid)->getLinkType();
}


void LinkManager::addLink(LinkInterface *link, const ConnectionProfile &requestedProfile)
{
    if (m_shuttingDown || !link) {
        QLOG_WARN() << "Ignoring link added during terminal shutdown";
        return;
    }
    QPointer<LinkInterface> guardedLink(link);
    const int linkId = link->getId();
    if (m_connectionMap.contains(linkId)) {
        QLOG_WARN() << "Ignoring duplicate physical link registration" << linkId;
        return;
    }
    const bool alreadyConnected = link->isConnected();
    const LinkInterface::LinkType linkType = link->getLinkType();
    ConnectionProfile profile = requestedProfile;
    if (profile.id.isEmpty()) {
        if (profile.signingRequired) profile.error = tr("Required signing profile identity is missing.");
        profile.id = MavlinkSigningProfiles::newProfileId();
    }
    QSettings settings;
    const auto policy = MavlinkSigningProfiles::load(settings, profile.id, profile.signingRequired);
    profile.signingRequired = policy.required;
    if (!policy.error.isEmpty()) profile.error = policy.error;
    if (!m_connectionRestoreError.isEmpty()) profile.error = m_connectionRestoreError;
    QList<int> duplicateLinks;
    for (auto it = m_connectionProfiles.begin(); it != m_connectionProfiles.end(); ++it) {
        if (it.key() != linkId && it->id == profile.id) {
            profile.error = tr("Duplicate connection profile identity; signing policy is quarantined.");
            it->error = profile.error;
            it->signingRequired = true;
            m_exactLinkTransmitter->setSigningRequired(it.key(), true);
            duplicateLinks.append(it.key());
        }
    }
    if (profile.signingRequired && profile.error.isEmpty()
        && !m_signingManager->requireSigning(linkId, profile.id, policy.fingerprint, &profile.error)) {
        if (profile.error.isEmpty()) profile.error = tr("Cannot restore required signing policy.");
    }
    if (!profile.error.isEmpty()) profile.signingRequired = true;
    m_connectionProfiles.insert(linkId, profile);
    m_exactLinkTransmitter->setSigningRequired(linkId, profile.signingRequired);
    m_connectionMap.insert(linkId, link);
    for (int duplicateId : duplicateLinks) {
        disconnectLink(duplicateId);
        if (!guardedLink || m_shuttingDown || getLink(linkId) != guardedLink.data()) return;
    }
    if (alreadyConnected && !signingReady(linkId)) {
        link->disconnect();
    } else if (alreadyConnected && !qobject_cast<UDPLink *>(link)) {
        activateLinkSession(link);
    } else {
        m_exactLinkTransmitter->setMotorStopLinkEligible(
            linkId, linkType == LinkInterface::SERIAL_LINK
                || linkType == LinkInterface::TCP_LINK);
    }
    // UDPLink is a known listening/broadcast transport: every outgoing
    // datagram is copied to every configured or learned peer. CompassMot's
    // stop ACK is not target-filtered by ArduCopter, so it is never eligible.
    // Ordered Serial/TCP streams are merely eligible: the safety UI separately
    // requires the operator to confirm this instance is a dedicated direct
    // connection rather than a router or radio network.
    if (!guardedLink
        || m_connectionMap.value(linkId, nullptr) != guardedLink.data()) {
        return;
    }
    emit newLink(linkId);
//    saveSettings();
}

LinkInterface* LinkManager::getLink(int linkId) const
{
    return m_connectionMap.value(linkId, nullptr);
}

bool LinkManager::isCurrentPhysicalIngress(LinkInterface *link) const
{
    if (m_shuttingDown || !link || getLink(link->getId()) != link
        || !signingReady(link->getId())) return false;
    if (auto *udp = qobject_cast<UDPLink *>(link)) {
        const quint64 revision = udp->peerSnapshot().revision;
        return revision != 0 && revision == m_udpIngressRevision.value(link->getId());
    }
    return true;
}

void LinkManager::receiveUdpDatagram(UDPLink *link, const QByteArray &bytes,
                                     quint64 peerRevision)
{
    if (m_shuttingDown || !link || getLink(link->getId()) != link
        || !link->isConnected() || !signingReady(link->getId()) || peerRevision == 0
        || link->peerSnapshot().revision != peerRevision) return;
    const QPointer<UDPLink> guardedLink(link);
    const int id = link->getId();
    if (m_udpIngressRevision.value(id) != peerRevision
        || currentPhysicalLinkSession(id) == 0) {
        // Retire the OLD epoch while it still names the old peer revision.
        // Final writes during retirement must fail, never reach the new peer.
        invalidateLinkSession(id);
        if (!guardedLink || getLink(id) != guardedLink
            || !guardedLink->isConnected()
            || guardedLink->peerSnapshot().revision != peerRevision) return;
        m_udpIngressRevision.insert(id, peerRevision);
        if (!activateLinkSession(link)) return;
    }
    if (!guardedLink || !isCurrentPhysicalIngress(guardedLink)
        || m_udpIngressRevision.value(id) != peerRevision) return;
    m_mavlinkProtocol->receiveBytes(guardedLink, bytes);
}

bool LinkManager::writeRawBytes(int linkId, const QByteArray &bytes)
{
    if (QThread::currentThread() != thread() || m_shuttingDown
        || signingRequired(linkId) || !m_signingManager->rawWritesAllowed(linkId)) return false;
    return writeBytesToTransport(linkId, bytes);
}

bool LinkManager::writeBytesToTransport(int linkId, const QByteArray &bytes)
{
    if (QThread::currentThread() != thread() || m_shuttingDown || !signingReady(linkId)) return false;
    const QPointer<LinkManager> self(this);
    QPointer<LinkInterface> link(m_connectionMap.value(linkId, nullptr));
    if (!link || !link->isConnected() || bytes.isEmpty()) {
        return false;
    }
    if (auto *udp = qobject_cast<UDPLink *>(link.data())) {
        if (!udp->enqueueForPeerRevision(bytes, m_udpIngressRevision.value(linkId))) {
            return false;
        }
    } else {
        link->writeBytes(bytes.constData(), bytes.size());
    }
    return self && link && m_connectionMap.value(linkId, nullptr) == link
        && link->isConnected() && isCurrentPhysicalIngress(link);
}

bool LinkManager::writeSequencedFrame(int linkId, const QByteArray &frame)
{
    if (QThread::currentThread() != thread() || m_shuttingDown) return false;
    const QPointer<LinkManager> self(this);
    const QPointer<LinkInterface> link(getLink(linkId));
    const quint64 epoch = currentPhysicalLinkSession(linkId);
    if (!link || !link->isConnected() || epoch == 0 || !isCurrentPhysicalIngress(link)) return false;
    const QByteArray &output = frame; // already signed once, before the writer callback
    // Decode only our freshly finalized bytes for the post-write observer.
    // Live authentication always consumes the original ingress bytes instead.
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned state = MAVLINK_FRAMING_INCOMPLETE;
    for (const char byte : output) state = parser.parseByte(quint8(byte), &message);
    if (state != MAVLINK_FRAMING_OK) return false;
    const bool submitted = writeBytesToTransport(linkId, output);
    if (submitted && self && link && !m_shuttingDown
        && getLink(linkId) == link && currentPhysicalLinkSession(linkId) == epoch
        && message.msgid != MAVLINK_MSG_ID_SETUP_SIGNING) {
        emit mavlinkMessageSubmitted(linkId, epoch, message);
    }
    return submitted;
}

bool LinkManager::writeMavlinkMessage(
    LinkInterface *physicalLink, mavlink_message_t message)
{
    if (QThread::currentThread() != thread() || m_shuttingDown || !physicalLink) return false;
    const int id = physicalLink->getId();
    if (getLink(id) != physicalLink) return false;
    return m_exactLinkTransmitter->sendMessage(id, message.sysid, message.compid, message)
        == ExactLinkTransmitter::SendResult::Sent;
}

const MAVLinkSigningManager *LinkManager::signingManager() const
{
    return m_signingManager.get();
}

LinkManager::ConnectionProfile LinkManager::connectionProfile(int linkId) const
{ return m_connectionProfiles.value(linkId); }

bool LinkManager::signingRequired(int linkId) const
{
    const auto profile = m_connectionProfiles.constFind(linkId);
    return m_signingManager->protectedLink(linkId)
        || (profile != m_connectionProfiles.cend() && (profile->signingRequired || !profile->error.isEmpty()));
}

bool LinkManager::signingReady(int linkId) const
{
    const auto profile = m_connectionProfiles.constFind(linkId);
    if (profile == m_connectionProfiles.cend() || !profile->error.isEmpty()) return false;
    return !signingRequired(linkId) || m_signingManager->status(linkId).keyAvailable;
}

MavAuthKeyService *LinkManager::mavAuthKeyService()
{
    if (m_shuttingDown || QThread::currentThread() != thread()) return nullptr;
    if (!m_mavAuthKeyService) {
        const QString directory = QDir(AppPaths::writableDataDirectory()).filePath(QStringLiteral("mavlink-signing"));
        if (!AppPaths::ensureDirectory(directory)) return nullptr;
        m_mavAuthKeyService = new MavAuthKeyService(QDir(directory).filePath(QStringLiteral("authkeys.vault")), this);
    }
    return m_mavAuthKeyService;
}

bool LinkManager::configureSigning(int linkId, const QString &connectionProfileId,
                                   const QString &keyName, const QByteArray &key,
                                   QString *error)
{
    if (error) error->clear();
    if (QThread::currentThread() != thread()) {
        if (error) *error = tr("Signing configuration belongs to the link manager thread.");
        return false;
    }
    LinkInterface *link = getLink(linkId);
    if (m_shuttingDown || !link || link->isConnected()
        || currentPhysicalLinkSession(linkId) != 0) {
        if (error) *error = tr("Disconnect the physical link before selecting its signing key.");
        return false;
    }
    auto profile = m_connectionProfiles.find(linkId);
    if (profile == m_connectionProfiles.end() || profile->id != connectionProfileId
        || !profile->error.isEmpty() || key.size() != 32
        || std::all_of(key.cbegin(), key.cend(), [](char c) { return c == 0; })) {
        if (error) *error = tr("Invalid key, mismatched profile or quarantined signing configuration.");
        return false;
    }
    // Publish the stable connection identity before its security record. A
    // crash after publishing the requirement must restore this same identity.
    if (!saveSettings()) {
        if (error) *error = tr("Cannot persist the connection identity before protecting it.");
        return false;
    }
    const QByteArray fingerprint = QCryptographicHash::hash(key, QCryptographicHash::Sha256);
    QSettings settings;
    const auto existing = MavlinkSigningProfiles::load(settings, connectionProfileId, profile->signingRequired);
    if (!existing.error.isEmpty() || (existing.required && existing.fingerprint != fingerprint)) {
        if (error) *error = existing.error.isEmpty() ? tr("The connection requires a different signing key.") : existing.error;
        return false;
    }
    // Once publication is attempted, an I/O error has uncertain durability.
    // Keep this process blocked too; never reconnect unsigned after a failure.
    profile->signingRequired = true;
    m_exactLinkTransmitter->setSigningRequired(linkId, true);
    if (!MavlinkSigningProfiles::saveRequired(settings, connectionProfileId, fingerprint, &profile->error)) {
        if (profile->error.isEmpty()) profile->error = tr("Signing policy publication failed.");
        if (error) *error = profile->error;
        return false;
    }
    if (connectionProfileId.startsWith(QStringLiteral("startup-udp-"))) {
        settings.setValue(QStringLiteral("MAVLinkSigning/StartupRequiredPorts/%1")
            .arg(connectionProfileId.mid(12)), true);
        settings.sync();
    }
    if (!m_signingManager->requireSigning(linkId, connectionProfileId, fingerprint, &profile->error)
        || settings.status() != QSettings::NoError || !saveSettings()) {
        if (profile->error.isEmpty()) profile->error = tr("Signing requirement could not be safely persisted.");
        if (error) *error = profile->error;
        return false;
    }
    const QString directory = QDir(AppPaths::writableDataDirectory())
        .filePath(QStringLiteral("mavlink-signing"));
    if (!AppPaths::ensureDirectory(directory)) {
        if (error) *error = tr("Cannot create the private signing-state directory.");
        return false;
    }
    if (!m_signingManager->protectLink(linkId, connectionProfileId, keyName, key,
                                      QDateTime::currentMSecsSinceEpoch(), error)) return false;
    m_exactLinkTransmitter->setSigningRequired(linkId, true);
    return true;
}

MAVLinkSigningManager::Verification LinkManager::verifyIncomingFrame(
    int linkId, quint64 epoch, const QByteArray &frame)
{
    if (!signingReady(linkId)) return {MAVLinkSigningManager::VerifyVerdict::NotReady,
        tr("Unlock and select the required signing key while the link is disconnected.")};
    return m_signingManager->verifyFrame(linkId, epoch, frame,
                                         QDateTime::currentMSecsSinceEpoch());
}

bool LinkManager::isUdpPortInUse(quint16 port) const
{
    for (LinkInterface *link : m_connectionMap) {
        const auto *udp = qobject_cast<UDPLink *>(link);
        if (udp && udp->isConnected() && udp->getPort() == port) {
            return true;
        }
    }
    return false;
}

void LinkManager::removeLink(LinkInterface *link)
{
   // This is called with a LINK_ID not an interface. needs mor rework
    //This function is not yet supported, it will be once we support multiple MAVs
    Q_ASSERT(link == nullptr); // This shoud not be called, assert if it anything but NULL
}

void LinkManager::removeLink(int linkId)
{
    LinkInterface *link = m_connectionMap.value(linkId, nullptr);
    if (!link) {
        return;
    }
    // Give active exact-target services their final bounded write opportunity
    // while the physical-link lookup is still valid, then invalidate every
    // session before the link object can be reused or destroyed.
    invalidateLinkSession(linkId);

    // Fail exact-link lookups and detach ingress before the worker begins
    // shutting down. Deleting a still-running QThread is undefined and was a
    // second shutdown-crash path when a connection was removed at runtime.
    m_connectionMap.remove(linkId);
    m_signingManager->removeLink(linkId);
    m_connectionProfiles.remove(linkId);
    m_exactLinkTransmitter->setSigningRequired(linkId, false);
    m_udpIngressRevision.remove(linkId);
    m_startupUdpLinkIds.remove(linkId);
    if (m_mavlinkProtocol) {
        disconnect(link,
                   SIGNAL(bytesReceived(LinkInterface*,QByteArray)),
                   m_mavlinkProtocol.data(),
                   SLOT(receiveBytes(LinkInterface*,QByteArray)));
    }
    link->disconnect();
    link->requestInterruption();
    link->quit();
    if (link->isRunning() && !link->wait(5000)) {
        QLOG_ERROR() << "Link required an extended removal wait:" << linkId;
        link->wait();
    }
    delete link;
    emit linkRemoved(linkId);
    if (!m_shuttingDown) {
        saveSettings();
    }
}

bool LinkManager::connectLink(int index)
{
    if (m_shuttingDown || !signingReady(index)) return false;
    if (m_connectionMap.contains(index))
    {
        return m_connectionMap.value(index)->connect();
    }
    return false;
}

void LinkManager::disconnectLink(int index)
{
    if (m_connectionMap.contains(index))
    {
        m_connectionMap.value(index)->disconnect();
    }
}

void LinkManager::linkUpdated(LinkInterface *link)
{
    // Editing an automatically-created listener turns it into a manual link;
    // otherwise the visible change would be silently discarded on shutdown.
    if (link) {
        m_startupUdpLinkIds.remove(link->getId());
    }
    emit linkChanged(link);
    emit linkChanged(link->getId());
}

QString LinkManager::getLinkName(int linkid)
{
    if (!m_connectionMap.contains(linkid))
    {
        return QString();
    }
    return m_connectionMap.value(linkid)->getName();
}

QString LinkManager::getLinkShortName(int linkid)
{
    if (!m_connectionMap.contains(linkid))
    {
        return QString();
    }
    return m_connectionMap.value(linkid)->getShortName();
}

QString LinkManager::getLinkDetail(int linkid)
{
    if (!m_connectionMap.contains(linkid))
    {
        return QString();
    }

    return m_connectionMap.value(linkid)->getDetail();
}

QString LinkManager::getSerialLinkPort(int linkid)
{
    if (m_connectionMap.contains(linkid))
    {
        if(SerialLinkInterface *iface = qobject_cast<SerialLinkInterface*>(m_connectionMap.value(linkid)))
        {
            return iface->getPortName();
        }
    }
    return QString();
}
bool LinkManager::getLinkConnected(int linkid)
{
    if (!m_connectionMap.contains(linkid))
    {
        return false;
    }
    return m_connectionMap.value(linkid)->isConnected();
}

int LinkManager::getSerialLinkBaud(int linkid)
{
    if (!m_connectionMap.contains(linkid))
    {
        return 0;
    }
    SerialLinkInterface *iface = qobject_cast<SerialLinkInterface*>(m_connectionMap.value(linkid));
    if (!iface)
    {
        return 0;
    }
    return iface->getBaudRate();
}

QStringList LinkManager::getCurrentPorts()
{
    QStringList m_portList;
    QList<QSerialPortInfo> portList =  QSerialPortInfo::availablePorts();

    if( portList.count() == 0){
        QLOG_INFO() << "No Ports Found" << m_portList;
        return m_portList;
    }

    m_portList.reserve(portList.count());
    foreach (const QSerialPortInfo &info, portList)
    {
        QLOG_TRACE() << "PortName    : " << info.portName()
                     << "Description : " << info.description();
        QLOG_TRACE() << "Manufacturer: " << info.manufacturer();

        m_portList.append(info.portName());
    }
    return m_portList;
}

void LinkManager::receiveMessage(LinkInterface* link,mavlink_message_t message)
{
    if (m_shuttingDown || !link
        || m_connectionMap.value(link->getId(), nullptr) != link) {
        return;
    }
    QPointer<LinkInterface> guardedLink(link);
    const int linkId = link->getId();
    const quint64 ingressSwarmSession =
        m_swarmTelemetryRegistry->currentLinkSessionEpoch(linkId);
    // A queued byte batch can arrive after physical disconnect while the
    // LinkInterface is still kept in the configuration map. Never let such a
    // packet revive legacy target/services or escape on the public stream.
    if (ingressSwarmSession == 0) {
        return;
    }
    const auto linkIsCurrent = [this, &guardedLink, linkId,
                                ingressSwarmSession]() {
        return guardedLink
            && m_connectionMap.value(linkId, nullptr) == guardedLink.data()
            && isCurrentPhysicalIngress(guardedLink)
            && m_swarmTelemetryRegistry->currentLinkSessionEpoch(linkId)
                == ingressSwarmSession;
    };
    if (!linkIsCurrent()) return;
    m_vehicleCommandService->observeMessage(linkId, message);
    if (!linkIsCurrent()) {
        return;
    }
    m_compassCalibrationService->observeMessage(linkId, message);
    if (!linkIsCurrent()) {
        return;
    }
    // ParameterService already consumed this physical frame before the UAS
    // gate. Replaying it here could acknowledge an operation submitted by a
    // synchronous completion callback for the preceding operation.
    m_mavFtpService->observeMessage(linkId, message);
    if (!linkIsCurrent()) {
        return;
    }
    // MP10 propagates RADIO/RADIO_STATUS to every vehicle on the link; the
    // monitor keys the statistics by this physical link only.
    m_radioStatusMonitor->observe(linkId, message,
                                  QDateTime::currentMSecsSinceEpoch());
    if (!linkIsCurrent()) {
        return;
    }
    if (VehicleTargetManager::isVisibleDiscoveryMessage(message.msgid)) {
        VehicleEndpoint endpoint;
        endpoint.linkId = link->getId();
        endpoint.systemId = message.sysid;
        endpoint.componentId = message.compid;
        endpoint.linkName = link->getShortName();
        endpoint.componentName = componentNameFromDiscoveryMessage(message);
        // Mission Planner makes the first visible MAVList entry current for a
        // port. Globally we only do so while no explicit target exists.
        m_vehicleTargetManager->observeEndpoint(
            endpoint, message.compid != MAV_COMP_ID_MISSIONPLANNER);
        if (!linkIsCurrent()) {
            return;
        }
        if (message.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
            mavlink_heartbeat_t heartbeat{};
            mavlink_msg_heartbeat_decode(&message, &heartbeat);
            m_vehicleTargetManager->observeHeartbeat(
                endpoint,
                (heartbeat.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0,
                heartbeat.autopilot, heartbeat.type);
        }
    }
    if (!linkIsCurrent()) {
        return;
    }
    if (m_swarmTelemetryRegistry->currentLinkSessionEpoch(linkId)
        != ingressSwarmSession) {
        return;
    }
    if (ingressSwarmSession != 0) {
        m_swarmTelemetryRegistry->observeMessage(
            linkId, ingressSwarmSession, message);
    }
    if (!linkIsCurrent()) {
        return;
    }
    m_exactMissionSnapshotService->observeMessage(
        linkId, ingressSwarmSession, message);
    if (!linkIsCurrent()) {
        return;
    }
    m_exactLogTransferService->observeMessage(linkId, ingressSwarmSession, message);
    if (!linkIsCurrent()) return;
    emit messageReceived(guardedLink.data(), message);
}

quint64 LinkManager::currentPhysicalLinkSession(int linkId) const
{
    return m_swarmTelemetryRegistry->currentLinkSessionEpoch(linkId);
}

UASInterface* LinkManager::getUas(int id)
{
    if (m_uasMap.contains(id))
    {
        return m_uasMap.value(id).data();
    }
    return nullptr;
}
QList<int> LinkManager::getLinks() const
{
    QList<int> links;
    links.reserve(m_connectionMap.values().count());
    foreach(LinkInterface *link, m_connectionMap.values())
    {
        links.append(link->getId());
    }
    return links;
}
void LinkManager::addSimObject(uint8_t sysid,UASObject *obj)
{
    if (!obj) {
        return;
    }
    delete m_uasObjectMap.take(sysid);
    m_uasObjectMap[sysid] = obj;
    obj->moveToThread(QApplication::instance()->thread());
}
void LinkManager::removeSimObject(uint8_t sysid)
{
    delete m_uasObjectMap.take(sysid);
}

UASInterface* LinkManager::createUAS(MAVLinkProtocol* mavlink, LinkInterface* link, int sysid, mavlink_heartbeat_t* heartbeat, QObject* parent)
{
    if (m_shuttingDown || !mavlink || !link || !heartbeat) {
        return nullptr;
    }
    QPointer<QObject> p (parent ? parent : mavlink );

    UASInterface* uas;

    switch (heartbeat->autopilot)
    {
    case MAV_AUTOPILOT_GENERIC:
    {
        UAS* mav = new UAS(mavlink, sysid);
        // Set the system type
        mav->setSystemType(static_cast<int>(heartbeat->type));
        // Connect this robot to the UAS object
        connect(mavlink, SIGNAL(messageReceived(LinkInterface*, mavlink_message_t)), mav, SLOT(receiveMessage(LinkInterface*, mavlink_message_t)));
#ifdef QGC_PROTOBUF_ENABLED
        connect(mavlink, SIGNAL(extendedMessageReceived(LinkInterface*, std::tr1::shared_ptr<google::protobuf::Message>)), mav, SLOT(receiveExtendedMessage(LinkInterface*, std::tr1::shared_ptr<google::protobuf::Message>)));
#endif
        uas = mav;
    }
    break;
//    case MAV_AUTOPILOT_PX4:
//    {
//        PxQuadMAV* mav = new PxQuadMAV(0, sysid);
//        // Set the system type
//        mav->setSystemType((int)heartbeat->type);
//        // Connect this robot to the UAS object
//        // it is IMPORTANT here to use the right object type,
//        // else the slot of the parent object is called (and thus the special
//        // packets never reach their goal)
//        connect(mavlink, SIGNAL(messageReceived(LinkInterface*, mavlink_message_t)), mav, SLOT(receiveMessage(LinkInterface*, mavlink_message_t)));
//#ifdef QGC_PROTOBUF_ENABLED
//        connect(mavlink, SIGNAL(extendedMessageReceived(LinkInterface*, std::tr1::shared_ptr<google::protobuf::Message>)), mav, SLOT(receiveExtendedMessage(LinkInterface*, std::tr1::shared_ptr<google::protobuf::Message>)));
//#endif
//        uas = mav;
//    }
//    break;
    case MAV_AUTOPILOT_SLUGS:
    {
        SlugsMAV* mav = new SlugsMAV(mavlink, sysid);
        // Set the system type
        mav->setSystemType(static_cast<int>(heartbeat->type));
        // Connect this robot to the UAS object
        // it is IMPORTANT here to use the right object type,
        // else the slot of the parent object is called (and thus the special
        // packets never reach their goal)
        connect(mavlink, SIGNAL(messageReceived(LinkInterface*, mavlink_message_t)), mav, SLOT(receiveMessage(LinkInterface*, mavlink_message_t)));
        uas = mav;
    }
    break;
    case MAV_AUTOPILOT_ARDUPILOTMEGA:
    {
        ArduPilotMegaMAV* mav = new ArduPilotMegaMAV(mavlink, sysid);

        // Set the system type
        mav->setSystemType(static_cast<int>(heartbeat->type));
        // Connect this robot to the UAS object
        // it is IMPORTANT here to use the right object type,
        // else the slot of the parent object is called (and thus the special
        // packets never reach their goal)
        connect(mavlink, SIGNAL(messageReceived(LinkInterface*, mavlink_message_t)), mav, SLOT(receiveMessage(LinkInterface*, mavlink_message_t)));
        uas = mav;
    }
    break;
#ifdef QGC_USE_SENSESOAR_MESSAGES
    case MAV_AUTOPILOT_SENSESOAR:
        {
            senseSoarMAV* mav = new senseSoarMAV(0,sysid);
            mav->setSystemType((int)heartbeat->type);
            connect(mavlink, SIGNAL(messageReceived(LinkInterface*, mavlink_message_t)), mav, SLOT(receiveMessage(LinkInterface*, mavlink_message_t)));
            uas = mav;
            break;
        }
#endif
    default:
    {
        UAS* mav = new UAS(mavlink, sysid);
        mav->setSystemType(static_cast<int>(heartbeat->type));
        // Connect this robot to the UAS object
        // it is IMPORTANT here to use the right object type,
        // else the slot of the parent object is called (and thus the special
        // packets never reach their goal)
        connect(mavlink, SIGNAL(messageReceived(LinkInterface*, mavlink_message_t)), mav, SLOT(receiveMessage(LinkInterface*, mavlink_message_t)));
        uas = mav;
    }
    break;
    }

    UASObject *obj = new UASObject();
    connect(mavlink,SIGNAL(messageReceived(LinkInterface*,mavlink_message_t)),obj,SLOT(messageReceived(LinkInterface*,mavlink_message_t)));
    delete m_uasObjectMap.take(sysid);
    m_uasObjectMap[sysid] = obj;
    connect(uas, &QObject::destroyed, this,
            [this, sysid, obj]() {
        // A replacement with the same sysid owns a different UASObject.  An
        // older vehicle's deferred destruction must not delete that object.
        if (m_uasObjectMap.value(sysid, nullptr) == obj) {
            delete m_uasObjectMap.take(sysid);
        }
    });

    m_uasMap.insert(sysid,uas);

    // Set the autopilot type
    uas->setAutopilotType(static_cast<int>(heartbeat->autopilot));
    uas->setParamManager(m_parameterManager);

    // Make UAS aware that this link can be used to communicate with the actual robot
    uas->addLink(link);

    // Now add UAS to "official" list, which makes the whole application aware of it
    UASManager::instance()->addUAS(uas);

    return uas;
}

UASObject *LinkManager::getUasObject(int uasid)
{
    if (m_uasObjectMap.contains(uasid))
    {
        return m_uasObjectMap.value(uasid);
    }
    return nullptr;
}

void LinkManager::protocolStatusMessageRec(QString title,QString text)
{
    emit protocolStatusMessage(title,text);
    QLOG_DEBUG() << "Protocol Status Message:" << title << text;
}

void LinkManager::linkConnected(LinkInterface* link)
{
    if (!link || m_connectionMap.value(link->getId(), nullptr) != link) {
        return;
    }
    const int linkId = link->getId();
    if (m_shuttingDown || !signingReady(linkId)) {
        disconnectLink(linkId);
        return;
    }
    if (qobject_cast<UDPLink *>(link)) {
        // A bound listening socket is not a peer session. Its first stamped
        // datagram establishes the exact physical identity before parsing.
        invalidateLinkSession(linkId);
        m_udpIngressRevision.remove(linkId);
        emit linkChanged(linkId);
        return;
    }
    if (!activateLinkSession(link)) {
        return;
    }
    emit linkChanged(linkId);
}

void LinkManager::linkDisonnected(LinkInterface* link)
{
    if (!link) {
        return;
    }
    const int linkId = link->getId();
    const QString linkName = link->getName();
    QLOG_DEBUG() << "LinkManager::linkDisonnected: "
                 << linkName << linkId;
    if (m_connectionMap.value(linkId, nullptr) == link) {
        // A live LinkInterface may reconnect under the same integer id. Treat
        // every physical disconnect as an epoch boundary immediately: an old
        // heartbeat/lease must never authorize commands to the next peer.
        invalidateLinkSession(linkId);
        m_udpIngressRevision.remove(linkId);
    }
    if (m_connectionMap.contains(linkId)) {
        emit linkChanged(linkId);
    }
}

bool LinkManager::activateLinkSession(LinkInterface *link)
{
    if (m_shuttingDown || !link
        || m_connectionMap.value(link->getId(), nullptr) != link) {
        return false;
    }
    if (!signingReady(link->getId())) {
        disconnectLink(link->getId());
        return false;
    }
    QPointer<LinkInterface> guardedLink(link);
    const int linkId = link->getId();
    const QString linkName = link->getShortName();
    const LinkInterface::LinkType linkType = link->getLinkType();

    // Some transports can reconnect/rebind without first delivering the typed
    // disconnected(LinkInterface*) signal. A new connected signal is always
    // an epoch boundary for every parser and exact-target service.
    invalidateLinkSession(linkId);
    if (m_shuttingDown || !guardedLink || !signingReady(linkId)
        || m_connectionMap.value(linkId, nullptr) != guardedLink.data()
        || !guardedLink->isConnected()
        || m_swarmTelemetryRegistry->currentLinkSessionEpoch(linkId) != 0) {
        return false;
    }
    m_exactLinkTransmitter->setMotorStopLinkEligible(
        linkId, linkType == LinkInterface::SERIAL_LINK
            || linkType == LinkInterface::TCP_LINK);
    const quint64 session =
        m_swarmTelemetryRegistry->beginLinkSession(linkId, linkName);
    return !m_shuttingDown && guardedLink && guardedLink->isConnected()
        && m_connectionMap.value(linkId, nullptr) == guardedLink.data()
        && m_swarmTelemetryRegistry->currentLinkSessionEpoch(linkId)
            == session;
}

void LinkManager::invalidateLinkSession(int linkId)
{
    if (linkId < 0) {
        return;
    }
    const quint64 swarmSession =
        m_swarmTelemetryRegistry->currentLinkSessionEpoch(linkId);
    if (swarmSession != 0) {
        m_swarmTelemetryRegistry->endLinkSession(linkId, swarmSession);
    }
    m_mavFtpService->forgetLink(linkId);
    m_compassCalibrationService->forgetLink(linkId);
    m_guidedTargetService->forgetLink(linkId);
    m_movingBaseService->forgetLink(linkId);
    m_vehicleTargetManager->removeLink(linkId);
    m_vehicleCommandService->forgetLink(linkId);
    m_parameterService->forgetLink(linkId);
    m_exactLinkTransmitter->forgetLink(linkId);
    m_radioStatusMonitor->forgetLink(linkId);
    if (m_mavlinkProtocol) {
        m_mavlinkProtocol->forgetLink(linkId);
    }
}

void LinkManager::linkErrorRec(LinkInterface *link,QString errorstring)
{
    emit linkError(link->getId(),errorstring);
}

void LinkManager::linkTimeoutTriggered(LinkInterface *link)
{
    Q_UNUSED(link)
    //Link has had a timeout
    //Disabled until it is fixed and more more robust - MLC
    //emit linkError(link->getId(),"Connected to link, but unable to receive any mavlink packets, (link is silent). Disconnecting");
    //link->disconnect();
}

void LinkManager::disableTimeouts(int index)
{
    if (!m_connectionMap.contains(index))
    {
        return;
    }
    m_connectionMap.value(index)->disableTimeouts();
}

void LinkManager::enableTimeouts(int index)
{
    if (!m_connectionMap.contains(index))
    {
        return;
    }
    m_connectionMap.value(index)->enableTimeouts();
}

void LinkManager::disableAllTimeouts()
{
    for (QMap<int,LinkInterface*>::const_iterator i = m_connectionMap.constBegin(); i != m_connectionMap.constEnd();i++)
    {
        i.value()->disableTimeouts();
    }
}

void LinkManager::enableAllTimeouts()
{
    for (QMap<int,LinkInterface*>::const_iterator i = m_connectionMap.constBegin(); i != m_connectionMap.constEnd();i++)
    {
        i.value()->disableTimeouts();
    }
}
