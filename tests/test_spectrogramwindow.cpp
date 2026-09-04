#include <QtTest>

#include "ui/SpectrogramWindow.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryFile>
#include <QThread>

#include <atomic>
#include <stdexcept>

class SpectrogramWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void initialStateMatchesMp10();
    void validatesFileAndDbRangeBeforeStarting();
    void rendersOffTheUiThreadAndReportsMetadata();
    void generatorExceptionClearsPreviousImages();
    void cancellationAndCloseJoinTheWorker();
    void uncooperativeWorkerDoesNotBlockWindowClose();
};

namespace
{
SpectrogramWindow::Dependencies successfulDependencies(
    std::atomic_bool *ranOffUi = nullptr)
{
    SpectrogramWindow::Dependencies dependencies;
    dependencies.chooseLog = [](QWidget *) { return QString(); };
    dependencies.generate = [ranOffUi](
        const QString &, const QString &, int, int,
        const SpectrogramWindow::CancelRequested &) {
        if (ranOffUi) {
            ranOffUi->store(QThread::currentThread()
                            != QCoreApplication::instance()->thread());
        }
        SpectrogramWindow::RenderResult result;
        result.success = true;
        result.warning = QStringLiteral("1 recoverable parser issue");
        result.startSeconds = 1.25;
        result.endSeconds = 4.5;
        result.maximumFrequency = 200.0;
        result.sampleCount = 2048;
        result.windowCount = 5;
        result.images[0] = QImage(5, 512, QImage::Format_RGB32);
        result.images[1] = QImage(5, 512, QImage::Format_RGB32);
        result.images[2] = QImage(5, 512, QImage::Format_RGB32);
        result.images[0].fill(Qt::red);
        result.images[1].fill(Qt::green);
        result.images[2].fill(Qt::blue);
        return result;
    };
    return dependencies;
}

QTemporaryFile *makeLog(const QString &suffix)
{
    auto *file = new QTemporaryFile(
        QDir::tempPath() + QStringLiteral("/apm-spectrogram-XXXXXX.")
        + suffix);
    if (!file->open() || file->write("fixture") < 0 || !file->flush()) {
        delete file;
        return nullptr;
    }
    return file;
}
}

