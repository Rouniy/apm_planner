#include "ui/configuration/MavFtpFileDownload.h"

#include "comm/MavFtpServiceInterface.h"
#include "comm/VehicleTargetManager.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QPointer>
#include <QProgressDialog>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

namespace {
VehicleEndpoint endpoint(int linkId = 7, int systemId = 42)
{
    VehicleEndpoint value;
    value.linkId = linkId;
    value.systemId = systemId;
    value.componentId = 1;
    value.linkName = QStringLiteral("Test TCP");
    value.componentName = QStringLiteral("Autopilot");
    return value;
}

void selectDefaultTarget(VehicleTargetManager *targets)
{
    QVERIFY(targets);
    QVERIFY(targets->observeEndpoint(endpoint(), true));
    QVERIFY(targets->acquireTarget().isValid());
    QVERIFY(targets->isTargetGenerationSettled());
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
} // namespace

class FakeOwnedMavFtpService final : public MavFtpServiceInterface
{
public:
    explicit FakeOwnedMavFtpService(QObject *parent = nullptr)
        : MavFtpServiceInterface(parent)
    {
    }

    bool isBusy() const override { return m_busy; }
    Operation operation() const override
    {
        return m_busy ? Operation::Download : Operation::None;
    }
    quint64 activeTargetGeneration() const override
    {
        return m_busy ? m_target.generation : 0;
    }
    quint64 activeOperationId() const override { return m_busy ? m_id : 0; }
    QString lastError() const override { return QStringLiteral("test start error"); }

    StartResult startList(const QString &) override
    { return StartResult::TransportUnavailable; }
    StartResult startDownload(const QString &) override
    { return StartResult::TransportUnavailable; }
    StartResult startUpload(const QString &, const QByteArray &) override
    { return StartResult::TransportUnavailable; }
    StartResult startMakeDirectory(const QString &) override
    { return StartResult::TransportUnavailable; }
    StartResult startRemoveFile(const QString &) override
    { return StartResult::TransportUnavailable; }
    StartResult startRemoveDirectory(const QString &) override
    { return StartResult::TransportUnavailable; }

    StartResult startDownloadForTarget(
        const QString &path, const VehicleTargetLease &target,
        quint64 *operationIdOut) override
    {
        ++startCalls;
        if (operationIdOut) *operationIdOut = 0;
        if (m_busy)
            return StartResult::Busy;
        const StartResult selected = nextStartResult;
        nextStartResult = StartResult::Started;
        if (selected != StartResult::Started)
            return selected;
        m_busy = true;
        m_id = ++m_nextId;
        m_target = target;
        m_path = path;
        if (operationIdOut) *operationIdOut = m_id;
        emit operationStarted(Operation::Download, target.generation, path);
        emit stateChanged();
        if (completeSynchronously) {
            if (synchronousError.isEmpty())
                completeSuccess(synchronousData);
            else
                completeFailure(synchronousError);
        }
        return StartResult::Started;
    }

    void cancel() override { ++unscopedCancelCalls; }

    bool cancelOperation(quint64 operationId) override
    {
        attemptedCancelIds.append(operationId);
        if (!m_busy || operationId == 0 || operationId != m_id)
            return false;
        ++ownedCancelCalls;
        if (!delayCancellation)
            completeCancelled();
        return true;
    }

    void setForeignBusy(quint64 id = 900)
    {
        m_busy = true;
        m_id = id;
        m_target = VehicleTargetLease();
        m_path = QStringLiteral("foreign");
    }

    void clearForeignBusy()
    {
        m_busy = false;
        m_id = 0;
        m_path.clear();
    }

    void report(qint64 completed, qint64 total)
    {
        emit operationProgress(m_id, m_target.generation, completed, total);
    }

    void emitForeignResult(quint64 id)
    {
        Result result;
        result.operation = Operation::Download;
        result.operationId = id;
        result.targetGeneration = m_target.generation;
        result.remotePath = m_path;
        result.error = QStringLiteral("foreign failure");
        emit operationFinished(result);
    }

    void completeSuccess(const QByteArray &data)
    {
        Result result = ownedResult();
        result.data = data;
        finish(result);
    }

    void completeFailure(const QString &error)
    {
        Result result = ownedResult();
        result.error = error;
        finish(result);
    }

    void completeCancelled()
    {
        Result result = ownedResult();
        result.cancelled = true;
        result.error = QStringLiteral("cancelled by fake");
        finish(result);
    }

