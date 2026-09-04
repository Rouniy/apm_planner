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
            refreshSpeech();
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
    QObject::connect(view, &ConfigPlannerView::heartbeatChanged,
                     mainWindow, &MainWindow::enableHeartbeat);
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
