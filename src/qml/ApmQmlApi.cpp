#include "ApmQmlApi.h"

#include "AppPaths.h"
#include "ui/flightplanner/MissionCommandCatalog.h"
#include "uas/UASInterface.h"

#include <QCoreApplication>
#include <QMetaMethod>
#include <QThread>
#include <QQmlEngine>

namespace
{

QPointer<ApmQmlApi> s_applicationApi;

// UASInterface's legacy read getters are not part of its Qt meta-object. Keep
// the raw QObject API extensible, then fall back to the direct core interface.
UASInterface *uasInterface(QObject *object)
{
    if (!object || !object->inherits("UASInterface")) {
        return nullptr;
    }
    return static_cast<UASInterface *>(object);
}

bool invokeIntGetter(QObject *object, const char *signature, int *value)
{
    if (!object) {
        return false;
    }
    const int methodIndex = object->metaObject()->indexOfMethod(signature);
    return methodIndex >= 0
        && object->metaObject()->method(methodIndex).invoke(
            object, Qt::DirectConnection, Q_RETURN_ARG(int, *value));
}

bool invokeBoolGetter(QObject *object, const char *signature, bool *value)
{
    if (!object) {
        return false;
    }
    const int methodIndex = object->metaObject()->indexOfMethod(signature);
    return methodIndex >= 0
        && object->metaObject()->method(methodIndex).invoke(
            object, Qt::DirectConnection, Q_RETURN_ARG(bool, *value));
}

bool invokeStringGetter(QObject *object, const char *signature, QString *value)
{
    if (!object) {
        return false;
    }
    const int methodIndex = object->metaObject()->indexOfMethod(signature);
    return methodIndex >= 0
        && object->metaObject()->method(methodIndex).invoke(
            object, Qt::DirectConnection, Q_RETURN_ARG(QString, *value));
}

} // namespace

ApmQmlApi::ApmQmlApi(QObject *uasManager, QObject *linkManager,
                     QObject *settings, QObject *pluginManager,
                     QObject *parent)
    : QObject(parent)
    , m_uasManager(uasManager)
    , m_linkManager(linkManager)
    , m_settings(settings)
    , m_pluginManager(pluginManager)
{
    s_applicationApi = this;
}

ApmQmlApi::~ApmQmlApi()
{
    clearVehicleActions();
    if (s_applicationApi == this) {
        s_applicationApi = nullptr;
    }
}

ApmQmlApi *ApmQmlApi::instance()
{
    return s_applicationApi.data();
}

QString ApmQmlApi::applicationName() const
{
    return QCoreApplication::applicationName();
}

QString ApmQmlApi::applicationVersion() const
{
    return QCoreApplication::applicationVersion();
}

QString ApmQmlApi::resourceRoot() const
{
    return AppPaths::resourceRoot();
}

QString ApmQmlApi::writableDataPath() const
{
    return AppPaths::writableDataDirectory();
}

QObject *ApmQmlApi::application() const
{
    return QCoreApplication::instance();
}

QObject *ApmQmlApi::vehicleTargetManager() const
{
    QObject *targetManager = nullptr;
    if (m_linkManager
        && m_linkManager->metaObject()->indexOfMethod(
               "vehicleTargetManagerObject()") >= 0) {
        QMetaObject::invokeMethod(
            m_linkManager.data(), "vehicleTargetManagerObject",
            Qt::DirectConnection, Q_RETURN_ARG(QObject *, targetManager));
    }
    return targetManager;
}

QObject *ApmQmlApi::vehicleCommandService() const
{
    QObject *service = nullptr;
    if (m_linkManager
        && m_linkManager->metaObject()->indexOfMethod(
               "vehicleCommandServiceObject()") >= 0) {
        QMetaObject::invokeMethod(
            m_linkManager.data(), "vehicleCommandServiceObject",
            Qt::DirectConnection, Q_RETURN_ARG(QObject *, service));
    }
    return service;
}

QObject *ApmQmlApi::parameterService() const
{
    QObject *service = nullptr;
    if (m_linkManager
        && m_linkManager->metaObject()->indexOfMethod(
               "parameterServiceObject()") >= 0) {
        QMetaObject::invokeMethod(
            m_linkManager.data(), "parameterServiceObject",
            Qt::DirectConnection, Q_RETURN_ARG(QObject *, service));
    }
    return service;
}

QObject *ApmQmlApi::missionCommandCatalog() const
{
    return MissionCommandCatalog::instance();
}

int ApmQmlApi::getUASID() const
{
    int value = -1;
    if (invokeIntGetter(m_activeVehicle.data(), "getUASID()", &value)) {
        return value;
    }
    UASInterface *const vehicle = uasInterface(m_activeVehicle.data());
    return vehicle ? vehicle->getUASID() : -1;
}

QString ApmQmlApi::getUASName() const
{
    QString value;
    if (invokeStringGetter(m_activeVehicle.data(), "getUASName()", &value)) {
        return value;
    }
    UASInterface *const vehicle = uasInterface(m_activeVehicle.data());
    return vehicle ? vehicle->getUASName() : QString();
}

bool ApmQmlApi::isArmed() const
{
    bool value = false;
    if (invokeBoolGetter(m_activeVehicle.data(), "isArmed()", &value)) {
        return value;
    }
    UASInterface *const vehicle = uasInterface(m_activeVehicle.data());
    return vehicle ? vehicle->isArmed() : false;
}

QString ApmQmlApi::getShortMode() const
{
    QString value;
    if (invokeStringGetter(m_activeVehicle.data(), "getShortMode()", &value)) {
        return value;
    }
    UASInterface *const vehicle = uasInterface(m_activeVehicle.data());
    return vehicle ? vehicle->getShortMode() : QString();
}

