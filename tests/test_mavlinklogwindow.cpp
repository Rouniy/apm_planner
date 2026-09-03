#include <QtTest>

#include "logging.h"
#include "ui/MavlinkLogWindow.h"

#include <QDialog>
#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QLabel>
#include <QPointer>
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
    void exportRunsOffTheUiThreadAndReportsCompletion();
    void closeCancelsAndJoinsBackgroundExport();
    void windowsAreIndependentModelessTopLevels();
};

namespace
{
const QStringList kImplementedButtons = {
    QStringLiteral("convertKmlButton"),
    QStringLiteral("convertGpxButton"),
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
    QVERIFY(!matlab->property("unavailableReason").toString().isEmpty());
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
    QVERIFY(!window.findChild<QPushButton *>(
                 QStringLiteral("convertMatlabButton"))->isEnabled());

    window.setTlogPath(tlog.fileName() + QStringLiteral(".missing"));
    QVERIFY(window.tlogPath().isEmpty());
    QCOMPARE(window.statusText(),
             QStringLiteral("Select an existing .tlog file."));
    for (const QString &name : kImplementedButtons) {
        QVERIFY(!window.findChild<QPushButton *>(name)->isEnabled());
    }
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

QTEST_MAIN(MavlinkLogWindowTest)
#include "test_mavlinklogwindow.moc"
