#include "services/MavAuthKeyService.h"
#include "services/MavAuthKeyStore.h"

#include <QtTest>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <stdexcept>
#include <thread>

namespace {
const QString Master = QStringLiteral("An async test vault passphrase 2026");

bool waitFor(QSignalSpy &spy, quint64 token, bool success = true)
{
    if (!token || (spy.isEmpty() && !spy.wait(10000))) return false;
    if (spy.size() != 1) return false;
    const auto result = spy.takeFirst();
    return result.size() == 3 && result.at(0).toULongLong() == token
        && result.at(1).toBool() == success
        && (success ? result.at(2).toString().isEmpty() : !result.at(2).toString().isEmpty());
}

bool run(MavAuthKeyService &service, const std::function<quint64()> &operation, bool success = true)
{
    QSignalSpy spy(&service, &MavAuthKeyService::operationFinished);
    return waitFor(spy, operation(), success);
}

QByteArray fileBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

class MavAuthKeyServiceTest final : public QObject
{
    Q_OBJECT
private slots:
    void roundTripOwnerThreadExportAndReopen();
    void busyIsBoundedAndInputsAreImmutable();
    void wrongPasswordDuplicateAndMissingKeysPreserveData();
    void missingCorruptAndInvalidInputsNeverCreateImplicitly();
    void observerClosureDoesNotOwnService();
    void callbackCanDeleteServiceOrThrow();
    void admissionAndCompletionObserversCanDeleteService();
    void completionIsDeferredPastAdmissionAndTokenReturn();
    void foreignThreadMutationsAreRejectedWithoutStateChange();
    void shutdownDrainsAndCancelsSecretDelivery();
    void destructionDuringKdfJoinsAndReleasesFileLock();
    void metaObjectContainsNoSecretChannel();
};

void MavAuthKeyServiceTest::roundTripOwnerThreadExportAndReopen()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("vault.keys");
    const QString name = QString::fromUtf8("Борт α");
    const QString seed = QString::fromUtf8("секретный seed 🔐");
    const QByteArray expected = QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256);
    {
        MavAuthKeyService service(path);
        QVERIFY(!service.busy());
        QVERIFY(!service.isUnlocked());
        QVERIFY(run(service, [&] { return service.create(Master); }));
        QVERIFY(service.isUnlocked());
        QVERIFY(service.keyNames().isEmpty());
        QVERIFY(run(service, [&] { return service.addSeed(name, seed); }));
        QCOMPARE(service.keyNames(), QStringList{name});
        QByteArray retained;
        int callbacks = 0;
        QVERIFY(run(service, [&] {
            return service.requestKey(name, [&](bool ok, const QByteArray &key, const QString &error) {
                QVERIFY(ok);
                QVERIFY(error.isEmpty());
                QVERIFY(service.busy());
                QCOMPARE(QThread::currentThread(), service.thread());
                QCOMPARE(key, expected);
                retained = key; // explicit caller-owned copy survives service cleanup
                ++callbacks;
                QCOMPARE(service.lock(), quint64(0)); // one job until key delivery returns
            });
        }));
        QCOMPARE(callbacks, 1);
        QCOMPARE(retained, expected);
        QVERIFY(!service.busy());
        QVERIFY(run(service, [&] { return service.lock(); }));
        QVERIFY(!service.isUnlocked());
        QVERIFY(service.keyNames().isEmpty());
        QVERIFY(!QFileInfo::exists(path + ".lock"));
        QVERIFY(!fileBytes(path).contains(expected));
        QVERIFY(!fileBytes(path).contains(seed.toUtf8()));
        QVERIFY(!fileBytes(path).contains(Master.toUtf8()));
    }
    MavAuthKeyService reopened(path);
    QVERIFY(run(reopened, [&] { return reopened.unlock(Master); }));
    QCOMPARE(reopened.keyNames(), QStringList{name});
    QVERIFY(run(reopened, [&] { return reopened.removeKey(name); }));
    QVERIFY(reopened.keyNames().isEmpty());
    QVERIFY(run(reopened, [&] { return reopened.lock(); }));
    QVERIFY(run(reopened, [&] { return reopened.unlock(Master); }));
    QVERIFY(reopened.keyNames().isEmpty());
}

