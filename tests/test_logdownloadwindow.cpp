#include <QtTest>

#include "ui/LogDownloadViewModel.h"
#include "ui/LogDownloadWindow.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>

#include <limits>

class LogDownloadWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void matchesMp10ModelessSurfaceAndOwnsParentlessModel();
    void toolbarWrapsAtMinimumWidth();
    void eraseConfirmationToleratesWindowDeletion();
};

void LogDownloadWindowTest::
matchesMp10ModelessSurfaceAndOwnsParentlessModel()
{
    QWidget owner;
    owner.setGeometry(100, 100, 900, 700);
    auto *model = new LogDownloadViewModel(
        nullptr, LogDownloadViewModel::TargetAcquirer{});
    model->setTargetSource(QStringLiteral("Telemetry / sys 42 / comp 1"));
    auto *window = new LogDownloadWindow(model, &owner);
    QPointer<LogDownloadWindow> guardedWindow(window);
    QPointer<LogDownloadViewModel> guardedModel(model);

    QCOMPARE(window->objectName(), QStringLiteral("LogDownloadWindow"));
    QCOMPARE(window->windowTitle(), QStringLiteral("Download Logs (MAVLink)"));
    QCOMPARE(window->windowType(), Qt::Window);
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QCOMPARE(window->size(), QSize(540, 440));
    QCOMPARE(window->minimumSize(), QSize(420, 320));
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));
    QVERIFY(qobject_cast<QDialog *>(window) == nullptr);
    QCOMPARE(model->parent(), window);
    QCOMPARE(window->viewModel(), model);

    const QStringList buttonNames = {
        QStringLiteral("RefreshListButton"),
        QStringLiteral("DownloadSelectedButton"),
        QStringLiteral("DownloadAllButton"),
        QStringLiteral("EraseAllButton"),
        QStringLiteral("CancelDownloadButton")
    };
    for (const QString &name : buttonNames) {
        QVERIFY2(window->findChild<QAbstractButton *>(name),
                 qPrintable(QStringLiteral("missing %1").arg(name)));
    }
    QVERIFY(window->findChild<QCheckBox *>(
        QStringLiteral("CreateKmlCheckBox")));
    QTableWidget *const table = window->findChild<QTableWidget *>(
        QStringLiteral("LogDownloadGrid"));
    QVERIFY(table);
    QCOMPARE(table->columnCount(), 3);
    QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("Id"));
    QCOMPARE(table->horizontalHeaderItem(1)->text(), QStringLiteral("Date"));
    QCOMPARE(table->horizontalHeaderItem(2)->text(), QStringLiteral("Size"));
    QVERIFY(window->findChild<QProgressBar *>(
        QStringLiteral("LogDownloadProgress")));
    QLabel *const status = window->findChild<QLabel *>(
        QStringLiteral("LogDownloadStatus"));
    QVERIFY(status);
    QVERIFY(status->text().contains(
        QStringLiteral("Telemetry / sys 42 / comp 1")));

    window->show();
    QCoreApplication::processEvents();
    QVERIFY(window->isVisible());
    window->close();
    QTRY_VERIFY(guardedWindow.isNull());
    QVERIFY(guardedModel.isNull());
}

void LogDownloadWindowTest::toolbarWrapsAtMinimumWidth()
{
    auto *model = new LogDownloadViewModel(
        nullptr, LogDownloadViewModel::TargetAcquirer{});
    auto *window = new LogDownloadWindow(model);
    window->resize(window->minimumSize());
    window->show();
    QCoreApplication::processEvents();

    QWidget *const toolbar = window->findChild<QWidget *>(
        QStringLiteral("LogDownloadToolbar"));
    QVERIFY(toolbar);
    int minimumY = std::numeric_limits<int>::max();
    int maximumY = std::numeric_limits<int>::min();
    const QList<QAbstractButton *> controls =
        toolbar->findChildren<QAbstractButton *>(
            QString(), Qt::FindDirectChildrenOnly);
    QCOMPARE(controls.size(), 6);
    for (QAbstractButton *control : controls) {
        QVERIFY(control->isVisible());
        QVERIFY(toolbar->rect().contains(control->geometry()));
        minimumY = qMin(minimumY, control->geometry().top());
        maximumY = qMax(maximumY, control->geometry().top());
    }
    QVERIFY2(maximumY > minimumY,
             "six toolbar controls did not wrap at 420 px");
    delete window;
}

void LogDownloadWindowTest::eraseConfirmationToleratesWindowDeletion()
{
    ExactLogTransferService service(
        nullptr, nullptr,
        [](const SwarmVehicleInstanceLease &, QString *) { return true; });
    SwarmVehicleInstanceLease lease;
    lease.endpoint.linkId = 7;
    lease.endpoint.systemId = 42;
    lease.endpoint.componentId = MAV_COMP_ID_AUTOPILOT1;
    lease.linkSessionEpoch = 1;
    lease.instanceEpoch = 1;

    auto *model = new LogDownloadViewModel(
        &service, [lease]() { return lease; });
    auto *window = new LogDownloadWindow(model);
    QPointer<LogDownloadWindow> guardedWindow(window);
    QPushButton *const erase = window->findChild<QPushButton *>(
        QStringLiteral("EraseAllButton"));
    QVERIFY(erase);

    // Runs in QMessageBox::exec()'s nested event loop. This reproduces an
    // application-owner shutdown while the destructive confirmation is open.
    QTimer::singleShot(0, qApp, [guardedWindow]() {
        if (guardedWindow) {
            delete guardedWindow.data();
        }
    });
    erase->click();
    QVERIFY(guardedWindow.isNull());
}

QTEST_MAIN(LogDownloadWindowTest)
#include "test_logdownloadwindow.moc"
