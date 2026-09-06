#include "GuidedNavigationRuntimeAudit.h"

#include "comm/LinkManager.h"
#include "comm/LinkManagerFactory.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/TCPLink.h"
#include "comm/VehicleCommandService.h"
#include "services/GuidedAltitudeStore.h"
#include "services/GuidedNavigationService.h"
#include "ui/FlightDataView.h"
#include "ui/GuidedAltitudeDialog.h"
#include "ui/GuidedNavigationController.h"
#include "ui/MainWindow.h"
#include "ui/Terrain3DWindow.h"
#include "ui/map/AbstractMapWidget.h"
#include "ui/map/QGCMapTool.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include <cmath>
#include <functional>
#include <memory>

namespace
{
constexpr int FixtureLinkId = 910124;
constexpr quint8 FixtureSystemId = 236;

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}

template<typename T>
T *find(QObject *owner, const char *name)
{
    return owner
        ? owner->findChild<T *>(QString::fromLatin1(name)) : nullptr;
}

template<typename T>
T *visible(QObject *owner, const char *name)
{
    if (!owner) {
        return nullptr;
    }
    const auto children = owner->findChildren<T *>(QString::fromLatin1(name));
    for (T *child : children) {
        if (child->isVisible()) {
            return child;
        }
    }
    return nullptr;
}

