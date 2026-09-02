#include <QtTest>

#include "qml/ApmQmlApi.h"
#include "qml/QmlPluginManager.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWidget>

namespace
{

class TestActiveVehicle final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double latitude READ getLatitude CONSTANT)
    Q_PROPERTY(double longitude READ getLongitude CONSTANT)
    Q_PROPERTY(double altitudeRelative READ getAltitudeRelative CONSTANT)
    Q_PROPERTY(double roll READ getRoll CONSTANT)
    Q_PROPERTY(double pitch READ getPitch CONSTANT)
    Q_PROPERTY(double yaw READ getYaw CONSTANT)

public:
    explicit TestActiveVehicle(QObject *parent = nullptr)
        : QObject(parent)
    {
        setObjectName(QStringLiteral("testVehicle"));
    }

    Q_INVOKABLE int getUASID() const { return 42; }
    Q_INVOKABLE QString getUASName() const
    {
        return QStringLiteral("Test Rover");
    }
    Q_INVOKABLE bool isArmed() const { return true; }
    Q_INVOKABLE QString getShortMode() const
    {
        return QStringLiteral("AUTO");
    }

    double getLatitude() const { return 35.125; }
    double getLongitude() const { return 33.875; }
    double getAltitudeRelative() const { return 12.5; }
    double getRoll() const { return 0.1; }
    double getPitch() const { return -0.2; }
    double getYaw() const { return 1.25; }

    Q_INVOKABLE QList<int> getComponentIds() const { return {1, 154}; }
    Q_INVOKABLE QList<QString> getParameterNames(int component) const
    {
        return component == 1
            ? QList<QString>({QStringLiteral("ARMING_CHECK"),
                              QStringLiteral("RTL_ALT")})
            : QList<QString>();
    }
    Q_INVOKABLE QVariant getParameterValue(
        int component, const QString &parameter) const
    {
        if (component != 1) {
            return QVariant();
        }
        if (parameter == QLatin1String("ARMING_CHECK")) {
            return 1;
        }
        if (parameter == QLatin1String("RTL_ALT")) {
            return 1500.5;
        }
        return QVariant();
    }
};

class TestLinkManager final : public QObject
{
    Q_OBJECT
public:
    explicit TestLinkManager(QObject *parent = nullptr)
        : QObject(parent)
    {
        m_vehicleTargetManager.setObjectName(
            QStringLiteral("testVehicleTargetManager"));
        m_vehicleCommandService.setObjectName(
            QStringLiteral("testVehicleCommandService"));
        m_parameterService.setObjectName(
            QStringLiteral("testParameterService"));
    }

    Q_INVOKABLE QObject *vehicleTargetManagerObject()
    {
        return &m_vehicleTargetManager;
    }

    Q_INVOKABLE QObject *parameterServiceObject()
    {
        return &m_parameterService;
    }

    Q_INVOKABLE QObject *vehicleCommandServiceObject()
    {
        return &m_vehicleCommandService;
    }

private:
    QObject m_vehicleTargetManager;
    QObject m_vehicleCommandService;
    QObject m_parameterService;
};

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

bool createPlugin(const QString &root, const QString &directoryName,
                  const QString &id, const QString &title,
                  const QByteArray &qml)
{
    const QString directory = QDir(root).filePath(directoryName);
    const QJsonObject manifest{
        {QStringLiteral("manifestVersion"), 1},
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), title},
        {QStringLiteral("version"), QStringLiteral("1.0.0")},
        {QStringLiteral("apiMajor"), 1},
        {QStringLiteral("main"), QStringLiteral("Main.qml")},
        {QStringLiteral("ui"), QJsonObject{
             {QStringLiteral("type"), QStringLiteral("toolPage")},
             {QStringLiteral("title"), title}}}
    };
    return writeFile(QDir(directory).filePath(QStringLiteral("apm-plugin.json")),
                     QJsonDocument(manifest).toJson())
        && writeFile(QDir(directory).filePath(QStringLiteral("Main.qml")), qml);
}

