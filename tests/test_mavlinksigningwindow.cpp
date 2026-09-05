#include "ui/MavlinkSigningWindow.h"
#include "services/MavAuthKeyService.h"

#include <QtTest>
#include <QComboBox>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <utility>

namespace {
const QString Master = QStringLiteral("A local window test master password 2026");
struct WindowOwner {
    QPointer<MavlinkSigningWindow> window;
    explicit WindowOwner(MavlinkSigningWindow *value) : window(value) { window->show(); }
    ~WindowOwner() { delete window.data(); }
    MavlinkSigningWindow *get() const { return window.data(); }
};
bool waitFor(QSignalSpy &spy, bool success = true)
{
    if (spy.isEmpty() && !spy.wait(10000)) return false;
    if (spy.size() != 1) return false;
    return spy.takeFirst().at(1).toBool() == success;
}
bool operation(MavAuthKeyService &service, const std::function<quint64()> &start, bool success = true)
{
    QSignalSpy spy(&service, &MavAuthKeyService::operationFinished);
    return start() != 0 && waitFor(spy, success);
}
bool click(QWidget *window, const char *name)
{
    auto *button = window->findChild<QPushButton *>(name);
    if (!button || !button->isEnabled()) return false;
    button->click();
    return true;
}
void input(QWidget *window, const char *name, const QString &text)
{
    if (auto *edit = window->findChild<QLineEdit *>(name)) edit->setText(text);
}
MavlinkSigningWindow::Connection connection(QObject *identity)
{
    MavlinkSigningWindow::Connection result;
    result.linkId = 11;
    result.profileId = "11111111-1111-4111-8111-111111111111";
    result.name = "Test physical link";
    result.identity = identity;
    result.revision = 4;
    result.required = true;
    result.fingerprint = QString(64, 'a');
    return result;
}
bool prepare(MavAuthKeyService &service)
{
    return operation(service, [&] { return service.create(Master); })
        && operation(service, [&] { return service.addSeed("alpha", "abc"); });
}
}

class MavlinkSigningWindowTest final : public QObject
{
    Q_OBJECT
private slots:
    void realVaultWorkflowMasksConfirmationAndNoImplicitCreate();
    void deleteDefaultsToCancelAndRequiresExplicitConfirmation();
    void firstProtectionNeedsExplicitConsent();
    void namedKeyActivatesOnlyExactOfflineSnapshot_data();
    void namedKeyActivatesOnlyExactOfflineSnapshot();
    void closeCancelsPendingActivationAndKeepsService();
    void providerAndActivatorMayDestroyWindow();
    void metadataIsPlainTextAndSelectionSurvivesRefresh();
};

