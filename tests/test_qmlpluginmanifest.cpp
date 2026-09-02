#include <QtTest>

#include "qml/QmlPluginManifest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

QJsonObject validManifest()
{
    return {
        {QStringLiteral("manifestVersion"), 1},
        {QStringLiteral("id"), QStringLiteral("org.example.battery-monitor")},
        {QStringLiteral("name"), QStringLiteral("Battery Monitor")},
        {QStringLiteral("version"), QStringLiteral("1.2.3-beta.1+linux")},
        {QStringLiteral("apiMajor"), 1},
        {QStringLiteral("main"), QStringLiteral("qml/Main.qml")},
        {QStringLiteral("ui"), QJsonObject{
             {QStringLiteral("type"), QStringLiteral("toolPage")},
             {QStringLiteral("title"), QStringLiteral("Battery Monitor")}}}
    };
}

QmlPluginManifest parse(const QJsonObject &object,
                        const QString &pluginDirectory)
{
    return QmlPluginManifest::parse(
        QJsonDocument(object).toJson(QJsonDocument::Compact),
        pluginDirectory);
}

bool writeFile(const QString &path, const QByteArray &contents = QByteArray())
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

} // namespace

class QmlPluginManifestTest final : public QObject
{
    Q_OBJECT

private slots:
    void acceptsCompleteManifestAndResolvesPaths();
    void loadsManifestFromFile();
    void rejectsInvalidJsonAndNonObjectRoot();
    void rejectsMissingAndUnknownFields();
    void rejectsWrongFieldTypes();
    void validatesIds_data();
    void validatesIds();
    void validatesSemanticVersions_data();
    void validatesSemanticVersions();
    void rejectsUnsupportedSchemaApiAndUiType();
    void rejectsUnsafePaths_data();
    void rejectsUnsafePaths();
    void rejectsExistingSymlinkEscape();
};

void QmlPluginManifestTest::acceptsCompleteManifestAndResolvesPaths()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(writeFile(directory.filePath(QStringLiteral("qml/Main.qml")),
                      QByteArrayLiteral("import QtQuick 2.15\nItem {}\n")));
    QVERIFY(writeFile(directory.filePath(QStringLiteral("assets/icon.svg")),
                      QByteArrayLiteral("<svg/>")));

    QJsonObject object = validManifest();
    object.insert(QStringLiteral("description"),
                  QStringLiteral("Battery diagnostics"));
    object.insert(QStringLiteral("author"), QStringLiteral("Example"));
    QJsonObject ui = object.value(QStringLiteral("ui")).toObject();
    ui.insert(QStringLiteral("icon"), QStringLiteral("assets/icon.svg"));
    object.insert(QStringLiteral("ui"), ui);

    const QmlPluginManifest manifest = parse(object, directory.path());
    QVERIFY2(manifest.isValid(), qPrintable(manifest.errorString()));
    QCOMPARE(manifest.error(), QmlPluginManifest::Error::None);
    QCOMPARE(manifest.manifestVersion(), 1);
    QCOMPARE(manifest.apiMajor(), 1);
    QCOMPARE(manifest.id(), QStringLiteral("org.example.battery-monitor"));
    QCOMPARE(manifest.name(), QStringLiteral("Battery Monitor"));
    QCOMPARE(manifest.version(), QStringLiteral("1.2.3-beta.1+linux"));
    QCOMPARE(manifest.main(), QStringLiteral("qml/Main.qml"));
    QCOMPARE(manifest.mainFilePath(),
             QFileInfo(directory.filePath(QStringLiteral("qml/Main.qml")))
                 .absoluteFilePath());
    QCOMPARE(manifest.ui().type, QStringLiteral("toolPage"));
    QCOMPARE(manifest.ui().title, QStringLiteral("Battery Monitor"));
    QCOMPARE(manifest.ui().icon, QStringLiteral("assets/icon.svg"));
    QCOMPARE(manifest.ui().iconFilePath,
             QFileInfo(directory.filePath(QStringLiteral("assets/icon.svg")))
                 .absoluteFilePath());
    QCOMPARE(manifest.description(), QStringLiteral("Battery diagnostics"));
    QCOMPARE(manifest.author(), QStringLiteral("Example"));
    QCOMPARE(manifest.pluginDirectory(),
             QFileInfo(directory.path()).canonicalFilePath());
}

void QmlPluginManifestTest::loadsManifestFromFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString manifestPath = directory.filePath(
        QStringLiteral("apm-plugin.json"));
    QVERIFY(writeFile(manifestPath,
                      QJsonDocument(validManifest()).toJson()));

    const QmlPluginManifest manifest = QmlPluginManifest::load(manifestPath);
    QVERIFY2(manifest.isValid(), qPrintable(manifest.errorString()));
    QCOMPARE(manifest.manifestFilePath(),
             QFileInfo(manifestPath).absoluteFilePath());

    const QmlPluginManifest missing = QmlPluginManifest::load(
        directory.filePath(QStringLiteral("missing.json")));
    QCOMPARE(missing.error(), QmlPluginManifest::Error::CannotOpen);
}

