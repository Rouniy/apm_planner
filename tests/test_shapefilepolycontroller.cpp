#include "ui/configuration/ShapefilePolyController.h"

#include <QtTest>

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPointF>
#include <QProgressDialog>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeWidget>
#include <QWidget>
#include <QtEndian>

#include <atomic>
#include <cstring>
#include <memory>

namespace
{
template<typename T>
T *visible(QWidget *owner, const char *name)
{
    if (!owner)
        return nullptr;
    const auto objects = owner->findChildren<T *>(
        QString::fromLatin1(name), Qt::FindChildrenRecursively);
    for (T *object : objects) {
        if (object->isVisible())
            return object;
    }
    return nullptr;
}

template<typename T>
T *waitVisible(QWidget *owner, const char *name, int timeout = 3000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() <= timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (T *object = visible<T>(owner, name))
            return object;
        QTest::qWait(2);
    }
    return nullptr;
}

void putBe32(QByteArray *bytes, int offset, quint32 value)
{
    qToBigEndian(value,
        reinterpret_cast<uchar *>(bytes->data() + offset));
}

void putLe32(QByteArray *bytes, int offset, quint32 value)
{
    qToLittleEndian(value,
        reinterpret_cast<uchar *>(bytes->data() + offset));
}

void putLeDouble(QByteArray *bytes, int offset, double value)
{
    quint64 bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double width");
    std::memcpy(&bits, &value, sizeof(bits));
    qToLittleEndian(bits,
        reinterpret_cast<uchar *>(bytes->data() + offset));
}

