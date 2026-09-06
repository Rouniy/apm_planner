#include "ui/flightdata/DataFlashLogToolsController.h"
#include "ui/flightdata/DataFlashLogsWidget.h"

#include <QtTest>

#include <QApplication>
#include <QAbstractButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeWidget>

#include <atomic>
#include <memory>

namespace
{
template<typename T>
T *visible(QWidget *owner, const char *name)
{
    if (!owner)
        return nullptr;
    for (T *item : owner->findChildren<T *>(
             QString::fromLatin1(name), Qt::FindChildrenRecursively)) {
        if (item->isVisible())
            return item;
    }
    return nullptr;
}

template<typename T>
T *waitVisible(QWidget *owner, const char *name, int timeout = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (T *item = visible<T>(owner, name))
            return item;
        QTest::qWait(2);
    }
    return nullptr;
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        && file.write(contents) == contents.size() && file.flush();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool choose(QFileDialog *dialog, const QString &path)
{
    if (!dialog)
        return false;
    if (QLineEdit *edit = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
        edit->setText(path);
    else
        dialog->selectFile(path);
    const QStringList selected = dialog->selectedFiles();
    if (selected.size() != 1
        || QFileInfo(selected.first()).absoluteFilePath()
            != QFileInfo(path).absoluteFilePath()) {
        return false;
    }
    return QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
}

QPushButton *defaultButton(QDialog *dialog)
{
    if (!dialog)
        return nullptr;
    for (QPushButton *button : dialog->findChildren<QPushButton *>()) {
        if (button->isDefault())
            return button;
    }
    return nullptr;
}

bool clickAccept(QDialog *dialog)
{
    if (!dialog)
        return false;
    if (QDialogButtonBox *box = dialog->findChild<QDialogButtonBox *>()) {
        for (QAbstractButton *button : box->buttons()) {
            const auto role = box->buttonRole(button);
            if (role == QDialogButtonBox::YesRole
                || role == QDialogButtonBox::AcceptRole) {
                button->click();
                return true;
            }
        }
    }
    return false;
}

bool logsContain(const QSignalSpy &spy, const QString &needle)
{
    for (const QList<QVariant> &arguments : spy) {
        if (!arguments.isEmpty()
            && arguments.first().toString().contains(needle, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

struct FakeState {
    std::atomic_int convertCalls{0};
    std::atomic_int kmlCalls{0};
    std::atomic_int gpxCalls{0};
    std::atomic_int analyzeCalls{0};
    std::atomic_int matlabPrepareCalls{0};
    std::atomic_int matlabExportCalls{0};
    bool failGpx = false;
    QString kmlInput;
    QByteArray frozenBytes;
};

DataFlashLogToolsController::Operations fakeOperations(
    const std::shared_ptr<FakeState> &state)
{
    DataFlashLogToolsController::Operations operations;
    operations.convertBinToLog = [state](
        const QString &, const QString &output,
        const DataFlashBinToLogConverter::CancelCheck &cancel,
        const DataFlashBinToLogConverter::Progress &progress) {
        DataFlashBinToLogConverter::Result result;
        ++state->convertCalls;
        if (cancel && cancel()) { result.cancelled = true; return result; }
        if (progress) progress(1, 1);
        if (!writeFile(output, QByteArrayLiteral("FMT,1\nGPS,2\n"))) {
            result.error = QStringLiteral("write failed");
            return result;
        }
        result.success = true;
        result.outputPath = output;
        result.recordsWritten = 2;
        return result;
    };
    operations.exportKml = [state](
        const QString &input, const QString &output,
        const DataFlashKmlExporter::CancellationCheck &cancel) {
        DataFlashKmlExporter::Result result;
        ++state->kmlCalls;
        state->kmlInput = input;
        state->frozenBytes = readFile(input);
        if (cancel && cancel()) { result.cancelled = true; return result; }
        if (!writeFile(output, QByteArrayLiteral("<kml/>"))) {
            result.error = QStringLiteral("KML write failed");
            return result;
        }
        result.succeeded = true;
        result.pointCount = 4;
        return result;
    };
    operations.exportGpx = [state](
        const QString &, const QString &output,
        const DataFlashGpxExporter::CancelCheck &cancel,
        const DataFlashGpxExporter::Progress &progress) {
        DataFlashGpxExporter::Result result;
        ++state->gpxCalls;
        if (cancel && cancel()) { result.cancelled = true; return result; }
        if (state->failGpx) {
            result.error = QStringLiteral("deliberate GPX failure");
            return result;
        }
        if (progress) progress(1, 1);
        if (!writeFile(output, QByteArrayLiteral("<gpx/>"))) {
            result.error = QStringLiteral("GPX write failed");
            return result;
        }
        result.success = true;
        result.outputPath = output;
        result.pointCount = 3;
        return result;
    };
    operations.analyze = [state](
        const QString &, DataFlashLogAnalyzer::Cancel cancel,
        DataFlashLogAnalyzer::Progress progress) {
        DataFlashLogAnalyzer::Result result;
        ++state->analyzeCalls;
        if (cancel && cancel()) { result.cancelled = true; return result; }
        if (progress) progress(1000, 1000);
        result.success = true;
        DataFlashLogAnalyzer::TestResult test;
        test.name = QStringLiteral("GPS");
        test.status = QStringLiteral("GOOD");
        test.message = QStringLiteral("Track is available");
        result.tests.append(test);
        result.warnings.append(QStringLiteral("Advisory only"));
        return result;
    };
    operations.prepareMatlab = [state](
        const QString &input,
        const DataFlashMatlabExporter::CancelCheck &cancel,
        const DataFlashMatlabExporter::Progress &progress) {
        ++state->matlabPrepareCalls;
        return DataFlashMatlabExporter::Prepare(input, cancel, progress);
    };
    operations.exportMatlab = [state](
        const DataFlashMatlabExporter::Plan &plan,
        const DataFlashMatlabExporter::CancelCheck &cancel,
        const DataFlashMatlabExporter::Progress &progress) {
        ++state->matlabExportCalls;
        return DataFlashMatlabExporter::Export(plan, cancel, progress);
    };
    operations.analyzeDirectory = [](
        const QString &root, const FlightLogOrganizer::Cancel &cancel,
        const FlightLogOrganizer::Progress &progress) {
        return FlightLogOrganizer::Analyze(root, cancel, progress);
    };
    operations.executeOrganizer = [](
        const FlightLogOrganizer::Plan &plan,
        const FlightLogOrganizer::Cancel &cancel,
        const FlightLogOrganizer::Progress &progress) {
        return FlightLogOrganizer::Execute(plan, cancel, progress);
    };
    return operations;
}

struct HeldAnalysis {
    std::atomic_bool entered{false};
    std::atomic_bool sawCancel{false};
    std::atomic_bool release{false};
    ~HeldAnalysis() { release.store(true, std::memory_order_release); }
};

struct ReleaseHeldAnalysis {
    std::shared_ptr<HeldAnalysis> state;
    ~ReleaseHeldAnalysis()
    {
        if (state)
            state->release.store(true, std::memory_order_release);
    }
};
} // namespace

class DataFlashLogToolsControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void widgetHasEightFaithfulActions();
    void reviewPickerFreezesAndSharesInput();
    void kmlGpxRequiresConsentAndReportsPartial();
    void binToLogIsSaveAsAndNewOnly();
    void matlabPlanIsExactDefaultCancelAndNewOnly();
    void matlabPreparationCancelStaysNonPublishing();
    void autoAnalysisIsReadableAndModeless();
    void organizerShowsEveryEntryAndRequiresConsent();
    void shutdownCancelsAndWaitsForWorker();
    void callbacksMayDeleteController();
    void disappearingDialogParentDoesNotStrandFlow();
    void matlabPreparationOwnerLossDoesNotStrandFlow();
    void reviewCompletionCannotOutrunShutdown();
};

void DataFlashLogToolsControllerTest::disappearingDialogParentDoesNotStrandFlow()
{
    QTemporaryDir directory;
    const QString input = directory.filePath("source.bin");
    QVERIFY(writeFile(input, "source"));
    DataFlashLogsWidget widget;
    QPointer<QWidget> parent = new QWidget;
    DataFlashLogToolsController controller(&widget, parent);
    controller.setSelectedLogPath(input);
    connect(&controller, &DataFlashLogToolsController::busyChanged,
            &controller, [&](bool busy) { if (busy && parent) delete parent.data(); });
    controller.startAutoAnalysis();
    QVERIFY(!parent);
    QVERIFY(!controller.busy());
    QSignalSpy ready(&controller, &DataFlashLogToolsController::shutdownReady);
    controller.shutdown();
    QCOMPARE(ready.size(), 1);
}

void DataFlashLogToolsControllerTest::matlabPreparationOwnerLossDoesNotStrandFlow()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("owner-loss.log"));
    QVERIFY(writeFile(input, QByteArrayLiteral(
        "FMT,150,19,GPS,Qff,TimeUS,Lat,Lng\n"
        "GPS,1000,47.5,8.5\n")));
    DataFlashLogsWidget widget;
    QPointer<QWidget> owner = new QWidget;
    owner->show();
    DataFlashLogToolsController controller(&widget, owner);
    const auto fake = std::make_shared<FakeState>();
    auto operations = fakeOperations(fake);
    const auto held = std::make_shared<HeldAnalysis>();
    const ReleaseHeldAnalysis releaseOnExit{held};
    operations.prepareMatlab = [held](
        const QString &path, const DataFlashMatlabExporter::CancelCheck &cancel,
        const DataFlashMatlabExporter::Progress &progress) {
        DataFlashMatlabExporter::PlanResult result =
            DataFlashMatlabExporter::Prepare(path, cancel, progress);
        held->entered.store(true, std::memory_order_release);
        QElapsedTimer deadline;
        deadline.start();
        while (!held->release.load(std::memory_order_acquire)
               && deadline.elapsed() < 10000) {
            QThread::msleep(2);
        }
        return result;
    };
    controller.setOperationsForTesting(operations);
    controller.setSelectedLogPath(input);

    controller.startMatlab();
    QTRY_VERIFY_WITH_TIMEOUT(held->entered.load(std::memory_order_acquire), 10000);
    delete owner.data();
    QVERIFY(owner.isNull());
    held->release.store(true, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
    QVERIFY(!QFileInfo::exists(input + QStringLiteral("-2.mat")));
}

void DataFlashLogToolsControllerTest::reviewCompletionCannotOutrunShutdown()
{
    QTemporaryDir directory;
    const QString input = directory.filePath("source.bin");
    QVERIFY(writeFile(input, "source"));
    QWidget owner; owner.show();
    DataFlashLogsWidget widget(&owner);
    DataFlashLogToolsController controller(&widget, &owner);
    QSignalSpy review(&controller, &DataFlashLogToolsController::reviewLogRequested);
    connect(&controller, &DataFlashLogToolsController::busyChanged,
            &controller, [&](bool busy) { if (!busy) controller.shutdown(); });
    controller.startReview();
    auto *picker = waitVisible<QFileDialog>(&owner, "DataFlashLogInputDialog");
    QVERIFY(picker);
    QVERIFY(choose(picker, input));
    QVERIFY(controller.shutdownPending());
    QVERIFY(!controller.busy());
    QCOMPARE(review.size(), 0);
}

void DataFlashLogToolsControllerTest::widgetHasEightFaithfulActions()
{
    DataFlashLogsWidget widget;
    const char *working[] = {
        "DataFlashDownloadButton", "DataFlashReviewButton",
        "DataFlashAutoAnalysisButton", "DataFlashKmlGpxButton",
        "DataFlashBinToLogButton", "DataFlashMatlabButton",
        "DataFlashOrganizeButton"};
    for (const char *name : working) {
        QPushButton *button = widget.findChild<QPushButton *>(QString::fromLatin1(name));
        QVERIFY(button);
        QVERIFY(button->isEnabled());
        QCOMPARE(button->minimumWidth(), 0);
    }
    for (const char *name : {"DataFlashGeoReferenceButton"}) {
        QPushButton *button = widget.findChild<QPushButton *>(QString::fromLatin1(name));
        QVERIFY(button);
        QVERIFY(!button->isEnabled());
        QVERIFY(button->toolTip().contains(QStringLiteral("not yet ported")));
    }
    QCOMPARE(widget.findChildren<QPushButton *>().size(), 8);
    QVERIFY(widget.findChild<QLabel *>(QStringLiteral("DataFlashLogStatus")));
}

void DataFlashLogToolsControllerTest::reviewPickerFreezesAndSharesInput()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("review.bin"));
    QVERIFY(writeFile(input, QByteArrayLiteral("source")));
    QWidget owner;
    owner.show();
    auto *widget = new DataFlashLogsWidget(&owner);
    auto *controller = new DataFlashLogToolsController(widget, &owner);
    QSignalSpy reviewSpy(controller, &DataFlashLogToolsController::reviewLogRequested);
    QSignalSpy busySpy(controller, &DataFlashLogToolsController::busyChanged);

    controller->startReview();
    QFileDialog *picker = waitVisible<QFileDialog>(&owner, "DataFlashLogInputDialog");
    QVERIFY(picker);
    QVERIFY(choose(picker, input));
    QTRY_COMPARE_WITH_TIMEOUT(reviewSpy.size(), 1, 3000);
    QCOMPARE(reviewSpy.first().first().toString(), QFileInfo(input).absoluteFilePath());
    QCOMPARE(controller->selectedLogPath(), QFileInfo(input).absoluteFilePath());
    QVERIFY(!controller->busy());
    QVERIFY(busySpy.size() >= 2);

    controller->startReview();
    picker = waitVisible<QFileDialog>(&owner, "DataFlashLogInputDialog");
    QVERIFY(picker);
    picker->reject();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 3000);
    QCOMPARE(reviewSpy.size(), 1);
}