void MavAuthKeyServiceTest::busyIsBoundedAndInputsAreImmutable()
{
    QTemporaryDir dir;
    MavAuthKeyService service(dir.filePath("vault.keys"));
    QString input = Master;
    QSignalSpy completed(&service, &MavAuthKeyService::operationFinished);
    int eventLoopTicks = 0;
    QTimer timer;
    timer.setInterval(0);
    connect(&timer, &QTimer::timeout, this, [&] { ++eventLoopTicks; });
    timer.start();
    const quint64 first = service.create(input);
    QVERIFY(first);
    input.fill('X');
    QVERIFY(service.busy());
    for (int i = 0; i < 100; ++i) {
        QCOMPARE(service.unlock(Master), quint64(0));
        QCOMPARE(service.lock(), quint64(0));
        QCOMPARE(service.addSeed("queued", "must not be queued"), quint64(0));
    }
    QVERIFY(waitFor(completed, first));
    timer.stop();
    QVERIFY(eventLoopTicks > 0); // KDF ran off-thread while the owner dispatched events
    QVERIFY(service.keyNames().isEmpty());
    QString name = "original";
    QString seed = "original seed";
    const QByteArray expected = QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256);
    const quint64 added = service.addSeed(name, seed);
    QVERIFY(added > first);
    name.fill('N'); seed.fill('S');
    QVERIFY(waitFor(completed, added));
    QCOMPARE(service.keyNames(), QStringList{"original"});
    QByteArray received;
    QVERIFY(run(service, [&] { return service.requestKey("original",
        [&](bool ok, const QByteArray &key, const QString &) { QVERIFY(ok); received = key; }); }));
    QCOMPARE(received, expected);
    QVERIFY(run(service, [&] { return service.lock(); }));
    QVERIFY(run(service, [&] { return service.unlock(Master); }));
}

void MavAuthKeyServiceTest::wrongPasswordDuplicateAndMissingKeysPreserveData()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("vault.keys");
    MavAuthKeyService service(path);
    QVERIFY(run(service, [&] { return service.create(Master); }));
    QVERIFY(run(service, [&] { return service.addSeed("alpha", "first seed"); }));
    const QByteArray original = fileBytes(path);
    QVERIFY(run(service, [&] { return service.addSeed("alpha", "replacement"); }, false));
    QCOMPARE(fileBytes(path), original);
    QCOMPARE(service.keyNames(), QStringList{"alpha"});
    bool called = false;
    QVERIFY(run(service, [&] { return service.requestKey("missing",
        [&](bool ok, const QByteArray &key, const QString &error) {
            called = true; QVERIFY(!ok); QVERIFY(key.isEmpty()); QVERIFY(!error.isEmpty());
        }); }, false));
    QVERIFY(called);
    QVERIFY(run(service, [&] { return service.removeKey("missing"); }, false));
    QCOMPARE(fileBytes(path), original);
    QVERIFY(run(service, [&] { return service.lock(); }));
    QVERIFY(run(service, [&] { return service.unlock("A valid but wrong master password"); }, false));
    QVERIFY(!service.isUnlocked());
    QVERIFY(service.keyNames().isEmpty());
    QCOMPARE(fileBytes(path), original);
    QVERIFY(run(service, [&] { return service.create(Master); }, false));
    QCOMPARE(fileBytes(path), original);
    QVERIFY(run(service, [&] { return service.unlock(Master); }));
    QCOMPARE(service.keyNames(), QStringList{"alpha"});
}

void MavAuthKeyServiceTest::missingCorruptAndInvalidInputsNeverCreateImplicitly()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("missing.keys");
    MavAuthKeyService missing(path);
    QVERIFY(run(missing, [&] { return missing.unlock(Master); }, false));
    QVERIFY(!QFileInfo::exists(path));
    MavAuthKeyService noDirectory(dir.filePath("absent/vault.keys"));
    QVERIFY(run(noDirectory, [&] { return noDirectory.create(Master); }, false));
    QVERIFY(!QFileInfo::exists(dir.filePath("absent")));
    QFile corrupt(path);
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    QCOMPARE(corrupt.write("not a vault"), qint64(11));
    corrupt.close();
    QVERIFY(run(missing, [&] { return missing.unlock(Master); }, false));
    QVERIFY(run(missing, [&] { return missing.create(Master); }, false));
    QCOMPARE(fileBytes(path), QByteArray("not a vault"));

    QSignalSpy finished(&missing, &MavAuthKeyService::operationFinished);
    for (const QString &input : {QString(), QString("short"), QString(1025, 'a'),
                                 QString(513, QChar(0x0430)), QString(QChar(0xd800))})
        QCOMPARE(missing.unlock(input), quint64(0));
    for (const QString &name : {QString(), QString(129, 'n'), QString(65, QChar(0x0430)),
                                QString("control\nname"), QString("   ")}) {
        QCOMPARE(missing.addSeed(name, "seed"), quint64(0));
        QCOMPARE(missing.removeKey(name), quint64(0));
        QCOMPARE(missing.requestKey(name, [](bool, const QByteArray &, const QString &) {}), quint64(0));
    }
    for (const QString &seed : {QString(), QString(4097, 's'), QString(2049, QChar(0x0430)),
                                QString(QChar(0)), QString("   ")})
        QCOMPARE(missing.addSeed("valid", seed), quint64(0));
    QCOMPARE(missing.requestKey("valid", {}), quint64(0));
    QVERIFY(!missing.lastError().isEmpty());
    QVERIFY(!missing.busy());
    QCOMPARE(finished.size(), 0);
    MavAuthKeyService relative("relative.keys");
    QCOMPARE(relative.create(Master), quint64(0));
}