bool ApmQmlApi::registerVehicleAction(const QString &name,
                                      const QJSValue &callback)
{
    return registerVehicleActionImpl(name, callback, QVariantMap());
}

bool ApmQmlApi::registerVehicleAction(const QString &name,
                                      const QJSValue &callback,
                                      const QVariantMap &options)
{
    return registerVehicleActionImpl(name, callback, options);
}

bool ApmQmlApi::unregisterVehicleAction(const QString &name)
{
    const int index = vehicleActionIndex(name.trimmed());
    if (index < 0) {
        return false;
    }
    m_vehicleActions.removeAt(index);
    emit vehicleActionsChanged();
    return true;
}

QStringList ApmQmlApi::vehicleActions() const
{
    QStringList names;
    names.reserve(m_vehicleActions.size());
    for (const VehicleAction &action : m_vehicleActions) {
        names.append(action.name);
    }
    return names;
}

QString ApmQmlApi::vehicleActionAfter(const QString &name) const
{
    const int index = vehicleActionIndex(name);
    return index >= 0 ? m_vehicleActions.at(index).after : QString();
}

QString ApmQmlApi::vehicleActionBefore(const QString &name) const
{
    const int index = vehicleActionIndex(name);
    return index >= 0 ? m_vehicleActions.at(index).before : QString();
}

bool ApmQmlApi::invokeVehicleAction(const QString &name,
                                    QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (QThread::currentThread() != thread()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral(
                "Vehicle actions must run on the GUI thread");
        }
        return false;
    }

    const int index = vehicleActionIndex(name);
    if (index < 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unknown QML vehicle action: %1")
                                .arg(name);
        }
        return false;
    }

    // Copy both values because trusted plugin code may unregister itself.
    // QJSValue::call() is non-const in Qt 5.
    const QString actionName = m_vehicleActions.at(index).name;
    QJSValue callback = m_vehicleActions.at(index).callback;
    const QJSValue result = callback.call(
        QJSValueList{QJSValue(actionName)});
    if (result.isError()) {
        if (errorMessage) {
            *errorMessage = result.toString();
        }
        return false;
    }
    return true;
}

void ApmQmlApi::clearVehicleActions()
{
    if (m_vehicleActions.isEmpty()) {
        return;
    }
    m_vehicleActions.clear();
    emit vehicleActionsChanged();
}

bool ApmQmlApi::registerVehicleActionImpl(
    const QString &name, const QJSValue &callback,
    const QVariantMap &options)
{
    if (QThread::currentThread() != thread() || !callback.isCallable()) {
        return false;
    }

    const QString resolvedName = name.trimmed();
    if (resolvedName.isEmpty() || vehicleActionIndex(resolvedName) >= 0) {
        return false;
    }
    for (auto it = options.cbegin(); it != options.cend(); ++it) {
        if (it.key() != QLatin1String("after")
            && it.key() != QLatin1String("before")) {
            return false;
        }
    }

    VehicleAction action;
    action.name = resolvedName;
    action.callback = callback;
    action.after = options.value(QStringLiteral("after")).toString().trimmed();
    action.before = options.value(QStringLiteral("before")).toString().trimmed();
    m_vehicleActions.append(action);
    emit vehicleActionsChanged();
    return true;
}

int ApmQmlApi::vehicleActionIndex(const QString &name) const
{
    for (int index = 0; index < m_vehicleActions.size(); ++index) {
        if (m_vehicleActions.at(index).name == name) {
            return index;
        }
    }
    return -1;
}

QString ApmQmlApi::resourcePath(const QString &relativePath) const
{
    return AppPaths::resourcePath(relativePath);
}

QObject *ApmQmlApi::service(const QString &name) const
{
    if (name == QLatin1String("application")) {
        return application();
    }
    if (name == QLatin1String("uasManager")) {
        return m_uasManager.data();
    }
    if (name == QLatin1String("activeVehicle")) {
        return m_activeVehicle.data();
    }
    if (name == QLatin1String("linkManager")) {
        return m_linkManager.data();
    }
    if (name == QLatin1String("vehicleTargetManager")) {
        return vehicleTargetManager();
    }
    if (name == QLatin1String("vehicleCommandService")) {
        return vehicleCommandService();
    }
    if (name == QLatin1String("parameterService")) {
        return parameterService();
    }
    if (name == QLatin1String("missionCommandCatalog")) {
        return missionCommandCatalog();
    }
    if (name == QLatin1String("settings")) {
        return m_settings.data();
    }
    if (name == QLatin1String("mainWindow")) {
        return m_mainWindow.data();
    }
    if (name == QLatin1String("pluginManager")) {
        return m_pluginManager.data();
    }
    return nullptr;
}

void ApmQmlApi::setMainWindow(QObject *mainWindow)
{
    if (m_mainWindow == mainWindow) {
        return;
    }
    m_mainWindow = mainWindow;
    emit mainWindowChanged();
}

void ApmQmlApi::setActiveVehicle(QObject *activeVehicle)
{
    if (m_activeVehicle == activeVehicle) {
        return;
    }

    QObject::disconnect(m_activeVehicleDestroyedConnection);
    m_activeVehicleDestroyedConnection = QMetaObject::Connection();
    m_activeVehicle = activeVehicle;
    if (activeVehicle) {
        QQmlEngine::setObjectOwnership(activeVehicle,
                                       QQmlEngine::CppOwnership);
        m_activeVehicleDestroyedConnection = connect(
            activeVehicle, &QObject::destroyed, this, [this]() {
                m_activeVehicle = nullptr;
                m_activeVehicleDestroyedConnection = QMetaObject::Connection();
                emit activeVehicleChanged();
            });
    }
    emit activeVehicleChanged();
}
