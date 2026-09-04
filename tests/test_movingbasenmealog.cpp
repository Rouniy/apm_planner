#include "comm/MovingBaseNmeaLog.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class MovingBaseNmeaLogTest final : public QObject
{
    Q_OBJECT

private slots:
    void appendsAndRotatesWithinOneBackup();
    void capsAnOversizedPreexistingLogBackup();
    void rejectsUnframedAndOversizedInput();
};

void MovingBaseNmeaLogTest::appendsAndRotatesWithinOneBackup()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("MovingBase.txt"));
    MovingBaseNmeaLog log(path, 24);
    QString error;

    QVERIFY2(log.appendLine(QByteArray("first-line"), &error), qPrintable(error));
    QVERIFY2(log.appendLine(QByteArray("second-line"), &error), qPrintable(error));
    QVERIFY2(log.appendLine(QByteArray("third-line"), &error), qPrintable(error));
    log.close();

    QFile backup(log.backupPath());
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QCOMPARE(backup.readAll(), QByteArray("first-line\nsecond-line\n"));
    QFile current(path);
    QVERIFY(current.open(QIODevice::ReadOnly));
    QCOMPARE(current.readAll(), QByteArray("third-line\n"));
    QVERIFY(current.size() <= log.maximumBytes());
    QVERIFY(backup.size() <= log.maximumBytes());
}

void MovingBaseNmeaLogTest::capsAnOversizedPreexistingLogBackup()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("MovingBase.txt"));
    QFile existing(path);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write(QByteArray("0123456789ABCDEFGHIJ")), qint64(20));
    existing.close();

    MovingBaseNmeaLog log(path, 12);
    QString error;
    QVERIFY2(log.appendLine(QByteArray("new"), &error), qPrintable(error));
    log.close();

    QFile backup(log.backupPath());
    QVERIFY(backup.open(QIODevice::ReadOnly));
    QCOMPARE(backup.readAll(), QByteArray("89ABCDEFGHIJ"));
    QVERIFY(backup.size() <= log.maximumBytes());
    QFile current(path);
    QVERIFY(current.open(QIODevice::ReadOnly));
    QCOMPARE(current.readAll(), QByteArray("new\n"));
}

void MovingBaseNmeaLogTest::rejectsUnframedAndOversizedInput()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MovingBaseNmeaLog log(
        directory.filePath(QStringLiteral("MovingBase.txt")), 8192);
    QString error;

    QVERIFY(!log.appendLine(QByteArray("bad\nline"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!log.appendLine(
        QByteArray(MovingBaseNmeaLog::MaximumLineBytes + 1, 'x'), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(log.path()));
}

QTEST_GUILESS_MAIN(MovingBaseNmeaLogTest)

#include "test_movingbasenmealog.moc"