QByteArray validQml()
{
    return QByteArrayLiteral(
        "import QtQuick 2.12\n"
        "import APMPlanner.Core 1.0\n"
        "Item {\n"
        "  id: root\n"
        "  width: 320; height: 200\n"
        "  property int apiMajorSeen: APMPlanner.apiMajor\n"
        "  property string uasName: APMPlanner.uasManager.objectName\n"
        "  property string vehicleObjectName: APMPlanner.activeVehicle.objectName\n"
        "  property int vehicleId: APMPlanner.getUASID()\n"
        "  property string vehicleName: APMPlanner.getUASName()\n"
        "  property bool vehicleArmed: APMPlanner.isArmed()\n"
        "  property string vehicleMode: APMPlanner.getShortMode()\n"
        "  property real vehicleLatitude: APMPlanner.activeVehicle.latitude\n"
        "  property real vehicleLongitude: APMPlanner.activeVehicle.longitude\n"
        "  property real vehicleAltitudeRelative: APMPlanner.activeVehicle.altitudeRelative\n"
        "  property real vehicleRoll: APMPlanner.activeVehicle.roll\n"
        "  property real vehiclePitch: APMPlanner.activeVehicle.pitch\n"
        "  property real vehicleYaw: APMPlanner.activeVehicle.yaw\n"
        "  property int parameterComponentCount: APMPlanner.activeVehicle.getComponentIds().length\n"
        "  property int firstParameterComponent: APMPlanner.activeVehicle.getComponentIds()[0]\n"
        "  property int parameterNameCount: APMPlanner.activeVehicle.getParameterNames(1).length\n"
        "  property string firstParameterName: APMPlanner.activeVehicle.getParameterNames(1)[0]\n"
        "  property int armingCheck: APMPlanner.activeVehicle.getParameterValue(1, \"ARMING_CHECK\")\n"
        "  property real rtlAltitude: APMPlanner.activeVehicle.getParameterValue(1, \"RTL_ALT\")\n"
        "  property bool missingParameterIsNullish: {\n"
        "    var value = APMPlanner.activeVehicle.getParameterValue(1, \"MISSING\")\n"
        "    return value === undefined || value === null\n"
        "  }\n"
        "  property bool missingComponentIsEmpty: APMPlanner.activeVehicle.getParameterNames(99).length === 0\n"
        "  property string managerName: APMPlanner.pluginManager.objectName\n"
        "  property string targetManagerName: APMPlanner.vehicleTargetManager.objectName\n"
        "  property string commandServiceName: APMPlanner.vehicleCommandService.objectName\n"
        "  property string parameterServiceName: APMPlanner.parameterService.objectName\n"
        "  property string pluginIdSeen: plugin.id\n"
        "  property bool serviceMatches: APMPlanner.service(\"uasManager\") === APMPlanner.uasManager\n"
        "  property bool parameterServiceMatches: APMPlanner.service(\"parameterService\") === APMPlanner.parameterService\n"
        "  property bool missionCatalogMatches: APMPlanner.service(\"missionCommandCatalog\") === APMPlanner.missionCommandCatalog\n"
        "  property int missionCommandCount: APMPlanner.missionCommandCatalog.commandNames.length\n"
        "  property bool commandServiceMatches: APMPlanner.service(\"vehicleCommandService\") === APMPlanner.vehicleCommandService\n"
        "  property int customActionInvocations: 0\n"
        "  property string customActionArgument: \"\"\n"
        "  property bool customActionRegistered: false\n"
        "  Component.onCompleted: {\n"
        "    customActionRegistered = APMPlanner.registerVehicleAction(\n"
        "      \"QML Test Action\", function(name) {\n"
        "        root.customActionInvocations += 1\n"
        "        root.customActionArgument = name\n"
        "      }, {\"after\": \"Mission Start\"})\n"
        "  }\n"
        "}\n");
}

} // namespace

class QmlPluginManagerTest final : public QObject
{
    Q_OBJECT

private slots:
    void discoversAndOpensToolPageWithDirectCoreApi();
    void clearsDestroyedActiveVehicleAndNotifies();
    void rejectsDuplicateIds();
    void isolatesBrokenQmlAtRuntime();
};

