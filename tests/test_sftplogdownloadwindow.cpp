#include "ui/SftpLogDownloadWindow.h"

#include <QtTest>

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIODevice>
#include <QLineEdit>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>

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
    if (!window)
        return nullptr;
    const auto dialogs = window->findChildren<T *>(
        QString::fromLatin1(name), Qt::FindChildrenRecursively);
    for (T *dialog : dialogs) {
        if (dialog->isVisible())
            return dialog;
    }
    return nullptr;
}

struct FakeState
{
    mutable QMutex mutex;
    QVector<SftpLogEntry> entries;
    QString fingerprint = QStringLiteral("SHA256:test-host-key");
    QStringList trustedFingerprints;
    QStringList passwords;
    QStringList deletedPaths;
    QThread *factoryThread = nullptr;
    QThread *methodThread = nullptr;
    QThread *destructorThread = nullptr;
    std::atomic_bool downloadStarted{false};
    std::atomic_bool blockDownload{false};
    std::atomic_bool connectStarted{false};
    std::atomic_bool blockConnect{false};
    std::atomic_int stopCalls{0};
    std::atomic_int connectCalls{0};
};

class FakeSession final : public SftpLogSession
{
public:
    explicit FakeSession(std::shared_ptr<FakeState> state)
        : m_state(std::move(state))
    {
    }

    ~FakeSession() override
    {
        QMutexLocker locker(&m_state->mutex);
        m_state->destructorThread = QThread::currentThread();
    }

    bool isConnected() const override { return m_connected; }

    bool connect(const SftpLogConnection &connection,
                 const QString &trustedFingerprint,
                 SshHostKeyChallenge *challenge, QString *error,
                 Cancel cancel) override
    {
        observeThread();
        ++m_state->connectCalls;
        {
            QMutexLocker locker(&m_state->mutex);
            m_state->trustedFingerprints.append(trustedFingerprint);
            m_state->passwords.append(connection.password);
        }
        m_state->connectStarted.store(true, std::memory_order_relaxed);
        QElapsedTimer timer;
        timer.start();
        while (m_state->blockConnect.load(std::memory_order_relaxed)
               && !(cancel && cancel()) && timer.elapsed() < 10000) {
            QThread::msleep(1);
        }
        if (cancel && cancel()) {
            if (error)
                *error = QStringLiteral("cancelled");
            return false;
        }
        if (!fingerprintsEqual(trustedFingerprint, m_state->fingerprint)) {
            if (challenge) {
                challenge->host = connection.host;
                challenge->port = connection.port;
                challenge->algorithm = QStringLiteral("ssh-ed25519");
                challenge->keyLength = 256;
                challenge->expectedFingerprint = trustedFingerprint;
                challenge->presentedFingerprint = m_state->fingerprint;
            }
            if (error)
                *error = QStringLiteral("host key not trusted");
            m_connected = false;
            return false;
        }
        m_connected = true;
        if (error)
            error->clear();
        return true;
    }

    bool listLogs(const QString &, QVector<SftpLogEntry> *entries,
                  QString *error, Cancel cancel) override
    {
        observeThread();
        if (cancel && cancel()) {
            if (error)
                *error = QStringLiteral("cancelled");
            return false;
        }
        QMutexLocker locker(&m_state->mutex);
        *entries = m_state->entries;
        if (error)
            error->clear();
        return true;
    }

    bool download(const SftpLogEntry &entry, QIODevice *destination,
                  qint64 *copied, QString *error, Cancel cancel,
                  Progress progress) override
    {
        observeThread();
        m_state->downloadStarted.store(true, std::memory_order_relaxed);
        QElapsedTimer timer;
        timer.start();
        while (m_state->blockDownload.load(std::memory_order_relaxed)
               && !(cancel && cancel()) && timer.elapsed() < 10000) {
            QThread::msleep(1);
        }
        if (cancel && cancel()) {
            if (error)
                *error = QStringLiteral("cancelled");
            return false;
        }
        QByteArray bytes(qMax<qint64>(1, qMin<qint64>(entry.length, 4096)), '\x42');
        if (!destination || destination->write(bytes) != bytes.size()) {
            if (error)
                *error = QStringLiteral("write failed");
            return false;
        }
        if (copied)
            *copied = bytes.size();
        if (progress)
            progress(bytes.size());
        if (error)
            error->clear();
        return true;
    }