    Result ownedResult() const
    {
        Result result;
        result.operation = Operation::Download;
        result.operationId = m_id;
        result.targetGeneration = m_target.generation;
        result.remotePath = m_path;
        return result;
    }

    void finish(const Result &result)
    {
        m_busy = false;
        m_id = 0;
        emit stateChanged();
        emit operationFinished(result);
    }

    StartResult nextStartResult = StartResult::Started;
    bool delayCancellation = false;
    bool completeSynchronously = false;
    QByteArray synchronousData;
    QString synchronousError;
    int startCalls = 0;
    int ownedCancelCalls = 0;
    int unscopedCancelCalls = 0;
    QList<quint64> attemptedCancelIds;
    QString m_path;
    VehicleTargetLease m_target;

private:
    bool m_busy = false;
    quint64 m_id = 0;
    quint64 m_nextId = 100;
};

class MavFtpFileDownloadTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndPickerCancellation();
    void downloadsExactPathAndPublishesAtomically();
    void synchronousCompletionIsOwnedBeforeCallbacks();
    void admissionCallbacksCannotCancelAReplacementOrUseDeletedController();
    void invalidAndChangedTargetsCancelBeforeAdmission();
    void sharedBusyAndStartFailureNeverCancelForeignWork();
    void timeoutAndExplicitCancelAreOperationScoped();
    void successfulTerminalAfterCancelIsNotPublished();
    void closeAndDestructionCancelOnlyOwnedOperation();
    void foreignAndOldResultsCannotCompleteReplacement();
    void failureAndUnsafeOutputPreserveExistingFile();
    void serviceAndTargetDestructionFailClosed();
};

void MavFtpFileDownloadTest::defaultsAndPickerCancellation()
{
    QWidget owner;
    VehicleTargetManager targets;
    selectDefaultTarget(&targets);
    FakeOwnedMavFtpService service;
    MavFtpFileDownload controller(&service, &targets, &owner);
    QSignalSpy busySpy(&controller, &MavFtpFileDownload::busyChanged);

    controller.start();
    QVERIFY(controller.busy());
    auto *path = owner.findChild<QInputDialog *>(
        QStringLiteral("DeveloperMavFtpPathDialog"));
    QVERIFY(path);
    QCOMPARE(path->inputMode(), QInputDialog::TextInput);
    QCOMPARE(path->textValue(), QStringLiteral("@SYS/threads.txt"));
    path->reject();
    QVERIFY(!controller.busy());
    QCOMPARE(service.startCalls, 0);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    controller.start();
    path = owner.findChild<QInputDialog *>(
        QStringLiteral("DeveloperMavFtpPathDialog"));
    QVERIFY(path);
    path->accept();
    auto *output = owner.findChild<QFileDialog *>(
        QStringLiteral("DeveloperMavFtpOutputDialog"));
    QVERIFY(output);
    QCOMPARE(output->acceptMode(), QFileDialog::AcceptSave);
    QVERIFY(!output->testOption(QFileDialog::DontConfirmOverwrite));
    QVERIFY(output->selectedFiles().value(0).endsWith(
        QStringLiteral("threads.txt")));
    output->reject();
    QVERIFY(!controller.busy());
    QCOMPARE(service.startCalls, 0);
    QCOMPARE(busySpy.count(), 4);
}

void MavFtpFileDownloadTest::downloadsExactPathAndPublishesAtomically()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QWidget owner;
    VehicleTargetManager targets;
    selectDefaultTarget(&targets);
    const VehicleTargetLease expected = targets.acquireTarget();
    FakeOwnedMavFtpService service;
    MavFtpFileDownload controller(&service, &targets, &owner);
    QSignalSpy logs(&controller, &MavFtpFileDownload::logMessage);
    const QString destination = directory.filePath(QStringLiteral("saved result.bin"));

    controller.start();
    auto *path = owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog");
    QVERIFY(path);
    path->setTextValue(QStringLiteral("/APM/Log File With Spaces.bin"));
    path->accept();
    auto *output = owner.findChild<QFileDialog *>("DeveloperMavFtpOutputDialog");
    QVERIFY(output);
    output->setDirectory(directory.path());
    output->selectFile(QFileInfo(destination).fileName());
    QVERIFY(QMetaObject::invokeMethod(output, "accept", Qt::DirectConnection));

    QCOMPARE(service.startCalls, 1);
    QCOMPARE(service.m_path, QStringLiteral("/APM/Log File With Spaces.bin"));
    QVERIFY(service.m_target.endpoint.sameIdentity(expected.endpoint));
    QCOMPARE(service.m_target.generation, expected.generation);
    QVERIFY(controller.busy());
    auto *progress = owner.findChild<QProgressDialog *>(
        QStringLiteral("DeveloperMavFtpProgressDialog"));
    QVERIFY(progress);
    service.report(2, 8);
    QCOMPARE(progress->value(), 250);

    const QByteArray bytes("downloaded\0bytes", 16);
    service.completeSuccess(bytes);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
    QCOMPARE(readFile(destination), bytes);
    QVERIFY(!logs.isEmpty());
    QVERIFY(logs.last().at(0).toString().contains(QStringLiteral("16 bytes written")));
    QCOMPARE(service.unscopedCancelCalls, 0);
}

