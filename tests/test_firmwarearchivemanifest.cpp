#include "services/FirmwareArchiveManifest.h"

#include <QDomElement>
#include <QtTest>

namespace {
QString key(const QUrl &url)
{
    return QString::fromLatin1(url.toEncoded(QUrl::FullyEncoded));
}

QVector<QDomElement> descendants(const QDomElement &root)
{
    QVector<QDomElement> result;
    QVector<QDomNode> pending;
    for (QDomNode child = root.lastChild(); !child.isNull();
         child = child.previousSibling()) {
        pending.append(child);
    }
    while (!pending.isEmpty()) {
        const QDomNode node = pending.takeLast();
        if (node.isElement()) result.append(node.toElement());
        for (QDomNode child = node.lastChild(); !child.isNull();
             child = child.previousSibling()) {
            pending.append(child);
        }
    }
    return result;
}

QString localName(const QDomElement &element)
{
    return element.localName().isEmpty() ? element.tagName() : element.localName();
}
}

class FirmwareArchiveManifestTest final : public QObject
{
    Q_OBJECT

private slots:
    void urlPolicyAndArchivePaths();
    void parsesNamespacesUtf8AndDeduplicates();
    void rejectsUnsafeOrInvalidManifest_data();
    void rejectsUnsafeOrInvalidManifest();
    void rejectsDepthSizeAndDownloadLimits();
    void rewritesEverySuccessfulDuplicateOnly();
    void rewriteRejectsUnplannedOrChangedPaths();
    void includesRootUrlAndCanonicalizesEncoding();
};

void FirmwareArchiveManifestTest::urlPolicyAndArchivePaths()
{
    const QUrl secure(QStringLiteral(
        "https://cdn.example/vehicle/copter.apj?version=1"), QUrl::StrictMode);
    QVERIFY(FirmwareArchiveManifest::allowedUrl(secure, true));
    QVERIFY(FirmwareArchiveManifest::allowedUrl(secure, false));
    QCOMPARE(FirmwareArchiveManifest::relativePath(secure),
             QStringLiteral("files/cdn.example/95123b1c8854-copter.apj"));

    QVERIFY(!FirmwareArchiveManifest::allowedUrl(
        QUrl(QStringLiteral("http://cdn.example/fw.apj"), QUrl::StrictMode), true));
    QVERIFY(FirmwareArchiveManifest::allowedUrl(
        QUrl(QStringLiteral("http://cdn.example/fw.apj"), QUrl::StrictMode), false));
    QVERIFY(!FirmwareArchiveManifest::allowedUrl(
        QUrl(QStringLiteral("ftp://cdn.example/fw.apj"), QUrl::StrictMode), false));
    QVERIFY(!FirmwareArchiveManifest::allowedUrl(
        QUrl(QStringLiteral("../fw.apj"), QUrl::StrictMode), false));
    QVERIFY(!FirmwareArchiveManifest::allowedUrl(
        QUrl(QStringLiteral("https://user:secret@cdn.example/fw.apj"),
             QUrl::StrictMode), false));
    QVERIFY(!FirmwareArchiveManifest::allowedUrl(
        QUrl(QStringLiteral("https://@cdn.example/fw.apj"), QUrl::StrictMode), false));

    const QUrl first(QStringLiteral(
        "https://cdn.example/a/%2E%2E/%2Fescape.apj?version=one"),
        QUrl::StrictMode);
    const QUrl second(QStringLiteral(
        "https://cdn.example/a/%2E%2E/%2Fescape.apj?version=two"),
        QUrl::StrictMode);
    const QString firstPath = FirmwareArchiveManifest::relativePath(first);
    const QString secondPath = FirmwareArchiveManifest::relativePath(second);
    QVERIFY(firstPath.startsWith(QStringLiteral("files/cdn.example/")));
    QVERIFY(!firstPath.contains(QStringLiteral("..")));
    QVERIFY(!firstPath.contains(QLatin1Char(':')));
    QVERIFY(!firstPath.contains(QLatin1Char('\\')));
    QVERIFY(firstPath != secondPath);

    const QString idn = FirmwareArchiveManifest::relativePath(QUrl(
        QStringLiteral("https://münich.example/прошивка.apj"), QUrl::StrictMode));
    QVERIFY(idn.startsWith(QStringLiteral("files/xn--mnich-kva.example/")));
    QVERIFY(idn.endsWith(QStringLiteral("-прошивка.apj")));
    const QString reservedHost = FirmwareArchiveManifest::relativePath(QUrl(
        QStringLiteral("https://con/CON"), QUrl::StrictMode));
    QVERIFY(reservedHost.startsWith(QStringLiteral("files/_con/")));
    QVERIFY(reservedHost.endsWith(QStringLiteral("-_CON")));
    const QString ipv6 = FirmwareArchiveManifest::relativePath(QUrl(
        QStringLiteral("https://[2001:db8::1]/fw.apj"), QUrl::StrictMode));
    QVERIFY(ipv6.startsWith(QStringLiteral("files/2001_db8__1/")));

    const QString longPath = FirmwareArchiveManifest::relativePath(QUrl(
        QStringLiteral("https://cdn.example/") + QString(200, QLatin1Char('x'))
            + QStringLiteral(".apj"), QUrl::StrictMode));
    QCOMPARE(longPath.section(QLatin1Char('/'), -1).size(), 12 + 1 + 96);
}