    bool remove(const SftpLogEntry &expected, QString *error,
                Cancel cancel) override
    {
        observeThread();
        if (cancel && cancel()) {
            if (error)
                *error = QStringLiteral("cancelled");
            return false;
        }
        QMutexLocker locker(&m_state->mutex);
        for (int index = 0; index < m_state->entries.size(); ++index) {
            const SftpLogEntry &current = m_state->entries.at(index);
            if (current.remotePath() != expected.remotePath())
                continue;
            if (current.length != expected.length
                || current.lastWriteTimeUtc != expected.lastWriteTimeUtc) {
                if (error)
                    *error = QStringLiteral("remote log changed");
                return false;
            }
            m_state->deletedPaths.append(expected.remotePath());
            m_state->entries.remove(index);
            if (error)
                error->clear();
            return true;
        }
        if (error)
            *error = QStringLiteral("missing");
        return false;
    }

    void stop() override
    {
        observeThread();
        ++m_state->stopCalls;
        m_connected = false;
    }

private:
    void observeThread() const
    {
        QMutexLocker locker(&m_state->mutex);
        if (!m_state->methodThread)
            m_state->methodThread = QThread::currentThread();
        else
            Q_ASSERT(m_state->methodThread == QThread::currentThread());
    }

    std::shared_ptr<FakeState> m_state;
    bool m_connected = false;
};

SftpLogSessionFactory factoryFor(const std::shared_ptr<FakeState> &state)
{
    return [state]() -> std::unique_ptr<SftpLogSession> {
        {
            QMutexLocker locker(&state->mutex);
            state->factoryThread = QThread::currentThread();
        }
        return std::make_unique<FakeSession>(state);
    };
}

SftpLogEntry entry(const QString &name, qint64 length = 4)
{
    return {QStringLiteral("/var/log/dataflash"), name, length,
            QDateTime::fromString(QStringLiteral("2026-08-22T10:00:00Z"),
                                  Qt::ISODate)};
}

void configure(SftpLogDownloadWindow *window, const QString &password)
{
    control<QLineEdit>(window, "SftpHost")->setText(QStringLiteral("companion.local"));
    control<QLineEdit>(window, "SftpPort")->setText(QStringLiteral("22"));
    control<QLineEdit>(window, "SftpUsername")->setText(QStringLiteral("pilot"));
    control<QLineEdit>(window, "SftpPassword")->setText(password);
    control<QLineEdit>(window, "SftpRemoteDirectory")->setText(
        QStringLiteral("/var/log/dataflash"));
    control<QCheckBox>(window, "SftpCreateKmlCheckBox")->setChecked(false);
}

void trustCurrentHost(SftpLogDownloadWindow *window)
{
    auto *prompt = visibleDialog<QMessageBox>(window, "SftpHostKeyConfirmation");
    QVERIFY(prompt);
    QCOMPARE(prompt->defaultButton(),
             qobject_cast<QPushButton *>(prompt->button(QMessageBox::Cancel)));
    control<QPushButton>(prompt, "SftpTrustHostKeyButton")->click();
}

void refreshAndTrust(SftpLogDownloadWindow *window)
{
    control<QPushButton>(window, "SftpRefreshButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(visibleDialog<QMessageBox>(
        window, "SftpHostKeyConfirmation") != nullptr, 5000);
    trustCurrentHost(window);
    QTRY_VERIFY_WITH_TIMEOUT(!window->busy(), 10000);
}

void chooseDirectory(QFileDialog *dialog, const QString &path)
{
    QVERIFY(dialog);
    dialog->setDirectory(path);
    if (QLineEdit *edit = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
        edit->setText(path);
    dialog->selectFile(path);
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
}
} // namespace

class SftpLogDownloadWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void matchesMissionPlannerSurface();
    void sessionLivesEntirelyOnOneDedicatedThreadAndIsReused();
    void unknownAndChangedHostKeysArePinnedOnlyAfterDefaultCancelConsent();
    void changedHostKeyRequiresSeparateExplicitReplacement();
    void downloadAndDeleteUseFrozenRowsAndTruthfulReceipts();
    void changingFieldsInvalidatesOldListing();
    void passwordClearMayDeleteWindow();
    void cancellingCloseRestoresLateHostKeyConsent();
    void closeBusyIsDefaultCancelThenWaitsForWorker();
};