void SpectrogramWindowTest::initialStateMatchesMp10()
{
    SpectrogramWindow window(successfulDependencies());
    QCOMPARE(window.objectName(), QStringLiteral("SpectrogramWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("DataFlash Spectrogram"));
    QCOMPARE(window.size(), QSize(1050, 820));
    QCOMPARE(window.minimumSize(), QSize(700, 500));
    QCOMPARE(window.statusText(),
             QStringLiteral("Open a DataFlash .bin or .log file."));

    auto *sensor = window.findChild<QComboBox *>(
        QStringLiteral("sensorCombo"));
    QVERIFY(sensor);
    QCOMPARE(sensor->count(), 10);
    QCOMPARE(sensor->itemText(0), QStringLiteral("ACC1"));
    QCOMPARE(sensor->itemText(4), QStringLiteral("ACC5"));
    QCOMPARE(sensor->itemText(5), QStringLiteral("GYR1"));
    QCOMPARE(sensor->itemText(9), QStringLiteral("GYR5"));
    QCOMPARE(window.findChild<QSpinBox *>(
                 QStringLiteral("minimumDbSpin"))->value(), -80);
    QCOMPARE(window.findChild<QSpinBox *>(
                 QStringLiteral("maximumDbSpin"))->value(), -20);
    QVERIFY(!window.findChild<QPushButton *>(
                 QStringLiteral("redrawButton"))->isEnabled());
    QVERIFY(!window.findChild<QPushButton *>(
                 QStringLiteral("cancelButton"))->isEnabled());
    for (const QString &axis : {QStringLiteral("X"),
                                QStringLiteral("Y"),
                                QStringLiteral("Z")}) {
        QVERIFY(window.findChild<QLabel *>(
            QStringLiteral("axis%1Image").arg(axis)));
        QCOMPARE(window.findChild<QLabel *>(
                     QStringLiteral("axis%1Title").arg(axis))->text(),
                 QStringLiteral("%1 — frequency (low → high)").arg(axis));
    }
}

void SpectrogramWindowTest::validatesFileAndDbRangeBeforeStarting()
{
    std::atomic_int calls(0);
    SpectrogramWindow::Dependencies dependencies = successfulDependencies();
    const auto base = dependencies.generate;
    dependencies.generate = [&calls, base](
        const QString &path, const QString &sensor, int min, int max,
        const SpectrogramWindow::CancelRequested &cancel) {
        ++calls;
        return base(path, sensor, min, max, cancel);
    };
    SpectrogramWindow window(dependencies);
    window.setLogPath(QStringLiteral("/missing/input.log"));
    QVERIFY(window.logPath().isEmpty());
    QVERIFY(window.statusText().contains(QStringLiteral("existing")));

    std::unique_ptr<QTemporaryFile> wrong(makeLog(QStringLiteral("txt")));
    QVERIFY(wrong);
    window.setLogPath(wrong->fileName());
    QVERIFY(window.logPath().isEmpty());

    std::unique_ptr<QTemporaryFile> log(makeLog(QStringLiteral("log")));
    QVERIFY(log);
    window.setLogPath(log->fileName());
    QVERIFY(!window.logPath().isEmpty());
    auto *minimum = window.findChild<QSpinBox *>(
        QStringLiteral("minimumDbSpin"));
    auto *maximum = window.findChild<QSpinBox *>(
        QStringLiteral("maximumDbSpin"));
    minimum->setValue(-20);
    maximum->setValue(-20);
    window.findChild<QPushButton *>(
        QStringLiteral("redrawButton"))->click();
    QCOMPARE(calls.load(), 0);
    QCOMPARE(window.statusText(),
             QStringLiteral("Min dB must be smaller than Max dB."));
}

void SpectrogramWindowTest::rendersOffTheUiThreadAndReportsMetadata()
{
    std::unique_ptr<QTemporaryFile> log(makeLog(QStringLiteral("BIN")));
    QVERIFY(log);
    std::atomic_bool ranOffUi(false);
    SpectrogramWindow window(successfulDependencies(&ranOffUi));
    window.setLogPath(log->fileName());
    window.findChild<QComboBox *>(QStringLiteral("sensorCombo"))
        ->setCurrentText(QStringLiteral("GYR3"));
    window.findChild<QPushButton *>(
        QStringLiteral("redrawButton"))->click();
    QVERIFY(window.isBusy());
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 3000);
    QVERIFY(ranOffUi.load());
    QVERIFY(window.statusText().contains(QStringLiteral("GYR3")));
    QVERIFY(window.statusText().contains(QStringLiteral("1.250–4.500 s")));
    QVERIFY(window.statusText().contains(QStringLiteral("0–200.0 Hz")));
    QVERIFY(window.statusText().contains(
        QStringLiteral("Warning: 1 recoverable parser issue")));
    for (const QString &axis : {QStringLiteral("X"),
                                QStringLiteral("Y"),
                                QStringLiteral("Z")}) {
        const QLabel *label = window.findChild<QLabel *>(
            QStringLiteral("axis%1Image").arg(axis));
        QVERIFY(label);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPixmap pixmap = label->pixmap();
#else
        const QPixmap pixmap = label->pixmap(Qt::ReturnByValue);
#endif
        QVERIFY(!pixmap.isNull());
    }
}

void SpectrogramWindowTest::generatorExceptionClearsPreviousImages()
{
    std::unique_ptr<QTemporaryFile> log(makeLog(QStringLiteral("log")));
    QVERIFY(log);
    std::atomic_int calls(0);
    SpectrogramWindow::Dependencies dependencies = successfulDependencies();
    const auto success = dependencies.generate;
    dependencies.generate = [&calls, success](
        const QString &path, const QString &sensor, int minimum, int maximum,
        const SpectrogramWindow::CancelRequested &cancel) {
        if (calls.fetch_add(1) == 0) {
            return success(path, sensor, minimum, maximum, cancel);
        }
        throw std::runtime_error("fixture generator failed");
    };

    SpectrogramWindow window(dependencies);
    window.setLogPath(log->fileName());
    auto *redraw = window.findChild<QPushButton *>(
        QStringLiteral("redrawButton"));
    auto *image = window.findChild<QLabel *>(QStringLiteral("axisXImage"));
    redraw->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 3000);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QVERIFY(!image->pixmap().isNull());
#else
    QVERIFY(!image->pixmap(Qt::ReturnByValue).isNull());
#endif

    redraw->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 3000);
    QVERIFY(window.statusText().contains(
        QStringLiteral("fixture generator failed")));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QVERIFY(image->pixmap().isNull());