void MavFtpFileDownloadTest::synchronousCompletionIsOwnedBeforeCallbacks()
{
    QTemporaryDir directory;
    QWidget owner;
    VehicleTargetManager targets;
    selectDefaultTarget(&targets);
    FakeOwnedMavFtpService service;
    service.completeSynchronously = true;
    service.synchronousData = QByteArrayLiteral("inline");
    MavFtpFileDownload controller(&service, &targets, &owner);
    const QString destination = directory.filePath(QStringLiteral("inline.bin"));

    controller.start();
    owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog")->accept();
    auto *output = owner.findChild<QFileDialog *>("DeveloperMavFtpOutputDialog");
    output->setDirectory(directory.path());
    output->selectFile(QFileInfo(destination).fileName());
    QVERIFY(QMetaObject::invokeMethod(output, "accept", Qt::DirectConnection));

    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
    QCOMPARE(readFile(destination), QByteArrayLiteral("inline"));
    QCOMPARE(service.ownedCancelCalls, 0);
}

void MavFtpFileDownloadTest::admissionCallbacksCannotCancelAReplacementOrUseDeletedController()
{
    QTemporaryDir directory;
    QWidget owner;
    VehicleTargetManager targets;
    selectDefaultTarget(&targets);
    FakeOwnedMavFtpService service;
    service.completeSynchronously = true;
    service.synchronousError = QStringLiteral("inline rejection");
    MavFtpFileDownload controller(&service, &targets, &owner);
    bool restarted = false;
    connect(&controller, &MavFtpFileDownload::logMessage,
            &controller, [&](const QString &message) {
        if (!restarted && message.contains(QStringLiteral("inline rejection"))) {
            restarted = true;
            service.completeSynchronously = false;
            controller.start();
        }
    });

    controller.start();
    owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog")->accept();
    auto *output = owner.findChild<QFileDialog *>("DeveloperMavFtpOutputDialog");
    output->setDirectory(directory.path());
    output->selectFile(QStringLiteral("first.bin"));
    QVERIFY(QMetaObject::invokeMethod(output, "accept", Qt::DirectConnection));
    QVERIFY(restarted);
    QVERIFY(controller.busy());
    QVERIFY(owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog"));
    QCOMPARE(service.ownedCancelCalls, 0);
    controller.cancel();

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    controller.start();
    auto *reentrantPrompt = owner.findChild<QInputDialog *>(
        "DeveloperMavFtpPathDialog");
    QVERIFY(reentrantPrompt);
    bool promptRestarted = false;
    connect(reentrantPrompt, &QDialog::finished, &owner, [&]() {
        if (promptRestarted)
            return;
        promptRestarted = true;
        controller.cancel();
        if (!controller.busy())
            controller.start();
    });
    controller.cancel();
    QVERIFY(promptRestarted);
    QVERIFY(controller.busy());
    QVERIFY(owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog"));
    controller.cancel();

    auto *deleted = new MavFtpFileDownload(&service, &targets, &owner);
    QPointer<MavFtpFileDownload> guard(deleted);
    connect(deleted, &MavFtpFileDownload::busyChanged, deleted, [deleted]() {
        delete deleted;
    });
    deleted->start();
    QVERIFY(!guard);

    auto *deletingOwner = new QWidget;
    auto *deletingController = new MavFtpFileDownload(
        &service, &targets, deletingOwner);
    QPointer<MavFtpFileDownload> deletingGuard(deletingController);
    deletingController->start();
    auto *deletingPrompt = deletingOwner->findChild<QInputDialog *>(
        "DeveloperMavFtpPathDialog");
    QVERIFY(deletingPrompt);
    connect(deletingPrompt, &QDialog::finished, deletingOwner,
            [deletingOwner]() { delete deletingOwner; });
    deletingController->cancel();
    QVERIFY(!deletingGuard);
}

void MavFtpFileDownloadTest::invalidAndChangedTargetsCancelBeforeAdmission()
{
    QWidget owner;
    VehicleTargetManager targets;
    FakeOwnedMavFtpService service;
    MavFtpFileDownload controller(&service, &targets, &owner);
    QSignalSpy logs(&controller, &MavFtpFileDownload::logMessage);

    controller.start();
    QVERIFY(!controller.busy());
    QVERIFY(!logs.isEmpty());
    selectDefaultTarget(&targets);
    QVERIFY(targets.observeEndpoint(endpoint(8, 43), false));

    controller.start();
    QVERIFY(controller.busy());
    QVERIFY(targets.selectTarget(8, 43, 1));
    QVERIFY(!controller.busy());
    QCOMPARE(service.startCalls, 0);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    controller.start();
    auto *path = owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog");
    QVERIFY(path);
    path->accept();
    QVERIFY(owner.findChild<QFileDialog *>("DeveloperMavFtpOutputDialog"));
    QVERIFY(targets.selectTarget(7, 42, 1));
    QVERIFY(!controller.busy());
    QCOMPARE(service.startCalls, 0);
}

void MavFtpFileDownloadTest::sharedBusyAndStartFailureNeverCancelForeignWork()
{
    QTemporaryDir directory;
    QWidget owner;
    VehicleTargetManager targets;
    selectDefaultTarget(&targets);
    FakeOwnedMavFtpService service;
    MavFtpFileDownload controller(&service, &targets, &owner);
    service.setForeignBusy(777);

    controller.start();
    QVERIFY(!controller.busy());
    QCOMPARE(service.startCalls, 0);
    QCOMPARE(service.ownedCancelCalls, 0);
    QCOMPARE(service.unscopedCancelCalls, 0);
    service.clearForeignBusy();
    service.nextStartResult = MavFtpServiceInterface::StartResult::InvalidPath;

    controller.start();
    owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog")->accept();
    auto *output = owner.findChild<QFileDialog *>("DeveloperMavFtpOutputDialog");
    output->setDirectory(directory.path());
    output->selectFile(QStringLiteral("unused.bin"));
    QVERIFY(QMetaObject::invokeMethod(output, "accept", Qt::DirectConnection));
    QVERIFY(!controller.busy());
    QCOMPARE(service.startCalls, 1);
    QCOMPARE(service.ownedCancelCalls, 0);
}

void MavFtpFileDownloadTest::timeoutAndExplicitCancelAreOperationScoped()
{
    QTemporaryDir directory;
    QWidget owner;
    VehicleTargetManager targets;
    selectDefaultTarget(&targets);
    FakeOwnedMavFtpService service;
    MavFtpFileDownload controller(&service, &targets, &owner);
    controller.setTimeoutForTesting(20);

    auto startTransfer = [&](const QString &name) {
        controller.start();
        owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog")->accept();
        auto *output = owner.findChild<QFileDialog *>("DeveloperMavFtpOutputDialog");
        output->setDirectory(directory.path());
        output->selectFile(name);
        QVERIFY(QMetaObject::invokeMethod(output, "accept", Qt::DirectConnection));
        QVERIFY(controller.busy());
    };
    startTransfer(QStringLiteral("timeout.bin"));
    QTRY_COMPARE_WITH_TIMEOUT(service.ownedCancelCalls, 1, 1000);
    QVERIFY(!controller.busy());
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("timeout.bin"))));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    controller.setTimeoutForTesting(1000);
    startTransfer(QStringLiteral("cancel.bin"));
    const quint64 owned = service.activeOperationId();
    controller.cancel();
    QVERIFY(!controller.busy());
    QCOMPARE(service.ownedCancelCalls, 2);
    QVERIFY(service.attemptedCancelIds.contains(owned));
    QCOMPARE(service.unscopedCancelCalls, 0);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    startTransfer(QStringLiteral("already-ended.bin"));
    service.clearForeignBusy(); // Simulates shutdown clearing the token without a result.
    controller.cancel();
    QVERIFY(!controller.busy());
    QCOMPARE(service.ownedCancelCalls, 2);
}

