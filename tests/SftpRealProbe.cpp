#include "services/SftpLogSession.h"
#include "ui/Loghandling/DataFlashBinToLogConverter.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QDebug>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 4 && app.arguments()[1] == "--convert") {
        const auto result = DataFlashBinToLogConverter::Convert(app.arguments()[2], app.arguments()[3]);
        qInfo() << result.success << result.error << result.recordsWritten << result.warnings;
        return result.success ? 0 : 1;
    }
    if (app.arguments().size() != 3) return 2;
    QTemporaryDir root;
    QFile file(root.filePath("fixture.bin"));
    const QByteArray payload = QByteArray("binary\0fixture", 14) + QByteArray(200000, char(0xe7));
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size()) return 2;
    file.close();
    QProcess server;
    server.setProcessChannelMode(QProcess::SeparateChannels);
    server.start(app.arguments()[1], {app.arguments()[2], root.path()});
    if (!server.waitForStarted(5000) || !server.waitForReadyRead(10000)) return 2;
    const auto config = QJsonDocument::fromJson(server.readLine()).object();
    const auto finish = [&](int code) { server.terminate(); if (!server.waitForFinished(3000)) { server.kill(); server.waitForFinished(3000); } return code; };
    if (config["port"].toInt() <= 0) return finish(2);
    SftpLogConnection connection{"127.0.0.1", quint16(config["port"].toInt()), "apm-test", "apm-test-password"};
    auto session = createSftpLogSession();
    QString error; SshHostKeyChallenge challenge;
    QByteArray events;
    int failures = 0;
    const auto check = [&](bool ok, const char *reason) { if (!ok) { ++failures; qCritical() << reason << error; } };
    check(!session->connect(connection, {}, &challenge, &error), "unknown key was trusted");
    check(challenge.presentedFingerprint == config["fingerprint"].toString(), "host fingerprint differs");
    events += server.readAllStandardOutput();
    check(!events.contains("authentication"), "password sent before unknown key approval");
    check(!session->connect(connection, "SHA256:invalid", &challenge, &error), "changed key was trusted");
    check(challenge.isChanged(), "changed-key challenge absent");
    events += server.readAllStandardOutput();
    check(!events.contains("authentication"), "password sent after changed-key rejection");
    auto wrongPassword = connection;
    wrongPassword.password = "wrong-test-password";
    check(!session->connect(wrongPassword, config["fingerprint"].toString(), &challenge, &error)
          && challenge.presentedFingerprint.isEmpty(), "wrong password authenticated or reported key failure");
    const bool connected = session->connect(connection, config["fingerprint"].toString(), &challenge, &error);
    check(connected, "approved host key did not authenticate");
    if (connected) {
        QVector<SftpLogEntry> rows;
        check(session->listLogs("/", &rows, &error), "list failed");
        check(rows.size() == 1, "unexpected remote list");
        if (rows.size() == 1) {
            QBuffer output; output.open(QIODevice::WriteOnly); qint64 copied = 0;
            check(session->download(rows.first(), &output, &copied, &error), "download failed");
            check(output.data() == payload && copied == payload.size(), "binary roundtrip mismatch");
            auto changed = rows.first(); ++changed.length;
            check(!session->remove(changed, &error) && QFile::exists(file.fileName()), "changed listed file was removed");
            check(session->remove(rows.first(), &error) && !QFile::exists(file.fileName()), "approved unchanged delete failed");
        }
    }
    session->stop(); session.reset();
    events += server.readAllStandardOutput();
    check(events.count("authentication") == 2, "unexpected number of password authentication attempts");
    qInfo() << "SFTP real probe failures:" << failures;
    return finish(failures ? 1 : 0);
}
