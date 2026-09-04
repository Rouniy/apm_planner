#include "ConfigFlightModesPageFactory.h"

#include "ConfigFlightModesView.h"
#include "FlightModeTelemetryMonitor.h"
#include "LinkInterface.h"
#include "LinkManager.h"
#include "QGCUASParamManager.h"
#include "comm/VehicleTargetManager.h"

#include <QDesktopServices>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>

namespace {
constexpr int kHeartbeatFreshnessMs = 3000;
constexpr int kFreshnessPollMs = 250;

using Family = ConfigFlightModesViewModel::Family;

Family pageFamily(const ConfigFlightModesPageContext &context)
{
    const QString px4First = QStringLiteral("COM_FLTMODE1");
    const int component = context.target.isValid()
        ? context.target.endpoint.componentId : 1;
    const bool hasPx4Schema = std::any_of(
        context.parameters.constBegin(), context.parameters.constEnd(),
        [component, &px4First](
            const ConfigFriendlyParameterValue &parameter) {
        return parameter.componentId == component
            && parameter.name.compare(px4First, Qt::CaseInsensitive) == 0;
    });
    if (hasPx4Schema) {
        return Family::Px4;
    }
    switch (context.firmwareFamily) {
    case ParameterFirmwareFamily::ArduCopter:
        return Family::Copter;
    case ParameterFirmwareFamily::ArduPlane:
        return Family::Plane;
    case ParameterFirmwareFamily::Rover:
        return Family::Rover;
    case ParameterFirmwareFamily::Unknown:
    case ParameterFirmwareFamily::ArduSub:
    case ParameterFirmwareFamily::AntennaTracker:
        return Family::Unsupported;
    }
    return Family::Unsupported;
}

bool optionContains(const QList<ParamOption> &options,
                    const QVariant &value)
{
    bool wantedOk = false;
    const double wanted = value.toDouble(&wantedOk);
    return std::any_of(options.constBegin(), options.constEnd(),
                       [wantedOk, wanted, &value](const ParamOption &option) {
        bool candidateOk = false;
        const double candidate = option.value.toDouble(&candidateOk);
        return wantedOk && candidateOk
            ? qAbs(wanted - candidate) <= 1.0e-6
            : option.value == value;
    });
}

void addOption(QList<ParamOption> *options, const QVariant &value,
               const QString &text)
{
    if (options && !optionContains(*options, value)) {
        options->append({value, text});
    }
}

QList<ParamOption> px4SlotOptions()
{
    // COM_FLTMODEn stores PX4's slot enum. These are deliberately not
    // heartbeat.custom_mode values, whose main/sub-mode fields are packed in
    // bits 16..31 and are decoded separately by CurrentModeText().
    return {
        {0, QStringLiteral("Unassigned")},
        {1, QStringLiteral("MANUAL")},
        {2, QStringLiteral("ALTITUDE CONTROL")},
        {3, QStringLiteral("POSITION CONTROL")},
        {4, QStringLiteral("AUTO / MISSION")},
        {5, QStringLiteral("AUTO / PAUSE")},
        {6, QStringLiteral("RETURN TO LAUNCH")},
        {7, QStringLiteral("ACRO")},
        {8, QStringLiteral("OFFBOARD")},
        {9, QStringLiteral("STABILIZED")},
        {10, QStringLiteral("RATTITUDE")},
        {11, QStringLiteral("AUTO / TAKEOFF")},
        {12, QStringLiteral("AUTO / LAND")}
    };
}

QList<ParamOption> modeOptions(
    const ParameterMetaDataCatalog &catalog, Family family)
{
    if (family == Family::Px4) {
        return px4SlotOptions();
    }
    QList<ParamOption> result;
    const QString prefix = ConfigFlightModesViewModel::ModePrefix(family);
    if (!prefix.isEmpty()) {
        const ParameterMetaData metadata = catalog.value(
            prefix + QStringLiteral("1"));
        result.reserve(metadata.values.size() + 1);
        for (const ParameterMetaDataOption &option : metadata.values) {
            addOption(&result, option.value,
                      option.label.isEmpty() ? option.rawCode : option.label);
        }
    }
    if (family == Family::Plane) {
        addOption(&result, 16, QStringLiteral("INITIALISING"));
    }
    // The model adds the MP10 fork's 31:ModelCal Copter option.
    return result;
}

QList<ConfigFriendlyParameterValue> snapshot(
    QGCUASParamManager *manager, int componentId)
{
    QList<ConfigFriendlyParameterValue> result;
    if (!manager) {
        return result;
    }
    const QList<QString> names = manager->getParameterNames(componentId);
    result.reserve(names.size());
    for (const QString &name : names) {
        QVariant value;
        if (manager->getParameterValue(componentId, name, value)) {
            result.append({componentId, name, value});
        }
    }
    return result;
}
} // namespace