void DataFlashLogToolsControllerTest::kmlGpxRequiresConsentAndReportsPartial()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("map.bin"));
    QVERIFY(writeFile(input, QByteArrayLiteral("immutable source")));
    QWidget owner;
    owner.show();
    auto *widget = new DataFlashLogsWidget(&owner);
    auto *controller = new DataFlashLogToolsController(widget, &owner);
    const auto state = std::make_shared<FakeState>();
    controller->setOperationsForTesting(fakeOperations(state));
    controller->setSelectedLogPath(input);
    QSignalSpy logSpy(controller, &DataFlashLogToolsController::logMessage);

    controller->startKmlGpx();
    QDialog *consent = waitVisible<QDialog>(&owner, "DataFlashKmlGpxConfirmationDialog");
    QVERIFY(consent);
    QPushButton *cancelButton = defaultButton(consent);
    QVERIFY(cancelButton);
    QVERIFY(cancelButton->text().contains(QStringLiteral("Cancel"), Qt::CaseInsensitive));
    const QString summary = consent->findChild<QLabel *>(
        QStringLiteral("DataFlashKmlGpxSummary"))->text();
    QVERIFY(summary.contains(directory.filePath(QStringLiteral("map.kml"))));
    QVERIFY(summary.contains(directory.filePath(QStringLiteral("map.gpx"))));
    consent->reject();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 3000);
    QCOMPARE(state->kmlCalls.load(), 0);

    state->failGpx = true;
    controller->startKmlGpx();
    consent = waitVisible<QDialog>(&owner, "DataFlashKmlGpxConfirmationDialog");
    QVERIFY(clickAccept(consent));
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    QCOMPARE(readFile(directory.filePath(QStringLiteral("map.kml"))), QByteArray("<kml/>"));
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("map.gpx"))));
    QVERIFY(logsContain(logSpy, QStringLiteral("Published before")));
    QVERIFY(state->kmlInput != input);
    QCOMPARE(state->frozenBytes, QByteArray("immutable source"));

    controller->startKmlGpx();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 3000);
    QVERIFY(!visible<QDialog>(&owner, "DataFlashKmlGpxConfirmationDialog"));
    QVERIFY(logsContain(logSpy, QStringLiteral("already exists")));

    const QString successfulInput = directory.filePath(QStringLiteral("map-success.bin"));
    QVERIFY(writeFile(successfulInput, QByteArrayLiteral("second immutable source")));
    state->failGpx = false;
    controller->setSelectedLogPath(successfulInput);
    controller->startKmlGpx();
    consent = waitVisible<QDialog>(&owner, "DataFlashKmlGpxConfirmationDialog");
    QVERIFY(clickAccept(consent));
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    QCOMPARE(readFile(directory.filePath(QStringLiteral("map-success.kml"))),
             QByteArray("<kml/>"));
    QCOMPARE(readFile(directory.filePath(QStringLiteral("map-success.gpx"))),
             QByteArray("<gpx/>"));
}