#else
    QVERIFY(image->pixmap(Qt::ReturnByValue).isNull());
#endif
}

void SpectrogramWindowTest::cancellationAndCloseJoinTheWorker()
{
    std::unique_ptr<QTemporaryFile> log(makeLog(QStringLiteral("log")));
    QVERIFY(log);
    std::atomic_bool entered(false);
    std::atomic_bool cancellationObserved(false);
    SpectrogramWindow::Dependencies dependencies;
    dependencies.chooseLog = [](QWidget *) { return QString(); };
    dependencies.generate = [&entered, &cancellationObserved](
        const QString &, const QString &, int, int,
        const SpectrogramWindow::CancelRequested &cancel) {
        entered.store(true);
        while (!cancel()) {
            QThread::msleep(1);
        }
        cancellationObserved.store(true);
        SpectrogramWindow::RenderResult result;
        result.cancelled = true;
        return result;
    };

    auto *window = new SpectrogramWindow(dependencies);
    window->setLogPath(log->fileName());
    window->show();
    window->findChild<QPushButton *>(
        QStringLiteral("redrawButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 3000);
    QVERIFY(window->findChild<QPushButton *>(
                QStringLiteral("cancelButton"))->isEnabled());
    window->findChild<QPushButton *>(
        QStringLiteral("cancelButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window->isBusy(), 3000);
    QVERIFY(cancellationObserved.load());
    QCOMPARE(window->statusText(),
             QStringLiteral("Spectrogram calculation cancelled."));

    entered.store(false);
    cancellationObserved.store(false);
    window->findChild<QPushButton *>(
        QStringLiteral("redrawButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 3000);
    QPointer<SpectrogramWindow> guarded(window);
    window->close();
    QTRY_VERIFY_WITH_TIMEOUT(guarded.isNull(), 3000);
    QVERIFY(cancellationObserved.load());
}

void SpectrogramWindowTest::uncooperativeWorkerDoesNotBlockWindowClose()
{
    std::unique_ptr<QTemporaryFile> log(makeLog(QStringLiteral("log")));
    QVERIFY(log);
    std::atomic_bool entered(false);
    std::atomic_bool completed(false);
    SpectrogramWindow::Dependencies dependencies;
    dependencies.chooseLog = [](QWidget *) { return QString(); };
    dependencies.generate = [&entered, &completed](
        const QString &, const QString &, int, int,
        const SpectrogramWindow::CancelRequested &) {
        entered.store(true);
        QThread::msleep(800);
        completed.store(true);
        SpectrogramWindow::RenderResult result;
        result.cancelled = true;
        return result;
    };

    auto *window = new SpectrogramWindow(dependencies);
    window->setLogPath(log->fileName());
    window->show();
    window->findChild<QPushButton *>(
        QStringLiteral("redrawButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 3000);

    QElapsedTimer timer;
    timer.start();
    QPointer<SpectrogramWindow> guarded(window);
    window->close();
    QTRY_VERIFY_WITH_TIMEOUT(guarded.isNull(), 700);
    QVERIFY(timer.elapsed() < 700);
    QTRY_VERIFY_WITH_TIMEOUT(completed.load(), 2000);
}

QTEST_MAIN(SpectrogramWindowTest)
#include "test_spectrogramwindow.moc"