void sendClick(QWidget *widget, const QPoint &position)
{
    if (!widget) {
        return;
    }
    QMouseEvent press(
        QEvent::MouseButtonPress, QPointF(position),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(
        QEvent::MouseButtonRelease, QPointF(position),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
}

class GuidedAuditLink final : public TCPLink
{
public:
    GuidedAuditLink()
        : TCPLink(QHostAddress::LocalHost,
                  QStringLiteral("Guided navigation audit"), 61984, false)
    {
    }

    int getId() const override { return FixtureLinkId; }
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

    void inject(const mavlink_message_t &message)
    {
        uint8_t buffer[MAVLINK_MAX_PACKET_LEN]{};
        const int size = mavlink_msg_to_send_buffer(buffer, &message);
        emit bytesReceived(
            this, QByteArray(reinterpret_cast<const char *>(buffer), size));
    }

    void heartbeat()
    {
        mavlink_message_t message{};
        mavlink_msg_heartbeat_pack(
            FixtureSystemId, 1, &message, MAV_TYPE_QUADROTOR,
            MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, MAV_STATE_STANDBY);
        inject(message);
    }

    void position()
    {
        mavlink_message_t message{};
        mavlink_msg_global_position_int_pack(
            FixtureSystemId, 1, &message, 1000,
            351856000, 333823000, 130000, 30000,
            0, 0, 0, 0);
        inject(message);
        mavlink_msg_attitude_pack(
            FixtureSystemId, 1, &message, 1000,
            0.0F, -0.35F, 0.0F, 0.0F, 0.0F, 0.0F);
        inject(message);
    }

    void home()
    {
        const float attitude[4]{1.0F, 0.0F, 0.0F, 0.0F};
        mavlink_message_t message{};
        mavlink_msg_home_position_pack(
            FixtureSystemId, 1, &message,
            351856000, 333823000, 130000,
            0.0F, 0.0F, 0.0F, attitude,
            0.0F, 0.0F, 0.0F, 1000000);
        inject(message);
    }

    void writeBytes(const char *bytes, qint64 size) override
    {
        mavlink_message_t message{};
        for (qint64 index = 0; index < size; ++index) {
            if (m_parser.parseByte(quint8(bytes[index]), &message)
                != MAVLINK_FRAMING_OK) {
                continue;
            }
            if (message.msgid == MAVLINK_MSG_ID_COMMAND_LONG) {
                mavlink_command_long_t startup{};
                mavlink_msg_command_long_decode(&message, &startup);
                if (startup.target_system != FixtureSystemId
                    || startup.target_component != 1) {
                    continue;
                }
                // Production startup asks for capabilities, message metadata
                // and stream intervals. Some application-owned requests use
                // the shared legacy command lane. This isolated fixture must
                // return a terminal ACK so they cannot keep the exact guided
                // lane busy.
                mavlink_command_ack_t acknowledgement{};
                acknowledgement.command = startup.command;
                const bool homeRequested =
                    startup.command == MAV_CMD_REQUEST_MESSAGE
                    && static_cast<quint32>(startup.param1)
                        == MAVLINK_MSG_ID_HOME_POSITION;
                acknowledgement.result = homeRequested
                    ? MAV_RESULT_ACCEPTED : MAV_RESULT_UNSUPPORTED;
                acknowledgement.target_system = message.sysid;
                acknowledgement.target_component = message.compid;
                mavlink_message_t reply{};
                mavlink_msg_command_ack_encode(
                    FixtureSystemId, 1, &reply, &acknowledgement);
                inject(reply);
                if (homeRequested) {
                    home();
                }
                continue;
            }
            if (message.msgid != MAVLINK_MSG_ID_COMMAND_INT) {
                continue;
            }
            mavlink_command_int_t command{};
            mavlink_msg_command_int_decode(&message, &command);
            if (command.command != MAV_CMD_DO_REPOSITION
                || command.target_system != FixtureSystemId
                || command.target_component != 1) {
                continue;
            }
            commands.append(command);
            QByteArray wire;
            wire.resize(mavlink_msg_get_send_buffer_length(&message));
            mavlink_msg_to_send_buffer(
                reinterpret_cast<uint8_t *>(wire.data()), &message);
            submittedFrames.append(wire);

            // Match the other production runtime fixtures: emit the reply at
            // the transport boundary immediately. Physical ingress remains
            // queued by LinkManagerFactory, so it cannot run until after the
            // frame writer returns and VehicleCommandService has recorded the
            // attempted frame.
            mavlink_command_ack_t acknowledgement{};
            acknowledgement.command = command.command;
            acknowledgement.result = MAV_RESULT_ACCEPTED;
            acknowledgement.progress = 100;
            acknowledgement.target_system = message.sysid;
            acknowledgement.target_component = message.compid;
            mavlink_message_t reply{};
            mavlink_msg_command_ack_encode(
                FixtureSystemId, 1, &reply, &acknowledgement);
            inject(reply);
        }
    }

    QVector<mavlink_command_int_t> commands;
    QVector<QByteArray> submittedFrames;

private:
    MAVLinkFrameParser m_parser;
    bool m_connected = false;
};

Terrain3DCore::Snapshot terrainSnapshot(
    const GuidedAltitudeStore::Context &context, bool staleInstance,
    qint64 capturedMonotonicMs)
{
    Terrain3DCore::Snapshot snapshot;
    snapshot.vehicle = {35.1856, 33.3823, 130.0};
    snapshot.relativeAltitudeM = 30.0;
    snapshot.pitchDeg = -25.0;
    snapshot.linkId = context.target.endpoint.linkId;
    snapshot.targetGeneration = context.target.generation;
    snapshot.linkSessionEpoch = context.vehicle.linkSessionEpoch;
    snapshot.vehicleInstanceEpoch = context.vehicle.instanceEpoch
        + (staleInstance ? 1 : 0);
    snapshot.capturedMonotonicMs = capturedMonotonicMs;
    snapshot.mode = QStringLiteral("STABILIZE");
    snapshot.systemId = quint8(context.target.endpoint.systemId);
    snapshot.componentId = quint8(context.target.endpoint.componentId);
    return snapshot;
}

Terrain3DWindow *openSyntheticTerrain(
    const GuidedAltitudeStore::Context &context,
    GuidedNavigationController *controller,
    SwarmTelemetryRegistry *registry,
    bool staleInstance,
    QWidget *owner)
{
    const QPointer<SwarmTelemetryRegistry> guardedRegistry(registry);
    const qint64 capturedMonotonicMs = registry
        ? registry->observationClockNowMs() : 0;
    const auto snapshot = std::make_shared<Terrain3DCore::Snapshot>(
        terrainSnapshot(context, staleInstance, capturedMonotonicMs));
    const QPointer<GuidedNavigationController> guardedController(controller);
    Terrain3DWindow::Dependencies dependencies;
    dependencies.snapshot = [snapshot]() { return *snapshot; };
    dependencies.monotonicClock = [guardedRegistry, capturedMonotonicMs]() {
        return guardedRegistry
            ? guardedRegistry->observationClockNowMs()
            : capturedMonotonicMs;
    };
    dependencies.elevation = [](double, double) { return 100.0; };
    dependencies.guidedTargetRequested =
        [guardedController](const Terrain3DCore::GeoPoint &point,
                            const Terrain3DCore::Snapshot &rendered) {
        if (guardedController) {
            guardedController->terrainClick(point, rendered);
        }
    };
    dependencies.guidedAltitudeEditRequested = [guardedController]() {
        if (guardedController) {
            guardedController->editAltitude();
        }
    };
    auto *window = new Terrain3DWindow(std::move(dependencies), owner);
    if (controller) {
        QObject::connect(
            controller, &GuidedNavigationController::statusChanged,
            window, &Terrain3DWindow::setGuidedStatus);
    }
    window->show();
    return window;
}

void closeWindow(QPointer<Terrain3DWindow> window)
{
    if (window) {
        window->close();
    }
    waitFor([&window]() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return window.isNull();
    }, 1500);
}

} // namespace

