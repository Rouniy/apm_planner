#include <QtTest>

#include "comm/CotEventSerializer.h"

#include <QXmlStreamReader>

#include <limits>

namespace {

QDateTime instant()
{
    return QDateTime(QDate(2026, 8, 21), QTime(12, 34, 56, 789), Qt::UTC);
}

CotNavigationState navigation()
{
    CotNavigationState state;
    state.latitudeDegrees = 35.1234567;
    state.longitudeDegrees = 33.7654321;
    state.altitudeAmslMetres = 123.45;
    state.courseDegrees = 270.5;
    state.speedMetresPerSecond = 12.25;
    return state;
}

} // namespace

class CotEventSerializerTest final : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndFallbackUidMatchMp10();
    void identityOverrideAddsAdvancedElementsInOrder();
    void emptyIdentityUidSuppressesFallbackAndAdvancedFields();
    void advancedModeOffKeepsOnlyGlobalCallsign();
    void canonicalSiFormattingAndUtcTimestampsAreDeterministic();
    void attributeValuesAreEscapedAsUtf8Xml();
    void percentPlaceholdersRemainLiteral();
    void invalidXmlCharactersAreRejected();
    void nonFiniteNavigationAndInvalidTimeAreRejected();
    void indentedAndCompactOutputContainTheSameEvent();
};

void CotEventSerializerTest::defaultsAndFallbackUidMatchMp10()
{
    const CotEventSettings settings;
    QCOMPARE(CotEventSerializer::ResolveUid(settings, 1, 100),
             QStringLiteral("MissionPlanner-1-100"));

    const QString xml = CotEventSerializer::Serialize(
        settings, 1, 100, navigation(), instant());
    const QString expected = QStringLiteral(
        "<event version=\"2.0\" uid=\"MissionPlanner-1-100\" "
        "type=\"a-f-A-M-F-Q\" time=\"2026-08-21T12:34:56.789Z\" "
        "start=\"2026-08-21T12:34:51.789Z\" "
        "stale=\"2026-08-21T12:36:56.789Z\" how=\"m-g\">"
        "<detail><track course=\"270.50\" speed=\"12.25\" /></detail>"
        "<point lat=\"35.1234567\" lon=\"33.7654321\" hae=\"123.45\" "
        "ce=\"1.0\" le=\"1.0\" /></event>");
    QCOMPARE(xml, expected);
    QVERIFY(!xml.startsWith(QStringLiteral("<?xml")));
    QVERIFY(!xml.endsWith(QLatin1Char('\n')));
}

void CotEventSerializerTest::identityOverrideAddsAdvancedElementsInOrder()
{
    CotEventSettings settings;
    settings.callsign = QStringLiteral("Global");
    CotIdentityOverride identity;
    identity.uid = QStringLiteral("UAS-42");
    identity.includeTakv = true;
    identity.contactCallsign = QStringLiteral("Falcon 42");
    identity.contactEndpoint = QStringLiteral("10.0.0.42:4242:tcp");
    identity.vmf = QStringLiteral("VMF-42");

    QCOMPARE(CotEventSerializer::ResolveUid(settings, 42, 1, &identity),
             QStringLiteral("UAS-42"));
    const QString xml = CotEventSerializer::Serialize(
        settings, 42, 1, navigation(), instant(), &identity);
    const int takv = xml.indexOf(QStringLiteral("<takv />"));
    const int contact = xml.indexOf(QStringLiteral("<contact "));
    const int vmf = xml.indexOf(QStringLiteral("<uid vmf="));
    const int track = xml.indexOf(QStringLiteral("<track "));
    QVERIFY(takv > 0);
    QVERIFY(takv < contact && contact < vmf && vmf < track);
    QVERIFY(xml.contains(QStringLiteral(
        "<contact callsign=\"Falcon 42\" endpoint=\"10.0.0.42:4242:tcp\" />")));
    QVERIFY(xml.contains(QStringLiteral("<uid vmf=\"VMF-42\" />")));
    QVERIFY(!xml.contains(QStringLiteral("Global-42")));
}

void CotEventSerializerTest::emptyIdentityUidSuppressesFallbackAndAdvancedFields()
{
    CotEventSettings settings;
    settings.callsign = QStringLiteral("Copter");
    CotIdentityOverride identity;
    identity.uid = QString();
    identity.includeTakv = true;
    identity.contactCallsign = QStringLiteral("Override");
    identity.contactEndpoint = QStringLiteral("*:4242:tcp");
    identity.vmf = QStringLiteral("VMF");

    const QString xml = CotEventSerializer::Serialize(
        settings, 7, 1, navigation(), instant(), &identity);
    QVERIFY(xml.contains(QStringLiteral(" uid=\"\" ")));
    QVERIFY(!xml.contains(QStringLiteral("MissionPlanner-7-1")));
    QVERIFY(!xml.contains(QStringLiteral("<takv")));
    QVERIFY(!xml.contains(QStringLiteral("endpoint=")));
    QVERIFY(!xml.contains(QStringLiteral("vmf=")));
    QVERIFY(xml.contains(QStringLiteral("<contact callsign=\"Copter-7\" />")));
}