QByteArray pointShapefile(const QVector<QPointF> &points)
{
    QByteArray bytes(100 + points.size() * 28, '\0');
    putBe32(&bytes, 0, 9994);
    putBe32(&bytes, 24, quint32(bytes.size() / 2));
    putLe32(&bytes, 28, 1000);
    putLe32(&bytes, 32, 1);
    if (!points.isEmpty()) {
        double minimumX = points.first().x();
        double maximumX = minimumX;
        double minimumY = points.first().y();
        double maximumY = minimumY;
        for (const QPointF &point : points) {
            minimumX = qMin(minimumX, point.x());
            maximumX = qMax(maximumX, point.x());
            minimumY = qMin(minimumY, point.y());
            maximumY = qMax(maximumY, point.y());
        }
        putLeDouble(&bytes, 36, minimumX);
        putLeDouble(&bytes, 44, minimumY);
        putLeDouble(&bytes, 52, maximumX);
        putLeDouble(&bytes, 60, maximumY);
    }
    for (int index = 0; index < points.size(); ++index) {
        const int at = 100 + index * 28;
        putBe32(&bytes, at, quint32(index + 1));
        putBe32(&bytes, at + 4, 10);
        putLe32(&bytes, at + 8, 1);
        putLeDouble(&bytes, at + 12, points.at(index).x());
        putLeDouble(&bytes, at + 20, points.at(index).y());
    }
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QFileDialog *openPicker(QWidget *owner, ShapefilePolyController *controller)
{
    const QPointer<ShapefilePolyController> guard(controller);
    controller->start();
    if (!guard || !guard->busy())
        return nullptr;
    return waitVisible<QFileDialog>(
        owner, "DeveloperShapefilePolyInputDialog");
}

bool acceptInput(QFileDialog *picker, const QString &path)
{
    if (!picker)
        return false;
    QLineEdit *name = picker->findChild<QLineEdit *>(
        QStringLiteral("fileNameEdit"));
    if (!name)
        return false;
    name->setText(path);
    const QStringList selected = picker->selectedFiles();
    if (selected.size() != 1
        || QDir::cleanPath(QFileInfo(selected.first()).absoluteFilePath())
            != QDir::cleanPath(QFileInfo(path).absoluteFilePath())) {
        return false;
    }
    return QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
}

QDialog *prepareConfirmation(QWidget *owner,
                             ShapefilePolyController *controller,
                             const QString &input)
{
    QFileDialog *picker = openPicker(owner, controller);
    if (!acceptInput(picker, input))
        return nullptr;
    return waitVisible<QDialog>(
        owner, "DeveloperShapefilePolyConfirmation", 5000);
}

bool anyLogContains(const QSignalSpy &spy, const QString &needle)
{
    for (const QList<QVariant> &arguments : spy) {
        if (!arguments.isEmpty()
            && arguments.first().toString().contains(
                needle, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

struct WaitState
{
    std::atomic_bool entered{false};
    std::atomic_bool cancelObserved{false};
    std::atomic_bool release{false};
    std::atomic_bool exited{false};
};

struct WaitRelease
{
    std::shared_ptr<WaitState> state;
    ~WaitRelease()
    {
        if (state)
            state->release.store(true, std::memory_order_release);
    }
};

ShapefileImportService::Preparation waitForPreparationCancel(
    const std::shared_ptr<WaitState> &state,
    const Shapefile::Cancel &cancel,
    const Shapefile::Progress &progress)
{
    if (progress)
        progress(1, 2, QStringLiteral("Reading held shapefile fixture"));
    state->entered.store(true, std::memory_order_release);
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 10000) {
        if (cancel && cancel()) {
            state->cancelObserved.store(true, std::memory_order_release);
            QElapsedTimer drain;
            drain.start();
            while (!state->release.load(std::memory_order_acquire)
                   && drain.elapsed() < 5000) {
                QThread::msleep(2);
            }
            ShapefileImportService::Preparation result;
            result.cancelled = true;
            result.error = QStringLiteral("cancelled fixture preparation");
            state->exited.store(true, std::memory_order_release);
            return result;
        }
        QThread::msleep(2);
    }
    ShapefileImportService::Preparation result;
    result.error = QStringLiteral("fixture preparation deadline expired");
    state->exited.store(true, std::memory_order_release);
    return result;
}

ShapefilePolyController::Prepare realPrepare()
{
    return [](const QString &path, const Shapefile::Cancel &cancel,
              const Shapefile::Progress &progress) {
        return ShapefileImportService::prepare(path, cancel, progress);
    };
}
} // namespace

class ShapefilePolyControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void pickerAndPreparedPlanAreDefaultCancel();
    void confirmedConversionWritesExactPlannedFiles();
    void emptyValidShapefileNeedsNoConsentOrOutput();
    void preparationCancelButtonStaysWaitingAndEscapeNeverReopens();
    void lateSuccessfulPreparationCannotBypassCancellation();
    void exportCancellationReportsActualPartialFiles();
    void closeAndDestructionCancelWorkerWithoutOutputs();
    void backendAndBusyCallbacksAreLifetimeSafe();
};

void ShapefilePolyControllerTest::pickerAndPreparedPlanAreDefaultCancel()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("points-%3.shp"));
    QVERIFY(writeFile(input, pointShapefile({QPointF(12.5, 34.25),
                                             QPointF(-3.0, 5.0)})));
    const QString first = directory.filePath(QStringLiteral("poly-1.poly"));
    QVERIFY(writeFile(first, QByteArray("OLD")));
    QWidget owner;
    owner.show();
    auto *controller = new ShapefilePolyController(&owner);
    QSignalSpy logSpy(controller, &ShapefilePolyController::logMessage);

    QFileDialog *picker = openPicker(&owner, controller);
    QVERIFY(picker);
    QCOMPARE(picker->fileMode(), QFileDialog::ExistingFile);
    QCOMPARE(picker->acceptMode(), QFileDialog::AcceptOpen);
    QVERIFY(picker->nameFilters().join(QLatin1Char(' ')).contains(
        QStringLiteral("*.shp *.SHP")));
    QVERIFY(QMetaObject::invokeMethod(picker, "reject", Qt::DirectConnection));
    QTRY_VERIFY(!controller->busy());
    QVERIFY(anyLogContains(logSpy, QStringLiteral("cancelled")));
    QCOMPARE(readFile(first), QByteArray("OLD"));

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QDialog *confirmation = prepareConfirmation(&owner, controller, input);
    QVERIFY(confirmation);
    QCOMPARE(confirmation->windowTitle(),
             QStringLiteral("Convert Shapefile to POLY"));
    auto *summary = confirmation->findChild<QLabel *>(
        QStringLiteral("DeveloperShapefilePolySummary"));
    QVERIFY(summary);
    QCOMPARE(summary->textFormat(), Qt::PlainText);
    QVERIFY(summary->text().contains(input));
    QVERIFY(summary->text().contains(directory.path()));
    QVERIFY(summary->text().contains(QStringLiteral("Points: 2")));
    QVERIFY(summary->text().contains(QStringLiteral("WGS84")));
    auto *tree = confirmation->findChild<QTreeWidget *>(
        QStringLiteral("DeveloperShapefilePolyOutputs"));
    QVERIFY(tree);
    QCOMPARE(tree->topLevelItemCount(), 2);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("Replace"));
    QCOMPARE(tree->topLevelItem(0)->text(1), QStringLiteral("1"));
    QCOMPARE(tree->topLevelItem(0)->text(2), first);
    QCOMPARE(tree->topLevelItem(1)->text(0), QStringLiteral("Create"));
    QCOMPARE(tree->topLevelItem(1)->text(2),
             directory.filePath(QStringLiteral("poly-2.poly")));
    auto *buttons = confirmation->findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    QVERIFY(buttons->button(QDialogButtonBox::Cancel)->isDefault());
    QVERIFY(!buttons->button(QDialogButtonBox::Yes)->isDefault());
    QCOMPARE(buttons->button(QDialogButtonBox::Yes)->objectName(),
             QStringLiteral("DeveloperShapefilePolyConfirmButton"));
    QTest::keyClick(confirmation, Qt::Key_Escape);
    QTRY_VERIFY(!controller->busy());
    QCOMPARE(readFile(first), QByteArray("OLD"));
    QVERIFY(!QFileInfo::exists(
        directory.filePath(QStringLiteral("poly-2.poly"))));
}

