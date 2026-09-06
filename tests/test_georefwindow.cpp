#include "GeoRefRuntimeFixture.h"
#include "ui/Loghandling/GeoRefWindow.h"

#include <QtTest>

#include <QApplication>
#include <QAbstractItemModel>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <memory>

namespace {

template<typename T>
T *visible(QWidget *owner, const char *name)
{
    if (!owner)
        return nullptr;
    for (T *candidate : owner->findChildren<T *>(
             QString::fromLatin1(name), Qt::FindChildrenRecursively)) {
        if (candidate->isVisible())
            return candidate;
    }
    return nullptr;
}

template<typename T>
T *waitVisible(QWidget *owner, const char *name, int timeout = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (T *candidate = visible<T>(owner, name))
            return candidate;
        QTest::qWait(2);
    }
    return nullptr;
}

bool choose(QFileDialog *dialog, const QString &path)
{
    if (!dialog)
        return false;
    if (QLineEdit *edit = dialog->findChild<QLineEdit *>(
            QStringLiteral("fileNameEdit"))) {
        edit->setText(path);
    } else {
        dialog->selectFile(path);
    }
    const QStringList selected = dialog->selectedFiles();
    if (selected.size() != 1)
        return false;
    const bool directory = dialog->fileMode() == QFileDialog::Directory;
    const QString actual = directory
        ? QDir(selected.first()).absolutePath()
        : QFileInfo(selected.first()).absoluteFilePath();
    const QString expected = directory
        ? QDir(path).absolutePath() : QFileInfo(path).absoluteFilePath();
    return actual == expected
        && QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
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

void acceptConsent(QDialog *dialog)
{
    QVERIFY(dialog);
    QPushButton *button = dialog->findChild<QPushButton *>(
        QStringLiteral("GeoRefConfirmButton"));
    QVERIFY(button);
    button->click();
}

struct CapturedOptions {
    GeoRefService::Options options;
    std::atomic_bool called{false};
};

class CloseOnProgressShow final : public QObject
{
public:
    explicit CloseOnProgressShow(GeoRefWindow *window)
        : m_window(window)
    {}

    bool fired() const { return m_fired; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!m_fired && event->type() == QEvent::Show
            && watched->objectName() == QLatin1String("GeoRefProgressDialog")) {
            m_fired = true;
            if (m_window)
                m_window->close();
        }
        return false;
    }

private:
    QPointer<GeoRefWindow> m_window;
    bool m_fired = false;
};

GeoRefWindow::Operations passThroughOperations()
{
    GeoRefWindow::Operations operations;
    operations.prepare = [](const GeoRefService::Options &options,
                            const GeoRefService::Cancel &cancel,
                            const GeoRefService::Progress &progress) {
        return GeoRefService::Prepare(options, cancel, progress);
    };
    operations.estimate = [](const GeoRefService::Options &options,
                             const GeoRefService::Cancel &cancel,
                             const GeoRefService::Progress &progress) {
        return GeoRefService::Estimate(options, cancel, progress);
    };
    operations.execute = [](const GeoRefService::Plan &plan,
                            const GeoRefService::Cancel &cancel,
                            const GeoRefService::Progress &progress) {
        return GeoRefService::Execute(plan, cancel, progress);
    };
    return operations;
}

} // namespace

class GeoRefWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void controlsMatchOfflineWorkflow();
    void realPickersAndEstimateFreezeOptions();
    void consentDefaultsToCancelAndExecutesFrozenPlan();
    void closeBusyCancelsAndWaitsForWorker();
    void closeFromProgressShowLeavesNoPhantomJob();
    void requestShutdownIsIdempotentAndWaitsForWorker();
    void admissionCallbackMayDeleteWindow();
    void resultModelCallbackMayDeleteWindow();
};