void FirmwareArchiveManifestTest::includesRootUrlAndCanonicalizesEncoding()
{
    const auto root = FirmwareArchiveManifest::parse("<url>https://example/fw.apj</url>");
    QVERIFY2(root.success, qPrintable(root.error));
    QCOMPARE(root.downloads.size(), 1);
    QString error;
    const QByteArray rootXml = FirmwareArchiveManifest::rewrite(root,
        {{key(root.downloads.first().uri), root.downloads.first().relativePath}}, &error);
    QVERIFY(error.isEmpty());
    QDomDocument rootOutput;
    QVERIFY(rootOutput.setContent(rootXml));
    QCOMPARE(rootOutput.documentElement().text(), root.downloads.first().relativePath);
    QVERIFY(rootXml.startsWith("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));

    const QString body = QStringLiteral("<options><name>café</name><url>https://example/fw.apj</url></options>");
    const QByteArray latin = (QStringLiteral("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>") + body).toLatin1();
    const QString utf16Text = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-16\"?>") + body;
    QByteArray utf16 = QByteArray::fromHex("fffe");
    for (const QChar c : utf16Text) {
        utf16.append(char(c.unicode() & 0xff));
        utf16.append(char(c.unicode() >> 8));
    }
    for (const QByteArray &input : {latin, utf16}) {
        const auto plan = FirmwareArchiveManifest::parse(input);
        QVERIFY2(plan.success, qPrintable(plan.error));
        const auto &download = plan.downloads.first();
        const QByteArray bytes = FirmwareArchiveManifest::rewrite(plan,
            {{key(download.uri), download.relativePath}}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(bytes.startsWith("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
        QVERIFY(!bytes.contains('\0'));
        QDomDocument reparsed;
        QVERIFY(reparsed.setContent(bytes));
        QCOMPARE(reparsed.elementsByTagName("name").at(0).toElement().text(), QStringLiteral("café"));
    }
}

void FirmwareArchiveManifestTest::parsesNamespacesUtf8AndDeduplicates()
{
    const QByteArray xml = QString::fromUtf8(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<options xmlns:fw=\"urn:firmware\"><!-- keep-comment -->"
        "<name>Архив прошивок</name>"
        "<fw:URLMain> https://cdn.example/vehicle/copter.apj </fw:URLMain>"
        "<urlDuplicate><![CDATA[https://cdn.example/vehicle/copter.apj]]></urlDuplicate>"
        "<fw:uRlEmpty/><noturl>https://ignored.example/fw.apj</noturl>"
        "</options>").toUtf8();

    const auto plan = FirmwareArchiveManifest::parse(xml);
    QVERIFY2(plan.success, qPrintable(plan.error));
    QCOMPARE(plan.downloads.size(), 1);
    QCOMPARE(plan.downloads.first().uri,
             QUrl(QStringLiteral("https://cdn.example/vehicle/copter.apj")));
    QVERIFY(plan.downloads.first().relativePath.startsWith(
        QStringLiteral("files/cdn.example/")));
    QVERIFY(plan.document.toByteArray().contains("keep-comment"));
    QVERIFY(plan.document.toByteArray().contains(QString::fromUtf8(
        "Архив прошивок").toUtf8()));
}

void FirmwareArchiveManifestTest::rejectsUnsafeOrInvalidManifest_data()
{
    QTest::addColumn<QByteArray>("xml");
    QTest::newRow("doctype-external-entity")
        << QByteArray("<!DOCTYPE options [<!ENTITY xxe SYSTEM 'file:///etc/passwd'>]>"
                      "<options><url>&xxe;</url></options>");
    QTest::newRow("ftp")
        << QByteArray("<options><url>ftp://example/fw.apj</url></options>");
    QTest::newRow("credentials")
        << QByteArray("<options><url>https://user:pw@example/fw.apj</url></options>");
    QTest::newRow("relative")
        << QByteArray("<options><url>../fw.apj</url></options>");
    QTest::newRow("one-invalid-invalidates-all")
        << QByteArray("<options><url>https://example/ok.apj</url>"
                      "<url2>file:///etc/passwd</url2></options>");
    QTest::newRow("malformed")
        << QByteArray("<options><url>https://example/fw.apj</options>");
    QTest::newRow("only-empty") << QByteArray("<options><url/></options>");
}

void FirmwareArchiveManifestTest::rejectsUnsafeOrInvalidManifest()
{
    QFETCH(QByteArray, xml);
    const auto plan = FirmwareArchiveManifest::parse(xml);
    QVERIFY(!plan.success);
    QVERIFY(!plan.error.isEmpty());
    QVERIFY(plan.downloads.isEmpty());
}

void FirmwareArchiveManifestTest::rejectsDepthSizeAndDownloadLimits()
{
    QByteArray deep;
    for (int i = 0; i < 129; ++i) deep += "<n>";
    deep += "<url>https://example/fw.apj</url>";
    for (int i = 0; i < 129; ++i) deep += "</n>";
    const auto deepPlan = FirmwareArchiveManifest::parse(deep);
    QVERIFY(!deepPlan.success);
    QVERIFY(deepPlan.error.contains(QStringLiteral("depth"), Qt::CaseInsensitive));

    QByteArray oversized(FirmwareArchive::MaximumManifestBytes + 1, 'x');
    const auto oversizedPlan = FirmwareArchiveManifest::parse(oversized);
    QVERIFY(!oversizedPlan.success);
    QVERIFY(oversizedPlan.error.contains(QStringLiteral("safety limit")));

    QByteArray nodes("<options><url>https://example/fw.apj</url>");
    for (int i = 0; i < 100000; ++i) nodes += "<x/>";
    nodes += "</options>";
    const auto nodesPlan = FirmwareArchiveManifest::parse(nodes);
    QVERIFY(!nodesPlan.success);
    QVERIFY(nodesPlan.error.contains(QStringLiteral("node"),
                                      Qt::CaseInsensitive));

    QByteArray downloads("<options>");
    for (int i = 0; i <= FirmwareArchive::MaximumDownloads; ++i) {
        downloads += "<url>https://example/fw-" + QByteArray::number(i)
            + ".apj</url>";
    }
    downloads += "</options>";
    const auto downloadsPlan = FirmwareArchiveManifest::parse(downloads);
    QVERIFY(!downloadsPlan.success);
    QVERIFY(downloadsPlan.error.contains(QStringLiteral("download"),
                                          Qt::CaseInsensitive));
}

void FirmwareArchiveManifestTest::rewritesEverySuccessfulDuplicateOnly()
{
    const QUrl first(QStringLiteral("https://cdn.example/a.apj"));
    const QUrl second(QStringLiteral("http://legacy.example/b.apj?x=1"));
    const QByteArray xml = QByteArray(
        "<options><!-- outside-comment --><Firmware><name>Copter</name>"
        "<url>") + first.toEncoded() + "</url><URL2>" + first.toEncoded()
        + "</URL2><url3> " + second.toEncoded()
        + " </url3></Firmware></options>";
    const auto plan = FirmwareArchiveManifest::parse(xml);
    QVERIFY2(plan.success, qPrintable(plan.error));
    QCOMPARE(plan.downloads.size(), 2);

    const QString firstPath = plan.downloads.at(0).relativePath;
    QHash<QString, QString> successful;
    successful.insert(key(first), firstPath);
    QString error;
    const QByteArray rewritten = FirmwareArchiveManifest::rewrite(
        plan, successful, &error);
    QVERIFY2(!rewritten.isEmpty(), qPrintable(error));
    QVERIFY(rewritten.contains("outside-comment"));

    QDomDocument output;
    QVERIFY(output.setContent(rewritten, true));
    int changed = 0;
    int unchanged = 0;
    for (const QDomElement &element : descendants(output.documentElement())) {
        if (!localName(element).startsWith(
                QStringLiteral("url"), Qt::CaseInsensitive)) continue;
        if (element.text() == firstPath) ++changed;
        if (element.text().trimmed() == second.toString(QUrl::FullyEncoded)) {
            ++unchanged;
        }
    }
    QCOMPARE(changed, 2);
    QCOMPARE(unchanged, 1);
    QCOMPARE(output.elementsByTagName(QStringLiteral("name")).at(0)
                 .toElement().text(), QStringLiteral("Copter"));
    QCOMPARE(plan.document.elementsByTagName(QStringLiteral("url")).at(0)
                 .toElement().text(), first.toString(QUrl::FullyEncoded));
}

void FirmwareArchiveManifestTest::rewriteRejectsUnplannedOrChangedPaths()
{
    const QUrl url(QStringLiteral("https://cdn.example/fw.apj"));
    const auto plan = FirmwareArchiveManifest::parse(QByteArray(
        "<options><url>https://cdn.example/fw.apj</url></options>"));
    QVERIFY(plan.success);

    QString error;
    QHash<QString, QString> successful;
    successful.insert(QStringLiteral("https://other.example/fw.apj"),
                      QStringLiteral("files/other/fw.apj"));
    QVERIFY(FirmwareArchiveManifest::rewrite(plan, successful, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    successful.clear();
    successful.insert(key(url), QStringLiteral("../escape.apj"));
    QVERIFY(FirmwareArchiveManifest::rewrite(plan, successful, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    auto changed = plan;
    changed.downloads[0].relativePath = QStringLiteral("files/changed/fw.apj");
    successful.clear();
    QVERIFY(FirmwareArchiveManifest::rewrite(changed, successful, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(FirmwareArchiveManifestTest)
#include "test_firmwarearchivemanifest.moc"