void CotEventSerializerTest::advancedModeOffKeepsOnlyGlobalCallsign()
{
    CotEventSettings settings;
    settings.callsign = QStringLiteral("Global");
    settings.advancedIdentityFields = false;
    CotIdentityOverride identity;
    identity.uid = QStringLiteral("UAS-9");
    identity.includeTakv = true;
    identity.contactCallsign = QStringLiteral("Override");
    identity.contactEndpoint = QStringLiteral("host:1:tcp");
    identity.vmf = QStringLiteral("VMF-9");

    const QString xml = CotEventSerializer::Serialize(
        settings, 9, 2, navigation(), instant(), &identity);
    QVERIFY(xml.contains(QStringLiteral(" uid=\"UAS-9\" ")));
    QVERIFY(xml.contains(QStringLiteral("<contact callsign=\"Global-9\" />")));
    QVERIFY(!xml.contains(QStringLiteral("Override")));
    QVERIFY(!xml.contains(QStringLiteral("endpoint=")));
    QVERIFY(!xml.contains(QStringLiteral("vmf=")));
    QVERIFY(!xml.contains(QStringLiteral("<takv")));
}

void CotEventSerializerTest::canonicalSiFormattingAndUtcTimestampsAreDeterministic()
{
    CotEventSettings settings;
    CotNavigationState state;
    state.latitudeDegrees = -0.5;
    state.longitudeDegrees = 179.0;
    state.altitudeAmslMetres = 10.0;          // stays metres, never feet
    state.courseDegrees = 5.0;
    state.speedMetresPerSecond = 2.0;         // stays m/s, never knots
    const QDateTime offset(QDate(2026, 8, 21), QTime(15, 34, 56, 7),
                           Qt::OffsetFromUTC, 3 * 3600);

    const QString xml = CotEventSerializer::Serialize(
        settings, 1, 1, state, offset);
    QVERIFY(xml.contains(QStringLiteral("time=\"2026-08-21T12:34:56.007Z\"")));
    QVERIFY(xml.contains(QStringLiteral("start=\"2026-08-21T12:34:51.007Z\"")));
    QVERIFY(xml.contains(QStringLiteral("stale=\"2026-08-21T12:36:56.007Z\"")));
    QVERIFY(xml.contains(QStringLiteral("course=\"5.00\" speed=\"2.00\"")));
    QVERIFY(xml.contains(QStringLiteral(
        "lat=\"-0.5000000\" lon=\"179.0000000\" hae=\"10.00\"")));
}

void CotEventSerializerTest::attributeValuesAreEscapedAsUtf8Xml()
{
    CotEventSettings settings;
    settings.uidPrefix = QString::fromUtf8("München<&\"");
    settings.eventType = QStringLiteral("a&b<c>\"d");
    settings.callsign = QStringLiteral("Unit\n\"A&<");
    CotIdentityOverride identity;
    identity.uid = QString::fromUtf8("БПЛА<&\"");
    identity.contactEndpoint = QStringLiteral("host?a=1&b=\"2\"");

    const QString xml = CotEventSerializer::Serialize(
        settings, 4, 1, navigation(), instant(), &identity);
    QVERIFY(xml.contains(QString::fromUtf8("uid=\"БПЛА&lt;&amp;&quot;\"")));
    QVERIFY(xml.contains(QStringLiteral("type=\"a&amp;b&lt;c&gt;&quot;d\"")));
    QVERIFY(xml.contains(QStringLiteral("callsign=\"Unit&#xA;&quot;A&amp;&lt;-4\"")));
    QVERIFY(xml.contains(QStringLiteral("endpoint=\"host?a=1&amp;b=&quot;2&quot;\"")));

    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
    }
    QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
    QVERIFY(xml.toUtf8().contains(QByteArray::fromRawData("\xD0\x91\xD0\x9F\xD0\x9B\xD0\x90", 8)));
}

void CotEventSerializerTest::percentPlaceholdersRemainLiteral()
{
    CotEventSettings settings;
    settings.uidPrefix = QStringLiteral("prefix-%1-%2");
    settings.callsign = QStringLiteral("call-%2-%1");

    QCOMPARE(CotEventSerializer::ResolveUid(settings, 42, 7),
             QStringLiteral("prefix-%1-%2-42-7"));
    const QString xml = CotEventSerializer::Serialize(
        settings, 42, 7, navigation(), instant());
    QVERIFY(xml.contains(QStringLiteral("uid=\"prefix-%1-%2-42-7\"")));
    QVERIFY(xml.contains(QStringLiteral("callsign=\"call-%2-%1-42\"")));
}

