#include "ConfigPlannerViewIntegration.h"

#include "ConfigPlannerView.h"
#include "GAudioOutput.h"
#include "LinkManager.h"
#include "MainWindow.h"
#include "QGCSettingsWidget.h"
#include "configuration.h"
#include "map/CompiledMapBackends.h"
#include "map/MapWidgetFactory.h"

#include <QDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QPointer>
#include <QVBoxLayout>

void BindConfigPlannerViewToApplication(ConfigPlannerView *view)
{
    if (!view) return;

    MainWindow *const mainWindow = MainWindow::instance();
    LinkManager *const links = LinkManager::instance();
    GAudioOutput *const audio = GAudioOutput::instance();
    MapWidgetFactory *const maps = MapWidgetFactory::instance();
    RegisterCompiledMapBackends();

    const QPointer<ConfigPlannerView> guardedView(view);
    const auto refreshSpeech = [guardedView, audio]() {
        if (!guardedView || !guardedView->viewModel()) return;
        if (!guardedView->viewModel()->speechEnabled()) {
            guardedView->setSpeechBackendStatus(QObject::tr(
                "Speech is disabled."));
        } else if (audio->isMuted()) {
            guardedView->setSpeechBackendStatus(QObject::tr(
                "Speech is enabled, but all audio output is muted."));
        } else if (!audio->isSpeechReady()) {
            guardedView->setSpeechBackendStatus(QObject::tr(
                "Speech engine is unavailable."));
        } else {
            guardedView->setSpeechBackendStatus(QObject::tr(
                "Speech engine is ready."));
        }
    };
    const auto refreshRuntime = [guardedView, mainWindow, links, audio]() {
        if (!guardedView) return;
        guardedView->setAudioMuted(audio->isMuted());
        guardedView->setHeartbeatEnabled(mainWindow->heartbeatEnabled());
        guardedView->setMavlinkLoggingEnabled(links->loggingEnabled());
        guardedView->setAutoProxyEnabled(mainWindow->autoProxyModeEnabled());
        guardedView->setLogDirectories(QGC::logDirectory(),
                                       QGC::MAVLinkLogDirectory());
    };
    const auto refreshMaps = [guardedView, maps]() {
        if (!guardedView) return;
        QList<ConfigPlannerMapBackend> choices;
        for (const MapWidgetBackendInfo &backend : maps->AvailableBackends()) {
            choices.append({backend.id, backend.displayName});
        }
        QString selected = maps->RequestedBackend();
        if (selected.isEmpty()) selected = maps->CurrentBackend();
        guardedView->setMapBackends(choices, selected, maps->LastStatus());
    };

    QObject::connect(view, &ConfigPlannerView::altitudeUnitsChanged,
                     mainWindow, &MainWindow::setPlannerAltitudeUnits);
    QObject::connect(view, &ConfigPlannerView::distanceUnitsChanged,
                     mainWindow, &MainWindow::setPlannerDistanceUnits);
    QObject::connect(mainWindow, &MainWindow::plannerAltitudeUnitsChanged,
                     view, [guardedView](const QString &units) {
        if (guardedView && guardedView->viewModel()) {
            guardedView->viewModel()->setAltitudeUnits(units);
        }
    });
    QObject::connect(mainWindow, &MainWindow::plannerDistanceUnitsChanged,
                     view, [guardedView](const QString &units) {
        if (guardedView && guardedView->viewModel()) {
            guardedView->viewModel()->setDistanceUnits(units);
        }
    });
    QObject::connect(view, &ConfigPlannerView::audioMuteChanged,
                     audio, &GAudioOutput::mute);
    QObject::connect(audio, &GAudioOutput::mutedChanged,
                     view, &ConfigPlannerView::setAudioMuted);
    QObject::connect(audio, &GAudioOutput::mutedChanged,
                     view, [refreshSpeech](bool) { refreshSpeech(); });
    QObject::connect(audio, &GAudioOutput::speechEnabledChanged,
                     view, [refreshSpeech](bool) { refreshSpeech(); });
    QObject::connect(view, &ConfigPlannerView::speechTestRequested,
                     view, [guardedView, audio, refreshSpeech]() {
        if (!guardedView || !guardedView->viewModel()) return;
        if (!guardedView->viewModel()->speechEnabled()) {
            guardedView->setSpeechBackendStatus(QObject::tr(
                "Enable Speech before running the test."));
            return;
        }
        if (audio->say(QStringLiteral(
                "Проверка звука Mission Planner 10"))) {
            guardedView->setSpeechBackendStatus(QObject::tr(
                "Speech test was sent to the audio engine."));
        } else {
            refreshSpeech();
        }
    });
    const auto promptTemplate = [guardedView](
            const QString &title, const QString &current,
            QString *configured) {
        if (!guardedView || !configured) return false;
        bool accepted = false;
        const QString text = QInputDialog::getText(
            guardedView, title, QObject::tr("What do you want it to say?"),
            QLineEdit::Normal, current, &accepted);
        if (!guardedView || !accepted || text.isEmpty()) return false;
        *configured = text;
        return true;
    };
    QObject::connect(
        view, &ConfigPlannerView::speechWaypointConfigurationRequested,
        view, [guardedView, promptTemplate]() {
        if (!guardedView || !guardedView->viewModel()) return;
        const QPointer<ConfigPlannerViewModel> model = guardedView->viewModel();
        QString speechTemplate;
        if (!promptTemplate(QObject::tr("Waypoint"),
                            model->speechWaypointTemplate(),
                            &speechTemplate)
            || !model) {
            return;
        }
        if (model->setSpeechWaypointTemplate(speechTemplate)) {
            model->setSpeechWaypointEnabled(true);
        }
    });
    QObject::connect(
        view, &ConfigPlannerView::speechModeConfigurationRequested,
        view, [guardedView, promptTemplate]() {
        if (!guardedView || !guardedView->viewModel()) return;
        const QPointer<ConfigPlannerViewModel> model = guardedView->viewModel();
        QString speechTemplate;
        if (!promptTemplate(QObject::tr("Mode"),
                            model->speechModeTemplate(), &speechTemplate)
            || !model) {
            return;
        }
        if (model->setSpeechModeTemplate(speechTemplate)) {
            model->setSpeechModeEnabled(true);
        }
    });
    QObject::connect(
        view, &ConfigPlannerView::speechCustomConfigurationRequested,
        view, [guardedView, promptTemplate]() {
        if (!guardedView || !guardedView->viewModel()) return;
        const QPointer<ConfigPlannerViewModel> model = guardedView->viewModel();
        QString speechTemplate;
        if (!promptTemplate(QObject::tr("Custom"),
                            model->speechCustomTemplate(), &speechTemplate)
            || !model) {
            return;
        }
        if (model->setSpeechCustomTemplate(speechTemplate)) {
            model->setSpeechCustomEnabled(true);
        }
    });
    QObject::connect(
        view, &ConfigPlannerView::speechBatteryConfigurationRequested,
        view, [guardedView, promptTemplate]() {
        if (!guardedView || !guardedView->viewModel()) return;
        const QPointer<ConfigPlannerViewModel> model = guardedView->viewModel();
        QString speechTemplate;
        if (!promptTemplate(QObject::tr("Battery"),
                            model->speechBatteryTemplate(),
                            &speechTemplate)
            || !guardedView || !model) {
            return;
        }
        bool accepted = false;
        const double voltage = QInputDialog::getDouble(
            guardedView, QObject::tr("Battery Level"),
            QObject::tr("What voltage do you want to warn at?"),
            model->speechBatteryWarningVoltage(), 0.0, 1000.0, 2,
            &accepted);
        if (!guardedView || !model || !accepted) return;
        const double percent = QInputDialog::getDouble(
            guardedView, QObject::tr("Battery Level"),
            QObject::tr("What percentage do you want to warn at?"),
            model->speechBatteryWarningPercent(), 0.0, 100.0, 1,
            &accepted);
        if (!guardedView || !model || !accepted) return;

        // Commit only after the complete configuration was accepted. This
        // avoids an enabled battery policy with a half-confirmed prompt set.
        if (!model->setSpeechBatteryTemplate(speechTemplate)
            || !model->setSpeechBatteryWarningVoltage(voltage)
            || !model->setSpeechBatteryWarningPercent(percent)) {
            return;
        }
        model->setSpeechBatteryEnabled(true);
    });
    QObject::connect(
        view, &ConfigPlannerView::speechAltWarningConfigurationRequested,
        view, [guardedView, promptTemplate]() {
        if (!guardedView || !guardedView->viewModel()) return;
        const QPointer<ConfigPlannerViewModel> model = guardedView->viewModel();
        QString speechTemplate;
        if (!promptTemplate(QObject::tr("Altitude Warning"),
                            model->speechAltWarningTemplate(),
                            &speechTemplate)
            || !guardedView || !model) {
            return;
        }

        const double currentHeight = model->speechAltWarningHeightConfigured()
            ? model->altitudeFromMeters(
                  model->speechAltWarningHeightMeters())
            : 2.0;
        bool accepted = false;
        const double displayHeight = QInputDialog::getDouble(
            guardedView, QObject::tr("Altitude Warning"),
            QObject::tr("What altitude do you want to warn at (%1)?")
                .arg(model->altitudeUnitLabel()),
            currentHeight, 0.0, 1000000000.0, 2, &accepted);
        if (!guardedView || !model || !accepted) return;
        const double heightMeters = model->altitudeToMeters(displayHeight);

        // The policy flag is always the final write. Cancelling either prompt
        // leaves the prior configuration and enable state untouched.
        if (!model->setSpeechAltWarningTemplate(speechTemplate)
            || !model->setSpeechAltWarningHeightMeters(heightMeters)) {
            return;
        }
        model->setSpeechAltWarningEnabled(true);
    });
    QObject::connect(
        view, &ConfigPlannerView::speechArmConfigurationRequested,
        view, [guardedView, promptTemplate]() {
        if (!guardedView || !guardedView->viewModel()) return;
        const QPointer<ConfigPlannerViewModel> model = guardedView->viewModel();
        QString armTemplate;
        if (!promptTemplate(QObject::tr("Arm"), model->speechArmTemplate(),
                            &armTemplate)
            || !guardedView || !model) {
            return;
        }
        QString disarmTemplate;
        if (!promptTemplate(QObject::tr("Disarmed"),
                            model->speechDisarmTemplate(),
                            &disarmTemplate)
            || !guardedView || !model) {
            return;
        }
        if (!model->setSpeechArmTemplate(armTemplate)
            || !model->setSpeechDisarmTemplate(disarmTemplate)) {
            return;
        }
        model->setSpeechArmDisarmEnabled(true);
    });
    QObject::connect(
        view, &ConfigPlannerView::speechLowSpeedConfigurationRequested,
        view, [guardedView, promptTemplate]() {
        if (!guardedView || !guardedView->viewModel()) return;
        const QPointer<ConfigPlannerViewModel> model = guardedView->viewModel();
        QString groundTemplate;
        if (!promptTemplate(QObject::tr("Ground Speed"),
                            model->speechLowGroundSpeedTemplate(),
                            &groundTemplate)
            || !guardedView || !model) {
            return;
        }

        bool accepted = false;
        const double groundTrigger = QInputDialog::getDouble(
            guardedView, QObject::tr("Speed trigger"),
            QObject::tr("What speed do you want to warn at (m/s)?"),
            model->speechLowGroundSpeedTriggerMps(),
            0.0, 1000000000.0, 2, &accepted);
        if (!guardedView || !model || !accepted) return;

        QString airTemplate;
        if (!promptTemplate(QObject::tr("Air Speed"),
                            model->speechLowAirSpeedTemplate(),
                            &airTemplate)
            || !guardedView || !model) {
            return;
        }
        const double airTrigger = QInputDialog::getDouble(
            guardedView, QObject::tr("Speed trigger"),
            QObject::tr("What speed do you want to warn at (m/s)?"),
            model->speechLowAirSpeedTriggerMps(),
            0.0, 1000000000.0, 2, &accepted);
        if (!guardedView || !model || !accepted) return;

        if (!model->setSpeechLowGroundSpeedTemplate(groundTemplate)
            || !model->setSpeechLowGroundSpeedTriggerMps(groundTrigger)
            || !model->setSpeechLowAirSpeedTemplate(airTemplate)
            || !model->setSpeechLowAirSpeedTriggerMps(airTrigger)) {
            return;
        }
        model->setSpeechLowSpeedEnabled(true);
    });
    QObject::connect(view, &ConfigPlannerView::heartbeatChanged,
                     mainWindow, &MainWindow::enableHeartbeat);
    QObject::connect(mainWindow, &MainWindow::heartbeatChanged,
                     view, &ConfigPlannerView::setHeartbeatEnabled);
    QObject::connect(view, &ConfigPlannerView::mavlinkLoggingChanged,
                     links, &LinkManager::enableLogging);
    QObject::connect(view, &ConfigPlannerView::autoProxyChanged,
                     mainWindow, &MainWindow::enableAutoProxyMode);
    QObject::connect(mainWindow, &MainWindow::autoProxyChanged,
                     view, &ConfigPlannerView::setAutoProxyEnabled);
    QObject::connect(view, &ConfigPlannerView::mapBackendRequested,
                     view, [guardedView, maps, refreshMaps](
                              const QString &backendId) {
        if (!guardedView) return;
        if (!maps->SetBackend(backendId)) {
            refreshMaps();
            return;
        }
        guardedView->setMapBackendStatus(QObject::tr(
            "Map renderer will change after restart."));
    });
    QObject::connect(maps, &MapWidgetFactory::AvailableBackendsChanged,
                     view, refreshMaps);
    QObject::connect(maps, &MapWidgetFactory::BackendChanged,
                     view, [refreshMaps](const QString &) { refreshMaps(); });
    QObject::connect(maps, &MapWidgetFactory::StatusMessage,
                     view, &ConfigPlannerView::setMapBackendStatus);
    QObject::connect(view,
                     &ConfigPlannerView::dataFlashLogDirectorySelected,
                     view, [guardedView](const QString &directory) {
        QGC::setLogDirectory(directory);
        QGC::saveSettings();
        if (guardedView) {
            guardedView->setLogDirectories(
                QGC::logDirectory(), QGC::MAVLinkLogDirectory());
        }
    });
    QObject::connect(view, &ConfigPlannerView::tlogDirectorySelected,
                     view, [guardedView](const QString &directory) {
        QGC::setMAVLinkLogDirectory(directory);
        QGC::saveSettings();
        if (guardedView) {
            guardedView->setLogDirectories(
                QGC::logDirectory(), QGC::MAVLinkLogDirectory());
        }
    });
    const auto openLegacyOptions =
        [guardedView, refreshRuntime, refreshMaps](bool telemetryRates) {
        if (!guardedView) return;
        QDialog dialog(guardedView);
        dialog.setWindowTitle(QObject::tr(
            "Legacy APM Planner Settings"));
        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *legacy = new QGCSettingsWidget(
            &dialog, QGCSettingsWidget::SurfaceMode::LegacyPlanner);
        if (telemetryRates) {
            legacy->selectLegacyTelemetryRates();
        }
        layout->addWidget(legacy);
        dialog.resize(1014, 839);
        dialog.exec();
        if (guardedView) {
            guardedView->reloadSettings();
            refreshRuntime();
            refreshMaps();
        }
    };
    QObject::connect(view, &ConfigPlannerView::legacyOptionsRequested,
                     view, [openLegacyOptions]() {
        openLegacyOptions(false);
    });
    QObject::connect(view,
                     &ConfigPlannerView::legacyTelemetryOptionsRequested,
                     view, [openLegacyOptions]() {
        openLegacyOptions(true);
    });

    refreshRuntime();
    refreshMaps();
    refreshSpeech();
}
