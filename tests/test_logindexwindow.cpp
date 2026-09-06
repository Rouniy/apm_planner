#include "ui/LogIndexWindow.h"

#include <QtTest>

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <memory>

namespace
{
template<typename T>
T *control(QWidget *window, const char *name)
{
    T *result = window->findChild<T *>(QString::fromLatin1(name));
    Q_ASSERT(result);
    return result;
}

template<typename T>
T *visibleDialog(QWidget *window, const char *name)
{
    const auto dialogs = window->findChildren<T *>(
        QString::fromLatin1(name), Qt::FindChildrenRecursively);
    for (T *dialog : dialogs) {
        if (dialog->isVisible())
            return dialog;
    }
    return nullptr;
}

QString writeLog(const QString &path, int bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    const QByteArray contents(qMax(1, bytes), 'x');
    if (file.write(contents) != contents.size())
        return {};
    file.close();
    return path;
}

void selectAllRows(QTableView *table)
{
    table->selectionModel()->clearSelection();
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        table->selectionModel()->select(
            table->model()->index(row, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
}
} // namespace

class LogIndexWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void matchesMissionPlannerSurface();
    void scansBothDefaultsSortsSelectsAndOpens();
    void customDirectoryPickerIsOwnedAndNonDestructive();
    void deleteIsDefaultCancelThenPublishesExactResult();
    void changedFileProducesTruthfulPartialDeletion();
    void closeWhileScanningCancelsAtWorkerBoundary();
    void idleCloseMarksTerminalSingletonState();
    void tileReaderFactoryMayDeleteWindowBeforeAdmission();
    void proxyLayoutChangeMayCloseWindowAtCompletion();
};

void LogIndexWindowTest::matchesMissionPlannerSurface()
{
    QPointer<LogIndexWindow> window = new LogIndexWindow;
    QCOMPARE(window->objectName(), QStringLiteral("LogIndexWindow"));
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QCOMPARE(window->size(), QSize(1380, 760));
    QCOMPARE(window->minimumSize(), QSize(900, 520));
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QVERIFY(!window->busy());
    QVERIFY(!window->isClosing());

    auto *directory = control<QLineEdit>(window, "LogIndexDirectory");
    auto *table = control<QTableView>(window, "LogGrid");
    QVERIFY(directory->isReadOnly());
    QCOMPARE(table->model()->columnCount(), 13);
    QCOMPARE(table->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(table->selectionBehavior(), QAbstractItemView::SelectRows);
    QCOMPARE(table->verticalHeader()->defaultSectionSize(), 78);
    const QStringList headers = {
        QStringLiteral("Map"), QStringLiteral("Date"),
        QStringLiteral("Directory"), QStringLiteral("Frame"),
        QStringLiteral("Aircraft / sysid"), QStringLiteral("Duration"),
        QStringLiteral("Name"), QStringLiteral("Size"),
        QStringLiteral("Home"), QStringLiteral("Time in air"),
        QStringLiteral("Distance"), QStringLiteral("CAM"),
        QStringLiteral("Read warning")};
    for (int column = 0; column < headers.size(); ++column) {
        QCOMPARE(table->model()->headerData(
                     column, Qt::Horizontal, Qt::DisplayRole).toString(),
                 headers.at(column));
    }
    QVERIFY(!control<QPushButton>(window, "DefaultDirectoryButton")->isEnabled());
    QVERIFY(!control<QPushButton>(window, "DefaultTlogDirectoryButton")->isEnabled());
    QVERIFY(!control<QPushButton>(window, "RefreshButton")->isEnabled());
    QVERIFY(!control<QPushButton>(window, "CancelButton")->isEnabled());
    QVERIFY(!control<QPushButton>(window, "DeleteButton")->isEnabled());
    QCOMPARE(control<QProgressBar>(window, "LogIndexProgressBar")->maximum(), 1);

    delete window;
    QVERIFY(window.isNull());
}

void LogIndexWindowTest::scansBothDefaultsSortsSelectsAndOpens()
{
    QTemporaryDir dataflash;
    QTemporaryDir telemetry;
    QVERIFY(dataflash.isValid());
    QVERIFY(telemetry.isValid());
    const QString small = writeLog(dataflash.filePath(QStringLiteral("small.log")), 17);
    const QString large = writeLog(dataflash.filePath(QStringLiteral("large.bin")), 2048);
    const QString tlog = writeLog(telemetry.filePath(QStringLiteral("flight.tlog")), 31);
    QVERIFY(!small.isEmpty());
    QVERIFY(!large.isEmpty());
    QVERIFY(!tlog.isEmpty());

    bool factoryOnGuiThread = false;
    int factoryCalls = 0;
    LogIndexWindow window;
    window.setDefaultDirectories(dataflash.path(), telemetry.path());
    window.setTileReaderFactory([&]() {
        ++factoryCalls;
        factoryOnGuiThread = QThread::currentThread()
            == QCoreApplication::instance()->thread();
        return LogIndex::TileReader();
    });
    QSignalSpy opened(&window, &LogIndexWindow::openLogRequested);
    window.show();
    QTRY_VERIFY(!window.busy());
    QCOMPARE(window.currentRoot(), QFileInfo(dataflash.path()).absoluteFilePath());
    QVERIFY(factoryOnGuiThread);
    QCOMPARE(factoryCalls, 1);

    auto *table = control<QTableView>(&window, "LogGrid");
    QCOMPARE(table->model()->rowCount(), 2);
    table->sortByColumn(7, Qt::AscendingOrder);
    QTRY_COMPARE(table->model()->index(0, 7).data().toString(),
                 QStringLiteral("17 B"));

    selectAllRows(table);
    QTRY_VERIFY(control<QLabel>(&window, "LogIndexSelectedSummary")
                    ->text().contains(QStringLiteral("Selected: 2")));
    QVERIFY(control<QPushButton>(&window, "DeleteButton")->isEnabled());

    QVERIFY(QMetaObject::invokeMethod(
        table, "doubleClicked", Qt::DirectConnection,
        Q_ARG(QModelIndex, QModelIndex())));
    QCOMPARE(opened.size(), 0);
    const QModelIndex first = table->model()->index(0, 0);
    QVERIFY(QMetaObject::invokeMethod(
        table, "doubleClicked", Qt::DirectConnection,
        Q_ARG(QModelIndex, first)));
    QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.takeFirst().at(0).toString(), small);

    control<QPushButton>(&window, "DefaultTlogDirectoryButton")->click();
    QTRY_VERIFY(!window.busy());
    QCOMPARE(window.currentRoot(), QFileInfo(telemetry.path()).absoluteFilePath());
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(factoryCalls, 2);
}

void LogIndexWindowTest::customDirectoryPickerIsOwnedAndNonDestructive()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LogIndexWindow window;
    window.show();
    control<QPushButton>(&window, "ChooseDirectoryButton")->click();

