#include "ui/OsdVideoOverlayWindow.h"
#include "ui/flightdata/HudControl.h"

#include <QCheckBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

class OsdVideoOverlayWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void completeWorkflowIsVisible();
    void browseActionsPopulateCanonicalPaths();
    void invalidInputsFailBeforeConfirmation();
    void rejectedConfirmationCreatesNoOutput();
    void openWindowActivatesTheExistingInstance();
};

void OsdVideoOverlayWindowTest::completeWorkflowIsVisible()
{
    OsdVideoOverlayWindow window;
    QCOMPARE(window.size(), QSize(1120, 720));
    QCOMPARE(window.minimumSize(), QSize(820, 600));
    QVERIFY(window.findChild<QLineEdit *>(
        QStringLiteral("osdVideoSourcePath")));
    QVERIFY(window.findChild<QLineEdit *>(
        QStringLiteral("osdVideoTlogPath")));
    QVERIFY(window.findChild<QLineEdit *>(
        QStringLiteral("osdVideoOutputPath")));
    QVERIFY(window.findChild<QSpinBox *>(
        QStringLiteral("osdVideoTimeOffset")));
    QVERIFY(window.findChild<QCheckBox *>(
        QStringLiteral("osdVideoFullResolution")));
    QVERIFY(window.findChild<QLabel *>(
        QStringLiteral("osdVideoPreview")));
    QVERIFY(window.findChild<QProgressBar *>(
        QStringLiteral("osdVideoProgress")));
    QVERIFY(window.findChild<QPushButton *>(
        QStringLiteral("osdVideoStart")));
    QVERIFY(window.findChild<QPushButton *>(
        QStringLiteral("osdVideoCancel")));
    QVERIFY(window.findChild<QPushButton *>(
        QStringLiteral("osdVideoClose")));
    QVERIFY(!window.isBusy());
    QVERIFY(window.statusText().contains(QStringLiteral("MJPEG AVI")));
    auto *exportHud = window.findChild<HudControl *>();
    QVERIFY(exportHud);
    QCOMPARE(exportHud->minimumSize(), QSize(0, 0));
}

void OsdVideoOverlayWindowTest::browseActionsPopulateCanonicalPaths()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString video = temporary.filePath(QStringLiteral("camera.mp4"));
    const QString tlog = temporary.filePath(QStringLiteral("flight.tlog"));
    const QString output = temporary.filePath(QStringLiteral("chosen.avi"));
    QFile(video).open(QIODevice::WriteOnly);
    QFile(tlog).open(QIODevice::WriteOnly);

    OsdVideoOverlayWindow::Dependencies dependencies;
    dependencies.confirm = [](QWidget *, const QString &, const QString &,
                              const QString &) { return false; };
    dependencies.chooseVideo = [video](QWidget *) { return video; };
    dependencies.chooseTlog = [tlog](QWidget *) { return tlog; };
    dependencies.chooseOutput = [output](QWidget *, const QString &) {
        return output;
    };
    OsdVideoOverlayWindow window(dependencies);

    window.findChild<QPushButton *>(
        QStringLiteral("osdVideoBrowseSource"))->click();
    QCOMPARE(window.findChild<QLineEdit *>(
                 QStringLiteral("osdVideoSourcePath"))->text(),
             QFileInfo(video).absoluteFilePath());
    QCOMPARE(window.findChild<QLineEdit *>(
                 QStringLiteral("osdVideoOutputPath"))->text(),
             QFileInfo(temporary.filePath(
                 QStringLiteral("camera-overlay.avi"))).absoluteFilePath());

    window.findChild<QPushButton *>(
        QStringLiteral("osdVideoBrowseTlog"))->click();
    QCOMPARE(window.findChild<QLineEdit *>(
                 QStringLiteral("osdVideoTlogPath"))->text(),
             QFileInfo(tlog).absoluteFilePath());
    window.findChild<QPushButton *>(
        QStringLiteral("osdVideoBrowseOutput"))->click();
    QCOMPARE(window.findChild<QLineEdit *>(
                 QStringLiteral("osdVideoOutputPath"))->text(),
             QFileInfo(output).absoluteFilePath());
}

void OsdVideoOverlayWindowTest::invalidInputsFailBeforeConfirmation()
{
    int confirmations = 0;
    OsdVideoOverlayWindow::Dependencies dependencies;
    dependencies.confirm = [&confirmations](
        QWidget *, const QString &, const QString &, const QString &) {
        ++confirmations;
        return true;
    };
    OsdVideoOverlayWindow window(dependencies);
    window.setVideoPath(QStringLiteral("/missing/camera.mp4"));
    window.setTlogPath(QStringLiteral("/missing/flight.tlog"));
    window.setOutputPath(QStringLiteral("/missing/result.avi"));

    window.findChild<QPushButton *>(
        QStringLiteral("osdVideoStart"))->click();

    QCOMPARE(confirmations, 0);
    QVERIFY(window.statusText().startsWith(
        QStringLiteral("Cannot start OSD video:")));
    QVERIFY(!window.isBusy());
}

void OsdVideoOverlayWindowTest::rejectedConfirmationCreatesNoOutput()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString video = temporary.filePath(QStringLiteral("camera.mp4"));
    const QString tlog = temporary.filePath(QStringLiteral("flight.tlog"));
    const QString output = temporary.filePath(QStringLiteral("result.avi"));
    QFile videoFile(video);
    QVERIFY(videoFile.open(QIODevice::WriteOnly));
    QVERIFY(videoFile.write("video") > 0);
    videoFile.close();
    QFile tlogFile(tlog);
    QVERIFY(tlogFile.open(QIODevice::WriteOnly));
    QVERIFY(tlogFile.write("tlog") > 0);
    tlogFile.close();

    int confirmations = 0;
    OsdVideoOverlayWindow::Dependencies dependencies;
    dependencies.confirm = [&confirmations](
        QWidget *, const QString &, const QString &, const QString &) {
        ++confirmations;
        return false;
    };
    OsdVideoOverlayWindow window(dependencies);
    window.setVideoPath(video);
    window.setTlogPath(tlog);
    window.setOutputPath(output);
    window.findChild<QPushButton *>(
        QStringLiteral("osdVideoStart"))->click();

    QCOMPARE(confirmations, 1);
    QVERIFY(window.statusText().contains(
        QStringLiteral("cancelled before any output")));
    QVERIFY(!QFileInfo::exists(output));
}

void OsdVideoOverlayWindowTest::openWindowActivatesTheExistingInstance()
{
    QPointer<OsdVideoOverlayWindow> first =
        OsdVideoOverlayWindow::OpenWindow();
    QVERIFY(first);
    QCOMPARE(OsdVideoOverlayWindow::OpenWindow(), first.data());
    first->close();
    QTRY_VERIFY(first.isNull());
}

QTEST_MAIN(OsdVideoOverlayWindowTest)
#include "test_osdvideooverlaywindow.moc"