void CotEventSerializerTest::invalidXmlCharactersAreRejected()
{
    CotEventSettings settings;
    settings.eventType = QStringLiteral("a-f-");
    settings.eventType.append(QChar(0x0001));

    QString xml = QStringLiteral("unchanged");
    QString error;
    QVERIFY(!CotEventSerializer::TrySerialize(
        settings, 1, 1, navigation(), instant(), &xml, &error));
    QVERIFY(xml.isNull());
    QVERIFY(error.contains(QStringLiteral("event type")));
    QVERIFY(CotEventSerializer::Serialize(
        settings, 1, 1, navigation(), instant()).isNull());

    settings = CotEventSettings();
    CotIdentityOverride identity;
    identity.uid = QStringLiteral("UAS-");
    identity.uid.append(QChar(0xD800)); // Unpaired high surrogate.
    xml = QStringLiteral("unchanged");
    QVERIFY(!CotEventSerializer::TrySerialize(
        settings, 1, 1, navigation(), instant(), &xml, &error, &identity));
    QVERIFY(xml.isNull());
    QVERIFY(error.contains(QStringLiteral("event UID")));

    identity.uid = QStringLiteral("UAS-");
    identity.uid.append(QChar(0xDC00)); // Unpaired low surrogate.
    QVERIFY(!CotEventSerializer::Validate(
        settings, 1, 1, navigation(), instant(), &error, &identity));
    QVERIFY(error.contains(QStringLiteral("event UID")));

    identity.uid = QStringLiteral("UAS-") + QString::fromUtf8("\xF0\x9F\x9A\x81");
    QVERIFY(CotEventSerializer::TrySerialize(
        settings, 1, 1, navigation(), instant(), &xml, &error, &identity));
    QVERIFY(error.isEmpty());
    QVERIFY(xml.contains(identity.uid));
}

void CotEventSerializerTest::nonFiniteNavigationAndInvalidTimeAreRejected()
{
    const QString fieldNames[] = {
        QStringLiteral("latitude"),
        QStringLiteral("longitude"),
        QStringLiteral("altitude"),
        QStringLiteral("course"),
        QStringLiteral("speed"),
    };
    for (int field = 0; field < 5; ++field) {
        CotNavigationState state = navigation();
        const double invalid = field == 4
            ? std::numeric_limits<double>::infinity()
            : std::numeric_limits<double>::quiet_NaN();
        switch (field) {
        case 0: state.latitudeDegrees = invalid; break;
        case 1: state.longitudeDegrees = invalid; break;
        case 2: state.altitudeAmslMetres = invalid; break;
        case 3: state.courseDegrees = invalid; break;
        case 4: state.speedMetresPerSecond = invalid; break;
        }

        QString xml = QStringLiteral("unchanged");
        QString error;
        QVERIFY(!CotEventSerializer::TrySerialize(
            CotEventSettings(), 1, 1, state, instant(), &xml, &error));
        QVERIFY(xml.isNull());
        QVERIFY2(error.contains(fieldNames[field]), qPrintable(error));
    }

    QString xml = QStringLiteral("unchanged");
    QString error;
    QVERIFY(!CotEventSerializer::TrySerialize(
        CotEventSettings(), 1, 1, navigation(), QDateTime(), &xml, &error));
    QVERIFY(xml.isNull());
    QCOMPARE(error, QStringLiteral("CoT timestamp is invalid."));
}

void CotEventSerializerTest::indentedAndCompactOutputContainTheSameEvent()
{
    CotEventSettings compactSettings;
    CotEventSettings indentedSettings;
    indentedSettings.indentXml = true;
    CotIdentityOverride identity;
    identity.uid = QStringLiteral("UAS-1");
    identity.includeTakv = true;
    identity.contactCallsign = QStringLiteral("One");

    const QString compact = CotEventSerializer::Serialize(
        compactSettings, 1, 1, navigation(), instant(), &identity);
    const QString indented = CotEventSerializer::Serialize(
        indentedSettings, 1, 1, navigation(), instant(), &identity);
#ifdef Q_OS_WIN
    const QString nl = QStringLiteral("\r\n");
#else
    const QString nl = QStringLiteral("\n");
#endif

    QVERIFY(!compact.contains(QLatin1Char('\n')));
    QVERIFY(indented.startsWith(QStringLiteral("<event") + nl
                                + QStringLiteral("  version=\"2.0\"")));
    QVERIFY(indented.contains(nl + QStringLiteral("  <detail>") + nl
                              + QStringLiteral("    <takv />")));
    QVERIFY(indented.contains(nl + QStringLiteral("    <contact") + nl
                              + QStringLiteral("      callsign=\"One\" />")));
    QVERIFY(indented.contains(nl + QStringLiteral("  <point") + nl
                              + QStringLiteral("    lat=\"35.1234567\"")));
    QVERIFY(indented.endsWith(nl + QStringLiteral("</event>")));
#ifdef Q_OS_WIN
    QCOMPARE(indented.count(QStringLiteral("\r\n")),
             indented.count(QLatin1Char('\n')));
#else
    QVERIFY(!indented.contains(QLatin1Char('\r')));
#endif
    QCOMPARE(indented.count(nl), 22);

    QXmlStreamReader compactReader(compact);
    QXmlStreamReader indentedReader(indented);
    while (!compactReader.atEnd()) compactReader.readNext();
    while (!indentedReader.atEnd()) indentedReader.readNext();
    QVERIFY(!compactReader.hasError());
    QVERIFY(!indentedReader.hasError());
}

QTEST_GUILESS_MAIN(CotEventSerializerTest)
#include "test_coteventserializer.moc"