void DataFlashLogToolsControllerTest::binToLogIsSaveAsAndNewOnly()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.bin"));
    const QString output = directory.filePath(QStringLiteral("chosen.log"));
    QVERIFY(writeFile(input, QByteArrayLiteral("BIN")));
    QWidget owner;
    owner.show();
    auto *widget = new DataFlashLogsWidget(&owner);
    auto *controller = new DataFlashLogToolsController(widget, &owner);
    const auto state = std::make_shared<FakeState>();
    controller->setOperationsForTesting(fakeOperations(state));
    controller->setSelectedLogPath(input);

    controller->startBinToLog();
    QFileDialog *picker = waitVisible<QFileDialog>(&owner, "DataFlashBinToLogOutputDialog");
    QVERIFY(picker);
    QCOMPARE(picker->acceptMode(), QFileDialog::AcceptSave);
    QVERIFY(choose(picker, output));
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    QCOMPARE(state->convertCalls.load(), 1);
    QVERIFY(readFile(output).contains("GPS"));

    controller->startBinToLog();
    picker = waitVisible<QFileDialog>(&owner, "DataFlashBinToLogOutputDialog");
    QVERIFY(choose(picker, output));
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 3000);
    QCOMPARE(state->convertCalls.load(), 1);
    QVERIFY(readFile(output).contains("GPS"));
}