void ShapefilePolyControllerTest::confirmedConversionWritesExactPlannedFiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("points.shp"));
    QVERIFY(writeFile(input, pointShapefile({QPointF(1.25, 2.5),
                                             QPointF(-4.0, 8.0)})));
    const QString first = directory.filePath(QStringLiteral("poly-1.poly"));
    const QString second = directory.filePath(QStringLiteral("poly-2.poly"));
    QVERIFY(writeFile(first, QByteArray("OLD")));
    QWidget owner;
    owner.show();
    auto *controller = new ShapefilePolyController(&owner);
    QSignalSpy logSpy(controller, &ShapefilePolyController::logMessage);

    QDialog *confirmation = prepareConfirmation(&owner, controller, input);
    QVERIFY(confirmation);
    auto *confirm = confirmation->findChild<QPushButton *>(
        QStringLiteral("DeveloperShapefilePolyConfirmButton"));
    QVERIFY(confirm);
    confirm->click();
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    QVERIFY(QFileInfo::exists(first));
    QVERIFY(QFileInfo::exists(second));
    QCOMPARE(readFile(first),
             QByteArray("#Shap to Poly - Mission Planner\r\n"
                        "2.5\t1.25\r\n"));
    QCOMPARE(readFile(second),
             QByteArray("#Shap to Poly - Mission Planner\r\n"
                        "8\t-4\r\n"));
    QVERIFY(anyLogContains(logSpy, QStringLiteral("wrote 2 file")));
    QVERIFY(anyLogContains(logSpy, QStringLiteral("2 point")));
    QVERIFY(anyLogContains(logSpy, first));
}

