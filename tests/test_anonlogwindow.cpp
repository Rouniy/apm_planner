#include <QtTest>

#include "ui/AnonLogWindow.h"
#include "ui/Loghandling/LogAnonymizeService.h"
#include "ui/Loghandling/LogAnonymizer.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

namespace {

const QByteArray kSmallTextLog(
    "FMT, 42, 11, GPS, LL, Lat,Lng\r\n"
    "GPS,10,20\r\n");

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(bytes) == bytes.size()
        && file.flush();
}

void setRequest(
    AnonLogWindow *window, const QString &input, const QString &output,
    const QString &latitude = QStringLiteral("1.5"),
    const QString &longitude = QStringLiteral("-2.25"))
{
    window->findChild<QLineEdit *>(
        QStringLiteral("AnonLogInputEdit"))->setText(input);
    window->findChild<QLineEdit *>(
        QStringLiteral("AnonLogOutputEdit"))->setText(output);
    window->findChild<QLineEdit *>(
        QStringLiteral("AnonLogLatitudeOffsetEdit"))->setText(latitude);
    window->findChild<QLineEdit *>(
        QStringLiteral("AnonLogLongitudeOffsetEdit"))->setText(longitude);
}

} // namespace

class AnonLogWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void modelessSurfaceAndLocalMistakesDoNotCreateOutput();
    void defaultConfirmationIsPrivacyExplicitAndCancelSafe();
    void confirmationUsesImmutableFieldsAndRealServiceCompletes();
    void deletionDuringConfirmationDoesNotStart();
    void cancelSignalMayDeleteObserver();
    void closeDetachesAndReopenCanExplicitlyCancelExactToken();
};

void AnonLogWindowTest::modelessSurfaceAndLocalMistakesDoNotCreateOutput()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString badInput = directory.filePath(QStringLiteral("flight.txt"));
    const QString badOutput = directory.filePath(QStringLiteral("flight-anon.txt"));
    QVERIFY(writeFile(badInput, kSmallTextLog));

    LogAnonymizeService service(this);
    QWidget owner;
    owner.setGeometry(100, 100, 1000, 760);
    bool confirmed = false;
    auto *window = new AnonLogWindow(
        &service, &owner,
        [&confirmed](QWidget *, const QString &, const QString &) {
            confirmed = true;
            return true;
        });
    QPointer<AnonLogWindow> guardedWindow(window);
    window->show();
    QCoreApplication::processEvents();
    QVERIFY(window->findChild<QPlainTextEdit *>(QStringLiteral("AnonLogResult"))->height() >= 110);

    QCOMPARE(window->objectName(), QStringLiteral("AnonLogWindow"));
    QCOMPARE(window->windowTitle(), QStringLiteral("Anon Log (Beta)"));
    QCOMPARE(window->windowType(), Qt::Window);
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QCOMPARE(window->service(), &service);
    QCOMPARE(window->observedToken(), quint64(0));

    const QStringList names = {
        QStringLiteral("AnonLogInputEdit"),
        QStringLiteral("AnonLogOutputEdit"),
        QStringLiteral("AnonLogLatitudeOffsetEdit"),
        QStringLiteral("AnonLogLongitudeOffsetEdit"),
        QStringLiteral("BrowseAnonLogInputButton"),
        QStringLiteral("BrowseAnonLogOutputButton"),
        QStringLiteral("AnonLogPrivacyWarning"),
        QStringLiteral("AnonLogStatus"),
        QStringLiteral("AnonLogProgress"),
        QStringLiteral("AnonLogResult"),
        QStringLiteral("StartAnonLogButton"),
        QStringLiteral("CancelAnonLogButton"),
        QStringLiteral("CloseAnonLogButton")
    };
    for (const QString &name : names) {
        QVERIFY2(window->findChild<QObject *>(name), qPrintable(name));
    }
    QLabel *const warning = window->findChild<QLabel *>(
        QStringLiteral("AnonLogPrivacyWarning"));
    QVERIFY(warning->text().contains(QStringLiteral("Beta")));
    QVERIFY(warning->text().contains(QStringLiteral("Serial identifiers")));
    QVERIFY(warning->text().contains(QStringLiteral("relative tracks")));
    QVERIFY(warning->text().contains(QStringLiteral("unrecognized")));
    QVERIFY(window->findChild<QPushButton *>(
        QStringLiteral("StartAnonLogButton"))->isEnabled());
    QVERIFY(!window->findChild<QPushButton *>(
        QStringLiteral("CancelAnonLogButton"))->isEnabled());

    setRequest(window, badInput, badOutput);
    window->findChild<QPushButton *>(
        QStringLiteral("StartAnonLogButton"))->click();
    QVERIFY(!confirmed);
    QVERIFY(!service.busy());
    QVERIFY(!QFileInfo::exists(badOutput));
    QVERIFY(window->findChild<QLabel *>(QStringLiteral("AnonLogStatus"))
                ->text().contains(QStringLiteral(".bin")));

    window->show();
    window->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(guardedWindow.isNull());
    QCOMPARE(service.parent(), this);
}

