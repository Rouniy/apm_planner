#include "services/SftpLogDownloadSupport.h"
#include <QtTest>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <cstring>

namespace {
QByteArray emptyBin() {
    QByteArray bytes(89, '\0');
    bytes[0] = char(0xa3); bytes[1] = char(0x95); bytes[2] = char(128);
    bytes[3] = char(128); bytes[4] = char(89);
    bytes.replace(5, 3, "FMT"); bytes.replace(9, 5, "BBnNZ");
    bytes.replace(25, 31, "Type,Length,Name,Format,Columns");
    return bytes;
}

QByteArray frame(quint8 type) {
    QByteArray result;
    result.append(char(0xa3)); result.append(char(0x95)); result.append(char(type));
    return result;
}
template<typename Integer> void appendLittle(QByteArray *output, Integer value) {
    uchar encoded[sizeof(Integer)] {};
    qToLittleEndian<Integer>(value, encoded);
    output->append(reinterpret_cast<const char *>(encoded), int(sizeof(encoded)));
}
void appendFloat(QByteArray *output, float value) {
    quint32 bits = 0; std::memcpy(&bits, &value, sizeof(bits)); appendLittle(output, bits);
}
QByteArray gpsFormat(quint8 type, quint8 length, const QByteArray &format,
                     const QByteArray &labels) {
    QByteArray payload(86, '\0');
    payload[0] = char(type); payload[1] = char(length);
    payload.replace(2, 3, "GPS");
    payload.replace(6, format.size(), format);
    payload.replace(22, labels.size(), labels);
    return frame(128) + payload;
}
QByteArray modernGps(quint64 timeUs, quint8 status, quint32 gms,
                     quint16 week, double latitude, double longitude) {
    QByteArray payload;
    appendLittle(&payload, timeUs); payload.append(char(0)); payload.append(char(status));
    appendLittle(&payload, gms); appendLittle(&payload, week); payload.append(char(12));
    appendLittle(&payload, qint16(90));
    appendLittle(&payload, qint32(std::llround(latitude * 1.0e7)));
    appendLittle(&payload, qint32(std::llround(longitude * 1.0e7)));
    appendLittle(&payload, qint32(58420));
    appendFloat(&payload, 1.5f); appendFloat(&payload, 90.0f);
    appendFloat(&payload, 0.1f); appendFloat(&payload, 0.0f);
    payload.append(char(1));
    return frame(130) + payload;
}
QByteArray modernLog(bool zeroAnchor, bool validWeek = true) {
    static const QByteArray format("QBBIHBcLLeffffB");
    static const QByteArray labels(
        "TimeUS,I,Status,GMS,GWk,NSats,HDop,Lat,Lng,Alt,Spd,GCrs,VZ,Yaw,U");
    constexpr quint32 gms = 131115000;
    constexpr quint16 week = 2383;
    QByteArray result = emptyBin() + gpsFormat(130, 51, format, labels);
    if (zeroAnchor) {
        result += modernGps(2000000, 3, gms, week, 0.0, 0.0);
        result += modernGps(3000000, 3, gms + 1000, week,
                            -35.3632621, 149.1652374);
    } else {
        result += modernGps(2000000, 3, gms, validWeek ? week : 9000,
                            -35.3632621, 149.1652374);
    }
    return result;
}
QByteArray legacyGps(quint8 status, quint32 timeMs, quint16 week,
                     double latitude, double longitude, quint32 boardTimeMs) {
    QByteArray payload;
    payload.append(char(status)); appendLittle(&payload, timeMs); appendLittle(&payload, week);
    payload.append(char(12)); appendLittle(&payload, qint16(90));
    appendLittle(&payload, qint32(std::llround(latitude * 1.0e7)));
    appendLittle(&payload, qint32(std::llround(longitude * 1.0e7)));
    appendLittle(&payload, qint32(1000)); appendLittle(&payload, qint32(58420));
    appendLittle(&payload, quint32(150)); appendLittle(&payload, qint32(9000));
    appendFloat(&payload, 0.1f); appendLittle(&payload, boardTimeMs);
    return frame(130) + payload;
}
QByteArray legacyLog() {
    static const QByteArray format("BIHBcLLeeEefI");
    static const QByteArray labels(
        "Status,TimeMS,Week,NSats,HDop,Lat,Lng,RelAlt,Alt,Spd,GCrs,VZ,T");
    return emptyBin() + gpsFormat(130, 45, format, labels)
        + legacyGps(2, 130040000, 1769, 0.0, 0.0, 127000)
        + legacyGps(3, 130040903, 1769, -35.3547178, 149.1696673, 127615);
}
QString expectedGpsStem(int week, qint64 gpsWeekMs, qint64 deltaMs = 0) {
    return QDateTime(QDate(1980, 1, 6), QTime(0, 0), Qt::UTC)
        .addDays(qint64(week) * 7).addMSecs(gpsWeekMs - 18000 + deltaMs)
        .toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH-mm-ss"));
}
QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QStringList paths(const QString &root) {
    QStringList result; QDirIterator iterator(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (iterator.hasNext()) result.append(QDir(root).relativeFilePath(iterator.next()));
    result.sort(); return result;
}
class Fake final : public SftpLogSession {
public:
    QByteArray bytes = emptyBin(); bool fail = false; bool replacePartial = false;
    int calls = 0;
    bool isConnected() const override { return true; }
    bool connect(const SftpLogConnection &, const QString &, SshHostKeyChallenge *, QString *, Cancel) override { return true; }
    bool listLogs(const QString &, QVector<SftpLogEntry> *, QString *, Cancel) override { return true; }
    bool download(const SftpLogEntry &, QIODevice *output, qint64 *copied, QString *error, Cancel cancel, Progress progress) override {
        ++calls;
        *copied = output->write(bytes);
        if (progress) progress(*copied);
        if (replacePartial) {
            auto *file = dynamic_cast<QFile *>(output);
            const QString path = file ? file->fileName() : QString();
            output->close();
            if (!path.isEmpty()) {
                QFile::remove(path);
                QFile replacement(path);
                if (replacement.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                    replacement.write("FOREIGN");
            }
            *error = "fixture replaced partial";
            return false;
        }
        if (fail || (cancel && cancel())) { *error = "fixture interrupted"; return false; }
        return *copied == bytes.size();
    }
    bool remove(const SftpLogEntry &, QString *, Cancel) override { return false; }
    void stop() override {}
};
SftpLogEntry entry() { return {"/logs", "remote.bin", emptyBin().size(), QDateTime::currentDateTimeUtc()}; }
}
class SftpLogDownloadSupportTest final : public QObject {
    Q_OBJECT
private slots:
    void namesArePortable() {
        QCOMPARE(SftpLogDownloadSupport::safeBinName("../evil.bin"), QString("evil.bin"));
        QCOMPARE(SftpLogDownloadSupport::safeBinName(".hidden.bin"), QString("log_hidden.bin"));
        QCOMPARE(SftpLogDownloadSupport::safeBinName("CON.bin"), QString("log_CON.bin"));
        QCOMPARE(SftpLogDownloadSupport::safeBinName(QString::fromUtf8("файл.bin")), QString("____.bin"));
        QVERIFY(SftpLogDownloadSupport::safeBinName(
            QString(500, QLatin1Char('x')) + QStringLiteral(".bin")).size() <= 100);
    }
    void savesBinAndDerivedFilesWithoutReplacingAnything() {
        Fake session; QTemporaryDir output;
        session.bytes = modernLog(false, false);
        auto selected = entry(); selected.length = session.bytes.size();
        QFile existing(output.filePath("remote.bin")); QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("original"); existing.close();
        const auto result = SftpLogDownloadSupport::download(session, {selected}, output.path(), true);
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.savedLogs, 1);
        QCOMPARE(read(existing.fileName()), QByteArray("original"));
        QCOMPARE(read(output.filePath("remote-1.bin")), session.bytes);
        QVERIFY(QFile::exists(output.filePath("remote-1.log")));
        QVERIFY(QFile::exists(output.filePath("remote-1.kml")));
        QCOMPARE(result.publishedPaths.size(), 3);
        QVERIFY(!paths(output.path()).join('\n').contains(".apm-sftp-"));
    }
    void interruptedDownloadRemovesOnlyPartial() {
        Fake session; session.fail = true; QTemporaryDir output;
        const auto result = SftpLogDownloadSupport::download(session, {entry()}, output.path(), false);
        QVERIFY(!result.success); QCOMPARE(result.savedLogs, 0);
        QVERIFY(result.publishedPaths.isEmpty()); QVERIFY(paths(output.path()).isEmpty());
    }
    void cleanupNeverDeletesAReplacement() {
        Fake session; session.replacePartial = true; QTemporaryDir output;
        const auto result = SftpLogDownloadSupport::download(
            session, {entry()}, output.path(), false);
        QVERIFY(!result.success); QVERIFY(result.publishedPaths.isEmpty());
        const QStringList retained = paths(output.path());
        QCOMPARE(retained.size(), 2);
        QVERIFY(retained.at(0).startsWith(QStringLiteral(".apm-sftp-")));
        QVERIFY(retained.at(1).endsWith(QStringLiteral("/0.bin")));
        QCOMPARE(read(output.filePath(retained.at(1))), QByteArray("FOREIGN"));
        QVERIFY(result.warnings.join(QLatin1Char('\n')).contains(
            QStringLiteral("identity changed; cleanup refused")));
    }
    void cancelAfterBinPublicationKeepsReceiptAndBin() {
        Fake session; QTemporaryDir output; bool stop = false;
        const auto result = SftpLogDownloadSupport::download(session, {entry()}, output.path(), true,
            [&] { return stop; }, [&](qint64, qint64, const QString &status) { if (status.startsWith("Processing")) stop = true; });
        QVERIFY(result.cancelled); QVERIFY(!result.success); QCOMPARE(result.savedLogs, 1);
        QCOMPARE(result.publishedPaths, QStringList{output.filePath("remote.bin")});
        QCOMPARE(read(result.publishedPaths.first()), session.bytes);
        QCOMPARE(paths(output.path()), QStringList{"remote.bin"});
    }
    void growthWarningAndInvalidSelection() {
        Fake session; QTemporaryDir output; auto selected = entry(); selected.length = 1;
        const auto result = SftpLogDownloadSupport::download(session, {selected}, output.path(), false);
        QVERIFY(result.success); QVERIFY(result.warnings.join('\n').contains("sizes differ"));
        selected.name = "../bad.bin"; const int before = session.calls;
        QVERIFY(!SftpLogDownloadSupport::download(session, {selected}, output.path(), false).success);
        QCOMPARE(session.calls, before);
    }
    void finalProgressCancellationIsReported() {
        Fake session; QTemporaryDir output; bool stop = false;
        const auto result = SftpLogDownloadSupport::download(
            session, {entry()}, output.path(), false, [&] { return stop; },
            [&](qint64, qint64, const QString &status) {
                if (status.startsWith(QStringLiteral("Saved "))) stop = true;
            });
        QVERIFY(result.cancelled); QVERIFY(!result.success);
        QCOMPARE(result.savedLogs, 1);
        QVERIFY(result.error.contains(QStringLiteral("completed files retained")));
        QCOMPARE(result.publishedPaths.size(), 2);
    }
    void repeatedNamesUseBoundedStableSuffixes() {
        Fake session; QTemporaryDir output;
        const auto selected = entry();
        const auto result = SftpLogDownloadSupport::download(
            session, {selected, selected, selected}, output.path(), false);
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.savedLogs, 3);
        for (int index = 0; index < 3; ++index) {
            const QString suffix = index ? QStringLiteral("-%1").arg(index) : QString();
            QCOMPARE(read(output.filePath(QStringLiteral("remote%1.bin").arg(suffix))),
                     session.bytes);
            QVERIFY(QFileInfo::exists(
                output.filePath(QStringLiteral("remote%1.log").arg(suffix))));
        }
    }
    void modernGpsRenameUsesFirstAcceptedFix() {
        Fake session; QTemporaryDir output; session.bytes = modernLog(true);
        auto selected = entry(); selected.name = QStringLiteral("modern.bin");
        selected.length = session.bytes.size();
        const auto result = SftpLogDownloadSupport::download(
            session, {selected}, output.path(), false);
        QVERIFY2(result.success, qPrintable(result.error));
        const QString stem = expectedGpsStem(2383, 131115000, 1000);
        QVERIFY(QFileInfo::exists(output.filePath(stem + QStringLiteral(".bin"))));
        QVERIFY(QFileInfo::exists(output.filePath(stem + QStringLiteral(".log"))));
    }
    void legacyGpsRenameUsesBoardTimeRatherThanTimeOfWeekDelta() {
        Fake session; QTemporaryDir output; session.bytes = legacyLog();
        auto selected = entry(); selected.name = QStringLiteral("legacy.bin");
        selected.length = session.bytes.size();
        const auto result = SftpLogDownloadSupport::download(
            session, {selected}, output.path(), false);
        QVERIFY2(result.success, qPrintable(result.error));
        const QString corrected = expectedGpsStem(1769, 130040903);
        const QString mp10Bug = expectedGpsStem(
            1769, 130040903, qint64(130040903) - 127615);
        QVERIFY(corrected != mp10Bug);
        QVERIFY(QFileInfo::exists(output.filePath(corrected + QStringLiteral(".bin"))));
        QVERIFY(!QFileInfo::exists(output.filePath(mp10Bug + QStringLiteral(".bin"))));
    }
    void missingGpsAnchorKeepsRemoteName() {
        Fake session; QTemporaryDir output; session.bytes = modernLog(false, false);
        auto selected = entry(); selected.name = QStringLiteral("unknown.bin");
        selected.length = session.bytes.size();
        const auto result = SftpLogDownloadSupport::download(
            session, {selected}, output.path(), false);
        QVERIFY2(result.success, qPrintable(result.error));
        QVERIFY(QFileInfo::exists(output.filePath(QStringLiteral("unknown.bin"))));
        QVERIFY(result.warnings.join(QLatin1Char('\n')).contains(
            QStringLiteral("no valid post-1980 UTC anchor")));
    }
};
QTEST_GUILESS_MAIN(SftpLogDownloadSupportTest)
#include "test_sftplogdownloadsupport.moc"
