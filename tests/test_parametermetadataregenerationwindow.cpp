#include <QtTest>

#include "core/parameters/ParameterMetaDataRegenerationService.h"
#include "ui/configuration/ParameterMetaDataRegenerationWindow.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>

class ParameterMetaDataRegenerationWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void modelessObserverSurfaceDoesNotOwnService();
    void defaultConfirmationUsesCancel();
    void exactConfirmationAndCloseLeaveApplicationRunAlive();
    void truthfulPartialResultAndVisibleLogAreBounded();
};

void ParameterMetaDataRegenerationWindowTest::
modelessObserverSurfaceDoesNotOwnService()
{
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    auto *service = new ParameterMetaDataRegenerationService(
        cache.path(), this);
    QWidget owner;
    owner.setGeometry(100, 100, 1000, 760);
    auto *window = new ParameterMetaDataRegenerationWindow(
        service, &owner,
        [](QWidget *, const QString &, const QString &) { return false; });
    QPointer<ParameterMetaDataRegenerationWindow> guardedWindow(window);
    QPointer<ParameterMetaDataRegenerationService> guardedService(service);

    QCOMPARE(window->objectName(),
             QStringLiteral("ParameterMetaDataRegenerationWindow"));
    QCOMPARE(window->windowTitle(),
             QStringLiteral("Parameter Metadata Generator"));
    QCOMPARE(window->windowType(), Qt::Window);
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QCOMPARE(window->size(), QSize(760, 560));
    QCOMPARE(window->minimumSize(), QSize(560, 400));
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QVERIFY(qobject_cast<QDialog *>(window) == nullptr);
    QCOMPARE(window->service(), service);
    QCOMPARE(service->parent(), this);

    const QStringList buttonNames = {
        QStringLiteral("RegenerateMetadataButton"),
        QStringLiteral("CancelMetadataButton"),
        QStringLiteral("CloseMetadataButton")
    };
    for (const QString &name : buttonNames) {
        QVERIFY2(window->findChild<QAbstractButton *>(name),
                 qPrintable(QStringLiteral("missing %1").arg(name)));
    }
    QVERIFY(window->findChild<QProgressBar *>(
        QStringLiteral("ParameterMetadataProgress")));
    QTableWidget *const artifacts = window->findChild<QTableWidget *>(
        QStringLiteral("ParameterMetadataArtifacts"));
    QVERIFY(artifacts);
    QCOMPARE(artifacts->columnCount(), 4);
    QCOMPARE(artifacts->horizontalHeaderItem(0)->text(),
             QStringLiteral("Artifact"));
    QCOMPARE(artifacts->horizontalHeaderItem(1)->text(),
             QStringLiteral("Source"));
    QCOMPARE(artifacts->horizontalHeaderItem(2)->text(),
             QStringLiteral("Status"));
    QCOMPARE(artifacts->horizontalHeaderItem(3)->text(),
             QStringLiteral("Size"));
    QPlainTextEdit *const log = window->findChild<QPlainTextEdit *>(
        QStringLiteral("ParameterMetadataLog"));
    QVERIFY(log);
    QCOMPARE(log->document()->maximumBlockCount(), 500);
    QLabel *const result = window->findChild<QLabel *>(
        QStringLiteral("ParameterMetadataResult"));
    QVERIFY(result);
    QVERIFY(result->text().contains(
        QStringLiteral("Existing parameter pages keep")));

    window->show();
    window->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(guardedWindow.isNull());
    QVERIFY(guardedService);
    delete service;
    QVERIFY(guardedService.isNull());
}

void ParameterMetaDataRegenerationWindowTest::defaultConfirmationUsesCancel()
{
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    ParameterMetaDataRegenerationService service(cache.path(), this);
    auto *window = new ParameterMetaDataRegenerationWindow(&service);
    QPushButton *const regenerate = window->findChild<QPushButton *>(
        QStringLiteral("RegenerateMetadataButton"));
    QVERIFY(regenerate);

    bool inspected = false;
    QTimer::singleShot(0, qApp, [&inspected]() {
        auto *dialog = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        inspected = dialog->windowTitle()
                == QStringLiteral("Regenerate Parameter Metadata")
            && dialog->text()
                == QStringLiteral("Download current master/stable ArduPilot "
                                  "parameter definitions and rebuild the "
                                  "local metadata cache?")
            && dialog->defaultButton()
                == dialog->button(QMessageBox::Cancel)
            && dialog->escapeButton()
                == dialog->button(QMessageBox::Cancel);
        dialog->button(QMessageBox::Cancel)->click();
    });
    regenerate->click();
    QVERIFY(inspected);
    QVERIFY(!service.busy());
    delete window;
}