void QmlPluginManifestTest::rejectsInvalidJsonAndNonObjectRoot()
{
    QTemporaryDir directory;
    const QmlPluginManifest invalid = QmlPluginManifest::parse(
        QByteArrayLiteral("{oops"), directory.path());
    QCOMPARE(invalid.error(), QmlPluginManifest::Error::InvalidJson);
    QVERIFY(!invalid.errorString().isEmpty());

    const QmlPluginManifest array = QmlPluginManifest::parse(
        QByteArrayLiteral("[]"), directory.path());
    QCOMPARE(array.error(), QmlPluginManifest::Error::RootNotObject);
}

void QmlPluginManifestTest::rejectsMissingAndUnknownFields()
{
    QTemporaryDir directory;
    QJsonObject object = validManifest();
    object.remove(QStringLiteral("name"));
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::MissingField);

    object = validManifest();
    object.insert(QStringLiteral("capabilities"), QJsonArray{});
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::UnknownField);

    object = validManifest();
    QJsonObject ui = object.value(QStringLiteral("ui")).toObject();
    ui.insert(QStringLiteral("dockArea"), QStringLiteral("right"));
    object.insert(QStringLiteral("ui"), ui);
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::UnknownField);

    object = validManifest();
    ui = object.value(QStringLiteral("ui")).toObject();
    ui.remove(QStringLiteral("title"));
    object.insert(QStringLiteral("ui"), ui);
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::MissingField);
}

void QmlPluginManifestTest::rejectsWrongFieldTypes()
{
    QTemporaryDir directory;
    const QList<QString> stringFields{
        QStringLiteral("id"), QStringLiteral("name"),
        QStringLiteral("version"), QStringLiteral("main")
    };
    for (const QString &field : stringFields) {
        QJsonObject object = validManifest();
        object.insert(field, 17);
        QCOMPARE(parse(object, directory.path()).error(),
                 QmlPluginManifest::Error::WrongType);
    }

    QJsonObject object = validManifest();
    object.insert(QStringLiteral("manifestVersion"), QStringLiteral("1"));
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::WrongType);

    object = validManifest();
    object.insert(QStringLiteral("apiMajor"), QStringLiteral("1"));
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::WrongType);

    object = validManifest();
    object.insert(QStringLiteral("ui"), QStringLiteral("toolPage"));
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::WrongType);

    object = validManifest();
    object.insert(QStringLiteral("description"), 42);
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::WrongType);

    object = validManifest();
    QJsonObject ui = object.value(QStringLiteral("ui")).toObject();
    ui.insert(QStringLiteral("icon"), 42);
    object.insert(QStringLiteral("ui"), ui);
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::WrongType);
}

void QmlPluginManifestTest::validatesIds_data()
{
    QTest::addColumn<QString>("id");
    QTest::addColumn<bool>("accepted");

    QTest::newRow("two parts") << QStringLiteral("org.example") << true;
    QTest::newRow("hyphen") << QStringLiteral("org.example.my-plugin") << true;
    QTest::newRow("one part") << QStringLiteral("example") << false;
    QTest::newRow("uppercase") << QStringLiteral("org.Example.plugin") << false;
    QTest::newRow("underscore") << QStringLiteral("org.example.my_plugin") << false;
    QTest::newRow("empty segment") << QStringLiteral("org..plugin") << false;
    QTest::newRow("trailing hyphen") << QStringLiteral("org.example.plugin-") << false;
}

void QmlPluginManifestTest::validatesIds()
{
    QFETCH(QString, id);
    QFETCH(bool, accepted);
    QTemporaryDir directory;
    QJsonObject object = validManifest();
    object.insert(QStringLiteral("id"), id);
    const QmlPluginManifest manifest = parse(object, directory.path());
    QCOMPARE(manifest.isValid(), accepted);
    if (!accepted) {
        QCOMPARE(manifest.error(), QmlPluginManifest::Error::InvalidId);
    }
}

void QmlPluginManifestTest::validatesSemanticVersions_data()
{
    QTest::addColumn<QString>("version");
    QTest::addColumn<bool>("accepted");

    QTest::newRow("release") << QStringLiteral("1.0.0") << true;
    QTest::newRow("prerelease") << QStringLiteral("2.7.1-rc.1") << true;
    QTest::newRow("build") << QStringLiteral("0.0.0+git.abc-12") << true;
    QTest::newRow("missing patch") << QStringLiteral("1.2") << false;
    QTest::newRow("leading zero") << QStringLiteral("01.2.3") << false;
    QTest::newRow("numeric prerelease leading zero")
                                         << QStringLiteral("1.2.3-01") << false;
    QTest::newRow("prefix") << QStringLiteral("v1.2.3") << false;
    QTest::newRow("whitespace") << QStringLiteral("1.2.3 ") << false;
}

