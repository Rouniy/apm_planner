#include <QtTest>

#include "logging.h"
#include "ui/MavlinkLogWindow.h"

#include <QDialog>
#include <QDir>
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryFile>
#include <QThread>

#include <atomic>

Q_LOGGING_CATEGORY(apmGeneral, "apm.general.test")

class MavlinkLogWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void initialStateMatchesMp10();
    void selectingAFileEnablesOnlyImplementedExports();
    void confirmationDefaultsCanRejectWithoutStartingWork();
    void outputCannotReplaceTheSelectedTlog();
    void matlabUsesFullInputFilenameAndNeverReplacesOutput();
    void callbacksCannotRetargetOrReenterAnAdmittedExport();
    void matlabProgressCanBeCancelledWithoutResurrection();
    void closeFromConfirmationCallbackStartsNoWorker();
    void exportRunsOffTheUiThreadAndReportsCompletion();
    void closeCancelsAndJoinsBackgroundExport();
    void windowsAreIndependentModelessTopLevels();
};

namespace
{
template<typename T>
T *visibleDialog(QWidget *window, const QString &name)
{
    const auto dialogs = window->findChildren<T *>(name);
    for (T *dialog : dialogs) {
        if (dialog->isVisible())
            return dialog;
    }
    return nullptr;
}

const QStringList kImplementedButtons = {
    QStringLiteral("convertKmlButton"),
    QStringLiteral("convertGpxButton"),
    QStringLiteral("convertMatlabButton"),
    QStringLiteral("convertCsvButton"),
    QStringLiteral("convertTextButton"),
    QStringLiteral("extractParametersButton"),
    QStringLiteral("extractMissionsButton")
};
}

void MavlinkLogWindowTest::initialStateMatchesMp10()
{
    MavlinkLogWindow window;
    QCOMPARE(window.objectName(), QStringLiteral("MavlinkLogWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("Tlog Convert"));
    QCOMPARE(window.size(), QSize(460, 340));
    QCOMPARE(window.minimumSize(), QSize(460, 340));
    QCOMPARE(window.maximumSize(), QSize(460, 340));
    QCOMPARE(window.statusText(), QStringLiteral("Pick a .tlog to convert."));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("tlogName"))->text(),
             QStringLiteral("(no file selected)"));
    QCOMPARE(window.findChild<QPushButton *>(
                 QStringLiteral("pickTlogButton"))->text(),
             QStringLiteral("Pick .tlog…"));

    for (const QString &name : kImplementedButtons) {
        QPushButton *button = window.findChild<QPushButton *>(name);
        QVERIFY2(button, qPrintable(name));
        QVERIFY(!button->isEnabled());
    }
    QPushButton *matlab = window.findChild<QPushButton *>(
        QStringLiteral("convertMatlabButton"));
    QVERIFY(matlab);
    QCOMPARE(matlab->text(), QStringLiteral("Matlab"));
    QVERIFY(!matlab->isEnabled());
    QVERIFY(window.findChild<QProgressBar *>(
        QStringLiteral("MavlinkLogExportProgressBar")));
    QVERIFY(window.findChild<QPushButton *>(
        QStringLiteral("MavlinkLogExportCancelButton")));
}

void MavlinkLogWindowTest::selectingAFileEnablesOnlyImplementedExports()
{
    QTemporaryFile tlog(QDir::tempPath()
                        + QStringLiteral("/apm-tlog-window-XXXXXX.tlog"));
    QVERIFY(tlog.open());
    QVERIFY(tlog.write("fixture") > 0);
    tlog.flush();

    MavlinkLogWindow window;
    window.setTlogPath(tlog.fileName());
    QCOMPARE(window.tlogPath(), QFileInfo(tlog.fileName()).absoluteFilePath());
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("tlogName"))->text(),
             QFileInfo(tlog.fileName()).fileName());
    QCOMPARE(window.statusText(), QStringLiteral("Ready. Choose a conversion."));
    for (const QString &name : kImplementedButtons) {
        QVERIFY2(window.findChild<QPushButton *>(name)->isEnabled(),
                 qPrintable(name));
    }
    QVERIFY(window.findChild<QPushButton *>(
                QStringLiteral("convertMatlabButton"))->isEnabled());

    window.setTlogPath(tlog.fileName() + QStringLiteral(".missing"));
    QVERIFY(window.tlogPath().isEmpty());
    QCOMPARE(window.statusText(),
             QStringLiteral("Select an existing .tlog file."));
    for (const QString &name : kImplementedButtons) {
        QVERIFY(!window.findChild<QPushButton *>(name)->isEnabled());
    }
}