void ParameterMetaDataRegenerationWindowTest::
exactConfirmationAndCloseLeaveApplicationRunAlive()
{
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    ParameterMetaDataRegenerationService service(
        cache.path(), QUrl(QStringLiteral("http://127.0.0.1:1")), this);

    QString observedTitle;
    QString observedMessage;
    bool allow = false;
    auto confirmation = [&observedTitle, &observedMessage, &allow](
                            QWidget *, const QString &title,
                            const QString &message) {
        observedTitle = title;
        observedMessage = message;
        return allow;
    };
    auto *window = new ParameterMetaDataRegenerationWindow(
        &service, nullptr, confirmation);
    QPushButton *const regenerate = window->findChild<QPushButton *>(
        QStringLiteral("RegenerateMetadataButton"));
    QPushButton *const cancel = window->findChild<QPushButton *>(
        QStringLiteral("CancelMetadataButton"));
    QVERIFY(regenerate);
    QVERIFY(cancel);
    QVERIFY(regenerate->isEnabled());
    QVERIFY(!cancel->isEnabled());

    regenerate->click();
    QCOMPARE(observedTitle,
             QStringLiteral("Regenerate Parameter Metadata"));
    QCOMPARE(observedMessage,
             QStringLiteral("Download current master/stable ArduPilot "
                            "parameter definitions and rebuild the local "
                            "metadata cache?"));
    QVERIFY(!service.busy());

    allow = true;
    regenerate->click();
    QVERIFY(service.busy());
    const auto token = service.snapshot().token;
    QVERIFY(token.isValid());
    QCOMPARE(window->observedToken(), token);
    QVERIFY(cancel->isEnabled());

    QPointer<ParameterMetaDataRegenerationWindow> guardedWindow(window);
    window->show();
    window->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(guardedWindow.isNull());
    QVERIFY2(service.busy(),
             "closing the observer implicitly cancelled the app-owned run");

    auto *reopened = new ParameterMetaDataRegenerationWindow(&service);
    QCOMPARE(reopened->observedToken(), token);
    QPushButton *const reopenedCancel = reopened->findChild<QPushButton *>(
        QStringLiteral("CancelMetadataButton"));
    QVERIFY(reopenedCancel);
    QVERIFY(reopenedCancel->isEnabled());
    reopenedCancel->click();
    QVERIFY(!service.busy());
    const auto terminal = service.snapshot();
    QVERIFY(terminal.terminal);
    QCOMPARE(terminal.outcome,
             ParameterMetaDataRegenerationService::Outcome::Cancelled);
    QLabel *const result = reopened->findChild<QLabel *>(
        QStringLiteral("ParameterMetadataResult"));
    QTRY_VERIFY(result->text().contains(QStringLiteral("was cancelled")));
    QVERIFY(result->text().contains(
        QStringLiteral("save or apply pending edits")));
    QVERIFY(result->text().contains(
        QStringLiteral("reconnecting or restarting the application")));
    delete reopened;
}

void ParameterMetaDataRegenerationWindowTest::
truthfulPartialResultAndVisibleLogAreBounded()
{
    QTemporaryDir cache;
    QVERIFY(cache.isValid());
    ParameterMetaDataRegenerationService service(
        cache.path(), QUrl(QStringLiteral("http://127.0.0.1:1")), this);
    ParameterMetaDataRegenerationService::RunToken token;
    QString error;
    QCOMPARE(service.start(&token, &error),
             ParameterMetaDataRegenerationService::StartResult::Started);
    QVERIFY(token.isValid());

    ParameterMetaDataRegenerationWindow window(&service, nullptr,
        [](QWidget *, const QString &, const QString &) { return false; });
    QCOMPARE(window.observedToken(), token);
    for (int index = 0; index < 620; ++index) {
        service.logLine(token, QStringLiteral("line %1").arg(index));
    }
    QPlainTextEdit *const log = window.findChild<QPlainTextEdit *>(
        QStringLiteral("ParameterMetadataLog"));
    QVERIFY(log);
    QVERIFY(log->document()->blockCount() <= 500);
    QVERIFY(!log->toPlainText().contains(QStringLiteral("line 0\n")));
    QVERIFY(log->toPlainText().contains(QStringLiteral("line 619")));

    ParameterMetaDataRegenerationService::ArtifactReport published;
    published.kind = ParameterMetaDataRegenerationService::ArtifactKind::
        GeneratedSource;
    published.key = QStringLiteral("ParameterMetaData.xml");
    published.sourceUrl = QUrl(QStringLiteral(
        "https://raw.githubusercontent.com/ArduPilot/ardupilot/master/"
        "ArduCopter/Parameters.cpp"));
    published.byteCount = 4096;
    published.published = true;
    ParameterMetaDataRegenerationService::ArtifactReport failed;
    failed.kind = ParameterMetaDataRegenerationService::ArtifactKind::
        PreparedPdef;
    failed.key = QStringLiteral("AP_Periph");
    failed.sourceUrl = QUrl(QStringLiteral(
        "https://autotest.ardupilot.org/Parameters/AP_Periph/"
        "apm.pdef.xml.gz"));
    failed.error = QStringLiteral("HTTP 503");

    ParameterMetaDataRegenerationService::Result result;
    result.token = token;
    result.outcome =
        ParameterMetaDataRegenerationService::Outcome::PartialFailure;
    result.artifacts = {published, failed};
    for (int index = 0; index < 620; ++index) {
        result.logLines.append(QStringLiteral("result %1").arg(index));
    }
    service.finished(result);

    QTableWidget *const artifacts = window.findChild<QTableWidget *>(
        QStringLiteral("ParameterMetadataArtifacts"));
    QCOMPARE(artifacts->rowCount(), 2);
    QCOMPARE(artifacts->item(0, 2)->text(), QStringLiteral("Published"));
    QVERIFY(artifacts->item(1, 2)->text().contains(
        QStringLiteral("HTTP 503")));
    QLabel *const summary = window.findChild<QLabel *>(
        QStringLiteral("ParameterMetadataResult"));
    QTRY_VERIFY(summary->text().contains(
        QStringLiteral("completed with errors")));
    QVERIFY(summary->text().contains(QStringLiteral("1 of 2")));
    QVERIFY(summary->text().contains(
        QStringLiteral("Existing parameter pages keep")));
    QVERIFY(log->document()->blockCount() <= 500);
    QVERIFY(log->toPlainText().contains(QStringLiteral("result 619")));

    QVERIFY(service.cancel(token));
}

QTEST_MAIN(ParameterMetaDataRegenerationWindowTest)
#include "test_parametermetadataregenerationwindow.moc"