void ShapefilePolyControllerTest::emptyValidShapefileNeedsNoConsentOrOutput()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("empty.shp"));
    QVERIFY(writeFile(input, pointShapefile({})));
    QWidget owner;
    owner.show();
    auto *controller = new ShapefilePolyController(&owner);
    QSignalSpy logSpy(controller, &ShapefilePolyController::logMessage);

    QFileDialog *picker = openPicker(&owner, controller);
    QVERIFY(acceptInput(picker, input));
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    QVERIFY(!visible<QDialog>(
        &owner, "DeveloperShapefilePolyConfirmation"));
    QVERIFY(anyLogContains(logSpy, QStringLiteral("no non-empty geometry")));
    QCOMPARE(QDir(directory.path()).entryList(
        QStringList{QStringLiteral("*.poly")}, QDir::Files), QStringList());
}

void ShapefilePolyControllerTest::preparationCancelButtonStaysWaitingAndEscapeNeverReopens()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("held.shp"));
    QVERIFY(writeFile(input, pointShapefile({QPointF(1, 2)})));
    for (bool escape : {false, true}) {
        QWidget owner;
        owner.show();
        auto *controller = new ShapefilePolyController(&owner);
        const auto wait = std::make_shared<WaitState>();
        const WaitRelease release{wait};
        Q_UNUSED(release);
        controller->setBackend(
            [wait](const QString &, const Shapefile::Cancel &cancel,
                   const Shapefile::Progress &progress) {
                return waitForPreparationCancel(wait, cancel, progress);
            }, [](const ShapefileImportService::Plan &,
                  const Shapefile::Cancel &,
                  const Shapefile::Progress &) {
                return ShapefileImportService::Result();
            });
        QFileDialog *picker = openPicker(&owner, controller);
        QVERIFY(acceptInput(picker, input));
        QProgressDialog *progress = waitVisible<QProgressDialog>(
            &owner, "DeveloperShapefilePolyPrepareProgressDialog");
        QVERIFY(progress);
        QTRY_VERIFY_WITH_TIMEOUT(
            wait->entered.load(std::memory_order_acquire), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(progress->value() > 0, 3000);
        if (escape) {
            QTest::keyClick(progress, Qt::Key_Escape);
        } else {
            QPushButton *cancelButton = progress->findChild<QPushButton *>();
            QVERIFY(cancelButton);
            cancelButton->click();
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            wait->cancelObserved.load(std::memory_order_acquire), 3000);
        QVERIFY(controller->busy());
        // Qt5 may route Escape through the Cancel shortcut (remaining
        // visible) or QDialog::reject (hidden). Either route must cancel,
        // and a hidden dialog must never be revived by later progress.
        const bool visibleAfterCancel = progress->isVisible();
        if (!escape)
            QVERIFY(visibleAfterCancel);
        const QString waitingText = progress->labelText();
        QVERIFY(waitingText.contains(QStringLiteral("Cancellation requested")));
        QTest::qWait(250);
        QCOMPARE(progress->isVisible(), visibleAfterCancel);
        QCOMPARE(progress->labelText(), waitingText);
        wait->release.store(true, std::memory_order_release);
        QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
        QCOMPARE(QDir(directory.path()).entryList(
            QStringList{QStringLiteral("*.poly")}, QDir::Files), QStringList());
    }
}