void AnonLogWindowTest::defaultConfirmationIsPrivacyExplicitAndCancelSafe()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    const QString output = directory.filePath(QStringLiteral("flight-anon.log"));
    QVERIFY(writeFile(input, kSmallTextLog));

    LogAnonymizeService service(this);
    auto *window = new AnonLogWindow(&service);
    setRequest(window, input, output);

    bool inspected = false;
    QTimer::singleShot(0, qApp, [&inspected, input, output]() {
        auto *dialog = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        inspected = dialog->objectName() == QStringLiteral("AnonLogConfirmation")
            && dialog->windowTitle() == QStringLiteral("Confirm Anon Log (Beta)")
            && dialog->text().contains(input)
            && dialog->text().contains(output)
            && dialog->text().contains(QStringLiteral("1.500000° latitude"))
            && dialog->text().contains(QStringLiteral("-2.250000° longitude"))
            && dialog->text().contains(QStringLiteral("does NOT fully sanitize"))
            && dialog->text().contains(QStringLiteral("Serial identifiers"))
            && dialog->defaultButton() == dialog->button(QMessageBox::Cancel)
            && dialog->escapeButton() == dialog->button(QMessageBox::Cancel);
        dialog->button(QMessageBox::Cancel)->click();
    });
    window->findChild<QPushButton *>(
        QStringLiteral("StartAnonLogButton"))->click();
    QVERIFY(inspected);
    QVERIFY(!service.busy());
    QCOMPARE(service.token(), quint64(0));
    QVERIFY(!QFileInfo::exists(output));
    delete window;
}

void AnonLogWindowTest::confirmationUsesImmutableFieldsAndRealServiceCompletes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    const QString output = directory.filePath(QStringLiteral("flight-anon.log"));
    QVERIFY(writeFile(input, kSmallTextLog));

    LogAnonymizeService service(this);
    bool mutate = true;
    QString confirmedMessage;
    auto *window = new AnonLogWindow(
        &service, nullptr,
        [&mutate, &confirmedMessage](
            QWidget *owner, const QString &, const QString &message) {
            confirmedMessage = message;
            if (mutate) {
                owner->findChild<QLineEdit *>(
                    QStringLiteral("AnonLogLongitudeOffsetEdit"))
                    ->setText(QStringLiteral("99"));
            }
            return true;
        });
    setRequest(window, input, output, QString(), QString());

    window->findChild<QPushButton *>(
        QStringLiteral("StartAnonLogButton"))->click();
    QVERIFY(confirmedMessage.contains(QStringLiteral("Coordinate offsets:")));
    QVERIFY(!service.busy());
    QCOMPARE(service.token(), quint64(0));
    QVERIFY(!QFileInfo::exists(output));
    QVERIFY(window->findChild<QLabel *>(QStringLiteral("AnonLogStatus"))
                ->text().contains(QStringLiteral("changed while confirmation")));

    mutate = false;
    window->findChild<QLineEdit *>(
        QStringLiteral("AnonLogLongitudeOffsetEdit"))->clear();
    window->findChild<QPushButton *>(
        QStringLiteral("StartAnonLogButton"))->click();
    QVERIFY(service.busy());
    QVERIFY(window->observedToken() != 0);
    QVERIFY(window->findChild<QPushButton *>(
        QStringLiteral("CancelAnonLogButton"))->isEnabled());

    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY2(service.result().success, qPrintable(service.result().error));
    QVERIFY(QFileInfo::exists(output));
    QCOMPARE(service.result().inputBytes, QFileInfo(input).size());
    QCOMPARE(service.result().outputBytes, QFileInfo(output).size());
    QVERIFY(service.result().patchedValues >= 2);
    QLabel *const status = window->findChild<QLabel *>(
        QStringLiteral("AnonLogStatus"));
    QTRY_COMPARE(status->text(), QStringLiteral("Completed"));
    QPlainTextEdit *const result = window->findChild<QPlainTextEdit *>(
        QStringLiteral("AnonLogResult"));
    QVERIFY(result->toPlainText().contains(QStringLiteral("Input bytes:")));
    QVERIFY(result->toPlainText().contains(QStringLiteral("Patched values:")));
    QVERIFY(result->toPlainText().contains(QStringLiteral("Warnings:")));
    QVERIFY(result->toPlainText().contains(QStringLiteral("does NOT fully sanitize")));
    delete window;
}