void QmlPluginManifestTest::validatesSemanticVersions()
{
    QFETCH(QString, version);
    QFETCH(bool, accepted);
    QTemporaryDir directory;
    QJsonObject object = validManifest();
    object.insert(QStringLiteral("version"), version);
    const QmlPluginManifest manifest = parse(object, directory.path());
    QCOMPARE(manifest.isValid(), accepted);
    if (!accepted) {
        QCOMPARE(manifest.error(), QmlPluginManifest::Error::InvalidVersion);
    }
}

void QmlPluginManifestTest::rejectsUnsupportedSchemaApiAndUiType()
{
    QTemporaryDir directory;
    QJsonObject object = validManifest();
    object.insert(QStringLiteral("manifestVersion"), 2);
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::UnsupportedManifestVersion);

    object = validManifest();
    object.insert(QStringLiteral("apiMajor"), 2);
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::UnsupportedApiMajor);

    object = validManifest();
    QJsonObject ui = object.value(QStringLiteral("ui")).toObject();
    ui.insert(QStringLiteral("type"), QStringLiteral("dockPanel"));
    object.insert(QStringLiteral("ui"), ui);
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::InvalidUiType);
}

void QmlPluginManifestTest::rejectsUnsafePaths_data()
{
    QTest::addColumn<bool>("iconPath");
    QTest::addColumn<QString>("path");

    QTest::newRow("main parent") << false << QStringLiteral("../Main.qml");
    QTest::newRow("main nested parent")
        << false << QStringLiteral("qml/../Main.qml");
    QTest::newRow("main absolute") << false << QStringLiteral("/tmp/Main.qml");
    QTest::newRow("main windows absolute")
        << false << QStringLiteral("C:/plugins/Main.qml");
    QTest::newRow("main backslash")
        << false << QStringLiteral("qml\\Main.qml");
    QTest::newRow("main wrong suffix")
        << false << QStringLiteral("qml/Main.js");
    QTest::newRow("icon parent") << true << QStringLiteral("../icon.svg");
    QTest::newRow("icon absolute") << true << QStringLiteral("/tmp/icon.svg");
    QTest::newRow("icon empty") << true << QString();
}

void QmlPluginManifestTest::rejectsUnsafePaths()
{
    QFETCH(bool, iconPath);
    QFETCH(QString, path);
    QTemporaryDir directory;
    QJsonObject object = validManifest();
    if (iconPath) {
        QJsonObject ui = object.value(QStringLiteral("ui")).toObject();
        ui.insert(QStringLiteral("icon"), path);
        object.insert(QStringLiteral("ui"), ui);
    } else {
        object.insert(QStringLiteral("main"), path);
    }
    QCOMPARE(parse(object, directory.path()).error(),
             QmlPluginManifest::Error::InvalidPath);
}

void QmlPluginManifestTest::rejectsExistingSymlinkEscape()
{
    QTemporaryDir pluginDirectory;
    QTemporaryDir outsideDirectory;
    QVERIFY(pluginDirectory.isValid());
    QVERIFY(outsideDirectory.isValid());
    const QString outsideMain = outsideDirectory.filePath(
        QStringLiteral("Outside.qml"));
    QVERIFY(writeFile(outsideMain, QByteArrayLiteral("import QtQuick 2.15\nItem {}\n")));
    QVERIFY(QDir().mkpath(pluginDirectory.filePath(QStringLiteral("qml"))));
    const QString linkedMain = pluginDirectory.filePath(
        QStringLiteral("qml/Main.qml"));
    if (!QFile::link(outsideMain, linkedMain)) {
        QSKIP("This platform does not permit creating a symbolic link");
    }

    const QmlPluginManifest manifest = parse(validManifest(),
                                             pluginDirectory.path());
    QCOMPARE(manifest.error(), QmlPluginManifest::Error::PathEscapesPlugin);

    QVERIFY(QFile::remove(linkedMain));
    QVERIFY(writeFile(linkedMain,
                      QByteArrayLiteral("import QtQuick 2.15\nItem {}\n")));
    const QString linkedIcon = pluginDirectory.filePath(
        QStringLiteral("assets/icon.svg"));
    QVERIFY(QDir().mkpath(QFileInfo(linkedIcon).absolutePath()));
    if (!QFile::link(outsideMain, linkedIcon)) {
        QSKIP("This platform does not permit creating a second symbolic link");
    }
    QJsonObject object = validManifest();
    QJsonObject ui = object.value(QStringLiteral("ui")).toObject();
    ui.insert(QStringLiteral("icon"), QStringLiteral("assets/icon.svg"));
    object.insert(QStringLiteral("ui"), ui);
    QCOMPARE(parse(object, pluginDirectory.path()).error(),
             QmlPluginManifest::Error::PathEscapesPlugin);
}

QTEST_GUILESS_MAIN(QmlPluginManifestTest)
#include "test_qmlpluginmanifest.moc"