ConfigFlightModesView *CreateConfigFlightModesPage(
    const ConfigFlightModesPageContext &context, QWidget *parent)
{
    auto *page = new ConfigFlightModesView(parent);
    const Family family = pageFamily(context);
    page->setFamily(family, modeOptions(context.catalog, family));

    LinkManager *const links = context.linkManager;
    VehicleTargetManager *const targets = links
        ? links->vehicleTargetManager() : nullptr;
    const VehicleTargetLease expectedTarget = context.target;
    const int expectedComponent = expectedTarget.isValid()
        ? expectedTarget.endpoint.componentId : 1;
    const QPointer<QGCUASParamManager> expectedManager(
        context.parameterManager);
    const QPointer<LinkInterface> expectedLink(
        expectedTarget.isValid() && links
            ? links->getLink(expectedTarget.endpoint.linkId) : nullptr);

    page->setParameterSnapshot(
        context.parameters, expectedComponent,
        context.parameterSnapshotComplete);

    const auto targetIsCurrent =
        [targets, expectedTarget, expectedManager, expectedLink]() {
        return expectedTarget.isValid() && targets && expectedManager
            && expectedLink
            && targets->isCurrentTarget(
                expectedTarget.endpoint.linkId,
                expectedTarget.endpoint.systemId,
                expectedTarget.endpoint.componentId,
                expectedTarget.generation);
    };
    const auto connectionIsCurrent =
        [targetIsCurrent, expectedLink]() {
        return targetIsCurrent() && expectedLink
            && expectedLink->isConnected();
    };
    const auto syncSafetyState =
        [page, targets, expectedTarget, connectionIsCurrent]() {
        const bool connected = connectionIsCurrent();
        page->setConnected(connected);
        const bool heartbeatFresh = connected && targets
            && targets->hasFreshHeartbeat(
                expectedTarget, kHeartbeatFreshnessMs);
        page->setHeartbeatFresh(heartbeatFresh);
        if (heartbeatFresh) {
            page->setArmed(targets->heartbeatArmed(expectedTarget));
        }
    };
    syncSafetyState();

    auto *freshnessTimer = new QTimer(page);
    freshnessTimer->setObjectName(
        QStringLiteral("flightModesHeartbeatFreshnessTimer"));
    freshnessTimer->setInterval(kFreshnessPollMs);
    QObject::connect(freshnessTimer, &QTimer::timeout,
                     page, syncSafetyState);
    freshnessTimer->start();

    if (expectedLink) {
        QObject::connect(expectedLink,
                         QOverload<bool>::of(&LinkInterface::connected),
                         page,
                         [syncSafetyState](bool) { syncSafetyState(); });
    }

    auto *monitor = new FlightModeTelemetryMonitor(
        targets, expectedTarget, page);
    monitor->setObjectName(QStringLiteral("flightModesTelemetryMonitor"));
    if (links) {
        QObject::connect(links, &LinkManager::messageReceived, monitor,
                         [monitor](LinkInterface *link,
                                   const mavlink_message_t &message) {
            if (link) {
                monitor->observeMessage(link->getId(), message);
            }
        });
    }
    QObject::connect(
        monitor, &FlightModeTelemetryMonitor::heartbeatObserved,
        page, [page](quint32 customMode, bool armed, int, int) {
        page->setHeartbeat(customMode, true, armed);
    });
    QObject::connect(monitor,
                     &FlightModeTelemetryMonitor::rcInputObserved,
                     page, &ConfigFlightModesView::setRcInput);
    QObject::connect(monitor,
                     &FlightModeTelemetryMonitor::targetInvalidated,
                     page, [page]() {
        page->setConnected(false);
        page->setHeartbeatFresh(false);
        page->clearRcInput();
    });

    QObject::connect(page, &ConfigFlightModesView::helpRequested,
                     page, [](const QUrl &url) {
        QDesktopServices::openUrl(url);
    });
    QObject::connect(
        page, &ConfigFlightModesView::refreshRequested, page,
        [page, targets, expectedTarget, targetIsCurrent,
         expectedManager, expectedComponent](int componentId) {
        if (!targetIsCurrent() || !expectedManager
            || componentId != expectedComponent) {
            page->refreshFailed(
                QObject::tr("not connected to the selected target"));
            return;
        }
        if (targets
            && targets->hasFreshHeartbeat(
                expectedTarget, kHeartbeatFreshnessMs)
            && targets->heartbeatArmed(expectedTarget)
            && QMessageBox::question(
                   page, QObject::tr("Refresh Params"),
                   QObject::tr("The vehicle is armed. Refreshing the complete "
                               "parameter list can consume telemetry bandwidth. "
                               "Continue?"),
                   QMessageBox::Yes | QMessageBox::No,
                   QMessageBox::No) != QMessageBox::Yes) {
            page->refreshCanceled();
            return;
        }
        if (expectedManager->parameterListInProgress()) {
            page->refreshFailed(
                QObject::tr("parameter refresh already in progress"));
            return;
        }
        expectedManager->requestParameterList();
    });
    QObject::connect(
        page, &ConfigFlightModesView::writeRequested, page,
        [page, targets, expectedTarget, targetIsCurrent,
         expectedManager, expectedLink, expectedComponent](
                quint64 requestId, int componentId,
                const QVariantList &changes) {
        const auto fail = [page, requestId](const QString &reason) {
            page->parameterWriteSubmissionFailed(requestId, reason);
        };
        if (!targetIsCurrent() || !expectedManager || !expectedLink
            || !expectedLink->isConnected()
            || componentId != expectedComponent) {
            fail(QObject::tr("not connected to the selected target"));
            return;
        }
        if (!targets || !targets->hasFreshHeartbeat(
                            expectedTarget, kHeartbeatFreshnessMs)) {
            fail(QObject::tr("a fresh exact-target heartbeat is required"));
            return;
        }
        if (targets->heartbeatArmed(expectedTarget)) {
            fail(QObject::tr("vehicle armed"));
            return;
        }
        if (changes.isEmpty()) {
            fail(QObject::tr("no changed flight mode parameters"));
            return;
        }
        const QList<QString> available =
            expectedManager->getParameterNames(componentId);
        for (const QVariant &item : changes) {
            const QString name = item.toMap()
                .value(QStringLiteral("name")).toString();
            if (!available.contains(name)) {
                fail(QObject::tr("parameter unavailable: %1").arg(name));
                return;
            }
        }
        const qulonglong batchId = expectedManager->writeParameters(
            componentId, changes, true);
        if (batchId == 0) {
            fail(QObject::tr("write was rejected for the selected target"));
            return;
        }
        page->parameterWriteSubmitted(requestId, batchId);
    });

    if (expectedManager) {
        QObject::connect(
            expectedManager,
            QOverload<int, QString, QVariant>::of(
                &QGCUASParamManager::parameterChanged),
            page,
            [page, targetIsCurrent, expectedComponent](
                    int componentId, const QString &name,
                    const QVariant &value) {
            if (targetIsCurrent() && componentId == expectedComponent) {
                page->parameterChanged(componentId, name, value);
            }
        });
        QObject::connect(
            expectedManager, &QGCUASParamManager::parameterWriteFailed,
            page,
            [page, targetIsCurrent, expectedComponent](
                    qulonglong, qulonglong batchId, int componentId,
                    const QString &name, int, const QString &reason) {
            if (targetIsCurrent() && componentId == expectedComponent) {
                page->parameterWriteFailed(
                    batchId, componentId, name, reason);
            }
        });
        QObject::connect(
            expectedManager,
            &QGCUASParamManager::parameterWriteCancelled, page,
            [page, targetIsCurrent, expectedComponent](
                    qulonglong, qulonglong batchId, int componentId,
                    const QString &name) {
            if (targetIsCurrent() && componentId == expectedComponent) {
                page->parameterWriteCancelled(
                    batchId, componentId, name);
            }
        });
        QObject::connect(
            expectedManager,
            &QGCUASParamManager::parameterBatchCompleted, page,
            [page](qulonglong batchId, int succeeded, int failed) {
            page->parameterBatchCompleted(batchId, succeeded, failed);
        });
        QObject::connect(
            expectedManager,
            &QGCUASParamManager::parameterListReadyChanged, page,
            [page, targetIsCurrent, expectedManager, expectedComponent,
             expectedTarget, firmwareFamily = context.firmwareFamily,
             catalog = context.catalog](bool ready) {
            if (!ready || !targetIsCurrent() || !expectedManager
                || page->viewModel()->HasPendingWrites()) {
                return;
            }
            const bool preserveStagedEdits =
                page->viewModel()->Dirty()
                && !page->viewModel()->ReconciliationRequired();
            const QList<ConfigFriendlyParameterValue> parameters =
                snapshot(expectedManager, expectedComponent);
            ConfigFlightModesPageContext refreshed;
            refreshed.firmwareFamily = firmwareFamily;
            refreshed.catalog = catalog;
            refreshed.parameters = parameters;
            refreshed.target = expectedTarget;
            const Family refreshedFamily = pageFamily(refreshed);
            if (refreshedFamily != page->viewModel()->VehicleFamily()) {
                page->setFamily(
                    refreshedFamily,
                    modeOptions(catalog, refreshedFamily));
            }
            page->setParameterSnapshot(
                parameters, expectedComponent, true,
                preserveStagedEdits);
        });
        QObject::connect(
            expectedManager,
            &QGCUASParamManager::parameterListLoadFailed,
            page, &ConfigFlightModesView::refreshFailed);
        QObject::connect(
            expectedManager,
            &QGCUASParamManager::parameterListLoadCanceled,
            page, &ConfigFlightModesView::refreshCanceled);
    }
    return page;
}