void SftpLogDownloadWindowTest::init()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

void SftpLogDownloadWindowTest::changedHostKeyRequiresSeparateExplicitReplacement()
{
    auto state = std::make_shared<FakeState>();
    state->entries = {entry(QStringLiteral("one.bin"))};
    const QString setting = SftpLogSession::trustedKeySettingName(
        QStringLiteral("companion.local"), 22);
    QSettings settings;
    settings.setValue(setting, QStringLiteral("SHA256:old-host-key"));
    settings.sync();

    SftpLogDownloadWindow window(nullptr, factoryFor(state));
    window.show();
    configure(&window, QStringLiteral("secret"));
    control<QPushButton>(&window, "SftpRefreshButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(visibleDialog<QMessageBox>(
        &window, "SftpHostKeyConfirmation") != nullptr, 5000);
    auto *prompt = visibleDialog<QMessageBox>(&window, "SftpHostKeyConfirmation");
    QVERIFY(prompt->text().contains(QStringLiteral("CHANGED")));
    qobject_cast<QPushButton *>(prompt->button(QMessageBox::Cancel))->click();
    QTRY_VERIFY(!window.busy());
    QCOMPARE(settings.value(setting).toString(), QStringLiteral("SHA256:old-host-key"));

    control<QLineEdit>(&window, "SftpPassword")->setText(QStringLiteral("secret"));
    control<QPushButton>(&window, "SftpRefreshButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(visibleDialog<QMessageBox>(
        &window, "SftpHostKeyConfirmation") != nullptr, 5000);
    prompt = visibleDialog<QMessageBox>(&window, "SftpHostKeyConfirmation");
    QVERIFY(prompt->text().contains(QStringLiteral("CHANGED")));
    control<QPushButton>(prompt, "SftpTrustHostKeyButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 10000);
    settings.sync();
    QVERIFY(SftpLogSession::fingerprintsEqual(settings.value(setting).toString(),
                                               state->fingerprint));
    QCOMPARE(control<QTableWidget>(&window, "RemoteLogGrid")->rowCount(), 1);
}

void SftpLogDownloadWindowTest::matchesMissionPlannerSurface()
{
    auto state = std::make_shared<FakeState>();
    QPointer<SftpLogDownloadWindow> window =
        new SftpLogDownloadWindow(nullptr, factoryFor(state));
    QCOMPARE(window->objectName(), QStringLiteral("SftpLogDownloadWindow"));
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QCOMPARE(window->size(), QSize(900, 680));
    QCOMPARE(window->minimumSize(), QSize(760, 560));
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QVERIFY(control<QLineEdit>(window, "SftpPassword")->echoMode()
            == QLineEdit::Password);
    QCOMPARE(control<QTableWidget>(window, "RemoteLogGrid")->columnCount(), 4);
    QVERIFY(!window->busy());
    delete window;
    QVERIFY(window.isNull());
}

void SftpLogDownloadWindowTest::sessionLivesEntirelyOnOneDedicatedThreadAndIsReused()
{
    auto state = std::make_shared<FakeState>();
    state->entries = {entry(QStringLiteral("one.bin"))};
    QSettings settings;
    settings.setValue(SftpLogSession::trustedKeySettingName(
        QStringLiteral("companion.local"), 22), state->fingerprint);
    settings.sync();
    auto *service = new SftpLogDownloadService(factoryFor(state));
    SftpLogConnection connection{QStringLiteral("companion.local"), 22,
        QStringLiteral("pilot"), QStringLiteral("secret")};
    quint64 refusedId = 99;
    QString error;
    QVERIFY(!service->download(connection, {}, QString(), false,
                               &refusedId, &error));
    QCOMPARE(refusedId, quint64(0));
    QSignalSpy finished(service, &SftpLogDownloadService::operationFinished);
    quint64 first = 0;
    QVERIFY(service->refresh(connection, QStringLiteral("/var/log/dataflash"),
                             &first, &error));
    QVERIFY(first != 0);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QVERIFY(service->lastListResult().success);
    QVERIFY(service->connected());
    quint64 second = 0;
    connection.password = QStringLiteral("consumed-on-reuse");
    QVERIFY(service->refresh(connection, QStringLiteral("/var/log/dataflash"),
                             &second, &error));
    QVERIFY(second != 0 && second != first);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
    QCOMPARE(state->connectCalls.load(), 1);

    delete service;
    QMutexLocker locker(&state->mutex);
    QVERIFY(state->factoryThread);
    QVERIFY(state->factoryThread != QThread::currentThread());
    QCOMPARE(state->methodThread, state->factoryThread);
    QCOMPARE(state->destructorThread, state->factoryThread);
}