void MavFtpFileDownloadTest::successfulTerminalAfterCancelIsNotPublished()
{
    QTemporaryDir directory;
    QWidget owner;
    VehicleTargetManager targets;
    selectDefaultTarget(&targets);
    FakeOwnedMavFtpService service;
    service.delayCancellation = true;
    MavFtpFileDownload controller(&service, &targets, &owner);
    const QString destination = directory.filePath(QStringLiteral("late-success.bin"));

    controller.start();
    owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog")->accept();
    auto *output = owner.findChild<QFileDialog *>("DeveloperMavFtpOutputDialog");
    output->setDirectory(directory.path());
    output->selectFile(QFileInfo(destination).fileName());
    QVERIFY(QMetaObject::invokeMethod(output, "accept", Qt::DirectConnection));
    controller.cancel();
    QVERIFY(controller.busy());
    QCOMPARE(service.ownedCancelCalls, 1);
    service.completeSuccess(QByteArrayLiteral("must not publish"));
    QVERIFY(!controller.busy());
    QVERIFY(!QFileInfo::exists(destination));
}

void MavFtpFileDownloadTest::closeAndDestructionCancelOnlyOwnedOperation()
{
    QTemporaryDir directory;
    VehicleTargetManager targets;
    selectDefaultTarget(&targets);
    FakeOwnedMavFtpService service;
    auto begin = [&](QWidget *owner, MavFtpFileDownload *controller,
                     const QString &name) {
        controller->start();
        owner->findChild<QInputDialog *>("DeveloperMavFtpPathDialog")->accept();
        auto *output = owner->findChild<QFileDialog *>("DeveloperMavFtpOutputDialog");
        output->setDirectory(directory.path());
        output->selectFile(name);
        QVERIFY(QMetaObject::invokeMethod(output, "accept", Qt::DirectConnection));
    };

    {
        QWidget owner;
        MavFtpFileDownload controller(&service, &targets, &owner);
        begin(&owner, &controller, QStringLiteral("close.bin"));
        QCloseEvent closeEvent;
        QApplication::sendEvent(&owner, &closeEvent);
        QCOMPARE(service.ownedCancelCalls, 1);
        QVERIFY(!controller.busy());
    }
    {
        auto *owner = new QWidget;
        auto *controller = new MavFtpFileDownload(&service, &targets, owner);
        begin(owner, controller, QStringLiteral("destroy.bin"));
        const quint64 owned = service.activeOperationId();
        QPointer<MavFtpFileDownload> guard(controller);
        delete owner;
        QVERIFY(!guard);
        QCOMPARE(service.ownedCancelCalls, 2);
        QVERIFY(service.attemptedCancelIds.contains(owned));
    }
    service.setForeignBusy(12345);
    {
        QWidget owner;
        MavFtpFileDownload controller(&service, &targets, &owner);
        controller.cancel();
    }
    QCOMPARE(service.ownedCancelCalls, 2);
    QCOMPARE(service.activeOperationId(), quint64(12345));
    service.clearForeignBusy();
}