void MavlinkLogWindowTest::matlabUsesFullInputFilenameAndNeverReplacesOutput()
{
    QTemporaryFile tlog(QDir::tempPath()
                        + QStringLiteral("/apm-tlog-matlab-XXXXXX.tlog"));
    QVERIFY(tlog.open());
    QVERIFY(tlog.write("matlab fixture") > 0);
    tlog.flush();
    const QString expectedOutput = QFileInfo(tlog.fileName()).absoluteFilePath()
        + QStringLiteral(".mat");
    QFile::remove(expectedOutput);

    std::atomic_bool exporterCalled(false);
    std::atomic_int formatSeen(-1);
    QString inputSeen;
    QString outputSeen;
    MavlinkLogWindow::Dependencies dependencies;
    dependencies.exportLog = [&](TlogExportFormat format, const QString &input,
                                 const QString &output,
                                 const TlogExportService::CancelRequested &) {
        exporterCalled.store(true);
        formatSeen.store(static_cast<int>(format));
        inputSeen = input;
        outputSeen = output;
        TlogExportResult result;
        result.success = true;
        result.outputPaths = QStringList{output};
        result.message = QStringLiteral("Wrote Matlab MAT file to ") + output;
        return result;
    };

    MavlinkLogWindow window(dependencies);
    window.show();
    window.setTlogPath(tlog.fileName());
    auto *matlab = window.findChild<QPushButton *>(
        QStringLiteral("convertMatlabButton"));
    matlab->click();
    auto *confirmation = visibleDialog<QMessageBox>(&window,
        QStringLiteral("MavlinkLogSensitiveExportConfirmation"));
    QVERIFY(confirmation && confirmation->isVisible());
    QCOMPARE(confirmation->defaultButton(),
             qobject_cast<QPushButton *>(confirmation->button(QMessageBox::Cancel)));
    qobject_cast<QPushButton *>(confirmation->button(QMessageBox::Cancel))->click();
    QTRY_VERIFY(!window.isBusy());
    QVERIFY(!exporterCalled.load());

    matlab->click();
    confirmation = visibleDialog<QMessageBox>(&window,
        QStringLiteral("MavlinkLogSensitiveExportConfirmation"));
    QVERIFY(confirmation && confirmation->isVisible());
    confirmation->findChild<QPushButton *>(
        QStringLiteral("MavlinkLogExportConfirmButton"))->click();
    auto *outputDialog = visibleDialog<QFileDialog>(&window,
        QStringLiteral("MavlinkLogOutputDialog"));
    QVERIFY(outputDialog && outputDialog->isVisible());
    QCOMPARE(QDir::cleanPath(outputDialog->selectedFiles().value(0)),
             QDir::cleanPath(expectedOutput));
    outputDialog->selectFile(expectedOutput);
    QVERIFY(QMetaObject::invokeMethod(outputDialog, "accept", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 3000);
    QVERIFY(exporterCalled.load());
    QCOMPARE(formatSeen.load(), static_cast<int>(TlogExportFormat::Matlab));
    QCOMPARE(inputSeen, QFileInfo(tlog.fileName()).absoluteFilePath());
    QCOMPARE(outputSeen, expectedOutput);

    exporterCalled.store(false);
    QFile existing(expectedOutput);
    QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(existing.write("keep"), qint64(4));
    existing.close();
    dependencies.confirmExport = [](const QString &) { return true; };
    QString suggestedSeen;
    QString labelSeen;
    QString extensionSeen;
    dependencies.chooseOutput = [&, expectedOutput](const QString &suggested,
                                                     const QString &label,
                                                     const QString &extension) {
        suggestedSeen = suggested;
        labelSeen = label;
        extensionSeen = extension;
        return expectedOutput;
    };
    MavlinkLogWindow refusal(dependencies);
    refusal.setTlogPath(tlog.fileName());
    refusal.findChild<QPushButton *>(QStringLiteral("convertMatlabButton"))->click();
    QVERIFY(!refusal.isBusy());
    QVERIFY(!exporterCalled.load());
    QCOMPARE(suggestedSeen, expectedOutput);
    QCOMPARE(labelSeen, QStringLiteral("Matlab"));
    QCOMPARE(extensionSeen, QStringLiteral("mat"));
    QVERIFY(refusal.statusText().contains(QStringLiteral("already exists")));
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArray("keep"));
    existing.close();
    QFile::remove(expectedOutput);
}

