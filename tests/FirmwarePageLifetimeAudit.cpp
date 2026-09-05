#include "FirmwarePageLifetimeAudit.h"

#include "ui/configuration/ApmCustomFirmwareConfig.h"

#include <QDebug>
#include <QList>
#include <QPointer>
#include <QScopedPointer>

namespace {
bool auditDestroyedHeaderLifetime()
{
    int calls = 0;
    bool disabled = false;
    QScopedPointer<QObject> lifetime(new QObject);
    QPointer<QObject> guardedLifetime(lifetime.data());
    ApmCustomFirmwareConfig page(
        lifetime.data(),
        [&calls, &disabled](bool value) {
            ++calls;
            disabled = value;
        },
        nullptr);

    page.show();
    if (calls != 1 || !disabled) {
        qWarning() << "firmware lifetime audit: show did not disable header";
        return false;
    }
    lifetime.reset();
    if (!guardedLifetime.isNull()) {
        qWarning() << "firmware lifetime audit: owner was not destroyed";
        return false;
    }

    // Mirrors MainWindow member teardown: its header has gone away before
    // SetupView resets and hides the selected firmware page.
    page.hide();
    if (calls != 1) {
        qWarning() << "firmware lifetime audit: dead header callback invoked";
        return false;
    }
    return true;
}

bool auditLiveHeaderBalance()
{
    QObject lifetime;
    QList<bool> calls;
    ApmCustomFirmwareConfig page(
        &lifetime,
        [&calls](bool disabled) { calls.append(disabled); },
        nullptr);
    page.show();
    page.hide();
    if (calls != QList<bool>{true, false}) {
        qWarning() << "firmware lifetime audit: live disable/restore unbalanced"
                   << calls;
        return false;
    }
    return true;
}
} // namespace

bool RunFirmwarePageLifetimeAudit()
{
    return auditDestroyedHeaderLifetime() && auditLiveHeaderBalance();
}
