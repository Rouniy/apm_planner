#include "MicrodroneDownlinkRuntimeAudit.h"
#include "ui/MainWindow.h"
#include "ui/MicrodroneDownlinkWindow.h"
#include "ui/configuration/ConfigDeveloperToolsView.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QThread>

namespace {
struct Capture { QByteArray bytes; bool open = false; int closes = 0; };
class Output final : public MavlinkMirrorOutput {
public:
    explicit Output(std::shared_ptr<Capture> value) : capture(std::move(value)) {}
    Kind kind() const override { return Kind::Serial; }
    QString selection() const override { return QStringLiteral("isolated-fixture"); }
    bool open(QString *) override { capture->open = true; return true; }
    void close() override { if (capture->open) ++capture->closes; capture->open = false; }
    bool isOpen() const override { return capture->open; }
    bool hasPeer() const override { return isOpen(); }
    qint64 write(const QByteArray &bytes) override {
        if (!isOpen()) return -1;
        capture->bytes.append(bytes); return bytes.size();
    }
    qint64 pendingBytes() const override { return 0; }
    QString statusText() const override { return QStringLiteral("Isolated capture"); }
    std::shared_ptr<Capture> capture;
};
template<class T> T *find(QObject *root, const char *name) {
    return root ? root->findChild<T *>(QString::fromLatin1(name)) : nullptr;
}
bool waitFor(const std::function<bool()> &ready, int timeout = 3000) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1);
    }
    return ready();
}
}

int RunMicrodroneDownlinkRuntimeAudit()
{
    int failures = 0;
    auto expect = [&](bool value, const char *why) {
        if (!value) { ++failures; qCritical() << "MicroDrone runtime:" << why; }
    };
    auto *main = MainWindow::instance();
    auto *action = find<QAction>(main, "actionDeveloperTools");
    expect(action, "Developer Tools route missing");
    if (!action) return 1;
    action->trigger(); QApplication::processEvents();
    auto *page = main->findChild<ConfigDeveloperToolsView *>();
    auto *button = find<QPushButton>(page, "MicroDroneDownlinkButton");
    expect(page && page->ActionCount() == 32 && page->ImplementedActionCount() == 31,
           "Developer inventory is not 31 of 32");
    expect(button && button->isEnabled(), "MicroDrone unavailable offline");
    if (!button) return 1;
    button->click(); QApplication::processEvents();
    QPointer<MicrodroneDownlinkWindow> production = main->findChild<MicrodroneDownlinkWindow *>();
    expect(production && production->isVisible() && production->isWindow()
               && production->windowModality() == Qt::NonModal,
           "actual Developer button did not open a modeless window");
    if (!production) return 1;
    expect(production->size() == QSize(580, 440)
               && production->viewModel()->selectedBaud() == 57600
               && production->viewModel()->bauds() == MicrodroneDownlinkService::Bauds()
               && !production->service()->isRunning(), "production defaults differ");
    const QString screenshot = qEnvironmentVariable("APM_MICRODRONE_AUDIT_SCREENSHOT");
    if (!screenshot.isEmpty()) expect(production->grab().save(screenshot), "offline screenshot failed");
    production->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    expect(production.isNull(), "production close did not release window");

    MicrodroneSource source;
    source.selection.endpoint = {910119, 42, 1, QStringLiteral("isolated fixture"), QString()};
    source.selection.generation = 1;
    source.instance = {source.selection.endpoint, 7, 9};
    source.linkName = QStringLiteral("isolated fixture");
    auto capture = std::make_shared<Capture>();
    MicrodroneDownlinkWindow::Dependencies dependencies;
    dependencies.enumeratePorts = [] { return QStringList{QStringLiteral("isolated-fixture")}; };
    dependencies.service.resolveSource = [&] { return source; };
    dependencies.service.outputFactory = [capture](const MicrodroneOutputSettings &, QString *) {
        return std::unique_ptr<MavlinkMirrorOutput>(new Output(capture));
    };
    const QDateTime now(QDate(2026, 9, 6), QTime(12, 34, 56), Qt::UTC);
    dependencies.service.clock = [now] { return now; };
    QPointer<MicrodroneDownlinkWindow> fixture = new MicrodroneDownlinkWindow(dependencies, main);
    fixture->show(); QApplication::processEvents();
    auto *service = fixture->service();
    mavlink_message_t message{};
    mavlink_msg_global_position_int_pack(42, 1, &message, 1000,
        351000000, 332000000, 100000, 12000, 0, 0, 0, 0);
    service->observeMessage(910119, 7, message);
    mavlink_raw_imu_t imu{}; imu.xmag = 101; imu.ymag = -202; imu.zmag = 303;
    mavlink_msg_raw_imu_encode(42, 1, &message, &imu);
    service->observeMessage(910119, 7, message);
    auto *toggle = find<QPushButton>(fixture, "ToggleMicrodroneOutputButton");
    auto *port = find<QComboBox>(fixture, "microdroneDownlinkPort");
    auto *baud = find<QComboBox>(fixture, "microdroneDownlinkBaud");
    expect(toggle && port && baud, "fixture controls missing");
    if (!toggle || !port || !baud) { fixture->close(); return 1; }
    toggle->click();
    expect(service->isRunning() && !port->isEnabled() && !baud->isEnabled()
               && toggle->text() == QStringLiteral("Stop"), "Connect did not lock settings/start output");
    const QByteArray first = MicrodroneDownlinkEncoder::EncodeFrame(service->telemetry(), now, 0);
    expect(capture->bytes == first && first.count("\r\n") == 7
               && first.contains("#9,101,-202,303,"), "first complete protocol frame differs");
    expect(waitFor([&] { return service->framesSubmitted() >= 2; }), "100 ms timer emitted no second frame");
    if (!screenshot.isEmpty()) expect(fixture->grab().save(screenshot + QStringLiteral(".running.png")),
                                     "running screenshot failed");
    ++source.selection.generation;
    service->synchronizeSource();
    expect(!service->isRunning() && !capture->open && port->isEnabled()
               && service->statusText() == MicrodroneDownlinkService::TargetChangedText(),
           "source generation change did not stop and release output");
    const QByteArray stoppedBytes = capture->bytes;
    service->emitNow(); expect(capture->bytes == stoppedBytes, "stopped session wrote more bytes");
    toggle->click();
    expect(service->isRunning(), "explicit restart failed");
    // Retained closed windows must stop too, not just their destructors.
    fixture->setAttribute(Qt::WA_DeleteOnClose, false);
    fixture->close();
    expect(fixture && !fixture->service()->isRunning() && !capture->open && capture->closes == 2,
           "retained window close left serial output alive");
    delete fixture.data();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qInfo() << "MicroDrone runtime audit failures:" << failures
            << "(actual offline route; isolated seven-record serial capture; no vehicle writes)";
    return failures ? 1 : 0;
}
