#include "ui/configuration/MavFTPUIView.h"

#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QtTest/QTest>

class FakeMavFtpService final : public MavFtpServiceInterface
{
public:
    explicit FakeMavFtpService(QObject *parent = nullptr)
        : MavFtpServiceInterface(parent)
    {
    }

    bool isBusy() const override { return m_busy; }
    quint64 activeOperationId() const override { return m_busy ? m_id : 0; }
    Operation operation() const override { return m_operation; }
    quint64 activeTargetGeneration() const override
    {
        return m_busy ? m_generation : 0;
    }
    QString lastError() const override { return m_lastError; }
    StartResult startOperation(Operation operation, const QString &path,
                               const QByteArray &data, quint64 *idOut) override
    {
        if (idOut) *idOut = 0;
        if (operation == Operation::ListDirectory) ++listStarts;
        if (operation == Operation::Upload) uploadData = data;
        return start(operation, path, idOut);
    }
    bool cancelOperation(quint64 id) override
    {
        if (!id || id != activeOperationId()) return false;
        cancel(); return true;
    }

    StartResult startList(const QString &path) override
    {
        ++listStarts;
        return start(Operation::ListDirectory, path);
    }
    StartResult startDownload(const QString &path) override
    {
        return start(Operation::Download, path);
    }
    StartResult startUpload(const QString &path,
                            const QByteArray &data) override
    {
        uploadData = data;
        return start(Operation::Upload, path);
    }
    StartResult startMakeDirectory(const QString &path) override
    {
        return start(Operation::MakeDirectory, path);
    }
    StartResult startRemoveFile(const QString &path) override
    {
        return start(Operation::RemoveFile, path);
    }
    StartResult startRemoveDirectory(const QString &path) override
    {
        return start(Operation::RemoveDirectory, path);
    }

    void cancel() override
    {
        ++cancelCalls;
        if (!m_busy) {
            return;
        }
        Result result;
        result.operation = m_operation;
        result.targetGeneration = m_generation;
        result.remotePath = activePath;
        result.cancelled = true;
        result.error = QStringLiteral("Cancelled by test");
        finish(result);
    }

    StartResult start(Operation requested, const QString &path, quint64 *idOut = nullptr)
    {
        if (m_busy) {
            return StartResult::Busy;
        }
        const StartResult result = nextStartResult;
        nextStartResult = StartResult::Started;
        if (result != StartResult::Started) {
            m_lastError = QStringLiteral("start rejected");
            return result;
        }
        m_busy = true;
        m_id = ++m_nextId;
        if (idOut) *idOut = m_id;
        m_operation = requested;
        activePath = path;
        emit operationStarted(requested, m_generation, path);
        emit stateChanged();
        return result;
    }

    void completeList(
        const QVector<MavFtpProtocol::DirectoryEntry> &entries,
        const QString &error = QString())
    {
        Result result;
        result.operation = Operation::ListDirectory;
        result.targetGeneration = m_generation;
        result.remotePath = activePath;
        result.entries = entries;
        result.error = error;
        finish(result);
    }

    void completeSuccess()
    {
        Result result;
        result.operation = m_operation;
        result.targetGeneration = m_generation;
        result.remotePath = activePath;
        finish(result);
    }

    void emitLateError(const QString &path, const QString &error)
    {
        Result result;
        result.operation = Operation::ListDirectory;
        result.targetGeneration = m_generation;
        result.remotePath = path;
        result.error = error;
        emit operationFinished(result);
    }

    void reportProgress(qint64 completed, qint64 total)
    {
        emit operationProgress(m_id, m_generation, completed, total);
        emit progressChanged(m_generation, completed, total);
    }

    void finish(Result result)
    {
        result.operationId = m_id;
        m_busy = false;
        m_operation = Operation::None;
        activePath.clear();
        emit stateChanged();
        emit operationFinished(result);
    }

    StartResult nextStartResult = StartResult::Started;
    QString activePath;
    QByteArray uploadData;
    int listStarts = 0;
    int cancelCalls = 0;

private:
    bool m_busy = false;
    Operation m_operation = Operation::None;
    quint64 m_generation = 17;
    QString m_lastError;
    quint64 m_id = 0, m_nextId = 0;
};