void SftpLogDownloadWindowTest::unknownAndChangedHostKeysArePinnedOnlyAfterDefaultCancelConsent()
{
    auto state = std::make_shared<FakeState>();
    state->entries = {entry(QStringLiteral("one.bin"))};
    SftpLogDownloadWindow window(nullptr, factoryFor(state));
    window.show();
    configure(&window, QStringLiteral("top-secret"));
    control<QPushButton>(&window, "SftpRefreshButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(visibleDialog<QMessageBox>(
        &window, "SftpHostKeyConfirmation") != nullptr, 5000);
    auto *prompt = visibleDialog<QMessageBox>(&window, "SftpHostKeyConfirmation");
    QVERIFY(prompt->text().contains(QStringLiteral("before password authentication")));
    qobject_cast<QPushButton *>(prompt->button(QMessageBox::Cancel))->click();
    QTRY_VERIFY(!window.busy());
    QCOMPARE(control<QTableWidget>(&window, "RemoteLogGrid")->rowCount(), 0);
    QVERIFY(control<QLineEdit>(&window, "SftpPassword")->text().isEmpty());
    QSettings settings;
    QVERIFY(settings.value(SftpLogSession::trustedKeySettingName(
        QStringLiteral("companion.local"), 22)).toString().isEmpty());

    control<QLineEdit>(&window, "SftpPassword")->setText(QStringLiteral("top-secret"));
    refreshAndTrust(&window);
    QCOMPARE(control<QTableWidget>(&window, "RemoteLogGrid")->rowCount(), 1);
    QVERIFY(SftpLogSession::fingerprintsEqual(
        settings.value(SftpLogSession::trustedKeySettingName(
            QStringLiteral("companion.local"), 22)).toString(),
        state->fingerprint));
    QCOMPARE(state->connectCalls.load(), 3);
    {
        QMutexLocker locker(&state->mutex);
        QCOMPARE(state->passwords, QStringList({QStringLiteral("top-secret"),
                                                QStringLiteral("top-secret"),
                                                QStringLiteral("top-secret")}));
    }
    QVERIFY(!control<QPlainTextEdit>(&window, "SftpActivityLog")
                 ->toPlainText().contains(QStringLiteral("top-secret")));

    // Even reuse consumes the UI password and does not authenticate again.
    control<QLineEdit>(&window, "SftpPassword")->setText(QStringLiteral("new-secret"));
    control<QPushButton>(&window, "SftpRefreshButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 5000);
    QVERIFY(control<QLineEdit>(&window, "SftpPassword")->text().isEmpty());
    QCOMPARE(state->connectCalls.load(), 3);
}