void GeoRefWindowTest::controlsMatchOfflineWorkflow()
{
    GeoRefWindow window;
    QCOMPARE(window.objectName(), QStringLiteral("GeoRefWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("Geo Ref Images"));
    QCOMPARE(window.size(), QSize(920, 660));
    QVERIFY(window.isWindow());
    QVERIFY(!window.isBusy());
    QVERIFY(!window.isClosing());

    QVERIFY(window.findChild<QLineEdit *>(QStringLiteral("GeoRefLogPath")));
    QVERIFY(window.findChild<QLineEdit *>(QStringLiteral("GeoRefPhotoDirectory")));
    QVERIFY(window.findChild<QLineEdit *>(QStringLiteral("GeoRefOutputDirectory")));
    auto *offset = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("GeoRefTimeOffsetSeconds"));
    QVERIFY(offset);
    QCOMPARE(offset->value(), 0.0);
    QCOMPARE(offset->minimum(), -1000000000.0);
    QCOMPARE(offset->maximum(), 1000000000.0);
    QVERIFY(window.findChild<QSpinBox *>(QStringLiteral("GeoRefShutterLag")));
    QVERIFY(window.findChild<QCheckBox *>(QStringLiteral("GeoRefUseGps2")));
    QVERIFY(window.findChild<QCheckBox *>(
        QStringLiteral("GeoRefUseAmslAltitude")));
    QVERIFY(window.findChild<QCheckBox *>(
        QStringLiteral("GeoRefUseGpsAltitude")));

    auto *results = window.findChild<QTableWidget *>(
        QStringLiteral("GeoRefResults"));
    QVERIFY(results);
    QCOMPARE(results->columnCount(), 5);
    QCOMPARE(results->rowCount(), 0);
    QVERIFY(window.findChild<QPlainTextEdit *>(
        QStringLiteral("GeoRefOutputLog")));
    QVERIFY(!window.findChild<QPushButton *>(
        QStringLiteral("GeoRefGeoTagButton"))->isEnabled());
}

void GeoRefWindowTest::realPickersAndEstimateFreezeOptions()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString logPath;
    QString photoDirectory;
    QVERIFY(GeoRefRuntimeFixture::create(
        directory.path(), &logPath, &photoDirectory));
    const QString outputDirectory = directory.filePath(
        QStringLiteral("second-geotag-run"));
    QVERIFY(QDir().mkpath(outputDirectory));

    auto captured = std::make_shared<CapturedOptions>();
    GeoRefWindow::Operations operations = passThroughOperations();
    operations.estimate = [captured](
        const GeoRefService::Options &options,
        const GeoRefService::Cancel &cancel,
        const GeoRefService::Progress &progress) {
        captured->options = options;
        captured->called.store(true, std::memory_order_release);
        if (progress)
            progress(1, 1);
        GeoRefService::EstimateResult result;
        if (cancel && cancel()) {
            result.cancelled = true;
            return result;
        }
        result.success = true;
        result.hasEstimate = true;
        result.offsetSeconds = 12.0;
        return result;
    };

    GeoRefWindow window;
    window.setOperationsForTesting(operations);
    window.show();
    window.findChild<QPushButton *>(QStringLiteral("GeoRefBrowseLogButton"))
        ->click();
    QFileDialog *logDialog = waitVisible<QFileDialog>(
        &window, "GeoRefLogDialog");
    QVERIFY(logDialog);
    QVERIFY(choose(logDialog, logPath));

    window.findChild<QPushButton *>(QStringLiteral("GeoRefBrowsePhotoButton"))
        ->click();
    QFileDialog *photoDialog = waitVisible<QFileDialog>(
        &window, "GeoRefPhotoDirectoryDialog");
    QVERIFY(photoDialog);
    QVERIFY(choose(photoDialog, photoDirectory));

    window.findChild<QPushButton *>(QStringLiteral("GeoRefBrowseOutputButton"))
        ->click();
    QFileDialog *outputDialog = waitVisible<QFileDialog>(
        &window, "GeoRefOutputDirectoryDialog");
    QVERIFY(outputDialog);
    QVERIFY(choose(outputDialog, outputDirectory));

    auto *offset = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("GeoRefTimeOffsetSeconds"));
    auto *lag = window.findChild<QSpinBox *>(QStringLiteral("GeoRefShutterLag"));
    auto *base = window.findChild<QDoubleSpinBox *>(
        QStringLiteral("GeoRefBaseAltitudeAdjustment"));
    auto *gps2 = window.findChild<QCheckBox *>(QStringLiteral("GeoRefUseGps2"));
    offset->setValue(3.25);
    lag->setValue(175);
    base->setValue(-4.5);
    gps2->setChecked(true);
    window.findChild<QPushButton *>(
        QStringLiteral("GeoRefEstimateOffsetButton"))->click();
    offset->setValue(99.0);
    QTRY_VERIFY_WITH_TIMEOUT(captured->called.load(std::memory_order_acquire),
                             5000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 5000);
    QCOMPARE(captured->options.logPath,
             QFileInfo(logPath).absoluteFilePath());
    QCOMPARE(captured->options.photoDirectory,
             QDir(photoDirectory).absolutePath());
    QCOMPARE(captured->options.outputDirectory,
             QDir(outputDirectory).absolutePath());
    QCOMPARE(captured->options.timeOffsetSeconds, 3.25);
    QCOMPARE(captured->options.shutterLagMilliseconds, 175);
    QCOMPARE(captured->options.baseAltitudeAdjustmentMeters, -4.5);
    QVERIFY(captured->options.useGps2);
    QCOMPARE(offset->value(), 12.0);
    QVERIFY(window.statusText().contains(QStringLiteral("estimated"),
                                         Qt::CaseInsensitive));
}