void MavAuthKeyServiceTest::observerClosureDoesNotOwnService()
{
    QTemporaryDir dir;
    MavAuthKeyService service(dir.filePath("vault.keys"));
    QVERIFY(run(service, [&] { return service.create(Master); }));
    QVERIFY(run(service, [&] { return service.addSeed("alpha", "seed"); }));
    QPointer<QObject> window = new QObject;
    bool touchedWindow = false;
    QSignalSpy finished(&service, &MavAuthKeyService::operationFinished);
    const auto token = service.requestKey("alpha", [window, &touchedWindow](bool, const QByteArray &, const QString &) {
        if (window) touchedWindow = true;
    });
    delete window.data();
    QVERIFY(waitFor(finished, token));
    QVERIFY(!touchedWindow);
    QVERIFY(service.isUnlocked());
    QCOMPARE(service.keyNames(), QStringList{"alpha"});
}

void MavAuthKeyServiceTest::callbackCanDeleteServiceOrThrow()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("vault.keys");
    auto *service = new MavAuthKeyService(path);
    QVERIFY(run(*service, [&] { return service->create(Master); }));
    QVERIFY(run(*service, [&] { return service->addSeed("alpha", "seed"); }));
    QVERIFY(run(*service, [&] { return service->requestKey("alpha",
        [](bool, const QByteArray &, const QString &) { throw std::runtime_error("never log callback contents"); }); }, false));
    QVERIFY(service->isUnlocked());
    QPointer<MavAuthKeyService> guard(service);
    QByteArray received;
    QVERIFY(service->requestKey("alpha", [&](bool ok, const QByteArray &key, const QString &) {
        QVERIFY(ok); received = key; delete service; service = nullptr;
    }));
    QTRY_VERIFY(guard.isNull());
    QCOMPARE(received, QCryptographicHash::hash("seed", QCryptographicHash::Sha256));
    QVERIFY(!QFileInfo::exists(path + ".lock"));
}

void MavAuthKeyServiceTest::admissionAndCompletionObserversCanDeleteService()
{
    QTemporaryDir dir;
    for (const bool onAdmission : {true, false}) {
        const QString path = dir.filePath(onAdmission ? "admission.keys" : "completion.keys");
        auto *service = new MavAuthKeyService(path);
        QPointer<MavAuthKeyService> guard(service);
        connect(service, &MavAuthKeyService::stateChanged, this, [&service, onAdmission] {
            if (service && (onAdmission ? service->busy() : !service->busy() && service->isUnlocked())) {
                delete service; service = nullptr;
            }
        });
        QVERIFY(service->create(Master));
        QTRY_VERIFY(guard.isNull());
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(path + ".lock"));
    }
}