void MavlinkSigningWindowTest::realVaultWorkflowMasksConfirmationAndNoImplicitCreate()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("vault.keys");
    MavAuthKeyService service(path);
    WindowOwner owner(new MavlinkSigningWindow(&service, {}, {}));
    auto *window = owner.get();
    QVERIFY(window->isWindow());
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QVERIFY(!window->isClosing());
    QCOMPARE(window->objectName(), QString("MavlinkSigningWindow"));
    QVERIFY(window->findChild<QLabel *>("SigningLocalOnlyBanner")->text().contains("does not send keys"));
    QVERIFY(window->findChild<QLabel *>("SigningLocalOnlyBanner")->text().contains("does not encrypt telemetry"));
    QVERIFY(window->findChild<QLabel *>("SigningLocalOnlyBanner")->text().contains("no recovery"));
    for (const char *name : {"SigningMasterPassword", "SigningConfirmPassword", "SigningKeySeed"})
        QCOMPARE(window->findChild<QLineEdit *>(name)->echoMode(), QLineEdit::Password);
    QVERIFY(!window->findChild<QCheckBox *>("SigningShowSeed")->isChecked());
    QCOMPARE(window->findChild<QLineEdit *>("SigningMasterPassword")->maxLength(), 1024);
    QCOMPARE(window->findChild<QLineEdit *>("SigningKeySeed")->maxLength(), 4096);
    QCOMPARE(window->findChild<QLineEdit *>("SigningKeyName")->maxLength(), 128);
    for (const char *name : {"SigningProvisionVehicle", "SigningDisableVehicle"}) {
        auto *button = window->findChild<QPushButton *>(name);
        QVERIFY(!button->isEnabled()); QVERIFY(!button->toolTip().isEmpty());
    }
    QVERIFY(!QFileInfo::exists(path));
    QSignalSpy finished(&service, &MavAuthKeyService::operationFinished);
    input(window, "SigningMasterPassword", Master);
    QVERIFY(click(window, "SigningUnlockVault"));
    QVERIFY(waitFor(finished, false));
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(!window->findChild<QLabel *>("SigningOperationStatus")->text().isEmpty());
    input(window, "SigningMasterPassword", Master);
    input(window, "SigningConfirmPassword", "not the same password");
    QVERIFY(click(window, "SigningCreateVault"));
    QVERIFY(!service.busy());
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(window->findChild<QLabel *>("SigningOperationStatus")->text().contains("do not match"));
    QVERIFY(window->findChild<QLineEdit *>("SigningMasterPassword")->text().isEmpty());
    QVERIFY(window->findChild<QLineEdit *>("SigningConfirmPassword")->text().isEmpty());
    input(window, "SigningMasterPassword", Master);
    input(window, "SigningConfirmPassword", Master);
    QVERIFY(click(window, "SigningCreateVault"));
    QVERIFY(!window->findChild<QPushButton *>("SigningUnlockVault")->isEnabled());
    QVERIFY(waitFor(finished));
    QVERIFY(service.isUnlocked());
    input(window, "SigningKeyName", "alpha");
    input(window, "SigningKeySeed", "abc");
    window->findChild<QCheckBox *>("SigningShowSeed")->setChecked(true);
    QCOMPARE(window->findChild<QLineEdit *>("SigningKeySeed")->echoMode(), QLineEdit::Normal);
    QVERIFY(click(window, "SigningAddKey"));
    QVERIFY(window->findChild<QLineEdit *>("SigningKeySeed")->text().isEmpty());
    QVERIFY(!window->findChild<QCheckBox *>("SigningShowSeed")->isChecked());
    QCOMPARE(window->findChild<QLineEdit *>("SigningKeySeed")->echoMode(), QLineEdit::Password);
    QVERIFY(waitFor(finished));
    QCOMPARE(window->findChild<QListWidget *>("SigningKeyList")->count(), 1);
    QVERIFY(window->findChild<QLabel *>("SigningVaultStatus")->text().contains("1 / 128"));
    QVERIFY(click(window, "SigningLockVault"));
    QVERIFY(waitFor(finished));
    QCOMPARE(window->findChild<QListWidget *>("SigningKeyList")->count(), 0);
    input(window, "SigningMasterPassword", "Another wrong password");
    QVERIFY(click(window, "SigningUnlockVault"));
    QVERIFY(waitFor(finished, false));
    QVERIFY(!service.isUnlocked());
    input(window, "SigningMasterPassword", Master);
    QVERIFY(click(window, "SigningUnlockVault"));
    QVERIFY(waitFor(finished));
    QCOMPARE(service.keyNames(), QStringList{"alpha"});
}

void MavlinkSigningWindowTest::deleteDefaultsToCancelAndRequiresExplicitConfirmation()
{
    QTemporaryDir dir;
    MavAuthKeyService service(dir.filePath("vault.keys"));
    QVERIFY(prepare(service));
    WindowOwner owner(new MavlinkSigningWindow(&service, {}, {}));
    auto *window = owner.get();
    QVERIFY(click(window, "SigningDeleteKey"));
    auto *question = window->findChild<QMessageBox *>("SigningDeleteConfirmation");
    QVERIFY(question);
    QCOMPARE(question->defaultButton(), question->button(QMessageBox::Cancel));
    QVERIFY(question->text().contains("does NOT disable vehicle signing"));
    QVERIFY(question->text().contains("revoke existing"));
    question->button(QMessageBox::Cancel)->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(service.keyNames(), QStringList{"alpha"});
    QVERIFY(!service.busy());
    QVERIFY(click(window, "SigningDeleteKey"));
    question = window->findChild<QMessageBox *>("SigningDeleteConfirmation");
    QVERIFY(question);
    QSignalSpy finished(&service, &MavAuthKeyService::operationFinished);
    question->button(QMessageBox::Yes)->click();
    QVERIFY(waitFor(finished));
    QVERIFY(service.keyNames().isEmpty());
}