void SftpLogDownloadWindowTest::downloadAndDeleteUseFrozenRowsAndTruthfulReceipts()
{
    QTemporaryDir destination;
    QVERIFY(destination.isValid());
    auto state = std::make_shared<FakeState>();
    state->entries = {entry(QStringLiteral("one.bin"), 4),
                      entry(QStringLiteral("two.bin"), 7)};
    SftpLogDownloadWindow window(nullptr, factoryFor(state));
    window.show();
    configure(&window, QStringLiteral("secret"));
    refreshAndTrust(&window);
    auto *grid = control<QTableWidget>(&window, "RemoteLogGrid");
    QCOMPARE(grid->rowCount(), 2);
    QCOMPARE(grid->item(0, 1)->text(), QStringLiteral("one.bin"));
    QTRY_VERIFY(grid->rowHeight(0) > 0);
    QTRY_VERIFY(grid->visualItemRect(grid->item(0, 1)).height() > 0);
    grid->item(0, 0)->setCheckState(Qt::Checked);

    control<QPushButton>(&window, "SftpDownloadSelectedButton")->click();
    auto *picker = visibleDialog<QFileDialog>(&window, "SftpDownloadDirectoryDialog");
    QVERIFY(picker);
    const QString frozenName = grid->item(0, 1)->text();
    chooseDirectory(picker, destination.path());
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 15000);
    const QStringList files = QDir(destination.path()).entryList(
        QDir::Files | QDir::NoDotAndDotDot);
    QVERIFY(!files.isEmpty());
    QVERIFY(control<QPlainTextEdit>(&window, "SftpActivityLog")
                ->toPlainText().contains(QStringLiteral("Published")));

    grid->item(0, 0)->setCheckState(Qt::Checked);
    control<QPushButton>(&window, "SftpDeleteSelectedButton")->click();
    auto *prompt = visibleDialog<QDialog>(&window, "SftpDeleteConfirmation");
    QVERIFY(prompt);
    auto *plan = control<QPlainTextEdit>(prompt, "SftpDeletePlan");
    QVERIFY(plan->toPlainText().contains(QStringLiteral("/var/log/dataflash/") + frozenName));
    auto *buttons = prompt->findChild<QDialogButtonBox *>();
    QVERIFY(buttons && buttons->button(QDialogButtonBox::Cancel)->isDefault());
    buttons->button(QDialogButtonBox::Cancel)->click();
    QTRY_VERIFY(!window.busy());
    {
        QMutexLocker locker(&state->mutex);
        QVERIFY(state->deletedPaths.isEmpty());
    }

    grid->item(0, 0)->setCheckState(Qt::Checked);
    control<QPushButton>(&window, "SftpDeleteSelectedButton")->click();
    prompt = visibleDialog<QDialog>(&window, "SftpDeleteConfirmation");
    QVERIFY(prompt);
    control<QPushButton>(prompt, "SftpDeleteConfirmButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 10000);
    QCOMPARE(grid->rowCount(), 1);
    {
        QMutexLocker locker(&state->mutex);
        QCOMPARE(state->deletedPaths.size(), 1);
        QCOMPARE(state->deletedPaths.first(),
                 QStringLiteral("/var/log/dataflash/") + frozenName);
    }
}

void SftpLogDownloadWindowTest::changingFieldsInvalidatesOldListing()
{
    auto state = std::make_shared<FakeState>();
    state->entries = {entry(QStringLiteral("one.bin"))};
    SftpLogDownloadWindow window(nullptr, factoryFor(state));
    window.show();
    configure(&window, QStringLiteral("secret"));
    refreshAndTrust(&window);
    QCOMPARE(control<QTableWidget>(&window, "RemoteLogGrid")->rowCount(), 1);
    control<QLineEdit>(&window, "SftpRemoteDirectory")->setText(QStringLiteral("/other"));
    QCOMPARE(control<QTableWidget>(&window, "RemoteLogGrid")->rowCount(), 0);
    QVERIFY(!control<QPushButton>(&window, "SftpDownloadAllButton")->isEnabled());
    QVERIFY(!control<QPushButton>(&window, "SftpDeleteAllButton")->isEnabled());
}