void DataFlashLogToolsControllerTest::matlabPlanIsExactDefaultCancelAndNewOnly()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("matlab.log"));
    QVERIFY(writeFile(input, QByteArrayLiteral(
        "FMT,150,19,GPS,Qff,TimeUS,Lat,Lng\n"
        "GPS,1000,47.5,8.5\n")));
    QWidget owner;
    owner.show();
    auto *widget = new DataFlashLogsWidget(&owner);
    auto *controller = new DataFlashLogToolsController(widget, &owner);
    const auto state = std::make_shared<FakeState>();
    controller->setOperationsForTesting(fakeOperations(state));
    controller->setSelectedLogPath(input);
    QSignalSpy logSpy(controller, &DataFlashLogToolsController::logMessage);

    controller->startMatlab();
    QDialog *consent = waitVisible<QDialog>(
        &owner, "DataFlashMatlabConfirmationDialog", 10000);
    QVERIFY(consent);
    QCOMPARE(state->matlabPrepareCalls.load(), 1);
    QPushButton *cancelButton = defaultButton(consent);
    QVERIFY(cancelButton);
    QVERIFY(cancelButton->text().contains(QStringLiteral("Cancel"), Qt::CaseInsensitive));
    QLabel *summary = consent->findChild<QLabel *>(
        QStringLiteral("DataFlashMatlabSummary"));
    QVERIFY(summary);
    QVERIFY(summary->text().contains(input));
    const QRegularExpression expression(
        QStringLiteral("Exact new output: ([^\\n]+)"));
    const QRegularExpressionMatch match = expression.match(summary->text());
    QVERIFY(match.hasMatch());
    const QString output = match.captured(1).trimmed();
    QCOMPARE(output, input + QStringLiteral("-2.mat"));
    consent->reject();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 3000);
    QVERIFY(!QFileInfo::exists(output));
    QCOMPARE(state->matlabExportCalls.load(), 0);

    controller->startMatlab();
    consent = waitVisible<QDialog>(
        &owner, "DataFlashMatlabConfirmationDialog", 10000);
    QVERIFY(clickAccept(consent));
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 10000);
    QCOMPARE(state->matlabPrepareCalls.load(), 2);
    QCOMPARE(state->matlabExportCalls.load(), 1);
    QVERIFY(QFileInfo::exists(output));
    QVERIFY(readFile(output).startsWith("MATLAB 5.0 MAT-file"));
    const QByteArray originalOutput = readFile(output);

    controller->startMatlab();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 10000);
    QVERIFY(!visible<QDialog>(&owner, "DataFlashMatlabConfirmationDialog"));
    QCOMPARE(state->matlabExportCalls.load(), 1);
    QCOMPARE(readFile(output), originalOutput);
    QVERIFY(logsContain(logSpy, QStringLiteral("exists"))
            || logsContain(logSpy, QStringLiteral("already")));
}