void MavlinkSigningWindowTest::firstProtectionNeedsExplicitConsent()
{
    QTemporaryDir dir;
    MavAuthKeyService service(dir.filePath("vault.keys"));
    QVERIFY(prepare(service));
    QObject identity;
    auto endpoint = connection(&identity);
    endpoint.required = false; endpoint.fingerprint.clear();
    int activations = 0;
    WindowOwner owner(new MavlinkSigningWindow(&service, [&] { return QVector<MavlinkSigningWindow::Connection>{endpoint}; },
        [&](const auto &, const QString &, const QByteArray &, QString *) { ++activations; return true; }));
    auto *window = owner.get();
    QVERIFY(click(window, "SigningUseLocally"));
    auto *question = window->findChild<QMessageBox *>("SigningUseConfirmation");
    QVERIFY(question);
    QCOMPARE(question->defaultButton(), question->button(QMessageBox::Cancel));
    QVERIFY(question->text().contains("after restart"));
    QVERIFY(question->text().contains(endpoint.profileId));
    QVERIFY(question->text().contains(endpoint.name));
    QVERIFY(question->text().contains("\"alpha\""));
    QVERIFY(question->text().contains("no supported reset or rekey"));
    QVERIFY(!service.busy());
    question->button(QMessageBox::Cancel)->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(activations, 0);
    QVERIFY(click(window, "SigningUseLocally"));
    question = window->findChild<QMessageBox *>("SigningUseConfirmation");
    QVERIFY(question);
    QSignalSpy finished(&service, &MavAuthKeyService::operationFinished);
    question->button(QMessageBox::Yes)->click();
    QVERIFY(waitFor(finished));
    QCOMPARE(activations, 1);
}

void MavlinkSigningWindowTest::namedKeyActivatesOnlyExactOfflineSnapshot_data()
{
    QTest::addColumn<QString>("change");
    for (const char *change : {"none", "connected", "disappeared", "new-identity", "destroyed-identity",
                               "new-profile", "revision", "fingerprint", "selection", "duplicate"})
        QTest::newRow(change) << QString::fromLatin1(change);
}

void MavlinkSigningWindowTest::namedKeyActivatesOnlyExactOfflineSnapshot()
{
    QFETCH(QString, change);
    QTemporaryDir dir;
    MavAuthKeyService service(dir.filePath("vault.keys"));
    QVERIFY(prepare(service));
    QPointer<QObject> identity = new QObject(this);
    QObject replacement;
    auto endpoint = connection(identity.data());
    QVector<MavlinkSigningWindow::Connection> connections{endpoint};
    int activations = 0;
    WindowOwner owner(new MavlinkSigningWindow(&service, [&] { return connections; },
        [&](const auto &received, const QString &name, const QByteArray &key, QString *) {
            if (received.identity != identity || received.profileId != endpoint.profileId
                || received.linkId != endpoint.linkId || received.connected
                || QThread::currentThread() != service.thread()
                || name != "alpha" || key != QCryptographicHash::hash("abc", QCryptographicHash::Sha256))
                return false;
            ++activations; return true;
        }));
    QSignalSpy finished(&service, &MavAuthKeyService::operationFinished);
    QVERIFY(click(owner.get(), "SigningUseLocally"));
    QVERIFY(service.busy());
    if (change == "connected") connections[0].connected = true;
    else if (change == "disappeared") connections.clear();
    else if (change == "new-identity") connections[0].identity = &replacement;
    else if (change == "destroyed-identity") delete identity.data();
    else if (change == "new-profile") connections[0].profileId += "-other";
    else if (change == "revision") ++connections[0].revision;
    else if (change == "fingerprint") connections[0].fingerprint = QString(64, 'b');
    else if (change == "selection") owner.get()->findChild<QListWidget *>("SigningKeyList")->setCurrentRow(-1);
    else if (change == "duplicate") connections.append(connections.first());
    QVERIFY(waitFor(finished));
    QCOMPARE(activations, change == "none" ? 1 : 0);
    if (change != "none")
        QVERIFY(owner.get()->findChild<QLabel *>("SigningOperationStatus")->text().contains("cancelled"));
    delete identity.data();
}