    auto *dialog = visibleDialog<QFileDialog>(
        &window, "LogIndexCustomDirectoryDialog");
    QVERIFY(dialog);
    QCOMPARE(dialog->fileMode(), QFileDialog::Directory);
    QVERIFY(dialog->testOption(QFileDialog::ShowDirsOnly));
    QCOMPARE(dialog->acceptMode(), QFileDialog::AcceptOpen);
    QVERIFY(window.busy());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject", Qt::DirectConnection));
    QTRY_VERIFY(!window.busy());
    QVERIFY(window.currentRoot().isEmpty());
}

void LogIndexWindowTest::deleteIsDefaultCancelThenPublishesExactResult()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString log = writeLog(directory.filePath(QStringLiteral("delete.log")), 64);
    QVERIFY(!log.isEmpty());
    LogIndexWindow window;
    window.show();
    window.scanDirectory(directory.path());
    QTRY_VERIFY(!window.busy());

    auto *table = control<QTableView>(&window, "LogGrid");
    QCOMPARE(table->model()->rowCount(), 1);
    selectAllRows(table);
    control<QPushButton>(&window, "DeleteButton")->click();
    QTRY_VERIFY(visibleDialog<QDialog>(
        &window, "LogIndexDeleteConfirmation"));
    auto *confirm = visibleDialog<QDialog>(
        &window, "LogIndexDeleteConfirmation");
    auto *buttons = confirm->findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    const auto labels = confirm->findChildren<QLabel *>();
    QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](const QLabel *label) {
        return label->text().contains(QStringLiteral("not locked"));
    }));
    QVERIFY(buttons->button(QDialogButtonBox::Cancel)->isDefault());
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isDefault());
    const QString exactPaths = control<QPlainTextEdit>(
        confirm, "LogIndexDeletePaths")->toPlainText();
    QVERIFY(exactPaths.split(QLatin1Char('\n')).contains(log));
    buttons->button(QDialogButtonBox::Cancel)->click();
    QTRY_VERIFY(!window.busy());
    QVERIFY(QFileInfo::exists(log));

    selectAllRows(table);
    control<QPushButton>(&window, "DeleteButton")->click();
    QTRY_VERIFY(visibleDialog<QDialog>(
        &window, "LogIndexDeleteConfirmation"));
    confirm = visibleDialog<QDialog>(&window, "LogIndexDeleteConfirmation");
    QVERIFY(QMetaObject::invokeMethod(confirm, "accept", Qt::DirectConnection));
    QTRY_VERIFY(!window.busy());
    QVERIFY(!QFileInfo::exists(log));
    QCOMPARE(table->model()->rowCount(), 0);
    QVERIFY(control<QLabel>(&window, "LogIndexStatus")->text().contains(
        QStringLiteral("Deleted 1 log")));
}

