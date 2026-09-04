#include "ConfigExtendedTuningPageFactory.h"

#include "ConfigExtendedTuningView.h"
#include "LinkInterface.h"
#include "LinkManager.h"
#include "QGCUASParamManager.h"
#include "comm/VehicleTargetManager.h"

#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

namespace {
constexpr int kHeartbeatFreshnessMs = 3000;
constexpr int kFreshnessPollMs = 250;

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

ConfigExtendedTuningView *CreateConfigExtendedTuningPage(
    const ConfigExtendedTuningPageContext &context, QWidget *parent)
{
    auto *page = new ConfigExtendedTuningView(parent);
    page->setCatalog(context.catalog, context.enforceMetadataRanges);

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
        const bool fresh = connected && targets
            && targets->hasFreshHeartbeat(
                expectedTarget, kHeartbeatFreshnessMs);
        page->setHeartbeatFresh(fresh);
        if (fresh) {
            page->setArmed(targets->heartbeatArmed(expectedTarget));
        }
    };
    syncSafetyState();

    auto *freshnessTimer = new QTimer(page);
    freshnessTimer->setObjectName(
        QStringLiteral("extendedTuningHeartbeatFreshnessTimer"));
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
    if (targets) {
        QObject::connect(targets,
                         &VehicleTargetManager::currentTargetChanged,
                         page, syncSafetyState);
    }

    QObject::connect(
        page, &ConfigExtendedTuningView::refreshRequested, page,
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
                   QObject::tr("The vehicle is armed. Refreshing the "
                               "complete parameter list can consume "
                               "telemetry bandwidth. Continue?"),
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
        page, &ConfigExtendedTuningView::writeRequested, page,
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
            fail(QObject::tr("no changed Extended Tuning parameters"));
            return;
        }
        const QList<QString> available =
            expectedManager->getParameterNames(componentId);
        for (const QVariant &item : changes) {
            const QString name = item.toMap()
                .value(QStringLiteral("name")).toString();
            if (name.isEmpty() || !available.contains(name)) {
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
            [page, targetIsCurrent](
                    qulonglong batchId, int succeeded, int failed) {
            if (targetIsCurrent()) {
                page->parameterBatchCompleted(
                    batchId, succeeded, failed);
            }
        });
        QObject::connect(
            expectedManager,
            &QGCUASParamManager::parameterListReadyChanged, page,
            [page, targetIsCurrent, expectedManager,
             expectedComponent](bool ready) {
            if (!ready || !targetIsCurrent() || !expectedManager
                || page->viewModel()->HasPendingWrites()) {
                return;
            }
            const bool preserveStagedEdits =
                page->viewModel()->Dirty()
                && !page->viewModel()->ReconciliationRequired();
            page->setParameterSnapshot(
                snapshot(expectedManager, expectedComponent),
                expectedComponent, true, preserveStagedEdits);
        });
        QObject::connect(
            expectedManager,
            &QGCUASParamManager::parameterListLoadFailed,
            page, [page, targetIsCurrent](const QString &reason) {
            if (targetIsCurrent()) {
                page->refreshFailed(reason);
            }
        });
        QObject::connect(
            expectedManager,
            &QGCUASParamManager::parameterListLoadCanceled,
            page, [page, targetIsCurrent]() {
            if (targetIsCurrent()) {
                page->refreshCanceled();
            }
        });
    }
    return page;
}