void ShapefilePolyControllerTest::lateSuccessfulPreparationCannotBypassCancellation()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("late-success.shp"));
    QVERIFY(writeFile(input, pointShapefile({QPointF(1, 2)})));
    const ShapefileImportService::Preparation successful =
        ShapefileImportService::prepare(input);
    QVERIFY2(successful.plan.isValid(), qPrintable(successful.error));

    QWidget owner;
    owner.show();
    auto *controller = new ShapefilePolyController(&owner);
    const auto wait = std::make_shared<WaitState>();
    const WaitRelease release{wait};
    Q_UNUSED(release);
    controller->setBackend(
        [wait, successful](const QString &, const Shapefile::Cancel &cancel,
                           const Shapefile::Progress &progress) {
            if (progress)
                progress(1, 2, QStringLiteral("Finishing held preparation"));
            wait->entered.store(true, std::memory_order_release);
            QElapsedTimer deadline;
            deadline.start();
            while (deadline.elapsed() < 10000) {
                if (cancel && cancel()) {
                    wait->cancelObserved.store(true, std::memory_order_release);
                    while (!wait->release.load(std::memory_order_acquire)
                           && deadline.elapsed() < 10000) {
                        QThread::msleep(2);
                    }
                    wait->exited.store(true, std::memory_order_release);
                    // Deliberately emulate a backend whose final successful
                    // result raced with the controller cancellation.
                    return successful;
                }
                QThread::msleep(2);
            }
            ShapefileImportService::Preparation failed;
            failed.error = QStringLiteral("late-success fixture timed out");
            wait->exited.store(true, std::memory_order_release);
            return failed;
        }, [](const ShapefileImportService::Plan &,
              const Shapefile::Cancel &, const Shapefile::Progress &) {
            return ShapefileImportService::Result();
        });
    QSignalSpy logSpy(controller, &ShapefilePolyController::logMessage);

    QVERIFY(acceptInput(openPicker(&owner, controller), input));
    QTRY_VERIFY_WITH_TIMEOUT(
        wait->entered.load(std::memory_order_acquire), 3000);
    controller->cancel();
    QTRY_VERIFY_WITH_TIMEOUT(
        wait->cancelObserved.load(std::memory_order_acquire), 3000);
    wait->release.store(true, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    QVERIFY(!visible<QDialog>(
        &owner, "DeveloperShapefilePolyConfirmation"));
    QVERIFY(anyLogContains(logSpy, QStringLiteral("preparation cancelled")));
    QCOMPARE(QDir(directory.path()).entryList(
        QStringList{QStringLiteral("*.poly")}, QDir::Files), QStringList());
}

void ShapefilePolyControllerTest::exportCancellationReportsActualPartialFiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("partial.shp"));
    QVERIFY(writeFile(input, pointShapefile({QPointF(1, 2), QPointF(3, 4)})));
    const auto wait = std::make_shared<WaitState>();
    const WaitRelease release{wait};
    Q_UNUSED(release);
    QWidget owner;
    owner.show();
    auto *controller = new ShapefilePolyController(&owner);
    controller->setBackend(realPrepare(),
        [wait](const ShapefileImportService::Plan &plan,
               const Shapefile::Cancel &cancel,
               const Shapefile::Progress &progress) {
        ShapefileImportService::Result result;
        const auto outputs = plan.outputs();
        if (outputs.isEmpty()) {
            result.error = QStringLiteral("fixture plan has no output");
            return result;
        }
        QFile first(outputs.first().path);
        if (!first.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || first.write("PARTIAL") != 7) {
            result.error = QStringLiteral("fixture output write failed");
            return result;
        }
        first.close();
        result.files.append(outputs.first().path);
        result.pointCount = outputs.first().pointCount;
        result.projectionName = plan.projectionName();
        if (progress)
            progress(1, outputs.size(), QStringLiteral("Published first exact output"));
        wait->entered.store(true, std::memory_order_release);
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < 10000) {
            if (cancel && cancel()) {
                wait->cancelObserved.store(true, std::memory_order_release);
                QElapsedTimer drain;
                drain.start();
                while (!wait->release.load(std::memory_order_acquire)
                       && drain.elapsed() < 5000) {
                    QThread::msleep(2);
                }
                result.cancelled = true;
                result.error = QStringLiteral("fixture export cancelled");
                wait->exited.store(true, std::memory_order_release);
                return result;
            }
            QThread::msleep(2);
        }
        result.error = QStringLiteral("fixture export deadline expired");
        wait->exited.store(true, std::memory_order_release);
        return result;
    });
    QSignalSpy logSpy(controller, &ShapefilePolyController::logMessage);

    QDialog *confirmation = prepareConfirmation(&owner, controller, input);
    QVERIFY(confirmation);
    auto *confirm = confirmation->findChild<QPushButton *>(
        QStringLiteral("DeveloperShapefilePolyConfirmButton"));
    QVERIFY(confirm);
    confirm->click();
    QTRY_VERIFY_WITH_TIMEOUT(wait->entered.load(std::memory_order_acquire), 3000);
    controller->cancel();
    QTRY_VERIFY_WITH_TIMEOUT(
        wait->cancelObserved.load(std::memory_order_acquire), 3000);
    wait->release.store(true, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(!controller->busy(), 5000);
    const QString first = directory.filePath(QStringLiteral("poly-1.poly"));
    QCOMPARE(readFile(first), QByteArray("PARTIAL"));
    QVERIFY(!QFileInfo::exists(
        directory.filePath(QStringLiteral("poly-2.poly"))));
    QVERIFY(anyLogContains(logSpy, QStringLiteral("cancelled after publishing 1")));
    QVERIFY(anyLogContains(logSpy, first));
}

