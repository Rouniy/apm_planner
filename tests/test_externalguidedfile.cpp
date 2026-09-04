#include <QtTest>

#include "ui/tools/ExternalGuidedFile.h"

#include <QDir>
#include <QFile>
#include <QLocale>
#include <QTemporaryDir>

class ExternalGuidedFileTest final : public QObject
{
    Q_OBJECT

private slots:
    void parsesValidTargets_data();
    void parsesValidTargets();
    void parsingIsIndependentOfDefaultLocale();
    void rejectsInvalidShapeOrNumbers_data();
    void rejectsInvalidShapeOrNumbers();
    void reportsRangeErrorsPrecisely_data();
    void reportsRangeErrorsPrecisely();
    void enforcesDecodedCharacterBound();
    void readsBoundedFilesAndReturnsAbsolutePath();
    void readsUtf8BomAndUnicodeWhitespace();
    void reportsMissingAndEmptyPaths();
};

void ExternalGuidedFileTest::parsesValidTargets_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<double>("latitude");
    QTest::addColumn<double>("longitude");
    QTest::addColumn<double>("altitude");

    QTest::newRow("zero-latitude-longitude")
        << QStringLiteral("0,0,25") << 0.0 << 0.0 << 25.0;
    QTest::newRow("trimmed-fields")
        << QStringLiteral(" \t34.1234567, 33.7654321, 50.5\n")
        << 34.1234567 << 33.7654321 << 50.5;
    QTest::newRow("exponents-and-lower-endpoints")
        << QStringLiteral("-9e1,-1.8e2,1e4")
        << -90.0 << -180.0 << 10000.0;
    QTest::newRow("upper-endpoints")
        << QStringLiteral("+90,+180,0.0001")
        << 90.0 << 180.0 << 0.0001;
}

void ExternalGuidedFileTest::parsesValidTargets()
{
    QFETCH(QString, text);
    QFETCH(double, latitude);
    QFETCH(double, longitude);
    QFETCH(double, altitude);

    const ExternalGuidedFileResult result = ExternalGuidedFile::parse(text);
    QVERIFY2(result.isValid(), qPrintable(result.error));
    QCOMPARE(result.errorCode, ExternalGuidedFileError::None);
    QCOMPARE(result.waypoint.latitude, latitude);
    QCOMPARE(result.waypoint.longitude, longitude);
    QCOMPARE(result.waypoint.relativeAltitudeM, altitude);
}

void ExternalGuidedFileTest::parsingIsIndependentOfDefaultLocale()
{
    const QLocale previous = QLocale();
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    const ExternalGuidedFileResult invariant =
        ExternalGuidedFile::parse(QStringLiteral("1.5,2.5,3.5"));
    QLocale::setDefault(previous);

    QVERIFY2(invariant.isValid(), qPrintable(invariant.error));
    QCOMPARE(invariant.waypoint.latitude, 1.5);
    QCOMPARE(invariant.waypoint.longitude, 2.5);
    QCOMPARE(invariant.waypoint.relativeAltitudeM, 3.5);
}

void ExternalGuidedFileTest::rejectsInvalidShapeOrNumbers_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<ExternalGuidedFileError>("expectedError");

    QTest::newRow("empty")
        << QString() << ExternalGuidedFileError::InvalidFieldCount;
    QTest::newRow("two-fields")
        << QStringLiteral("1,2")
        << ExternalGuidedFileError::InvalidFieldCount;
    QTest::newRow("four-fields")
        << QStringLiteral("1,2,3,4")
        << ExternalGuidedFileError::InvalidFieldCount;
    QTest::newRow("decimal-comma")
        << QStringLiteral("1.5,2.5,3,5")
        << ExternalGuidedFileError::InvalidFieldCount;
    QTest::newRow("empty-field")
        << QStringLiteral("1,,3")
        << ExternalGuidedFileError::InvalidNumber;
    QTest::newRow("text")
        << QStringLiteral("north,2,3")
        << ExternalGuidedFileError::InvalidNumber;
    QTest::newRow("nan")
        << QStringLiteral("NaN,2,3")
        << ExternalGuidedFileError::InvalidNumber;
    QTest::newRow("positive-infinity")
        << QStringLiteral("inf,2,3")
        << ExternalGuidedFileError::InvalidNumber;
    QTest::newRow("negative-infinity")
        << QStringLiteral("1,-inf,3")
        << ExternalGuidedFileError::InvalidNumber;
}

void ExternalGuidedFileTest::rejectsInvalidShapeOrNumbers()
{
    QFETCH(QString, text);
    QFETCH(ExternalGuidedFileError, expectedError);

    const ExternalGuidedFileResult result = ExternalGuidedFile::parse(text);
    QVERIFY(!result.isValid());
    QCOMPARE(result.errorCode, expectedError);
    QVERIFY(!result.error.isEmpty());
}

void ExternalGuidedFileTest::reportsRangeErrorsPrecisely_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<ExternalGuidedFileError>("expectedError");

    QTest::newRow("latitude-low")
        << QStringLiteral("-90.0000001,2,3")
        << ExternalGuidedFileError::LatitudeOutOfRange;
    QTest::newRow("latitude-high")
        << QStringLiteral("90.0000001,2,3")
        << ExternalGuidedFileError::LatitudeOutOfRange;
    QTest::newRow("longitude-low")
        << QStringLiteral("1,-180.0000001,3")
        << ExternalGuidedFileError::LongitudeOutOfRange;
    QTest::newRow("longitude-high")
        << QStringLiteral("1,180.0000001,3")
        << ExternalGuidedFileError::LongitudeOutOfRange;
    QTest::newRow("altitude-zero")
        << QStringLiteral("1,2,0")
        << ExternalGuidedFileError::AltitudeOutOfRange;
    QTest::newRow("altitude-negative")
        << QStringLiteral("1,2,-0.1")
        << ExternalGuidedFileError::AltitudeOutOfRange;
    QTest::newRow("altitude-high")
        << QStringLiteral("1,2,10000.0001")
        << ExternalGuidedFileError::AltitudeOutOfRange;
}