void SftpLogDownloadWindowTest::passwordClearMayDeleteWindow()
{
    auto state = std::make_shared<FakeState>();
    QPointer<SftpLogDownloadWindow> window =
        new SftpLogDownloadWindow(nullptr, factoryFor(state));
    window->show();
    configure(window, QString());
    QLineEdit *password = control<QLineEdit>(window, "SftpPassword");
    password->setFocus();
    QTest::keyClicks(password, QStringLiteral("transient-secret"));
    QVERIFY(password->isUndoAvailable());
    control<QLineEdit>(window, "SftpPort")->setText(QStringLiteral("0"));
    bool emptySignalObserved = false;
    connect(password, &QLineEdit::textChanged, password,
            [window, &emptySignalObserved](const QString &value) {
        if (value.isEmpty() && window) {
            emptySignalObserved = true;
            delete window.data();
        }
    });
    control<QPushButton>(window, "SftpRefreshButton")->click();
    QVERIFY(window);
    QVERIFY(!emptySignalObserved);
    QVERIFY(password->text().isEmpty());
    QVERIFY(!password->isUndoAvailable());
    password->setFocus();
    QTest::keyClick(password, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(password->text().isEmpty());
    delete window.data();
    QVERIFY(window.isNull());
}

void SftpLogDownloadWindowTest::cancellingCloseRestoresLateHostKeyConsent()
{
    auto state = std::make_shared<FakeState>();
    state->entries = {entry(QStringLiteral("one.bin"))};
    state->blockConnect.store(true, std::memory_order_relaxed);
    SftpLogDownloadWindow window(nullptr, factoryFor(state));
    window.show();
    configure(&window, QStringLiteral("secret"));
    control<QPushButton>(&window, "SftpRefreshButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        state->connectStarted.load(std::memory_order_relaxed), 5000);

    window.close();
    auto *closePrompt = visibleDialog<QMessageBox>(
        &window, "SftpCloseConfirmation");
    QVERIFY(closePrompt);
    state->blockConnect.store(false, std::memory_order_relaxed);
    QTRY_VERIFY_WITH_TIMEOUT(
        window.serviceForTesting()->awaitingHostKeyTrust(), 5000);
    QVERIFY(!visibleDialog<QMessageBox>(&window, "SftpHostKeyConfirmation"));

    qobject_cast<QPushButton *>(closePrompt->button(QMessageBox::Cancel))->click();
    QTRY_VERIFY_WITH_TIMEOUT(visibleDialog<QMessageBox>(
        &window, "SftpHostKeyConfirmation") != nullptr, 5000);
    auto *hostPrompt = visibleDialog<QMessageBox>(
        &window, "SftpHostKeyConfirmation");
    QVERIFY(hostPrompt);
    QCOMPARE(hostPrompt->defaultButton(),
             qobject_cast<QPushButton *>(hostPrompt->button(QMessageBox::Cancel)));
    qobject_cast<QPushButton *>(hostPrompt->button(QMessageBox::Cancel))->click();
    QTRY_VERIFY(!window.busy());
}

void SftpLogDownloadWindowTest::closeBusyIsDefaultCancelThenWaitsForWorker()
{
    QTemporaryDir destination;
    QVERIFY(destination.isValid());
    auto state = std::make_shared<FakeState>();
    state->entries = {entry(QStringLiteral("active.bin"), 4096)};
    QPointer<SftpLogDownloadWindow> window =
        new SftpLogDownloadWindow(nullptr, factoryFor(state));
    window->show();
    configure(window, QStringLiteral("secret"));
    refreshAndTrust(window);
    control<QTableWidget>(window, "RemoteLogGrid")->item(0, 0)
        ->setCheckState(Qt::Checked);
    state->blockDownload.store(true, std::memory_order_relaxed);
    control<QPushButton>(window, "SftpDownloadSelectedButton")->click();
    auto *picker = visibleDialog<QFileDialog>(window, "SftpDownloadDirectoryDialog");
    QVERIFY(picker);
    chooseDirectory(picker, destination.path());
    QTRY_VERIFY_WITH_TIMEOUT(state->downloadStarted.load(std::memory_order_relaxed), 5000);
    QSignalSpy resolved(window, &SftpLogDownloadWindow::closeResolved);
    window->close();
    auto *prompt = visibleDialog<QMessageBox>(window, "SftpCloseConfirmation");
    QVERIFY(prompt);
    QCOMPARE(prompt->defaultButton(),
             qobject_cast<QPushButton *>(prompt->button(QMessageBox::Cancel)));
    qobject_cast<QPushButton *>(prompt->button(QMessageBox::Cancel))->click();
    QTRY_VERIFY(!window->isClosing());
    QCOMPARE(resolved.size(), 1);
    QCOMPARE(resolved.at(0).at(0).toBool(), false);

    window->close();
    prompt = visibleDialog<QMessageBox>(window, "SftpCloseConfirmation");
    QVERIFY(prompt);
    qobject_cast<QPushButton *>(prompt->button(QMessageBox::Yes))->click();
    QTRY_VERIFY_WITH_TIMEOUT(window.isNull(), 15000);
    QVERIFY(resolved.size() >= 2);
    QCOMPARE(resolved.last().at(0).toBool(), true);
    QMutexLocker locker(&state->mutex);
    QCOMPARE(state->destructorThread, state->factoryThread);
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("APMPlannerSftpTest"));
    QCoreApplication::setApplicationName(QStringLiteral("SftpLogDownloadWindowTest"));
    QTemporaryDir settingsRoot;
    if (!settingsRoot.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsRoot.path());
    SftpLogDownloadWindowTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_sftplogdownloadwindow.moc"