namespace {

MavFtpProtocol::DirectoryEntry directory(const QString &name)
{
    MavFtpProtocol::DirectoryEntry entry;
    entry.type = MavFtpProtocol::DirectoryEntryType::Directory;
    entry.name = name;
    entry.typeTag = 'D';
    return entry;
}

MavFtpProtocol::DirectoryEntry file(const QString &name, quint64 size)
{
    MavFtpProtocol::DirectoryEntry entry;
    entry.type = MavFtpProtocol::DirectoryEntryType::File;
    entry.name = name;
    entry.size = size;
    entry.typeTag = 'F';
    return entry;
}

QPushButton *button(MavFTPUIView &view, const char *name)
{
    QPushButton *const result = view.findChild<QPushButton *>(
        QString::fromLatin1(name));
    Q_ASSERT(result);
    return result;
}

} // namespace

class MavFTPUIViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void surfaceIsConcreteAndComplete();
    void selectionAndBusyStateAreTruthful();
    void systemRootFallbackIsDeduplicated();
    void lazyDirectoryAndMutationRefresh();
    void errorsCancellationAndLateResultsAreBounded();
    void ownershipRejectsForeignReplacementAndStaleSamePath();
    void admissionAndCancellationAllowBrowserDestruction();
};

void MavFTPUIViewTest::surfaceIsConcreteAndComplete()
{
    FakeMavFtpService service;
    MavFTPUIView view(&service);

    QCOMPARE(view.objectName(), QStringLiteral("MavFTPUIView"));
    auto *title = view.findChild<QLabel *>(QStringLiteral("MavFtpTitle"));
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("MAVFTP — Remote Files"));
    QVERIFY(button(view, "RefreshButton"));
    QVERIFY(button(view, "DownloadBtn"));
    QVERIFY(button(view, "UploadBtn"));
    QVERIFY(button(view, "DeleteButton"));
    QVERIFY(button(view, "MkdirButton"));
    QVERIFY(button(view, "CancelButton"));
    QVERIFY(view.findChild<QLineEdit *>(QStringLiteral("NewFolderName")));

    auto *tree = view.findChild<QTreeWidget *>(QStringLiteral("DirectoryTree"));
    auto *table = view.findChild<QTableWidget *>(QStringLiteral("EntriesGrid"));
    auto *status = view.findChild<QLabel *>(QStringLiteral("MavFtpStatus"));
    auto *progress = view.findChild<QProgressBar *>(
        QStringLiteral("MavFtpProgress"));
    QVERIFY(tree);
    QVERIFY(table);
    QVERIFY(status);
    QVERIFY(progress);
    QCOMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("/"));
    QCOMPARE(table->columnCount(), 3);
    QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("Name"));
    QCOMPARE(table->horizontalHeaderItem(1)->text(), QStringLiteral("Type"));
    QCOMPARE(table->horizontalHeaderItem(2)->text(), QStringLiteral("Size"));
    QVERIFY(!status->text().isEmpty());

    QVERIFY(button(view, "RefreshButton")->isEnabled());
    QVERIFY(button(view, "UploadBtn")->isEnabled());
    QVERIFY(!button(view, "DownloadBtn")->isEnabled());
    QVERIFY(!button(view, "DeleteButton")->isEnabled());
    QVERIFY(!button(view, "MkdirButton")->isEnabled());
    QVERIFY(!button(view, "CancelButton")->isEnabled());
}

void MavFTPUIViewTest::selectionAndBusyStateAreTruthful()
{
    FakeMavFtpService service;
    MavFTPUIView view(&service);
    auto *table = view.findChild<QTableWidget *>(QStringLiteral("EntriesGrid"));
    auto *progress = view.findChild<QProgressBar *>(
        QStringLiteral("MavFtpProgress"));

    button(view, "RefreshButton")->click();
    QCOMPARE(service.operation(), MavFtpServiceInterface::Operation::ListDirectory);
    QCOMPARE(service.activePath, QStringLiteral("/"));
    QVERIFY(button(view, "CancelButton")->isEnabled());
    QVERIFY(!button(view, "RefreshButton")->isEnabled());
    QVERIFY(!table->isEnabled());
    service.reportProgress(1, 4);
    QCOMPARE(progress->value(), 25);

    service.completeList({directory(QStringLiteral("Logs")),
                          file(QStringLiteral("flight.bin"), 1234)});
    QCOMPARE(table->rowCount(), 2);
    QVERIFY(table->isEnabled());
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("Directory"));
    QCOMPARE(table->item(1, 1)->text(), QStringLiteral("File"));
    QCOMPARE(table->item(1, 2)->text(), QStringLiteral("1234"));

    table->selectRow(0);
    QVERIFY(!button(view, "DownloadBtn")->isEnabled());
    QVERIFY(button(view, "DeleteButton")->isEnabled());
    table->selectRow(1);
    QVERIFY(button(view, "DownloadBtn")->isEnabled());
    QVERIFY(button(view, "DeleteButton")->isEnabled());
}