void QmlPluginManagerTest::discoversAndOpensToolPageWithDirectCoreApi()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(createPlugin(root.path(), QStringLiteral("z-plugin"),
                         QStringLiteral("org.example.zulu"),
                         QStringLiteral("Zulu Tool"), validQml()));
    QVERIFY(createPlugin(root.path(), QStringLiteral("a-plugin"),
                         QStringLiteral("org.example.alpha"),
                         QStringLiteral("Alpha Tool"), validQml()));

    QObject uasManager;
    uasManager.setObjectName(QStringLiteral("testUasManager"));
    TestLinkManager linkManager;
    QObject settings;
    TestActiveVehicle activeVehicle;
    QWidget mainWindow;
    QMenu toolsMenu;
    QmlPluginManager manager(&uasManager, &linkManager, &settings);
    manager.setObjectName(QStringLiteral("testPluginManager"));
    manager.setActiveVehicle(&activeVehicle);
    manager.setMainWindow(&mainWindow);
    QSignalSpy vehicleActionsChangedSpy(
        manager.api(), &ApmQmlApi::vehicleActionsChanged);

    int metaObjectUasId = -1;
    QVERIFY(QMetaObject::invokeMethod(
        manager.api(), "getUASID", Qt::DirectConnection,
        Q_RETURN_ARG(int, metaObjectUasId)));
    QCOMPARE(metaObjectUasId, 42);
    QVERIFY(activeVehicle.metaObject()->indexOfMethod("getComponentIds()")
            >= 0);
    QVERIFY(activeVehicle.metaObject()->indexOfMethod(
                "getParameterNames(int)") >= 0);
    QVERIFY(activeVehicle.metaObject()->indexOfMethod(
                "getParameterValue(int,QString)") >= 0);
    QVariant metaObjectParameterValue;
    QVERIFY(QMetaObject::invokeMethod(
        &activeVehicle, "getParameterValue", Qt::DirectConnection,
        Q_RETURN_ARG(QVariant, metaObjectParameterValue), Q_ARG(int, 1),
        Q_ARG(QString, QStringLiteral("ARMING_CHECK"))));
    QCOMPARE(metaObjectParameterValue, QVariant(1));
    QVERIFY(!activeVehicle.getParameterValue(
                1, QStringLiteral("MISSING")).isValid());

    QQmlEngine *const sharedEngine = manager.engine();
    QVERIFY(sharedEngine);
    manager.start(&toolsMenu, {root.path()});

    QCOMPARE(manager.engine(), sharedEngine);
    QCOMPARE(manager.pluginCount(), 2);
    QCOMPARE(manager.pluginIds(),
             QStringList({QStringLiteral("org.example.alpha"),
                          QStringLiteral("org.example.zulu")}));
    QVERIFY(manager.errors().isEmpty());

    QMenu *pluginMenu = toolsMenu.findChild<QMenu *>(
        QStringLiteral("menuQmlPlugins"));
    QVERIFY(pluginMenu);
    QCOMPARE(pluginMenu->actions().size(), 2);
    QCOMPARE(pluginMenu->actions().first()->text(), QStringLiteral("Alpha Tool"));

    pluginMenu->actions().first()->trigger();
    QDialog *window = mainWindow.findChild<QDialog *>(
        QStringLiteral("QmlPluginWindow_org.example.alpha"));
    QTRY_VERIFY(window);
    QQuickWidget *view = window->findChild<QQuickWidget *>(
        QStringLiteral("QmlPluginView_org.example.alpha"));
    QVERIFY(view);
    QCOMPARE(view->engine(), sharedEngine);
    QCOMPARE(view->status(), QQuickWidget::Ready);
    QVERIFY(view->rootObject());
    QCOMPARE(view->rootObject()->property("apiMajorSeen").toInt(), 1);
    QCOMPARE(view->rootObject()->property("uasName").toString(),
             QStringLiteral("testUasManager"));
    QCOMPARE(view->rootObject()->property("vehicleObjectName").toString(),
             QStringLiteral("testVehicle"));
    QCOMPARE(view->rootObject()->property("vehicleId").toInt(), 42);
    QCOMPARE(view->rootObject()->property("vehicleName").toString(),
             QStringLiteral("Test Rover"));
    QVERIFY(view->rootObject()->property("vehicleArmed").toBool());
    QCOMPARE(view->rootObject()->property("vehicleMode").toString(),
             QStringLiteral("AUTO"));
    QCOMPARE(view->rootObject()->property("vehicleLatitude").toDouble(),
             35.125);
    QCOMPARE(view->rootObject()->property("vehicleLongitude").toDouble(),
             33.875);
    QCOMPARE(view->rootObject()
                 ->property("vehicleAltitudeRelative").toDouble(),
             12.5);
    QCOMPARE(view->rootObject()->property("vehicleRoll").toDouble(), 0.1);
    QCOMPARE(view->rootObject()->property("vehiclePitch").toDouble(), -0.2);
    QCOMPARE(view->rootObject()->property("vehicleYaw").toDouble(), 1.25);
    QCOMPARE(view->rootObject()->property("parameterComponentCount").toInt(),
             2);
    QCOMPARE(view->rootObject()->property("firstParameterComponent").toInt(),
             1);
    QCOMPARE(view->rootObject()->property("parameterNameCount").toInt(), 2);
    QCOMPARE(view->rootObject()->property("firstParameterName").toString(),
             QStringLiteral("ARMING_CHECK"));
    QCOMPARE(view->rootObject()->property("armingCheck").toInt(), 1);
    QCOMPARE(view->rootObject()->property("rtlAltitude").toDouble(), 1500.5);
    QVERIFY(view->rootObject()->property("missingParameterIsNullish").toBool());
    QVERIFY(view->rootObject()->property("missingComponentIsEmpty").toBool());
    QCOMPARE(view->rootObject()->property("managerName").toString(),
             QStringLiteral("testPluginManager"));
    QCOMPARE(view->rootObject()->property("pluginIdSeen").toString(),
             QStringLiteral("org.example.alpha"));
    QCOMPARE(view->rootObject()->property("targetManagerName").toString(),
             QStringLiteral("testVehicleTargetManager"));
    QCOMPARE(view->rootObject()->property("commandServiceName").toString(),
             QStringLiteral("testVehicleCommandService"));
    QCOMPARE(view->rootObject()->property("parameterServiceName").toString(),
             QStringLiteral("testParameterService"));
    QCOMPARE(manager.api()->service(QStringLiteral("vehicleTargetManager")),
             manager.api()->vehicleTargetManager());
    QCOMPARE(manager.api()->service(QStringLiteral("parameterService")),
             manager.api()->parameterService());
    QCOMPARE(manager.api()->service(QStringLiteral("vehicleCommandService")),
             manager.api()->vehicleCommandService());
    QVERIFY(view->rootObject()->property("serviceMatches").toBool());
    QVERIFY(view->rootObject()->property("parameterServiceMatches").toBool());
    QVERIFY(view->rootObject()->property("missionCatalogMatches").toBool());
    QVERIFY(view->rootObject()->property("missionCommandCount").toInt() > 100);
    QVERIFY(view->rootObject()->property("commandServiceMatches").toBool());
    QVERIFY(view->rootObject()->property("customActionRegistered").toBool());
    QCOMPARE(manager.api()->vehicleActions(),
             QStringList({QStringLiteral("QML Test Action")}));
    QCOMPARE(manager.api()->vehicleActionAfter(
                 QStringLiteral("QML Test Action")),
             QStringLiteral("Mission Start"));
    QCOMPARE(vehicleActionsChangedSpy.size(), 1);

    QString actionError;
    QVERIFY(manager.api()->invokeVehicleAction(
        QStringLiteral("QML Test Action"), &actionError));
    QVERIFY(actionError.isEmpty());
    QCOMPARE(view->rootObject()->property("customActionInvocations").toInt(),
             1);
    QCOMPARE(view->rootObject()->property("customActionArgument").toString(),
             QStringLiteral("QML Test Action"));
    QVERIFY(!manager.api()->invokeVehicleAction(
        QStringLiteral("Missing Action"), &actionError));
    QVERIFY(actionError.contains(QStringLiteral("Missing Action")));

    manager.shutdown();
    QCOMPARE(manager.pluginCount(), 0);
    QVERIFY(manager.api()->vehicleActions().isEmpty());
    QCOMPARE(vehicleActionsChangedSpy.size(), 2);
    QVERIFY(!toolsMenu.findChild<QMenu *>(QStringLiteral("menuQmlPlugins")));
}

