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

    AddToolAction(tr("Anon Log"), QStringLiteral("AnonLogButton"),
                  QStringLiteral("actionAnonLog"));
    AddToolAction(tr("MAVLink Inspector"),
                  QStringLiteral("MAVLinkInspectorButton"),
                  QStringLiteral("actionMavlinkInspector"));
    AddToolAction(tr("Mavlink Mirror"),
                  QStringLiteral("MavlinkMirrorButton"),
                  QStringLiteral("actionMavlinkMirror"));
    AddToolAction(tr("NMEA"),
                  QStringLiteral("NmeaButton"),
                  QStringLiteral("actionNmeaOutput"));
    AddToolAction(tr("Cursor-on-Target / TAK"),
                  QStringLiteral("CotTakButton"),
                  QStringLiteral("actionCotOutput"));
    AddToolAction(tr("Follow Me"),
                  QStringLiteral("FollowMeButton"),
                  QStringLiteral("actionFollowMe"));
    AddToolAction(tr("External Guided"),
                  QStringLiteral("ExternalGuidedButton"),
                  QStringLiteral("actionExternalGuided"));
    AddToolAction(tr("Moving Base"),
                  QStringLiteral("MovingBaseButton"),
                  QStringLiteral("actionMovingBase"));
    AddToolAction(tr("Map Tile Cache"),
                  QStringLiteral("MapTileCacheButton"),
                  QStringLiteral("actionMapTileCache"));
    AddToolAction(tr("MAVLink Signing"),
                  QStringLiteral("MavlinkSigningButton"),
                  QStringLiteral("actionMavlinkSigning"), false);
    AddToolAction(tr("FFT"), QStringLiteral("FftButton"),
                  QStringLiteral("actionFftAnalysis"));
    AddToolAction(tr("Spectrogram"),
                  QStringLiteral("SpectrogramButton"),
                  QStringLiteral("actionDataFlashSpectrogram"));
    AddToolAction(tr("Warning Manager"), QStringLiteral("WarningManagerButton"),
                  QStringLiteral("actionWarningManager"));
    AddToolAction(tr("Proximity"),
                  QStringLiteral("ProximityButton"),
                  QStringLiteral("actionProximity"));
    AddToolAction(tr("Param gen"), QStringLiteral("ParamGenButton"),
                  QStringLiteral("actionParameterMetaDataRegeneration"));
    AddUnavailableAction(tr("Support Proxy"),
                         QStringLiteral("SupportProxyButton"), notPorted);

    AppendLog(tr("%1 of %2 Mission Planner Advanced tools have complete workflows. "
                 "%3 additional local-only tool is available; vehicle provisioning "
                 "and disabling Signing are not yet available.")
                  .arg(m_implementedActionCount)
                  .arg(ActionCount()).arg(m_partialActionCount));
}

int ConfigAdvancedView::ImplementedActionCount() const
{
    return m_implementedActionCount;
}

QPushButton *ConfigAdvancedView::AddToolAction(
    const QString &label, const QString &buttonObjectName,
    const QString &actionObjectName, bool completeWorkflow)
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
    if (completeWorkflow) ++m_implementedActionCount;
    else ++m_partialActionCount;

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
