#include "UbloxRuntimeAudit.h"
#include "ui/MainWindow.h"
#include "ui/BackstageView.h"
#include "ui/configuration/SetupView.h"
#include "ui/configuration/ConfigGpsInjectView.h"
#include "comm/UbloxBaseStationService.h"
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QThread>
#include <QDebug>
#include <QtEndian>
#include <functional>
#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#endif

namespace {
bool waitFor(const std::function<bool()> &predicate, int limit = 12000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < limit) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}
template<class T> T *find(QObject *owner, const char *name) {
    return owner ? owner->findChild<T *>(QString::fromLatin1(name)) : nullptr;
}
}

int RunUbloxRuntimeAudit()
{
#ifdef Q_OS_LINUX
    int failures = 0;
    const auto check = [&](bool ok, const char *message) {
        if (!ok) { ++failures; qCritical() << "u-blox runtime:" << message; }
    };
    const int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        if (master >= 0) ::close(master);
        qCritical() << "Cannot create private receiver pseudo-terminal";
        return 1;
    }
    const QString port = QString::fromLocal8Bit(ptsname(master));
    auto *window = MainWindow::instance();
    auto *setup = window->findChild<SetupView *>();
    auto *backstage = setup ? setup->findChild<BackstageView *>() : nullptr;
    window->loadHardwareConfigView(); window->show();
    check(backstage && backstage->setCurrentPage(QStringLiteral("ConfigGpsInjectView")),
          "Offline SETUP RTK/GPS Inject route unavailable");
    auto *page = setup ? setup->findChild<ConfigGpsInjectView *>() : nullptr;
    if (!page) { ::close(master); return 1; }
    auto *model = page->viewModel();
    model->SetSelectedPort(port); model->RefreshPorts();
    auto *automatic = find<QCheckBox>(page, "gpsInjectAutoConfigCheck");
    auto *connect = find<QPushButton>(page, "gpsInjectConnectButton");
    check(automatic && connect, "Receiver controls missing");
    if (!automatic || !connect) { ::close(master); return 1; }
    automatic->setChecked(true);
    const auto consent = [&]() -> QMessageBox * {
        for (auto *box : page->findChildren<QMessageBox *>(QStringLiteral("gpsInjectUbloxAutoConfigureConfirmation")))
            if (box->isVisible()) return box;
        return nullptr;
    };
    connect->click();
    check(waitFor([&] { return consent(); }), "Auto Configure confirmation missing");
    if (auto *box = consent()) {
        check(box->defaultButton() == box->button(QMessageBox::Cancel), "Auto Configure not default Cancel");
        box->button(QMessageBox::Cancel)->click();
    }
    check(!model->Active(), "Cancel opened the receiver");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    UbloxBaseStationProtocol parser;
    QVector<UbloxBaseStationProtocol::Packet> received;
    bool surveyComplete = false;
    QElapsedTimer lastNavigation; lastNavigation.start();
    QTimer receiver;
    QObject::connect(&receiver, &QTimer::timeout, page, [&] {
        char bytes[4096];
        for (ssize_t size = ::read(master, bytes, sizeof(bytes)); size > 0;
             size = ::read(master, bytes, sizeof(bytes))) {
            for (int i = 0; i < size; ++i) {
                UbloxBaseStationProtocol::Packet packet;
                if (!parser.read(quint8(bytes[i]), &packet)) continue;
                received.append(packet);
                if (packet.messageClass == 6 && packet.messageId != 4) {
                    QByteArray payload; payload.append(char(6)); payload.append(char(packet.messageId));
                    const auto ack = UbloxBaseStationProtocol::frame(5, 1, payload);
                    ::write(master, ack.constData(), size_t(ack.size()));
                }
            }
        }
        if (!model->Connected() || lastNavigation.elapsed() < 1000) return;
        lastNavigation.restart();
        QByteArray survey(40, '\0');
        const auto put32 = [](QByteArray &out, int offset, qint32 value) {
            qToLittleEndian<qint32>(value, reinterpret_cast<uchar *>(out.data() + offset));
        };
        put32(survey, 8, 65); put32(survey, 12, 637813700);
        put32(survey, 28, 1000); put32(survey, 32, 100);
        survey[36] = surveyComplete ? 1 : 0;
        survey[37] = surveyComplete ? 0 : 1;
        const auto svin = UbloxBaseStationProtocol::frame(1, 0x3b, survey);
        ::write(master, svin.constData(), size_t(svin.size()));
        QByteArray position(92, '\0'); position[20] = 3; position[21] = 1; position[23] = 18;
        put32(position, 24, 330000000); put32(position, 28, 350000000);
        put32(position, 32, 123000); put32(position, 36, 103000);
        const auto pvt = UbloxBaseStationProtocol::frame(1, 7, position);
        ::write(master, pvt.constData(), size_t(pvt.size()));
    });
    receiver.start(20);
    connect->click();
    check(waitFor([&] { return consent(); }), "Second Auto Configure confirmation missing");
    if (auto *box = consent()) {
        check(box->text().contains(port), "Receiver consent omitted the serial port");
        const QString path = qEnvironmentVariable("APM_UBLOX_AUDIT_SCREENSHOT");
        if (!path.isEmpty()) check(box->grab().save(path + QStringLiteral(".consent.png")), "Consent screenshot failed");
        box->button(QMessageBox::Yes)->click();
    }
    auto *service = model->UbloxService();
    check(waitFor([&] { return service->lastReport().isValid() && !service->busy(); }, 20000),
          "Receiver configuration sequence did not finish");
    check(service->lastReport().outcome == UbloxBaseStationService::Outcome::Submitted,
          "Receiver sequence did not report host submission");
    const auto modeSent = [&](int mode) {
        for (const auto &packet : received)
            if (packet.messageClass == 6 && packet.messageId == 0x71 && packet.payload.size() == 40
                && (quint8(packet.payload[2]) & 3) == mode) return true;
        return false;
    };
    check(!modeSent(1) && !modeSent(0), "Connect unexpectedly reset/started Survey-In");
    auto *restart = find<QPushButton>(page, "gpsInjectRestartSurveyButton");
    check(restart && restart->isEnabled(), "Survey Restart button unavailable");
    const quint64 configured = service->lastReport().operationId;
    if (restart) restart->click();
    check(waitFor([&] { return !service->busy() && service->lastReport().operationId > configured; }, 20000),
          "Explicit Survey Restart did not finish");
    // An ordinary survey takes at least 60 seconds and supplies NAV, not RTCM.
    // Cross the production 30-second correction watchdog with NAV-only traffic.
    QElapsedTimer surveying; surveying.start();
    check(waitFor([&] { return surveying.elapsed() >= 31000; }, 32000),
          "NAV-only survey observation interval did not finish");
    check(model->Active() && model->Connected() && service->available(),
          "NAV-only survey was mistaken for a silent serial receiver");
    surveyComplete = true;
    check(waitFor([&] { return model->SurveyInValid() && model->HasCurrentBasePosition(); }),
          "Live NAV survey/position did not reach the page");
    check(waitFor([&] { return modeSent(1); }),
          "Survey-In TMODE3 was not written to the real serial source");
    auto *save = find<QPushButton>(page, "gpsInjectSaveCurrentPositionButton");
    check(save && save->isEnabled(), "Save Current Position unavailable after NAV fix");
    if (save) save->click();
    auto *use = find<QPushButton>(page, "gpsInjectUseBasePosition_0");
    check(use && use->isEnabled(), "Saved base Use button unavailable");
    const quint64 previous = service->lastReport().operationId;
    const int beforeFixed = received.size();
    if (use) use->click();
    check(waitFor([&] { return !service->busy() && service->lastReport().operationId > previous; }, 15000),
          "Use saved fixed base did not finish");
    check(waitFor([&] { return modeSent(2); }), "Fixed LLA TMODE3 was not transmitted");
    check(waitFor([&] { return received.size() >= beforeFixed + 2; }),
          "Fixed position poll was not transmitted");
    check(received.size() == beforeFixed + 2,
          "Connected Use unexpectedly repeated receiver setup/reset");
    if (received.size() >= beforeFixed + 2) {
        const auto &poll = received.at(beforeFixed + 1);
        check(poll.messageClass == 6 && poll.messageId == 0x71 && poll.payload.isEmpty(),
              "Connected Use did not poll TMODE3 after applying the fixed position");
    }
    const QString screenshot = qEnvironmentVariable("APM_UBLOX_AUDIT_SCREENSHOT");
    if (!screenshot.isEmpty()) check(page->grab().save(screenshot), "RTK page screenshot failed");
    connect->click();
    check(!model->Active() && !service->busy(), "Disconnect did not stop receiver source/service");
    receiver.stop(); ::close(master); window->close();
    qInfo() << "u-blox runtime audit failures:" << failures << "(private pseudo-terminal only)";
    return failures ? 1 : 0;
#else
    qCritical() << "Native serial pseudo-terminal audit is Linux-only.";
    return 2;
#endif
}