void MavlinkSigningWindowTest::closeCancelsPendingActivationAndKeepsService()
{
    QTemporaryDir dir;
    MavAuthKeyService service(dir.filePath("vault.keys"));
    QVERIFY(prepare(service));
    QObject identity;
    const auto endpoint = connection(&identity);
    int activations = 0;
    WindowOwner owner(new MavlinkSigningWindow(&service, [&] { return QVector<MavlinkSigningWindow::Connection>{endpoint}; },
        [&](const auto &, const QString &, const QByteArray &, QString *) { ++activations; return true; }));
    QSignalSpy finished(&service, &MavAuthKeyService::operationFinished);
    QVERIFY(click(owner.get(), "SigningUseLocally"));
    owner.get()->close();
    QVERIFY(!owner.window || owner.window->isClosing());
    QVERIFY(waitFor(finished));
    QCOMPARE(activations, 0);
    QTRY_VERIFY(owner.window.isNull());
    QVERIFY(service.isUnlocked());
    QCOMPARE(service.keyNames(), QStringList{"alpha"});
}

void MavlinkSigningWindowTest::providerAndActivatorMayDestroyWindow()
{
    QTemporaryDir dir;
    MavAuthKeyService service(dir.filePath("vault.keys"));
    QVERIFY(prepare(service));
    QObject identity;
    const auto endpoint = connection(&identity);
    for (const bool deleteInProvider : {false, true}) {
        bool armed = false;
        QPointer<MavlinkSigningWindow> window;
        WindowOwner owner(new MavlinkSigningWindow(&service, [&] {
            if (armed && deleteInProvider) delete window.data();
            return QVector<MavlinkSigningWindow::Connection>{endpoint};
        }, [&](const auto &, const QString &, const QByteArray &, QString *) {
            delete window.data(); return true;
        }));
        window = owner.get();
        QSignalSpy finished(&service, &MavAuthKeyService::operationFinished);
        QVERIFY(click(window, "SigningUseLocally"));
        armed = true;
        QVERIFY(waitFor(finished));
        QVERIFY(window.isNull());
        QVERIFY(service.isUnlocked());
    }
}

void MavlinkSigningWindowTest::metadataIsPlainTextAndSelectionSurvivesRefresh()
{
    QTemporaryDir dir;
    MavAuthKeyService service(dir.filePath("vault.keys"));
    QObject firstIdentity, secondIdentity;
    auto first = connection(&firstIdentity);
    auto second = connection(&secondIdentity);
    second.linkId = 22; second.profileId += "-second"; second.name = "Other connection";
    second.error = "<b>not markup</b>";
    second.signedReceived = 17;
    QVector<MavlinkSigningWindow::Connection> connections{first, second};
    WindowOwner owner(new MavlinkSigningWindow(&service, [&] { return connections; }, {}));
    auto *selector = owner.get()->findChild<QComboBox *>("SigningConnection");
    selector->setCurrentIndex(1);
    std::swap(connections[0], connections[1]);
    QTRY_VERIFY(selector->currentText().startsWith("Other connection ["));
    QTRY_COMPARE(selector->currentIndex(), 0);
    auto *status = owner.get()->findChild<QLabel *>("SigningConnectionStatus");
    QCOMPARE(status->textFormat(), Qt::PlainText);
    QVERIFY(status->text().contains("<b>not markup</b>"));
    QVERIFY(status->text().contains("Authenticated packets (shared key): 17"));
    for (int i = owner.get()->metaObject()->methodOffset(); i < owner.get()->metaObject()->methodCount(); ++i)
        QVERIFY(!owner.get()->metaObject()->method(i).parameterTypes().contains("QByteArray"));
}

QTEST_MAIN(MavlinkSigningWindowTest)
#include "test_mavlinksigningwindow.moc"