void MavFtpFileDownloadTest::foreignAndOldResultsCannotCompleteReplacement()
{
    QTemporaryDir directory;
    QWidget owner;
    VehicleTargetManager targets;
    selectDefaultTarget(&targets);
    FakeOwnedMavFtpService service;
    MavFtpFileDownload controller(&service, &targets, &owner);
    const QString destination = directory.filePath(QStringLiteral("owned.bin"));

    controller.start();
    owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog")->accept();
    auto *output = owner.findChild<QFileDialog *>("DeveloperMavFtpOutputDialog");
    output->setDirectory(directory.path());
    output->selectFile(QFileInfo(destination).fileName());
    QVERIFY(QMetaObject::invokeMethod(output, "accept", Qt::DirectConnection));
    const quint64 owned = service.activeOperationId();
    service.emitForeignResult(owned + 1);
    QVERIFY(controller.busy());
    QVERIFY(!QFileInfo::exists(destination));

    service.completeSuccess(QByteArrayLiteral("owned"));
    service.setForeignBusy(owned + 2);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
    QCOMPARE(readFile(destination), QByteArrayLiteral("owned"));
    controller.cancel();
    QCOMPARE(service.activeOperationId(), owned + 2);
    QCOMPARE(service.unscopedCancelCalls, 0);
    service.clearForeignBusy();
}