void GeoRefWindowTest::consentDefaultsToCancelAndExecutesFrozenPlan()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString logPath;
    QString photoDirectory;
    QVERIFY(GeoRefRuntimeFixture::create(
        directory.path(), &logPath, &photoDirectory));

    auto executeCalls = std::make_shared<std::atomic_int>(0);
    auto executedSource = std::make_shared<QString>();
    GeoRefWindow::Operations operations = passThroughOperations();
    operations.execute = [executeCalls, executedSource](
        const GeoRefService::Plan &plan,
        const GeoRefService::Cancel &cancel,
        const GeoRefService::Progress &progress) {
        ++*executeCalls;
        *executedSource = plan.options().logPath;
        GeoRefService::Result result;
        if (cancel && cancel()) {
            result.cancelled = true;
            return result;
        }
        if (progress)
            progress(plan.matches().size(), plan.matches().size());
        result.success = true;
        result.matches = plan.matches();
        result.taggedPhotos = result.matches.size();
        result.publishedPaths = plan.outputPaths();
        return result;
    };

    GeoRefWindow window;
    window.setOperationsForTesting(operations);
    window.setSource(logPath);
    window.findChild<QLineEdit *>(QStringLiteral("GeoRefPhotoDirectory"))
        ->setText(photoDirectory);
    window.findChild<QDoubleSpinBox *>(
        QStringLiteral("GeoRefTimeOffsetSeconds"))->setValue(12.0);
    window.show();
    QPushButton *geoTag = window.findChild<QPushButton *>(
        QStringLiteral("GeoRefGeoTagButton"));
    QVERIFY(geoTag->isEnabled());

    geoTag->click();
    QDialog *consent = waitVisible<QDialog>(
        &window, "GeoRefConfirmationDialog", 15000);
    QVERIFY(consent);
    QPushButton *cancel = consent->findChild<QPushButton *>(
        QStringLiteral("GeoRefConfirmationCancelButton"));
    QCOMPARE(defaultButton(consent), cancel);
    auto *summary = consent->findChild<QLabel *>(
        QStringLiteral("GeoRefConfirmationSummary"));
    auto *paths = consent->findChild<QPlainTextEdit *>(
        QStringLiteral("GeoRefConfirmationPaths"));
    QVERIFY(summary && paths);
    QVERIFY(summary->text().contains(QFileInfo(logPath).absoluteFilePath()));
    QVERIFY(summary->text().contains(QStringLiteral("Matched photos: 2")));
    QVERIFY(paths->toPlainText().contains(
        QDir(photoDirectory).filePath(QStringLiteral("geotagged/location.txt"))));
    cancel->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 5000);
    QCOMPARE(executeCalls->load(), 0);

    geoTag->click();
    consent = waitVisible<QDialog>(&window, "GeoRefConfirmationDialog", 15000);
    QVERIFY(consent);
    // Programmatic mutation cannot retarget the already prepared immutable plan.
    window.findChild<QLineEdit *>(QStringLiteral("GeoRefLogPath"))
        ->setText(directory.filePath(QStringLiteral("replacement.log")));
    acceptConsent(consent);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QCOMPARE(executeCalls->load(), 1);
    QCOMPARE(*executedSource, QFileInfo(logPath).absoluteFilePath());
    QCOMPARE(window.findChild<QTableWidget *>(
                 QStringLiteral("GeoRefResults"))->rowCount(), 2);
}