void ShapefilePolyControllerTest::closeAndDestructionCancelWorkerWithoutOutputs()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("close.shp"));
    QVERIFY(writeFile(input, pointShapefile({QPointF(1, 2)})));
    const auto wait = std::make_shared<WaitState>();
    const WaitRelease release{wait};
    Q_UNUSED(release);
    QPointer<QWidget> owner = new QWidget;
    owner->show();
    auto *controller = new ShapefilePolyController(owner);
    controller->setBackend(
        [wait](const QString &, const Shapefile::Cancel &cancel,
               const Shapefile::Progress &progress) {
            return waitForPreparationCancel(wait, cancel, progress);
        }, [](const ShapefileImportService::Plan &,
              const Shapefile::Cancel &,
              const Shapefile::Progress &) {
            return ShapefileImportService::Result();
        });
    QVERIFY(acceptInput(openPicker(owner, controller), input));
    QTRY_VERIFY_WITH_TIMEOUT(wait->entered.load(std::memory_order_acquire), 3000);
    delete owner;
    QVERIFY(owner.isNull());
    QTRY_VERIFY_WITH_TIMEOUT(
        wait->cancelObserved.load(std::memory_order_acquire), 3000);
    wait->release.store(true, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(wait->exited.load(std::memory_order_acquire), 5000);
    QCOMPARE(QDir(directory.path()).entryList(
        QStringList{QStringLiteral("*.poly")}, QDir::Files), QStringList());
}

void ShapefilePolyControllerTest::backendAndBusyCallbacksAreLifetimeSafe()
{
    QPointer<QWidget> owner = new QWidget;
    owner->show();
    auto *controller = new ShapefilePolyController(owner);
    connect(controller, &ShapefilePolyController::busyChanged,
            owner, [owner](bool busy) {
        if (busy && owner)
            delete owner;
    }, Qt::DirectConnection);
    controller->start();
    QVERIFY(owner.isNull());

    QWidget secondOwner;
    auto *second = new ShapefilePolyController(&secondOwner);
    QSignalSpy logSpy(second, &ShapefilePolyController::logMessage);
    second->setBackend(ShapefilePolyController::Prepare(),
                       ShapefilePolyController::Export());
    second->start();
    QVERIFY(!second->busy());
    QVERIFY(anyLogContains(logSpy, QStringLiteral("backend is unavailable")));
    second->cancel();
    QVERIFY(anyLogContains(logSpy, QStringLiteral("no conversion")));
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    ShapefilePolyControllerTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_shapefilepolycontroller.moc"
