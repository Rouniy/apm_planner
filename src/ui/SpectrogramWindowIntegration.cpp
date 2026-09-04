#include "SpectrogramWindow.h"

#include "Loghandling/DataFlashSpectrogramService.h"
#include "configuration.h"

#include <QApplication>
#include <QFileDialog>

namespace
{
SpectrogramWindow::Dependencies productionDependencies()
{
    SpectrogramWindow::Dependencies dependencies;
    dependencies.chooseLog = [](QWidget *parent) {
        return QFileDialog::getOpenFileName(
            parent, SpectrogramWindow::tr("Open DataFlash log"),
            QGC::logDirectory(),
            SpectrogramWindow::tr(
                "DataFlash log (*.bin *.BIN *.log *.LOG);;All files (*)"));
    };
    dependencies.generate = [](
        const QString &path, const QString &sensor,
        int minimumDb, int maximumDb,
        const SpectrogramWindow::CancelRequested &cancel) {
        const DataFlashSpectrogramAnalyzer::AnalysisResult analyzed =
            DataFlashSpectrogramService::Generate(
                path, sensor, minimumDb, maximumDb, cancel);
        SpectrogramWindow::RenderResult result;
        result.success = analyzed.ok();
        result.cancelled =
            analyzed.error
            == DataFlashSpectrogramAnalyzer::ErrorCode::Cancelled;
        if (result.success) {
            result.warning = analyzed.message;
        } else {
            result.error = analyzed.message;
        }
        result.startSeconds = analyzed.startSeconds;
        result.endSeconds = analyzed.endSeconds;
        result.maximumFrequency = analyzed.maximumFrequencyHz;
        result.sampleCount = analyzed.inputSampleCount;
        result.windowCount = analyzed.renderedWindowCount;
        for (int axis = 0; axis < 3; ++axis) {
            result.images[axis] = analyzed.axes[axis].image;
        }
        return result;
    };
    return dependencies;
}
}

SpectrogramWindow::SpectrogramWindow(QWidget *owner)
    : SpectrogramWindow(productionDependencies(), owner)
{
}

SpectrogramWindow *SpectrogramWindow::OpenWindow(QWidget *owner)
{
    QWidget *resolvedOwner = owner ? owner : QApplication::activeWindow();
    auto *window = new SpectrogramWindow(resolvedOwner);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}