void GeoRefWindowTest::closeBusyCancelsAndWaitsForWorker()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString logPath;
    QString photoDirectory;
    QVERIFY(GeoRefRuntimeFixture::create(
        directory.path(), &logPath, &photoDirectory));
    auto started = std::make_shared<std::atomic_bool>(false);
    GeoRefWindow::Operations operations = passThroughOperations();
    operations.estimate = [started](
        const GeoRefService::Options &, const GeoRefService::Cancel &cancel,
        const GeoRefService::Progress &) {
        started->store(true, std::memory_order_release);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000 && !(cancel && cancel()))
            QThread::msleep(2);
        GeoRefService::EstimateResult result;
        result.cancelled = cancel && cancel();
        if (!result.cancelled)
            result.error = QStringLiteral("test cancellation deadline expired");
        return result;
    };

    QPointer<GeoRefWindow> window = new GeoRefWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setOperationsForTesting(operations);
    window->setSource(logPath);
    window->findChild<QLineEdit *>(QStringLiteral("GeoRefPhotoDirectory"))
        ->setText(photoDirectory);
    window->show();
    window->findChild<QPushButton *>(
        QStringLiteral("GeoRefEstimateOffsetButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(started->load(std::memory_order_acquire), 5000);
    QVERIFY(window->isBusy());
    window->close();
    QVERIFY(window);
    QVERIFY(window->isBusy());
    QVERIFY(window->isClosing());
    QTRY_VERIFY_WITH_TIMEOUT(window.isNull(), 10000);
}

void GeoRefWindowTest::closeFromProgressShowLeavesNoPhantomJob()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString logPath = directory.filePath(QStringLiteral("input.log"));
    QFile log(logPath);
    QVERIFY(log.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QVERIFY(log.write("fixture") > 0);
    log.close();

    auto operationCalls = std::make_shared<std::atomic_int>(0);
    GeoRefWindow::Operations operations = passThroughOperations();
    operations.estimate = [operationCalls](
        const GeoRefService::Options &, const GeoRefService::Cancel &,
        const GeoRefService::Progress &) {
        ++*operationCalls;
        GeoRefService::EstimateResult result;
        result.success = true;
        result.hasEstimate = true;
        return result;
    };

    QPointer<GeoRefWindow> window = new GeoRefWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setOperationsForTesting(operations);
    window->setSource(logPath);
    window->findChild<QLineEdit *>(QStringLiteral("GeoRefPhotoDirectory"))
        ->setText(directory.path());
    window->show();
    CloseOnProgressShow filter(window);
    qApp->installEventFilter(&filter);
    window->findChild<QPushButton *>(
        QStringLiteral("GeoRefEstimateOffsetButton"))->click();
    qApp->removeEventFilter(&filter);
    QVERIFY(filter.fired());
    QCOMPARE(operationCalls->load(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(window.isNull(), 5000);
}

void GeoRefWindowTest::requestShutdownIsIdempotentAndWaitsForWorker()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString logPath;
    QString photoDirectory;
    QVERIFY(GeoRefRuntimeFixture::create(
        directory.path(), &logPath, &photoDirectory));
    auto started = std::make_shared<std::atomic_bool>(false);
    GeoRefWindow::Operations operations = passThroughOperations();
    operations.estimate = [started](
        const GeoRefService::Options &, const GeoRefService::Cancel &cancel,
        const GeoRefService::Progress &) {
        started->store(true, std::memory_order_release);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000 && !(cancel && cancel()))
            QThread::msleep(2);
        GeoRefService::EstimateResult result;
        result.cancelled = cancel && cancel();
        return result;
    };

    GeoRefWindow window;
    window.setOperationsForTesting(operations);
    window.setSource(logPath);
    window.findChild<QLineEdit *>(QStringLiteral("GeoRefPhotoDirectory"))
        ->setText(photoDirectory);
    window.show();
    window.findChild<QPushButton *>(
        QStringLiteral("GeoRefEstimateOffsetButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(started->load(std::memory_order_acquire), 5000);
    QSignalSpy ready(&window, &GeoRefWindow::shutdownReady);
    window.requestShutdown();
    window.requestShutdown();
    QCOMPARE(ready.size(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
    window.requestShutdown();
    QCoreApplication::processEvents();
    QCOMPARE(ready.size(), 1);
}

void GeoRefWindowTest::admissionCallbackMayDeleteWindow()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString logPath;
    QString photoDirectory;
    QVERIFY(GeoRefRuntimeFixture::create(
        directory.path(), &logPath, &photoDirectory));
    QPointer<GeoRefWindow> window = new GeoRefWindow;
    window->setSource(logPath);
    window->findChild<QLineEdit *>(QStringLiteral("GeoRefPhotoDirectory"))
        ->setText(photoDirectory);
    connect(window, &GeoRefWindow::busyChanged, qApp,
            [window](bool busy) {
        if (busy && window)
            delete window.data();
    }, Qt::DirectConnection);
    window->findChild<QPushButton *>(
        QStringLiteral("GeoRefEstimateOffsetButton"))->click();
    QVERIFY(window.isNull());
}

void GeoRefWindowTest::resultModelCallbackMayDeleteWindow()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString logPath;
    QString photoDirectory;
    QVERIFY(GeoRefRuntimeFixture::create(
        directory.path(), &logPath, &photoDirectory));

    GeoRefWindow::Operations operations = passThroughOperations();
    operations.execute = [](const GeoRefService::Plan &plan,
                            const GeoRefService::Cancel &,
                            const GeoRefService::Progress &) {
        GeoRefService::Result result;
        result.success = true;
        result.matches = plan.matches();
        result.taggedPhotos = result.matches.size();
        return result;
    };
    QPointer<GeoRefWindow> window = new GeoRefWindow;
    window->setOperationsForTesting(operations);
    window->setSource(logPath);
    window->findChild<QLineEdit *>(QStringLiteral("GeoRefPhotoDirectory"))
        ->setText(photoDirectory);
    window->show();
    QTableWidget *table = window->findChild<QTableWidget *>(
        QStringLiteral("GeoRefResults"));
    connect(table->model(), &QAbstractItemModel::rowsInserted, qApp,
            [window]() {
        if (window)
            delete window.data();
    }, Qt::DirectConnection);
    window->findChild<QPushButton *>(QStringLiteral("GeoRefGeoTagButton"))
        ->click();
    QDialog *consent = waitVisible<QDialog>(
        window, "GeoRefConfirmationDialog", 15000);
    QVERIFY(consent);
    acceptConsent(consent);
    QTRY_VERIFY_WITH_TIMEOUT(window.isNull(), 10000);
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QApplication application(argc, argv);
    GeoRefWindowTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_georefwindow.moc"