void DataFlashLogToolsControllerTest::matlabPreparationCancelStaysNonPublishing()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("cancel.log"));
    QVERIFY(writeFile(input, QByteArrayLiteral(
        "FMT,150,19,GPS,Qff,TimeUS,Lat,Lng\n"
        "GPS,1000,47.5,8.5\n")));
    QWidget owner;
    owner.show();
    auto *widget = new DataFlashLogsWidget(&owner);
    auto *controller = new DataFlashLogToolsController(widget, &owner);
    const auto state = std::make_shared<FakeState>();
    auto operations = fakeOperations(state);
    const auto held = std::make_shared<HeldAnalysis>();
    const ReleaseHeldAnalysis releaseOnExit{held};
    operations.prepareMatlab = [held](
        const QString &, const DataFlashMatlabExporter::CancelCheck &cancel,
        const DataFlashMatlabExporter::Progress &progress) {
        DataFlashMatlabExporter::PlanResult result;
        held->entered.store(true, std::memory_order_release);
        if (progress)
            progress(1, 2);
        QElapsedTimer deadline;
        deadline.start();
        while (!held->release.load(std::memory_order_acquire)
               && deadline.elapsed() < 10000) {
            if (cancel && cancel())
                held->sawCancel.store(true, std::memory_order_release);
            QThread::msleep(2);
        }
        result.cancelled = cancel && cancel();
        return result;
    };
    controller->setOperationsForTesting(operations);
    controller->setSelectedLogPath(input);

    controller->startMatlab();
    QTRY_VERIFY_WITH_TIMEOUT(held->entered.load(std::memory_order_acquire), 3000);
    QProgressDialog *progress = waitVisible<QProgressDialog>(
        &owner, "DataFlashLogProgressDialog");
    QVERIFY(progress);
    QPushButton *cancelButton = progress->findChild<QPushButton *>();
    QVERIFY(cancelButton);
    cancelButton->click();
    QTRY_VERIFY_WITH_TIMEOUT(held->sawCancel.load(std::memory_order_acquire), 3000);
    QVERIFY(controller->busy());
    held->release.store(true, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    QVERIFY(!visible<QDialog>(&owner, "DataFlashMatlabConfirmationDialog"));
    QCOMPARE(state->matlabExportCalls.load(), 0);
    QVERIFY(!QFileInfo::exists(input + QStringLiteral("-2.mat")));
}