void LogIndexWindowTest::changedFileProducesTruthfulPartialDeletion()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString first = writeLog(directory.filePath(QStringLiteral("first.log")), 61);
    const QString second = writeLog(directory.filePath(QStringLiteral("second.log")), 62);
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());
    LogIndexWindow window;
    window.show();
    window.scanDirectory(directory.path());
    QTRY_VERIFY(!window.busy());
    auto *table = control<QTableView>(&window, "LogGrid");
    QCOMPARE(table->model()->rowCount(), 2);
    selectAllRows(table);
    control<QPushButton>(&window, "DeleteButton")->click();
    QTRY_VERIFY(visibleDialog<QDialog>(
        &window, "LogIndexDeleteConfirmation"));
    auto *confirm = visibleDialog<QDialog>(
        &window, "LogIndexDeleteConfirmation");
    const QStringList planned = control<QPlainTextEdit>(
        confirm, "LogIndexDeletePaths")->toPlainText()
            .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QStringList plannedLogs;
    for (const QString &path : planned) {
        if (path.endsWith(QStringLiteral(".log"), Qt::CaseInsensitive))
            plannedLogs.append(path);
    }
    QCOMPARE(plannedLogs.size(), 2);
    QFile changed(plannedLogs.constLast());
    QVERIFY(changed.open(QIODevice::Append));
    QCOMPARE(changed.write("!", 1), qint64(1));
    changed.close();
    QVERIFY(QMetaObject::invokeMethod(confirm, "accept", Qt::DirectConnection));
    QTRY_VERIFY(!window.busy());
    QVERIFY(!QFileInfo::exists(plannedLogs.constFirst()));
    QVERIFY(QFileInfo::exists(plannedLogs.constLast()));
    QCOMPARE(table->model()->rowCount(), 1);
    const QString status = control<QLabel>(&window, "LogIndexStatus")->text();
    QVERIFY(status.contains(QStringLiteral("partially")));
    QVERIFY(status.contains(QStringLiteral("1 remain")));
}

void LogIndexWindowTest::closeWhileScanningCancelsAtWorkerBoundary()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (int index = 0; index < 16; ++index) {
        QVERIFY(!writeLog(directory.filePath(
            QStringLiteral("flight-%1.log").arg(index, 3, 10,
                                                 QLatin1Char('0'))),
            128).isEmpty());
    }
    QPointer<LogIndexWindow> window = new LogIndexWindow;
    window->show();
    window->scanDirectory(directory.path());
    QVERIFY(window->busy());
    window->close();
    auto *confirm = visibleDialog<QMessageBox>(
        window, "LogIndexBusyCloseConfirmation");
    QVERIFY(confirm);
    QCOMPARE(confirm->defaultButton(), confirm->button(QMessageBox::Cancel));
    QCOMPARE(confirm->escapeButton(), confirm->button(QMessageBox::Cancel));
    QVERIFY(!window->isClosing());
    confirm->button(QMessageBox::Yes)->click();
    QTRY_VERIFY(window.isNull());
}

void LogIndexWindowTest::idleCloseMarksTerminalSingletonState()
{
    QPointer<LogIndexWindow> window = new LogIndexWindow;
    window->show();
    window->close();
    QVERIFY(window);
    QVERIFY(window->isClosing());
    QCoreApplication::sendPostedEvents(window, QEvent::DeferredDelete);
    QVERIFY(window.isNull());
}

void LogIndexWindowTest::tileReaderFactoryMayDeleteWindowBeforeAdmission()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(!writeLog(directory.filePath(QStringLiteral("flight.log")), 20)
                 .isEmpty());
    QPointer<LogIndexWindow> window = new LogIndexWindow;
    bool callbackFinished = false;
    auto closureLifetime = std::make_shared<int>(42);
    window->setTileReaderFactory([&window, &callbackFinished,
                                  closureLifetime]() {
        delete window.data();
        callbackFinished = *closureLifetime == 42;
        return LogIndex::TileReader();
    });
    closureLifetime.reset();
    window->scanDirectory(directory.path());
    QVERIFY(window.isNull());
    QVERIFY(callbackFinished);
}

void LogIndexWindowTest::proxyLayoutChangeMayCloseWindowAtCompletion()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(!writeLog(directory.filePath(QStringLiteral("flight.log")), 20)
                 .isEmpty());
    QPointer<LogIndexWindow> window = new LogIndexWindow;
    auto *table = control<QTableView>(window, "LogGrid");
    connect(table->model(), &QAbstractItemModel::layoutChanged, this,
            [&window](const QList<QPersistentModelIndex> &,
                      QAbstractItemModel::LayoutChangeHint) {
        window->close();
    });
    window->show();
    window->scanDirectory(directory.path());
    QTRY_VERIFY(window.isNull());
}

QTEST_MAIN(LogIndexWindowTest)
#include "test_logindexwindow.moc"