void AnonLogWindowTest::deletionDuringConfirmationDoesNotStart()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    const QString output = directory.filePath(QStringLiteral("flight-anon.log"));
    QVERIFY(writeFile(input, kSmallTextLog));

    LogAnonymizeService service(this);
    auto *window = new AnonLogWindow(
        &service, nullptr,
        [](QWidget *owner, const QString &, const QString &) {
            delete owner;
            return true;
        });
    QPointer<AnonLogWindow> guardedWindow(window);
    setRequest(window, input, output);
    window->findChild<QPushButton *>(
        QStringLiteral("StartAnonLogButton"))->click();
    QVERIFY(guardedWindow.isNull());
    QVERIFY(!service.busy());
    QCOMPARE(service.token(), quint64(0));
    QVERIFY(!QFileInfo::exists(output));
}

void AnonLogWindowTest::cancelSignalMayDeleteObserver()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    const QString output = directory.filePath(QStringLiteral("flight-anon.log"));
    QVERIFY(writeFile(input, kSmallTextLog));

    LogAnonymizeService service(this);
    LogAnonymizeOptions options;
    options.latitudeOffset = 1.0;
    options.longitudeOffset = -1.0;
    QString error;
    QVERIFY2(service.start(input, output, options, &error), qPrintable(error));
    const quint64 token = service.token();

    auto *window = new AnonLogWindow(&service);
    QPointer<AnonLogWindow> guardedWindow(window);
    connect(&service, &LogAnonymizeService::changed, &service,
            [&service, &guardedWindow]() {
        if (service.cancellationRequested() && guardedWindow) {
            delete guardedWindow.data();
        }
    });
    window->findChild<QPushButton *>(
        QStringLiteral("CancelAnonLogButton"))->click();
    QVERIFY(guardedWindow.isNull());
    QCOMPARE(service.token(), token);
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
}

void AnonLogWindowTest::closeDetachesAndReopenCanExplicitlyCancelExactToken()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("large.log"));
    const QString output = directory.filePath(QStringLiteral("large-anon.log"));
    QByteArray inputBytes;
    inputBytes.reserve(3 * 1024 * 1024);
    inputBytes += QByteArrayLiteral("FMT, 42, 11, GPS, LL, Lat,Lng\r\n");
    while (inputBytes.size() < 3 * 1024 * 1024) {
        inputBytes += QByteArrayLiteral("GPS,10,20\r\n");
    }
    QVERIFY(writeFile(input, inputBytes));

    LogAnonymizeService service(this);
    LogAnonymizeOptions options;
    options.latitudeOffset = 1.0;
    options.longitudeOffset = -1.0;
    QString error;
    QVERIFY2(service.start(input, output, options, &error), qPrintable(error));
    const quint64 token = service.token();
    QVERIFY(token != 0);

    auto *window = new AnonLogWindow(&service);
    QCOMPARE(window->observedToken(), token);
    QVERIFY(window->findChild<QPushButton *>(
        QStringLiteral("CancelAnonLogButton"))->isEnabled());
    QPointer<AnonLogWindow> guardedWindow(window);
    window->show();
    window->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(guardedWindow.isNull());
    QVERIFY(service.busy());
    QVERIFY(!service.cancellationRequested());

    auto *reopened = new AnonLogWindow(&service);
    QCOMPARE(reopened->observedToken(), token);
    QPushButton *const cancel = reopened->findChild<QPushButton *>(
        QStringLiteral("CancelAnonLogButton"));
    QVERIFY(cancel->isEnabled());
    cancel->click();
    QVERIFY(service.cancellationRequested());
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 5000);
    QVERIFY(service.result().cancelled);
    QVERIFY(!service.result().success);
    QVERIFY(!QFileInfo::exists(output));
    QTRY_COMPARE(reopened->findChild<QLabel *>(
                     QStringLiteral("AnonLogStatus"))->text(),
                 QStringLiteral("Cancelled"));
    delete reopened;
}

QTEST_MAIN(AnonLogWindowTest)
#include "test_anonlogwindow.moc"