void QmlPluginManagerTest::clearsDestroyedActiveVehicleAndNotifies()
{
    QObject uasManager;
    TestLinkManager linkManager;
    QObject settings;
    QmlPluginManager manager(&uasManager, &linkManager, &settings);
    QSignalSpy changedSpy(manager.api(), &ApmQmlApi::activeVehicleChanged);

    auto *activeVehicle = new TestActiveVehicle;
    manager.setActiveVehicle(activeVehicle);
    QCOMPARE(manager.api()->activeVehicle(), activeVehicle);
    QCOMPARE(changedSpy.size(), 1);

    delete activeVehicle;

    QVERIFY(!manager.api()->activeVehicle());
    QVERIFY(!manager.api()->service(QStringLiteral("activeVehicle")));
    QCOMPARE(changedSpy.size(), 2);
    QCOMPARE(manager.api()->getUASID(), -1);
    QVERIFY(manager.api()->getUASName().isEmpty());
    QVERIFY(!manager.api()->isArmed());
    QVERIFY(manager.api()->getShortMode().isEmpty());
}

void QmlPluginManagerTest::rejectsDuplicateIds()
{
    QTemporaryDir firstRoot;
    QTemporaryDir secondRoot;
    QVERIFY(firstRoot.isValid());
    QVERIFY(secondRoot.isValid());
    QVERIFY(createPlugin(firstRoot.path(), QStringLiteral("one"),
                         QStringLiteral("org.example.duplicate"),
                         QStringLiteral("First"), validQml()));
    QVERIFY(createPlugin(secondRoot.path(), QStringLiteral("two"),
                         QStringLiteral("org.example.duplicate"),
                         QStringLiteral("Second"), validQml()));

    QObject uasManager;
    TestLinkManager linkManager;
    QObject settings;
    QMenu toolsMenu;
    QmlPluginManager manager(&uasManager, &linkManager, &settings);
    QSignalSpy errorSpy(&manager, &QmlPluginManager::pluginLoadError);
    manager.start(&toolsMenu, {firstRoot.path(), secondRoot.path()});

    QCOMPARE(manager.pluginCount(), 0);
    QCOMPARE(errorSpy.size(), 1);
    QCOMPARE(errorSpy.first().first().toString(),
             QStringLiteral("org.example.duplicate"));
    QVERIFY(manager.errors().constFirst().contains(QStringLiteral("Duplicate")));
    QVERIFY(!toolsMenu.findChild<QMenu *>(QStringLiteral("menuQmlPlugins")));
}