void ExternalGuidedFileTest::reportsRangeErrorsPrecisely()
{
    QFETCH(QString, text);
    QFETCH(ExternalGuidedFileError, expectedError);

    const ExternalGuidedFileResult result = ExternalGuidedFile::parse(text);
    QVERIFY(!result.isValid());
    QCOMPARE(result.errorCode, expectedError);
    QVERIFY(!result.error.isEmpty());
}

void ExternalGuidedFileTest::enforcesDecodedCharacterBound()
{
    const QString suffix = QStringLiteral("0,0,25");
    const QChar unicodeWhitespace(0x2003);
    const QString maximum(
        ExternalGuidedFile::MaximumCharacters - suffix.size(),
        unicodeWhitespace);
    const QString accepted = maximum + suffix;
    QCOMPARE(accepted.size(), ExternalGuidedFile::MaximumCharacters);

    const ExternalGuidedFileResult acceptedResult =
        ExternalGuidedFile::parse(accepted);
    QVERIFY2(acceptedResult.isValid(), qPrintable(acceptedResult.error));
    QCOMPARE(acceptedResult.waypoint.latitude, 0.0);

    const ExternalGuidedFileResult rejectedResult =
        ExternalGuidedFile::parse(unicodeWhitespace + accepted);
    QVERIFY(!rejectedResult.isValid());
    QCOMPARE(rejectedResult.errorCode,
             ExternalGuidedFileError::TooManyCharacters);
}

void ExternalGuidedFileTest::readsBoundedFilesAndReturnsAbsolutePath()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QByteArray suffix("0,0,25");
    QByteArray maximum(
        static_cast<int>(ExternalGuidedFile::MaximumFileBytes) - suffix.size(),
        ' ');
    maximum.append(suffix);
    QCOMPARE(maximum.size(),
             static_cast<int>(ExternalGuidedFile::MaximumFileBytes));

    const QString acceptedPath = directory.filePath(
        QStringLiteral("target with spaces.txt"));
    QFile acceptedFile(acceptedPath);
    QVERIFY(acceptedFile.open(QIODevice::WriteOnly));
    QCOMPARE(acceptedFile.write(maximum), qint64(maximum.size()));
    acceptedFile.close();

    const QString paddedPath = QStringLiteral("  %1  ").arg(acceptedPath);
    const ExternalGuidedFileResult accepted =
        ExternalGuidedFile::read(paddedPath);
    QVERIFY2(accepted.isValid(), qPrintable(accepted.error));
    QCOMPARE(accepted.absolutePath,
             QDir::cleanPath(QFileInfo(acceptedPath).absoluteFilePath()));
    QCOMPARE(accepted.waypoint.relativeAltitudeM, 25.0);

    const QString rejectedPath = directory.filePath(
        QStringLiteral("too-large.txt"));
    QFile rejectedFile(rejectedPath);
    QVERIFY(rejectedFile.open(QIODevice::WriteOnly));
    maximum.append(' ');
    QCOMPARE(maximum.size(),
             static_cast<int>(ExternalGuidedFile::MaximumFileBytes + 1));
    QCOMPARE(rejectedFile.write(maximum), qint64(maximum.size()));
    rejectedFile.close();

    const ExternalGuidedFileResult rejected =
        ExternalGuidedFile::read(rejectedPath);
    QVERIFY(!rejected.isValid());
    QCOMPARE(rejected.errorCode, ExternalGuidedFileError::TooManyBytes);
    QCOMPARE(rejected.absolutePath,
             QDir::cleanPath(QFileInfo(rejectedPath).absoluteFilePath()));
}

void ExternalGuidedFileTest::readsUtf8BomAndUnicodeWhitespace()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("unicode.csv"));

    QByteArray contents("\xef\xbb\xbf");
    contents.append(QString(1, QChar(0x2003))
                        .append(QStringLiteral("34.5,"))
                        .append(QChar(0x2003))
                        .append(QStringLiteral("33.5,50"))
                        .append(QChar(0x2003))
                        .toUtf8());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(contents), qint64(contents.size()));
    file.close();

    const ExternalGuidedFileResult result = ExternalGuidedFile::read(path);
    QVERIFY2(result.isValid(), qPrintable(result.error));
    QCOMPARE(result.waypoint.latitude, 34.5);
    QCOMPARE(result.waypoint.longitude, 33.5);
    QCOMPARE(result.waypoint.relativeAltitudeM, 50.0);
}

void ExternalGuidedFileTest::reportsMissingAndEmptyPaths()
{
    const ExternalGuidedFileResult empty =
        ExternalGuidedFile::read(QStringLiteral(" \t\n"));
    QVERIFY(!empty.isValid());
    QCOMPARE(empty.errorCode, ExternalGuidedFileError::EmptyPath);
    QVERIFY(empty.absolutePath.isEmpty());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString missingPath = directory.filePath(
        QStringLiteral("missing.txt"));
    const ExternalGuidedFileResult missing =
        ExternalGuidedFile::read(missingPath);
    QVERIFY(!missing.isValid());
    QCOMPARE(missing.errorCode, ExternalGuidedFileError::MissingFile);
    QCOMPARE(missing.absolutePath,
             QDir::cleanPath(QFileInfo(missingPath).absoluteFilePath()));
}

QTEST_APPLESS_MAIN(ExternalGuidedFileTest)
#include "test_externalguidedfile.moc"