void DataFlashLogToolsControllerTest::autoAnalysisIsReadableAndModeless()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("analysis.log"));
    QVERIFY(writeFile(input, QByteArrayLiteral("FMT,...\n")));
    QWidget owner;
    owner.show();
    auto *widget = new DataFlashLogsWidget(&owner);
    auto *controller = new DataFlashLogToolsController(widget, &owner);
    const auto state = std::make_shared<FakeState>();
    controller->setOperationsForTesting(fakeOperations(state));
    controller->setSelectedLogPath(input);

    controller->startAutoAnalysis();
    QDialog *report = waitVisible<QDialog>(&owner, "DataFlashAutoAnalysisDialog");
    QVERIFY(report);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 3000);
    QCOMPARE(report->windowModality(), Qt::NonModal);
    QPlainTextEdit *text = report->findChild<QPlainTextEdit *>(
        QStringLiteral("DataFlashAutoAnalysisReport"));
    QVERIFY(text);
    QVERIFY(text->isReadOnly());
    QVERIFY(text->toPlainText().contains(QStringLiteral("GPS")));
    QVERIFY(text->toPlainText().contains(QStringLiteral("Advisory only")));
}

void DataFlashLogToolsControllerTest::organizerShowsEveryEntryAndRequiresConsent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString emptyPath = directory.filePath(QStringLiteral("empty.log"));
    QVERIFY(writeFile(emptyPath, QByteArray()));
    QWidget owner;
    owner.show();
    auto *widget = new DataFlashLogsWidget(&owner);
    auto *controller = new DataFlashLogToolsController(widget, &owner);

    controller->startOrganize();
    QFileDialog *picker = waitVisible<QFileDialog>(
        &owner, "DataFlashLogOrganizerDirectoryDialog");
    QVERIFY(picker);
    QVERIFY(choose(picker, directory.path()));
    QDialog *plan = waitVisible<QDialog>(
        &owner, "DataFlashLogOrganizerPlanDialog", 10000);
    QVERIFY(plan);
    QTreeWidget *tree = plan->findChild<QTreeWidget *>(
        QStringLiteral("DataFlashLogOrganizerPlanTree"));
    QVERIFY(tree);
    QCOMPARE(tree->topLevelItemCount(), 1);
    QVERIFY(tree->topLevelItem(0)->text(2).contains(QStringLiteral("empty.log")));
    QPushButton *cancelButton = defaultButton(plan);
    QVERIFY(cancelButton);
    plan->reject();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 3000);
    QVERIFY(QFileInfo::exists(emptyPath));

    controller->startOrganize();
    picker = waitVisible<QFileDialog>(&owner, "DataFlashLogOrganizerDirectoryDialog");
    QVERIFY(choose(picker, directory.path()));
    plan = waitVisible<QDialog>(&owner, "DataFlashLogOrganizerPlanDialog", 10000);
    QVERIFY(clickAccept(plan));
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 10000);
    QVERIFY(!QFileInfo::exists(emptyPath));
}