void QmlPluginManagerTest::isolatesBrokenQmlAtRuntime()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(createPlugin(root.path(), QStringLiteral("broken"),
                         QStringLiteral("org.example.broken"),
                         QStringLiteral("Broken"),
                         QByteArrayLiteral("import QtQuick 2.12\nItem { broken: }\n")));

    QObject uasManager;
    TestLinkManager linkManager;
    QObject settings;
    QWidget mainWindow;
    QMenu toolsMenu;
    QmlPluginManager manager(&uasManager, &linkManager, &settings);
    manager.setMainWindow(&mainWindow);
    manager.start(&toolsMenu, {root.path()});
    QCOMPARE(manager.pluginCount(), 1);

    QSignalSpy errorSpy(&manager, &QmlPluginManager::pluginLoadError);
    QMenu *pluginMenu = toolsMenu.findChild<QMenu *>(
        QStringLiteral("menuQmlPlugins"));
    QVERIFY(pluginMenu);
    pluginMenu->actions().constFirst()->trigger();

    QCOMPARE(errorSpy.size(), 1);
    QCOMPARE(errorSpy.first().first().toString(),
             QStringLiteral("org.example.broken"));
    QVERIFY(!manager.errors().isEmpty());
    QVERIFY(!mainWindow.findChild<QDialog *>(
        QStringLiteral("QmlPluginWindow_org.example.broken")));
}

QTEST_MAIN(QmlPluginManagerTest)
#include "test_qmlpluginmanager.moc"