void MavAuthKeyServiceTest::
completionIsDeferredPastAdmissionAndTokenReturn()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    MavAuthKeyService service(dir.filePath("vault.keys"));
    QVERIFY(run(service, [&] { return service.create(Master); }));
    QVERIFY(run(service, [&] { return service.addSeed("alpha", "seed"); }));

    bool spinAdmission = false;
    bool requestReturned = false;
    bool callbackCalled = false;
    bool callbackBeforeReturn = false;
    connect(&service, &MavAuthKeyService::stateChanged, this, [&] {
        if (!spinAdmission || !service.busy()) return;
        spinAdmission = false;
        // A fast worker completion is deliberately allowed to reach the owner
        // queue while submit() is still inside its admission signal.
        QEventLoop nested;
        QTimer::singleShot(250, &nested, &QEventLoop::quit);
        nested.exec();
    });
    QSignalSpy finished(&service, &MavAuthKeyService::operationFinished);
    spinAdmission = true;
    const quint64 token = service.requestKey(
        QStringLiteral("alpha"),
        [&](bool success, const QByteArray &key, const QString &error) {
            callbackCalled = true;
            callbackBeforeReturn = !requestReturned;
            QVERIFY(success);
            QVERIFY(!key.isEmpty());
            QVERIFY(error.isEmpty());
        });
    requestReturned = true;

    QVERIFY(token != 0);
    QVERIFY(!callbackCalled);
    QVERIFY(!callbackBeforeReturn);
    QCOMPARE(finished.size(), 0);
    QVERIFY(waitFor(finished, token));
    QVERIFY(callbackCalled);
    QVERIFY(!callbackBeforeReturn);
}

void MavAuthKeyServiceTest::
foreignThreadMutationsAreRejectedWithoutStateChange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("vault.keys");
    MavAuthKeyService service(path);
    const QString errorBefore = service.lastError();
    std::atomic<quint64> token{1};
    std::thread foreign([&] {
        token.store(service.create(Master));
        service.shutdown();
    });
    foreign.join();

    QCOMPARE(token.load(), quint64(0));
    QVERIFY(!service.busy());
    QVERIFY(!service.isUnlocked());
    QVERIFY(!service.isShuttingDown());
    QCOMPARE(service.lastError(), errorBefore);
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(run(service, [&] { return service.create(Master); }));
}

void MavAuthKeyServiceTest::shutdownDrainsAndCancelsSecretDelivery()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("drained.keys");
    MavAuthKeyService creating(path);
    QSignalSpy finished(&creating, &MavAuthKeyService::operationFinished);
    const auto token = creating.create(Master);
    creating.shutdown();
    QVERIFY(creating.isShuttingDown());
    QVERIFY(!creating.isUnlocked());
    QVERIFY(creating.keyNames().isEmpty());
    QCOMPARE(creating.lock(), quint64(0));
    QVERIFY(waitFor(finished, token));
    QVERIFY(!creating.busy());
    QTRY_VERIFY(!QFileInfo::exists(path + ".lock"));
    QVERIFY(QFileInfo::exists(path));

    MavAuthKeyService exporting(dir.filePath("export.keys"));
    QVERIFY(run(exporting, [&] { return exporting.create(Master); }));
    QVERIFY(run(exporting, [&] { return exporting.addSeed("alpha", "seed"); }));
    QSignalSpy exported(&exporting, &MavAuthKeyService::operationFinished);
    bool callback = false;
    const auto exportToken = exporting.requestKey("alpha", [&](bool ok, const QByteArray &key, const QString &error) {
        callback = true; QVERIFY(!ok); QVERIFY(key.isEmpty()); QVERIFY(!error.isEmpty());
    });
    exporting.shutdown();
    QVERIFY(waitFor(exported, exportToken, false));
    QVERIFY(callback);
    QVERIFY(!exporting.isUnlocked());
    QVERIFY(exporting.keyNames().isEmpty());
    QCOMPARE(exporting.unlock(Master), quint64(0));
}

void MavAuthKeyServiceTest::destructionDuringKdfJoinsAndReleasesFileLock()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("vault.keys");
    auto *service = new MavAuthKeyService(path);
    QVERIFY(service->create(Master));
    delete service; // joins the admitted KDF, no terminate or owner event pumping
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(!QFileInfo::exists(path + ".lock"));
    MavAuthKeyStore reopened(path);
    QString error;
    QVERIFY2(reopened.unlock(Master, &error), qPrintable(error));
}

void MavAuthKeyServiceTest::metaObjectContainsNoSecretChannel()
{
    QTemporaryDir dir;
    MavAuthKeyService service(dir.filePath("vault.keys"));
    const QMetaObject *meta = service.metaObject();
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        const QMetaMethod method = meta->method(i);
        QVERIFY(!method.parameterTypes().contains("QByteArray"));
        QVERIFY(method.name() != "requestKey");
    }
}

QTEST_GUILESS_MAIN(MavAuthKeyServiceTest)
#include "test_mavauthkeyservice.moc"