void DataFlashLogToolsControllerTest::shutdownCancelsAndWaitsForWorker()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("held.bin"));
    QVERIFY(writeFile(input, QByteArrayLiteral("held")));
    QWidget owner;
    owner.show();
    auto *widget = new DataFlashLogsWidget(&owner);
    auto *controller = new DataFlashLogToolsController(widget, &owner);
    const auto fake = std::make_shared<FakeState>();
    auto operations = fakeOperations(fake);
    const auto held = std::make_shared<HeldAnalysis>();
    operations.analyze = [held](
        const QString &, DataFlashLogAnalyzer::Cancel cancel,
        DataFlashLogAnalyzer::Progress) {
        DataFlashLogAnalyzer::Result result;
        held->entered.store(true, std::memory_order_release);
        QElapsedTimer deadline;
        deadline.start();
        while (!held->release.load(std::memory_order_acquire)
               && deadline.elapsed() < 10000) {
            if (cancel && cancel())
                held->sawCancel.store(true, std::memory_order_release);
            QThread::msleep(2);
        }
        result.cancelled = true;
        return result;
    };
    controller->setOperationsForTesting(operations);
    controller->setSelectedLogPath(input);
    QSignalSpy readySpy(controller, &DataFlashLogToolsController::shutdownReady);

    controller->startAutoAnalysis();
    QTRY_VERIFY_WITH_TIMEOUT(held->entered.load(std::memory_order_acquire), 3000);
    controller->shutdown();
    QVERIFY(controller->shutdownPending());
    QVERIFY(controller->busy());
    QTRY_VERIFY_WITH_TIMEOUT(held->sawCancel.load(std::memory_order_acquire), 3000);
    QCOMPARE(readySpy.size(), 0);
    held->release.store(true, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.size(), 1, 3000);
}