void MavFTPUIViewTest::systemRootFallbackIsDeduplicated()
{
    FakeMavFtpService service;
    MavFTPUIView view(&service);
    auto *tree = view.findChild<QTreeWidget *>(QStringLiteral("DirectoryTree"));

    button(view, "RefreshButton")->click();
    service.completeList({directory(QStringLiteral("APM"))});
    QCOMPARE(tree->topLevelItemCount(), 2);
    QCOMPARE(tree->topLevelItem(1)->text(0), QStringLiteral("@SYS"));

    button(view, "RefreshButton")->click();
    service.completeList({directory(QStringLiteral("@sys")),
                          directory(QStringLiteral("APM"))});
    QCOMPARE(tree->topLevelItemCount(), 1);
    QTreeWidgetItem *const root = tree->topLevelItem(0);
    QCOMPARE(root->childCount(), 2);
    QCOMPARE(root->child(0)->text(0), QStringLiteral("@sys"));

    QVERIFY(!MavFTPUIView::ShouldAddSystemRoot(
        {QStringLiteral("logs"), QStringLiteral("@SYS")}));
    QVERIFY(MavFTPUIView::ShouldAddSystemRoot(
        {QStringLiteral("logs"), QStringLiteral("APM")}));
}

void MavFTPUIViewTest::lazyDirectoryAndMutationRefresh()
{
    FakeMavFtpService service;
    MavFTPUIView view(&service);
    auto *tree = view.findChild<QTreeWidget *>(QStringLiteral("DirectoryTree"));
    auto *table = view.findChild<QTableWidget *>(QStringLiteral("EntriesGrid"));
    auto *folder = view.findChild<QLineEdit *>(QStringLiteral("NewFolderName"));

    button(view, "RefreshButton")->click();
    service.completeList({directory(QStringLiteral("Logs"))});
    QTreeWidgetItem *const logs = tree->topLevelItem(0)->child(0);
    tree->setCurrentItem(logs);
    QCOMPARE(service.activePath, QStringLiteral("/Logs"));
    service.completeList({directory(QStringLiteral("Nested")),
                          file(QStringLiteral("log.bin"), 9)});
    QCOMPARE(table->rowCount(), 2);

    table->selectRow(0);
    QVERIFY(QMetaObject::invokeMethod(
        table, "itemDoubleClicked", Qt::DirectConnection,
        Q_ARG(QTableWidgetItem *, table->item(0, 0))));
    QCOMPARE(service.activePath, QStringLiteral("/Logs/Nested"));
    service.completeList({});
    QCOMPARE(tree->currentItem()->text(0), QStringLiteral("Nested"));

    folder->setText(QStringLiteral(".."));
    QVERIFY(!button(view, "MkdirButton")->isEnabled());
    folder->setText(QString(240, QLatin1Char('x')));
    QVERIFY(!button(view, "MkdirButton")->isEnabled());
    const int selectedPrefixBytes = QByteArrayLiteral("/Logs/Nested/").size();
    folder->setText(QString(
        MavFtpProtocol::MaximumPathBytes - selectedPrefixBytes,
        QLatin1Char('x')));
    QVERIFY(button(view, "MkdirButton")->isEnabled());
    folder->setText(QString(
        MavFtpProtocol::MaximumPathBytes - selectedPrefixBytes + 1,
        QLatin1Char('x')));
    QVERIFY(!button(view, "MkdirButton")->isEnabled());
    folder->setText(QStringLiteral("Archive"));
    QVERIFY(button(view, "MkdirButton")->isEnabled());
    const int listsBeforeMutation = service.listStarts;
    button(view, "MkdirButton")->click();
    QCOMPARE(service.operation(), MavFtpServiceInterface::Operation::MakeDirectory);
    QCOMPARE(service.activePath, QStringLiteral("/Logs/Nested/Archive"));
    service.completeSuccess();
    QCoreApplication::processEvents();
    QCOMPARE(folder->text(), QString());
    QCOMPARE(service.listStarts, listsBeforeMutation + 1);
    QCOMPARE(service.activePath, QStringLiteral("/Logs/Nested"));
}

