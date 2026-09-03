#include "ConfigAdvancedView.h"

#include <QAction>
#include <QObject>
#include <QPointer>
#include <QPushButton>

ConfigAdvancedView::ConfigAdvancedView(QObject *actionSource,
                                       QWidget *parent)
    : ActionPageView(
          tr("Advanced"),
          tr("The following tools are for advanced configuration only — "
             "use with caution."),
          parent)
    , m_actionSource(actionSource)
{
    setObjectName(QStringLiteral("ConfigAdvancedView"));

    const QString notPorted = tr("This Mission Planner tool has not yet "
                                 "been ported to Qt.");

    AddUnavailableAction(tr("Anon Log"),
                         QStringLiteral("AnonLogButton"), notPorted);
    AddToolAction(tr("MAVLink Inspector"),
                  QStringLiteral("MAVLinkInspectorButton"),
                  QStringLiteral("actionMavlinkInspector"));
    AddToolAction(tr("Mavlink Mirror"),
                  QStringLiteral("MavlinkMirrorButton"),
                  QStringLiteral("actionMavlinkMirror"));
    AddToolAction(tr("NMEA"),
                  QStringLiteral("NmeaButton"),
                  QStringLiteral("actionNmeaOutput"));
    AddUnavailableAction(tr("Cursor-on-Target / TAK"),
                         QStringLiteral("CotTakButton"), notPorted);
    AddUnavailableAction(tr("Follow Me"),
                         QStringLiteral("FollowMeButton"), notPorted);
    AddUnavailableAction(tr("External Guided"),
                         QStringLiteral("ExternalGuidedButton"), notPorted);
    AddUnavailableAction(tr("Moving Base"),
                         QStringLiteral("MovingBaseButton"), notPorted);
    AddToolAction(tr("Map Tile Cache"),
                  QStringLiteral("MapTileCacheButton"),
                  QStringLiteral("actionMapTileCache"));
    AddUnavailableAction(tr("MAVLink Signing"),
                         QStringLiteral("MavlinkSigningButton"), notPorted);
    AddUnavailableAction(tr("FFT"),
                         QStringLiteral("FftButton"), notPorted);
    AddUnavailableAction(tr("Spectrogram"),
                         QStringLiteral("SpectrogramButton"), notPorted);
    AddUnavailableAction(tr("Warning Manager"),
                         QStringLiteral("WarningManagerButton"), notPorted);
    AddToolAction(tr("Proximity"),
                  QStringLiteral("ProximityButton"),
                  QStringLiteral("actionProximity"));
    AddUnavailableAction(tr("Param gen"),
                         QStringLiteral("ParamGenButton"), notPorted);
    AddUnavailableAction(tr("Support Proxy"),
                         QStringLiteral("SupportProxyButton"), notPorted);

    AppendLog(tr("%1 of %2 Mission Planner Advanced tools are available. "
                 "Unavailable actions remain disabled until their full "
                 "workflow is ported.")
                  .arg(m_implementedActionCount)
                  .arg(ActionCount()));
}

int ConfigAdvancedView::ImplementedActionCount() const
{
    return m_implementedActionCount;
}

QPushButton *ConfigAdvancedView::AddToolAction(
    const QString &label, const QString &buttonObjectName,
    const QString &actionObjectName)
{
    QAction *action = m_actionSource
        ? m_actionSource->findChild<QAction *>(actionObjectName)
        : nullptr;
    if (!action) {
        return AddUnavailableAction(
            label, buttonObjectName,
            tr("The shared application action '%1' is unavailable.")
                .arg(actionObjectName));
    }

    QPointer<QAction> guardedAction(action);
    QPushButton *button = AddAction(
        label, buttonObjectName,
        [this, guardedAction, label]() {
            if (!guardedAction || !guardedAction->isEnabled()) {
                AppendLog(tr("%1 is currently unavailable.").arg(label));
                return;
            }
            guardedAction->trigger();
            AppendLog(tr("Opened %1.").arg(label));
        },
        action->isEnabled(), action->toolTip());
    ++m_implementedActionCount;

    connect(action, &QAction::changed, button,
            [button, guardedAction]() {
        button->setEnabled(guardedAction && guardedAction->isEnabled());
        button->setToolTip(guardedAction ? guardedAction->toolTip()
                                         : QString());
    });
    connect(action, &QObject::destroyed, button,
            [button]() { button->setEnabled(false); });
    return button;
}