void DataFlashLogToolsControllerTest::callbacksMayDeleteController()
{
    QPointer<QWidget> owner = new QWidget;
    owner->show();
    auto *widget = new DataFlashLogsWidget(owner);
    auto *controller = new DataFlashLogToolsController(widget, owner);
    connect(controller, &DataFlashLogToolsController::busyChanged,
            owner, [owner](bool busy) {
        if (busy && owner)
            delete owner;
    }, Qt::DirectConnection);
    controller->startReview();
    QVERIFY(owner.isNull());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString reviewPath = directory.filePath(QStringLiteral("reentrant.bin"));
    QVERIFY(writeFile(reviewPath, QByteArrayLiteral("review")));
    QPointer<QWidget> finishOwner = new QWidget;
    finishOwner->show();
    auto *finishWidget = new DataFlashLogsWidget(finishOwner);
    auto *finishController = new DataFlashLogToolsController(finishWidget, finishOwner);
    bool reviewEmitted = false;
    connect(finishController, &DataFlashLogToolsController::reviewLogRequested,
            qApp, [&reviewEmitted](const QString &) { reviewEmitted = true; });
    connect(finishController, &DataFlashLogToolsController::busyChanged,
            finishOwner, [finishOwner](bool busy) {
        if (!busy && finishOwner)
            delete finishOwner;
    }, Qt::DirectConnection);
    finishController->startReview();
    QFileDialog *finishPicker = waitVisible<QFileDialog>(
        finishOwner, "DataFlashLogInputDialog");
    QVERIFY(finishPicker);
    QVERIFY(choose(finishPicker, reviewPath));
    QVERIFY(finishOwner.isNull());
    QVERIFY(!reviewEmitted);

    QPointer<QWidget> secondOwner = new QWidget;
    secondOwner->show();
    auto *secondWidget = new DataFlashLogsWidget(secondOwner);
    auto *second = new DataFlashLogToolsController(secondWidget, secondOwner);
    connect(second, &DataFlashLogToolsController::logMessage,
            secondOwner, [secondOwner](const QString &) {
        if (secondOwner)
            delete secondOwner;
    }, Qt::DirectConnection);
    second->startReview();
    QFileDialog *picker = waitVisible<QFileDialog>(secondOwner, "DataFlashLogInputDialog");
    QVERIFY(picker);
    picker->reject();
    QCoreApplication::processEvents();
    QVERIFY(secondOwner.isNull());
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    DataFlashLogToolsControllerTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_dataflashlogtoolscontroller.moc"