void MavlinkLogWindowTest::callbacksCannotRetargetOrReenterAnAdmittedExport()
{
    QTemporaryFile original(QDir::tempPath()
        + QStringLiteral("/apm-tlog-frozen-XXXXXX.tlog"));
    QTemporaryFile replacement(QDir::tempPath()
        + QStringLiteral("/apm-tlog-replacement-XXXXXX.tlog"));
    QVERIFY(original.open());
    QVERIFY(replacement.open());
    original.write("original");
    replacement.write("replacement");
    original.flush();
    replacement.flush();
    const QString originalPath = QFileInfo(original.fileName()).absoluteFilePath();
    const QString replacementPath = QFileInfo(replacement.fileName()).absoluteFilePath();
    MavlinkLogWindow *window = nullptr;
    int confirmationCalls = 0;
    int outputCalls = 0;
    bool busySeen = false;
    QString suggestedSeen;
    std::atomic_int formatSeen(-1);
    QString inputSeen;
    MavlinkLogWindow::Dependencies dependencies;
    dependencies.confirmExport = [&](const QString &) {
        ++confirmationCalls;
        busySeen = window->isBusy();
        window->setTlogPath(replacementPath);
        window->findChild<QPushButton *>(QStringLiteral("convertCsvButton"))->click();
        return true;
    };
    dependencies.chooseOutput = [&](const QString &suggested,
        const QString &, const QString &) {
        ++outputCalls;
        suggestedSeen = suggested;
        window->setTlogPath(replacementPath);
        return originalPath + QStringLiteral(".new.mat");
    };
    dependencies.exportLog = [&](TlogExportFormat format, const QString &input,
                                  const QString &,
                                  const TlogExportService::CancelRequested &) {
        formatSeen.store(static_cast<int>(format));
        inputSeen = input;
        TlogExportResult result;
        result.success = true;
        result.message = QStringLiteral("done");
        return result;
    };
    MavlinkLogWindow concrete(dependencies);
    window = &concrete;
    window->setTlogPath(originalPath);
    window->findChild<QPushButton *>(QStringLiteral("convertMatlabButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window->isBusy(), 3000);
    QCOMPARE(confirmationCalls, 1);
    QCOMPARE(outputCalls, 1);
    QVERIFY(busySeen);
    QCOMPARE(suggestedSeen, originalPath + QStringLiteral(".mat"));
    QCOMPARE(formatSeen.load(), static_cast<int>(TlogExportFormat::Matlab));
    QCOMPARE(inputSeen, originalPath);
    QCOMPARE(window->tlogPath(), originalPath);
}

void MavlinkLogWindowTest::matlabProgressCanBeCancelledWithoutResurrection()
{
    QTemporaryFile tlog(QDir::tempPath()
                        + QStringLiteral("/apm-tlog-progress-XXXXXX.tlog"));
    QVERIFY(tlog.open());
    const QString outputPath = tlog.fileName() + QStringLiteral(".progress.mat");
    QFile::remove(outputPath);
    std::atomic_bool entered(false);
    std::atomic_bool cancelled(false);
    std::atomic_int formatSeen(-1);
    MavlinkLogWindow::Dependencies dependencies;
    dependencies.confirmExport = [](const QString &) { return true; };
    dependencies.chooseOutput = [outputPath](const QString &, const QString &,
                                             const QString &) {
        return outputPath;
    };
    dependencies.exportLogWithProgress = [&](TlogExportFormat format,
        const QString &, const QString &,
        const TlogExportService::CancelRequested &cancel,
        const TlogExportService::Progress &progress) {
        formatSeen.store(static_cast<int>(format));
        entered.store(true);
        progress(1, 10);
        QElapsedTimer timer;
        timer.start();
        while (!cancel() && timer.elapsed() < 5000)
            QThread::msleep(1);
        cancelled.store(cancel());
        progress(9, 10);
        TlogExportResult result;
        result.cancelled = true;
        return result;
    };
    MavlinkLogWindow window(dependencies);
    window.show();
    window.setTlogPath(tlog.fileName());
    window.findChild<QPushButton *>(QStringLiteral("convertMatlabButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 3000);
    auto *progress = window.findChild<QProgressBar *>(
        QStringLiteral("MavlinkLogExportProgressBar"));
    auto *cancel = window.findChild<QPushButton *>(
        QStringLiteral("MavlinkLogExportCancelButton"));
    QVERIFY(progress->isVisible());
    QVERIFY(cancel->isVisible());
    cancel->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 6000);
    QVERIFY(cancelled.load());
    QCOMPARE(formatSeen.load(), static_cast<int>(TlogExportFormat::Matlab));
    QVERIFY(!progress->isVisible());
    QVERIFY(!cancel->isVisible());
    QCOMPARE(window.statusText(), QStringLiteral("Matlab export cancelled."));
    QTest::qWait(150);
    QVERIFY(!progress->isVisible());
    QCOMPARE(window.statusText(), QStringLiteral("Matlab export cancelled."));
}

void MavlinkLogWindowTest::closeFromConfirmationCallbackStartsNoWorker()
{
    QTemporaryFile tlog(QDir::tempPath()
                        + QStringLiteral("/apm-tlog-callback-close-XXXXXX.tlog"));
    QVERIFY(tlog.open());
    QPointer<MavlinkLogWindow> window;
    bool outputCalled = false;
    bool exporterCalled = false;
    MavlinkLogWindow::Dependencies dependencies;
    dependencies.confirmExport = [&](const QString &) {
        window->close();
        return true;
    };
    dependencies.chooseOutput = [&](const QString &, const QString &,
                                     const QString &) {
        outputCalled = true;
        return QStringLiteral("/tmp/unused.mat");
    };
    dependencies.exportLog = [&](TlogExportFormat, const QString &,
                                  const QString &,
                                  const TlogExportService::CancelRequested &) {
        exporterCalled = true;
        return TlogExportResult();
    };
    window = new MavlinkLogWindow(dependencies);
    window->setTlogPath(tlog.fileName());
    window->show();
    window->findChild<QPushButton *>(QStringLiteral("convertMatlabButton"))->click();
    QTRY_VERIFY(window.isNull());
    QVERIFY(!outputCalled);
    QVERIFY(!exporterCalled);
}

void MavlinkLogWindowTest::confirmationDefaultsCanRejectWithoutStartingWork()
{
    QTemporaryFile tlog(QDir::tempPath()
                        + QStringLiteral("/apm-tlog-reject-XXXXXX.tlog"));
    QVERIFY(tlog.open());
    bool outputPickerCalled = false;
    bool exporterCalled = false;
    MavlinkLogWindow::Dependencies dependencies;
    dependencies.confirmExport = [](const QString &label) {
        return label != QStringLiteral("KML");
    };
    dependencies.chooseOutput = [&outputPickerCalled](
        const QString &, const QString &, const QString &) {
        outputPickerCalled = true;
        return QStringLiteral("unused.kml");
    };
    dependencies.exportLog = [&exporterCalled](
        TlogExportFormat, const QString &, const QString &,
        const TlogExportService::CancelRequested &) {
        exporterCalled = true;
        return TlogExportResult();
    };

    MavlinkLogWindow window(dependencies);
    window.setTlogPath(tlog.fileName());
    window.findChild<QPushButton *>(QStringLiteral("convertKmlButton"))->click();
    QCOMPARE(window.statusText(), QStringLiteral("KML export cancelled."));
    QVERIFY(!window.isBusy());
    QVERIFY(!outputPickerCalled);
    QVERIFY(!exporterCalled);
}

void MavlinkLogWindowTest::outputCannotReplaceTheSelectedTlog()
{
    QTemporaryFile tlog(QDir::tempPath()
                        + QStringLiteral("/apm-tlog-same-output-XXXXXX.tlog"));
    QVERIFY(tlog.open());
    QVERIFY(tlog.write("original telemetry") > 0);
    tlog.flush();
    bool exporterCalled = false;
    MavlinkLogWindow::Dependencies dependencies;
    dependencies.confirmExport = [](const QString &) { return true; };
    dependencies.chooseOutput = [&tlog](const QString &, const QString &,
                                        const QString &) {
        return tlog.fileName();
    };
    dependencies.exportLog = [&exporterCalled](
        TlogExportFormat, const QString &, const QString &,
        const TlogExportService::CancelRequested &) {
        exporterCalled = true;
        return TlogExportResult();
    };

    MavlinkLogWindow window(dependencies);
    window.setTlogPath(tlog.fileName());
    window.findChild<QPushButton *>(QStringLiteral("convertCsvButton"))->click();
    QVERIFY(!window.isBusy());
    QVERIFY(!exporterCalled);
    QVERIFY(window.statusText().contains(QStringLiteral("must not replace")));
    QVERIFY(tlog.seek(0));
    QCOMPARE(tlog.readAll(), QByteArray("original telemetry"));
}

void MavlinkLogWindowTest::exportRunsOffTheUiThreadAndReportsCompletion()
{
    QTemporaryFile tlog(QDir::tempPath()
                        + QStringLiteral("/apm-tlog-success-XXXXXX.tlog"));
    QVERIFY(tlog.open());
    std::atomic_bool ranOffUi(false);
    std::atomic_int formatSeen(-1);
    MavlinkLogWindow::Dependencies dependencies;
    dependencies.confirmExport = [](const QString &) { return true; };
    dependencies.chooseOutput = [](const QString &, const QString &,
                                   const QString &) {
        return QStringLiteral("/tmp/exported.csv");
    };
    dependencies.exportLog = [&ranOffUi, &formatSeen](
        TlogExportFormat format, const QString &, const QString &output,
        const TlogExportService::CancelRequested &) {
        ranOffUi.store(QThread::currentThread()
                       != QCoreApplication::instance()->thread());
        formatSeen.store(static_cast<int>(format));
        TlogExportResult result;
        result.success = true;
        result.itemCount = 3;
        result.outputPaths << output;
        result.message = QStringLiteral(
            "Wrote 3 decoded packets to /tmp/exported.csv");
        return result;
    };

    MavlinkLogWindow window(dependencies);
    window.setTlogPath(tlog.fileName());
    window.findChild<QPushButton *>(QStringLiteral("convertCsvButton"))->click();
    QVERIFY(window.isBusy());
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 3000);
    QVERIFY(ranOffUi.load());
    QCOMPARE(formatSeen.load(), static_cast<int>(TlogExportFormat::Csv));
    QCOMPARE(window.statusText(),
             QStringLiteral("Wrote 3 decoded packets to /tmp/exported.csv"));
}

void MavlinkLogWindowTest::closeCancelsAndJoinsBackgroundExport()
{
    QTemporaryFile tlog(QDir::tempPath()
                        + QStringLiteral("/apm-tlog-close-XXXXXX.tlog"));
    QVERIFY(tlog.open());
    std::atomic_bool entered(false);
    std::atomic_bool cancellationObserved(false);
    MavlinkLogWindow::Dependencies dependencies;
    dependencies.confirmExport = [](const QString &) { return true; };
    dependencies.chooseOutput = [](const QString &, const QString &,
                                   const QString &) {
        return QStringLiteral("/tmp/exported.gpx");
    };
    dependencies.exportLog = [&entered, &cancellationObserved](
        TlogExportFormat, const QString &, const QString &,
        const TlogExportService::CancelRequested &cancel) {
        entered.store(true);
        while (!cancel()) {
            QThread::msleep(1);
        }
        cancellationObserved.store(true);
        TlogExportResult result;
        result.cancelled = true;
        return result;
    };

    auto *window = new MavlinkLogWindow(dependencies);
    window->setTlogPath(tlog.fileName());
    window->show();
    window->findChild<QPushButton *>(QStringLiteral("convertGpxButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 3000);
    QPointer<MavlinkLogWindow> guarded(window);
    window->close();
    QTRY_VERIFY_WITH_TIMEOUT(guarded.isNull(), 3000);
    QVERIFY(cancellationObserved.load());
}

void MavlinkLogWindowTest::windowsAreIndependentModelessTopLevels()
{
    QWidget owner;
    owner.resize(900, 700);
    MavlinkLogWindow *first = MavlinkLogWindow::OpenWindow(&owner);
    MavlinkLogWindow *second = MavlinkLogWindow::OpenWindow(&owner);
    QPointer<MavlinkLogWindow> guardedFirst(first);

    QVERIFY(first != second);
    for (MavlinkLogWindow *window : {first, second}) {
        QCOMPARE(window->windowType(), Qt::Window);
        QVERIFY(window->isWindow());
        QCOMPARE(window->windowModality(), Qt::NonModal);
        QVERIFY(!window->isModal());
        QVERIFY(qobject_cast<QDialog *>(window) == nullptr);
        QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
        QCOMPARE(window->parentWidget(), &owner);
        QVERIFY(window->isVisible());
    }

    first->close();
    QTRY_VERIFY(guardedFirst.isNull());
    QVERIFY(second->isVisible());
    delete second;
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QApplication app(argc, argv);
    MavlinkLogWindowTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_mavlinklogwindow.moc"