int RunGuidedNavigationRuntimeAudit()
{
    int failures = 0;
    const auto expect = [&](bool condition, const char *description) {
        if (!condition) {
            ++failures;
            qCritical() << "Guided navigation runtime:" << description;
        }
    };

    LinkManager *const links = LinkManager::instance();
    MainWindow *const main = MainWindow::instance();
    expect(links && main, "application services are unavailable");
    if (!links || !main) {
        return 1;
    }

    QSettings settings;
    settings.remove(QStringLiteral("guided_alt"));
    settings.remove(QStringLiteral("guided_alt_frame"));
    settings.setValue(QStringLiteral("altunits"), QStringLiteral("Meters"));
    settings.sync();
    const QString screenshotPrefix = qEnvironmentVariable(
        "APM_GUIDED_NAVIGATION_AUDIT_SCREENSHOT");

    QPointer<GuidedAuditLink> fixture(new GuidedAuditLink);
    struct ObservedAcknowledgement {
        quint64 epoch = 0;
        quint8 systemId = 0;
        quint8 componentId = 0;
        mavlink_command_ack_t payload{};
    };
    QVector<ObservedAcknowledgement> observedAcknowledgements;
    QObject acknowledgementScope;
    QObject::connect(
        links, &LinkManager::mavlinkMessageObserved,
        &acknowledgementScope,
        [&observedAcknowledgements](int linkId, qulonglong epoch,
                                    mavlink_message_t message) {
            if (linkId != FixtureLinkId
                || message.msgid != MAVLINK_MSG_ID_COMMAND_ACK) {
                return;
            }
            ObservedAcknowledgement observed;
            observed.epoch = epoch;
            observed.systemId = message.sysid;
            observed.componentId = message.compid;
            mavlink_msg_command_ack_decode(
                &message, &observed.payload);
            observedAcknowledgements.append(observed);
        });
    LinkManager::ConnectionProfile profile;
    profile.id = QStringLiteral("e93ed8c4-f6d4-4db7-ae7f-5b14ab8a1d12");
    LinkManagerFactory::connectLinkSignals(fixture, links);
    links->addLink(fixture, profile);
    expect(links->connectLink(FixtureLinkId), "fixture link did not connect");
    fixture->heartbeat();
    fixture->position();
    // Warning telemetry legitimately asks for HOME_POSITION once heartbeat
    // and position are fresh. Publish the metadata this hermetic vehicle
    // claims to support so its five-second retry cannot race later guided
    // consent. Requests are also handled above for ordering variations.
    fixture->home();
    auto *targets = links->vehicleTargetManager();
    auto *registry = links->swarmTelemetryRegistry();
    expect(waitFor([targets]() {
        return targets
            && targets->contains(FixtureLinkId, FixtureSystemId, 1);
    }), "fixture heartbeat did not discover the exact target");
    expect(targets && targets->selectTarget(
        FixtureLinkId, FixtureSystemId, 1),
        "fixture exact target could not be selected");
    expect(waitFor([targets, registry]() {
        if (!targets || !registry) {
            return false;
        }
        const VehicleTargetLease target = targets->acquireTarget();
        return target.isValid()
            && targets->isTargetGenerationSettled()
            && registry->acquireVehicle(target.endpoint, 3000).isValid();
    }), "fixture exact physical instance did not settle");

    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout,
                     fixture, [fixture]() {
        if (fixture) {
            fixture->heartbeat();
        }
    });
    heartbeat.start(250);

    QAction *const flightAction = find<QAction>(main, "actionFlightView");
    expect(flightAction, "production Flight Data navigation action is missing");
    if (flightAction) {
        flightAction->trigger();
        QCoreApplication::processEvents();
    }
    FlightDataView *const data = main->findChild<FlightDataView *>();
    QGCMapTool *const mapTool = data
        ? data->findChild<QGCMapTool *>() : nullptr;
    AbstractMapWidget *const map = mapTool ? mapTool->mapWidget() : nullptr;
    GuidedNavigationController *const controller = mapTool
        ? mapTool->findChild<GuidedNavigationController *>() : nullptr;
    QAction *const flyToHere = mapTool
        ? mapTool->findChild<QAction *>(QStringLiteral("FlyToHereAction"))
        : nullptr;
    QAction *const editAltitude = mapTool
        ? mapTool->findChild<QAction *>(
              QStringLiteral("FlyToHereAltitudeAction"))
        : nullptr;
    QAction *const flyToCoordinates = mapTool
        ? mapTool->findChild<QAction *>(
              QStringLiteral("FlyToCoordinatesAction"))
        : nullptr;
    expect(data && data->isVisible() && mapTool && map && controller,
           "actual DATA map/controller route is missing");
    expect(flyToHere && editAltitude && flyToCoordinates,
           "actual DATA guided actions are missing");
    if (flyToHere && editAltitude && flyToCoordinates) {
        expect(flyToHere->text() == QStringLiteral("Fly To Here")
                   && editAltitude->text()
                       == QString::fromUtf8("Fly To Here Alt…")
                   && flyToCoordinates->text()
                       == QStringLiteral("Fly To Coords"),
               "actual DATA guided labels differ from MP10");
    }
    int liveGuidedMaps = 0;
    const auto mapTools = main->findChildren<QGCMapTool *>();
    for (QGCMapTool *const candidate : mapTools) {
        QAction *const fly = candidate->findChild<QAction *>(
            QStringLiteral("FlyToHereAction"));
        if (!fly) {
            continue;
        }
        ++liveGuidedMaps;
        const auto *const candidateController =
            candidate->findChild<GuidedNavigationController *>();
        QAction *const altitude = candidate->findChild<QAction *>(
            QStringLiteral("FlyToHereAltitudeAction"));
        QAction *const coordinates = candidate->findChild<QAction *>(
            QStringLiteral("FlyToCoordinatesAction"));
        expect(candidateController,
               "a live map with Fly To Here has no production controller");
        expect(fly->isEnabled()
                   && fly->text() == QStringLiteral("Fly To Here")
                   && altitude && altitude->isEnabled()
                   && altitude->text() == QString::fromUtf8(
                       "Fly To Here Alt…")
                   && coordinates && coordinates->isEnabled()
                   && coordinates->text() == QStringLiteral("Fly To Coords"),
               "a live map retained legacy or disabled guided actions");
    }
    expect(liveGuidedMaps >= 2,
           "not all constructed DATA/Plan/Simulation maps were audited");
    int actualMapTargetRequests = 0;
    double requestedLatitude = 0.0;
    double requestedLongitude = 0.0;
    if (map && fixture) {
        QObject::connect(
            map, &AbstractMapWidget::GuidedTargetRequested,
            fixture, [&actualMapTargetRequests, &requestedLatitude,
                      &requestedLongitude](double latitude,
                                           double longitude) {
                ++actualMapTargetRequests;
                requestedLatitude = latitude;
                requestedLongitude = longitude;
            });
    }

    const VehicleTargetLease selected = targets
        ? targets->acquireTarget() : VehicleTargetLease{};
    VehicleCommandService *const commands = links->vehicleCommandService();
    const auto commandLaneReady = [commands, selected]() {
        return commands && selected.isValid()
            && !commands->legacyCommandPendingFor(selected.endpoint);
    };
    GuidedAltitudeStore::Context initialContext;
    QString contextError;
    expect(links->guidedAltitudeStore()
               && links->guidedAltitudeStore()->prepareCurrent(
                   &initialContext, &contextError),
           "guided store did not prepare the production exact context");

    const int beforeTerrain = fixture ? fixture->commands.size() : 0;
    QPointer<Terrain3DWindow> staleTerrain;
    if (initialContext.target.isValid() && controller) {
        staleTerrain = openSyntheticTerrain(
            initialContext, controller, registry, true, main);
        expect(waitFor([staleTerrain]() {
            return staleTerrain && !staleTerrain->isBusy()
                && !staleTerrain->frame().isNull();
        }), "stale rendered Terrain fixture did not render");
        QLabel *const image = find<QLabel>(staleTerrain, "TerrainImage");
        if (image) {
            sendClick(image, image->rect().center());
        }
        expect(controller->statusText().contains(
                   QStringLiteral("old vehicle instance"),
                   Qt::CaseInsensitive),
               "stale rendered instance was not rejected");
        expect(!visible<QMessageBox>(main, "GuidedNavigationConfirmation")
                   && fixture->commands.size() == beforeTerrain,
               "stale Terrain frame reached consent or transport");
    }
    closeWindow(staleTerrain);

    QAction *const terrainAction = find<QAction>(main, "actionTerrain3D");
    expect(terrainAction, "production Terrain route is missing");
    QPointer<Terrain3DWindow> productionTerrain;
    QPointer<GuidedNavigationController> productionTerrainController;
    if (terrainAction) {
        terrainAction->trigger();
        QCoreApplication::processEvents();
        productionTerrain = main->findChild<Terrain3DWindow *>();
        expect(productionTerrain && productionTerrain->isVisible()
                   && productionTerrain->isWindow(),
               "production Terrain route did not open its real window");
        productionTerrainController = productionTerrain
            ? productionTerrain->findChild<GuidedNavigationController *>()
            : nullptr;
        expect(productionTerrainController,
               "production Terrain window has no guided controller");
        fixture->heartbeat();
        fixture->position();
        expect(waitFor([productionTerrain]() {
            return productionTerrain && !productionTerrain->isBusy()
                && !productionTerrain->frame().isNull();
        }, 8000),
               "production Terrain route did not render selected telemetry");
        if (productionTerrain && !screenshotPrefix.isEmpty()) {
            expect(productionTerrain->grab().save(
                       screenshotPrefix + QStringLiteral(".terrain.png")),
                   "Terrain window screenshot failed");
        }
        const int beforeMissingAltitude = fixture
            ? fixture->commands.size() : 0;
        if (auto *image = find<QLabel>(productionTerrain, "TerrainImage")) {
            sendClick(image, image->rect().center());
        }
        expect(waitFor([productionTerrain]() {
            return productionTerrain
                && productionTerrain->guidedStatusText().contains(
                    QStringLiteral("non-zero guided altitude"),
                    Qt::CaseInsensitive);
        }), "production Terrain mesh hit did not refuse missing altitude");
        expect(!visible<QMessageBox>(main, "GuidedNavigationConfirmation")
                   && fixture->commands.size() == beforeMissingAltitude,
               "production Terrain missing-altitude path reached consent or transport");
        auto *guidedButton = find<QPushButton>(
            productionTerrain, "TerrainGuidedAltitudeButton");
        expect(guidedButton && guidedButton->isEnabled(),
               "production Terrain window did not bind the shared editor");
        if (guidedButton) {
            guidedButton->click();
            expect(waitFor([main]() {
                return visible<GuidedAltitudeDialog>(
                    main, "GuidedAltitudeDialog") != nullptr;
            }), "production Terrain editor button opened no dialog");
            if (auto *dialog = visible<GuidedAltitudeDialog>(
                    main, "GuidedAltitudeDialog")) {
                const QLabel *const target = find<QLabel>(
                    dialog, "GuidedAltitudeTarget");
                expect(target && target->text().contains(
                           QString::number(FixtureSystemId)),
                       "Terrain editor omitted the frozen exact target");
                dialog->reject();
            }
        }
    }

    if (map && map->Widget()) {
        map->SetCurrentPosition(35.1856, 33.3823);
        sendClick(map->Widget(), map->Widget()->rect().center());
    }

    if (flyToHere) {
        flyToHere->trigger();
    }
    expect(actualMapTargetRequests == 1,
           "actual DATA Fly To Here action did not emit one frozen map point");
    GuidedAltitudeDialog *altitudeDialog = nullptr;
    expect(waitFor([main, &altitudeDialog]() {
        altitudeDialog = visible<GuidedAltitudeDialog>(
            main, "GuidedAltitudeDialog");
        return altitudeDialog != nullptr;
    }), "Fly To Here did not request an explicit altitude");
    if (altitudeDialog) {
        auto *value = find<QLineEdit>(altitudeDialog, "GuidedAltitudeValue");
        auto *frame = find<QComboBox>(altitudeDialog, "GuidedAltitudeFrame");
        auto *apply = find<QPushButton>(
            altitudeDialog, "GuidedAltitudeAcceptButton");
        expect(value && frame && apply,
               "guided altitude dialog controls are incomplete");
        if (value && frame && apply) {
            value->setText(QStringLiteral("25"));
            frame->setCurrentIndex(frame->findData(
                int(MAV_FRAME_GLOBAL)));
            if (!screenshotPrefix.isEmpty()) {
                expect(altitudeDialog->grab().save(
                           screenshotPrefix + QStringLiteral(".altitude.png")),
                       "guided altitude screenshot failed");
            }
            apply->click();
        }
    }

    QMessageBox *confirmation = nullptr;
    expect(waitFor([main, &confirmation]() {
        confirmation = visible<QMessageBox>(
            main, "GuidedNavigationConfirmation");
        return confirmation != nullptr;
    }), "accepted altitude opened no movement confirmation");
    if (confirmation) {
        expect(confirmation->defaultButton()
                   == confirmation->button(QMessageBox::Cancel)
                   && confirmation->escapeButton()
                       == confirmation->button(QMessageBox::Cancel),
               "guided movement confirmation is not default/Escape Cancel");
        if (!screenshotPrefix.isEmpty()) {
            expect(confirmation->grab().save(
                       screenshotPrefix + QStringLiteral(".consent.png")),
                   "guided consent screenshot failed");
        }
        confirmation->reject();
    }
    expect(waitFor([controller]() {
        return controller && !controller->busy();
    }), "cancelled movement consent left the controller busy");
    expect(fixture && fixture->commands.isEmpty(),
           "cancelled movement confirmation transmitted a frame");

    if (flyToHere) {
        flyToHere->trigger();
    }
    expect(actualMapTargetRequests == 2,
           "second DATA Fly To Here did not reuse the actual map source");
    confirmation = nullptr;
    expect(waitFor([main, &confirmation]() {
        confirmation = visible<QMessageBox>(
            main, "GuidedNavigationConfirmation");
        return confirmation != nullptr;
    }), "second Fly To Here opened no frozen confirmation");
    if (confirmation) {
        auto *yes = qobject_cast<QPushButton *>(
            confirmation->button(QMessageBox::Yes));
        expect(yes, "guided confirmation Yes button is unavailable");
        const bool laneReady = waitFor(commandLaneReady);
        expect(laneReady,
               "production startup command lane did not quiesce before guided admission");
        if (yes && laneReady) {
            yes->click();
        }
    }
    auto *navigation = links->guidedNavigationService();
    const bool dataCompleted = waitFor([fixture, navigation]() {
        return fixture && fixture->commands.size() == 1
            && navigation && !navigation->busy();
    });
    expect(dataCompleted,
           "confirmed guided command did not reach a terminal ACK");
    if (!dataCompleted && navigation) {
        const auto report = navigation->lastReport();
        qCritical() << "Guided DATA ACK diagnostic: physical ACKs"
                    << observedAcknowledgements.size()
                    << "commands" << (fixture ? fixture->commands.size() : -1)
                    << "busy" << navigation->busy()
                    << "status" << navigation->status()
                    << "operation" << navigation->currentOperationId()
                    << "report valid/outcome/result/attempted/attempts/recorded"
                    << report.isValid() << int(report.outcome)
                    << report.mavResult << report.frameAttempted
                    << report.transmissionAttempts << report.targetRecorded;
        if (!observedAcknowledgements.isEmpty()) {
            const auto &ack = observedAcknowledgements.constLast();
            qCritical() << "Last physical ACK epoch/source/command/result/target"
                        << ack.epoch << ack.systemId << ack.componentId
                        << ack.payload.command << ack.payload.result
                        << ack.payload.target_system
                        << ack.payload.target_component;
        }
    }
    if (fixture && fixture->commands.size() == 1) {
        const mavlink_command_int_t command = fixture->commands.constFirst();
        expect(command.command == MAV_CMD_DO_REPOSITION
                   && command.target_system == FixtureSystemId
                   && command.target_component == 1
                   && command.frame == MAV_FRAME_GLOBAL
                   && command.param1 == -1.0F
                   && command.param2 == 1.0F
                   && command.x
                       == static_cast<qint32>(requestedLatitude * 1.0e7)
                   && command.y
                       == static_cast<qint32>(requestedLongitude * 1.0e7)
                   && std::abs(command.z - 25.0F) < 0.0001F,
               "confirmed guided COMMAND_INT payload differs");
        expect(fixture->submittedFrames.constFirst().size() > 8,
               "exact post-encoding guided frame was not captured");
    }
    if (navigation) {
        const auto report = navigation->lastReport();
        expect(report.outcome == GuidedNavigationService::Outcome::Accepted
                   && report.transmissionAttempts == 1
                   && report.frameAttempted && report.targetRecorded,
               "accepted guided ACK was not reported truthfully");
    }

    GuidedAltitudeStore::Context acceptedContext;
    contextError.clear();
    expect(links->guidedAltitudeStore()->prepareCurrent(
               &acceptedContext, &contextError)
               && acceptedContext.altitudeSet
               && acceptedContext.frame == MAV_FRAME_GLOBAL,
           "accepted DATA altitude/frame did not remain in the shared store");
    const int beforeAcceptedTerrain = fixture
        ? fixture->commands.size() : 0;
    if (acceptedContext.target.isValid() && productionTerrain
        && productionTerrainController) {
        const qint64 earlierCapture =
            productionTerrain->renderedSnapshot().capturedMonotonicMs;
        QThread::msleep(2);
        fixture->position();
        expect(waitFor([productionTerrain, earlierCapture]() {
            return productionTerrain && !productionTerrain->isBusy()
                && !productionTerrain->frame().isNull()
                && productionTerrain->renderedSnapshot().capturedMonotonicMs
                    != earlierCapture;
        }, 8000), "production Terrain did not render a fresh exact frame");
        QLabel *const image = find<QLabel>(productionTerrain, "TerrainImage");
        if (image) {
            sendClick(image, image->rect().center());
        }
        QMessageBox *terrainConfirmation = nullptr;
        expect(waitFor([main, &terrainConfirmation]() {
            terrainConfirmation = visible<QMessageBox>(
                main, "GuidedNavigationConfirmation");
            return terrainConfirmation != nullptr;
        }), "exact Terrain mesh hit opened no movement consent");
        if (terrainConfirmation) {
            expect(terrainConfirmation->text().contains(
                       QStringLiteral("Does NOT change flight mode"))
                       && terrainConfirmation->text().contains(
                           QStringLiteral("RELATIVE"))
                       && terrainConfirmation->text().contains(
                           QStringLiteral("Absolute")),
                   "Terrain frame-3 reinterpretation warning is incomplete");
            if (!screenshotPrefix.isEmpty()) {
                expect(terrainConfirmation->grab().save(
                           screenshotPrefix
                               + QStringLiteral(".terrain-consent.png")),
                       "Terrain consent screenshot failed");
            }
            terrainConfirmation->reject();
        }
        expect(waitFor([productionTerrainController]() {
            return productionTerrainController
                && !productionTerrainController->busy();
        }), "Terrain confirmation Cancel left the controller busy");
        expect(fixture->commands.size() == beforeAcceptedTerrain,
               "Terrain confirmation Cancel transmitted a frame");

        if (image) {
            sendClick(image, image->rect().center());
        }
        terrainConfirmation = nullptr;
        expect(waitFor([main, &terrainConfirmation]() {
            terrainConfirmation = visible<QMessageBox>(
                main, "GuidedNavigationConfirmation");
            return terrainConfirmation != nullptr;
        }), "second production Terrain mesh hit opened no consent");
        if (terrainConfirmation) {
            auto *yes = qobject_cast<QPushButton *>(
                terrainConfirmation->button(QMessageBox::Yes));
            expect(yes, "Terrain consent Yes button is unavailable");
            const bool laneReady = waitFor(commandLaneReady);
            expect(laneReady,
                   "production startup command lane did not quiesce before Terrain admission");
            if (yes && laneReady) {
                yes->click();
            }
        }
        const bool terrainCompleted = waitFor(
            [fixture, navigation, beforeAcceptedTerrain]() {
            return fixture
                && fixture->commands.size() == beforeAcceptedTerrain + 1
                && navigation && !navigation->busy();
        });
        expect(terrainCompleted,
               "confirmed Terrain target did not reach a terminal ACK");
        if (!terrainCompleted && navigation) {
            const auto report = navigation->lastReport();
            qCritical() << "Guided Terrain ACK diagnostic: physical ACKs"
                        << observedAcknowledgements.size()
                        << "commands" << (fixture ? fixture->commands.size() : -1)
                        << "busy" << navigation->busy()
                        << "status" << navigation->status()
                        << "operation" << navigation->currentOperationId()
                        << "report valid/outcome/result/attempted/attempts/recorded"
                        << report.isValid() << int(report.outcome)
                        << report.mavResult << report.frameAttempted
                        << report.transmissionAttempts << report.targetRecorded;
            if (!observedAcknowledgements.isEmpty()) {
                const auto &ack = observedAcknowledgements.constLast();
                qCritical() << "Last physical ACK epoch/source/command/result/target"
                            << ack.epoch << ack.systemId << ack.componentId
                            << ack.payload.command << ack.payload.result
                            << ack.payload.target_system
                            << ack.payload.target_component;
            }
        }
        if (fixture
            && fixture->commands.size() == beforeAcceptedTerrain + 1) {
            const mavlink_command_int_t command = fixture->commands.constLast();
            expect(command.command == MAV_CMD_DO_REPOSITION
                       && command.target_system == FixtureSystemId
                       && command.target_component == 1
                       && command.frame == MAV_FRAME_GLOBAL_RELATIVE_ALT
                       && command.param1 == -1.0F
                       && command.param2 == 0.0F
                       && std::abs(command.z - 25.0F) < 0.0001F,
                   "confirmed Terrain COMMAND_INT was not frame 3/no-mode-change");
        }
        if (navigation) {
            const auto report = navigation->lastReport();
            expect(report.outcome
                           == GuidedNavigationService::Outcome::Accepted
                       && report.plan.purpose()
                           == GuidedNavigationService::Purpose::TerrainClick
                       && report.transmissionAttempts == 1
                       && report.frameAttempted && report.targetRecorded,
                   "accepted Terrain ACK was not reported truthfully");
        }
    }
    const int finalCommandCount = fixture ? fixture->commands.size() : 0;
    closeWindow(productionTerrain);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    expect(fixture && fixture->commands.size() == finalCommandCount,
           "closing Terrain emitted an extra guided frame");

    heartbeat.stop();
    if (controller) {
        controller->cancel();
    }
    links->removeLink(FixtureLinkId);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    expect(!fixture && (!navigation || !navigation->busy()),
           "guided fixture cleanup left a transport operation active");
    expect(selected.isValid(), "selected exact lease was unexpectedly empty");

    qInfo() << "Guided navigation runtime audit failures:" << failures
            << "(actual DATA/Terrain routes; isolated COMMAND_INT/ACK; no real vehicle)";
    return failures ? 1 : 0;
}