void MavFtpFileDownloadTest::failureAndUnsafeOutputPreserveExistingFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QWidget owner;
    VehicleTargetManager targets;
    selectDefaultTarget(&targets);
    FakeOwnedMavFtpService service;
    MavFtpFileDownload controller(&service, &targets, &owner);
    const QString existing = directory.filePath(QStringLiteral("existing.bin"));
    QVERIFY(writeFile(existing, QByteArrayLiteral("old")));

    controller.start();
    owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog")->accept();
    auto *output = owner.findChild<QFileDialog *>("DeveloperMavFtpOutputDialog");
    output->setOption(QFileDialog::DontConfirmOverwrite, true);
    output->setDirectory(directory.path());
    output->selectFile(QFileInfo(existing).fileName());
    QVERIFY(QMetaObject::invokeMethod(output, "accept", Qt::DirectConnection));
    QVERIFY(controller.busy());
    service.completeFailure(QStringLiteral("remote denied"));
    QVERIFY(!controller.busy());
    QCOMPARE(readFile(existing), QByteArrayLiteral("old"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

#ifndef Q_OS_WIN
    const QString target = directory.filePath(QStringLiteral("target.bin"));
    const QString alias = directory.filePath(QStringLiteral("alias.bin"));
    QVERIFY(writeFile(target, QByteArrayLiteral("protected")));
    QVERIFY(QFile::link(target, alias));
    controller.start();
    owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog")->accept();
    output = owner.findChild<QFileDialog *>("DeveloperMavFtpOutputDialog");
    output->setOption(QFileDialog::DontConfirmOverwrite, true);
    output->setDirectory(directory.path());
    output->selectFile(QFileInfo(alias).fileName());
    QVERIFY(QMetaObject::invokeMethod(output, "accept", Qt::DirectConnection));
    QVERIFY(!controller.busy());
    QCOMPARE(service.startCalls, 1);
    QCOMPARE(readFile(target), QByteArrayLiteral("protected"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    const QString originalParent = directory.filePath(QStringLiteral("original-parent"));
    const QString movedParent = directory.filePath(QStringLiteral("moved-parent"));
    const QString redirectedParent = directory.filePath(QStringLiteral("redirected-parent"));
    QVERIFY(QDir().mkdir(originalParent));
    QVERIFY(QDir().mkdir(redirectedParent));
    controller.start();
    owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog")->accept();
    output = owner.findChild<QFileDialog *>("DeveloperMavFtpOutputDialog");
    output->setDirectory(originalParent);
    output->selectFile(QStringLiteral("redirected.bin"));
    QVERIFY(QMetaObject::invokeMethod(output, "accept", Qt::DirectConnection));
    QVERIFY(controller.busy());
    QVERIFY(QDir().rename(originalParent, movedParent));
    QVERIFY(QFile::link(redirectedParent, originalParent));
    service.completeSuccess(QByteArrayLiteral("private bytes"));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5000);
    QVERIFY(!QFileInfo::exists(redirectedParent + QStringLiteral("/redirected.bin")));
    QVERIFY(!QFileInfo::exists(movedParent + QStringLiteral("/redirected.bin")));
#endif
}

void MavFtpFileDownloadTest::serviceAndTargetDestructionFailClosed()
{
    QTemporaryDir directory;
    QWidget owner;
    auto *targets = new VehicleTargetManager(&owner);
    selectDefaultTarget(targets);
    auto *service = new FakeOwnedMavFtpService(&owner);
    auto *controller = new MavFtpFileDownload(service, targets, &owner);

    controller->start();
    owner.findChild<QInputDialog *>("DeveloperMavFtpPathDialog")->accept();
    auto *output = owner.findChild<QFileDialog *>("DeveloperMavFtpOutputDialog");
    output->setDirectory(directory.path());
    output->selectFile(QStringLiteral("gone.bin"));
    QVERIFY(QMetaObject::invokeMethod(output, "accept", Qt::DirectConnection));
    QVERIFY(controller->busy());
    delete service;
    QVERIFY(!controller->busy());
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("gone.bin"))));

    service = new FakeOwnedMavFtpService(&owner);
    delete controller;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    controller = new MavFtpFileDownload(service, targets, &owner);
    controller->start();
    QVERIFY(controller->busy());
    delete targets;
    QVERIFY(!controller->busy());
}

QTEST_MAIN(MavFtpFileDownloadTest)
#include "test_mavftpfiledownload.moc"