void MavFTPUIViewTest::errorsCancellationAndLateResultsAreBounded()
{
    FakeMavFtpService service;
    MavFTPUIView view(&service);
    auto *status = view.findChild<QLabel *>(QStringLiteral("MavFtpStatus"));

    service.nextStartResult = MavFtpServiceInterface::StartResult::NoTarget;
    button(view, "RefreshButton")->click();
    QVERIFY(status->text().contains(QStringLiteral("No current exact")));
    QVERIFY(button(view, "RefreshButton")->isEnabled());

    button(view, "RefreshButton")->click();
    QVERIFY(button(view, "CancelButton")->isEnabled());
    button(view, "CancelButton")->click();
    QCOMPARE(service.cancelCalls, 1);
    QCOMPARE(status->text(), QStringLiteral("Cancelled by test"));
    QVERIFY(button(view, "RefreshButton")->isEnabled());
    const QString cancelledStatus = status->text();
    service.emitLateError(QStringLiteral("/"), QStringLiteral("late error"));
    QCOMPARE(status->text(), cancelledStatus);

    button(view, "RefreshButton")->click();
    service.completeList({}, QStringLiteral("List failed: malformed packet"));
    QCOMPARE(status->text(), QStringLiteral("List failed: malformed packet"));
    const QString errorStatus = status->text();
    service.emitLateError(QStringLiteral("/"), QStringLiteral("later duplicate"));
    QCOMPARE(status->text(), errorStatus);
}

void MavFTPUIViewTest::ownershipRejectsForeignReplacementAndStaleSamePath()
{
    FakeMavFtpService service;
    MavFTPUIView view(&service);
    auto *status = view.findChild<QLabel *>(QStringLiteral("MavFtpStatus"));
    auto *progress = view.findChild<QProgressBar *>(QStringLiteral("MavFtpProgress"));
    auto *table = view.findChild<QTableWidget *>(QStringLiteral("EntriesGrid"));
    button(view, "RefreshButton")->click();
    const quint64 first = service.activeOperationId();
    service.reportProgress(20, 100); QCOMPARE(progress->value(), 20);
    quint64 foreign = 0;
    const auto connection = connect(&service, &MavFtpServiceInterface::stateChanged, &view, [&] {
        if (!service.isBusy() && !foreign) {
            QCOMPARE(service.startOperation(MavFtpServiceInterface::Operation::ListDirectory,
                                            "/", {}, &foreign),
                     MavFtpServiceInterface::StartResult::Started);
            QVERIFY(foreign != first);
            QVERIFY(!button(view, "CancelButton")->isEnabled());
            button(view, "CancelButton")->click();
            service.reportProgress(95, 100);
        }
    });
    button(view, "CancelButton")->click();
    QCOMPARE(service.cancelCalls, 1); QCOMPARE(service.activeOperationId(), foreign);
    QCOMPARE(status->text(), QStringLiteral("Cancelled by test"));
    QCOMPARE(progress->value(), 0);
    disconnect(connection);
    service.completeList({file("foreign.bin", 99)});
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(status->text(), QStringLiteral("Cancelled by test"));

    button(view, "RefreshButton")->click();
    const quint64 current = service.activeOperationId(); QVERIFY(current != first);
    service.reportProgress(30, 100);
    MavFtpServiceInterface::Result stale;
    stale.operation = MavFtpServiceInterface::Operation::ListDirectory;
    stale.operationId = first; stale.targetGeneration = 17; stale.remotePath = "/";
    stale.entries = {file("stale.bin", 42)};
    emit service.operationFinished(stale);
    emit service.operationProgress(first, 17, 99, 100);
    QCOMPARE(progress->value(), 30); QCOMPARE(table->rowCount(), 0);
    QVERIFY(button(view, "CancelButton")->isEnabled());
    service.completeList({file("owned.bin", 2)});
    QCOMPARE(table->rowCount(), 1); QCOMPARE(table->item(0, 0)->text(), QStringLiteral("owned.bin"));
}

void MavFTPUIViewTest::admissionAndCancellationAllowBrowserDestruction()
{
    FakeMavFtpService service;
    auto *view = new MavFTPUIView(&service);
    QPointer<MavFTPUIView> guard(view);
    const auto started = connect(&service, &MavFtpServiceInterface::operationStarted,
                                &service, [&] { delete view; });
    button(*view, "RefreshButton")->click();
    QVERIFY(guard.isNull()); disconnect(started);
    service.cancel();

    view = new MavFTPUIView(&service); guard = view;
    button(*view, "RefreshButton")->click();
    connect(&service, &MavFtpServiceInterface::stateChanged, &service, [&] {
        if (!service.isBusy() && guard) delete view;
    });
    button(*view, "CancelButton")->click();
    QVERIFY(guard.isNull()); QVERIFY(!service.isBusy());
}

QTEST_MAIN(MavFTPUIViewTest)

#include "test_mavftpuiview.moc"
