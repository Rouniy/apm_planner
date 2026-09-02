#ifndef APMQMLAPI_H
#define APMQMLAPI_H

#include <QObject>
#include <QJSValue>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

/**
 * Direct, trusted in-process access to the application core for QML plugins.
 *
 * This object is exported as the APMPlanner singleton from
 * `APMPlanner.Core 1.0`.  It deliberately does not implement a permission or
 * sandbox boundary: user plugins run with the same authority as the desktop
 * application.
 */
class ApmQmlApi final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int apiMajor READ apiMajor CONSTANT)
    Q_PROPERTY(QString applicationName READ applicationName CONSTANT)
    Q_PROPERTY(QString applicationVersion READ applicationVersion CONSTANT)
    Q_PROPERTY(QString resourceRoot READ resourceRoot CONSTANT)
    Q_PROPERTY(QString writableDataPath READ writableDataPath CONSTANT)
    Q_PROPERTY(QObject *application READ application CONSTANT)
    Q_PROPERTY(QObject *uasManager READ uasManager CONSTANT)
    Q_PROPERTY(QObject *activeVehicle READ activeVehicle NOTIFY activeVehicleChanged)
    Q_PROPERTY(QObject *linkManager READ linkManager CONSTANT)
    Q_PROPERTY(QObject *vehicleTargetManager READ vehicleTargetManager CONSTANT)
    Q_PROPERTY(QObject *vehicleCommandService READ vehicleCommandService CONSTANT)
    Q_PROPERTY(QObject *parameterService READ parameterService CONSTANT)
    Q_PROPERTY(QObject *settings READ settings CONSTANT)
    Q_PROPERTY(QObject *mainWindow READ mainWindow NOTIFY mainWindowChanged)
    Q_PROPERTY(QObject *pluginManager READ pluginManager CONSTANT)
    Q_PROPERTY(QStringList vehicleActions READ vehicleActions
               NOTIFY vehicleActionsChanged)

public:
    static constexpr int ApiMajor = 1;

    explicit ApmQmlApi(QObject *uasManager, QObject *linkManager,
                       QObject *settings, QObject *pluginManager,
                       QObject *parent = nullptr);
    ~ApmQmlApi() override;

    static ApmQmlApi *instance();

    int apiMajor() const { return ApiMajor; }
    QString applicationName() const;
    QString applicationVersion() const;
    QString resourceRoot() const;
    QString writableDataPath() const;

    QObject *application() const;
    QObject *uasManager() const { return m_uasManager.data(); }
    QObject *activeVehicle() const { return m_activeVehicle.data(); }
    QObject *linkManager() const { return m_linkManager.data(); }
    QObject *vehicleTargetManager() const;
    QObject *vehicleCommandService() const;
    QObject *parameterService() const;
    QObject *settings() const { return m_settings.data(); }
    QObject *mainWindow() const { return m_mainWindow.data(); }
    QObject *pluginManager() const { return m_pluginManager.data(); }

    Q_INVOKABLE QString resourcePath(const QString &relativePath) const;
    Q_INVOKABLE QObject *service(const QString &name) const;
    Q_INVOKABLE int getUASID() const;
    Q_INVOKABLE QString getUASName() const;
    Q_INVOKABLE bool isArmed() const;
    Q_INVOKABLE QString getShortMode() const;
    Q_INVOKABLE bool registerVehicleAction(const QString &name,
                                           const QJSValue &callback);
    Q_INVOKABLE bool registerVehicleAction(const QString &name,
                                           const QJSValue &callback,
                                           const QVariantMap &options);
    Q_INVOKABLE bool unregisterVehicleAction(const QString &name);

    QStringList vehicleActions() const;
    QString vehicleActionAfter(const QString &name) const;
    QString vehicleActionBefore(const QString &name) const;
    bool invokeVehicleAction(const QString &name,
                             QString *errorMessage = nullptr);
    void clearVehicleActions();

    void setMainWindow(QObject *mainWindow);
    void setActiveVehicle(QObject *activeVehicle);

signals:
    void mainWindowChanged();
    void activeVehicleChanged();
    void vehicleActionsChanged();

private:
    struct VehicleAction
    {
        QString name;
        QJSValue callback;
        QString after;
        QString before;
    };

    bool registerVehicleActionImpl(const QString &name,
                                   const QJSValue &callback,
                                   const QVariantMap &options);
    int vehicleActionIndex(const QString &name) const;

    QPointer<QObject> m_uasManager;
    QPointer<QObject> m_activeVehicle;
    QPointer<QObject> m_linkManager;
    QPointer<QObject> m_settings;
    QPointer<QObject> m_pluginManager;
    QPointer<QObject> m_mainWindow;
    QMetaObject::Connection m_activeVehicleDestroyedConnection;
    QVector<VehicleAction> m_vehicleActions;
};

#endif // APMQMLAPI_H
