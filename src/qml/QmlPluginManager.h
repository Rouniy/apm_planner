#ifndef QMLPLUGINMANAGER_H
#define QMLPLUGINMANAGER_H

#include <QObject>
#include <QStringList>
#include <QVariantList>

#include <memory>

class ApmQmlApi;
class QMenu;
class QQmlEngine;

/**
 * Discovers trusted QML plugins and hosts all of them in one QQmlEngine.
 *
 * Version 1 intentionally supports one UI contribution only: a modeless
 * toolPage opened from the Tools menu.
 */
class QmlPluginManager final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int pluginCount READ pluginCount NOTIFY pluginsChanged)
    Q_PROPERTY(QStringList pluginIds READ pluginIds NOTIFY pluginsChanged)
    Q_PROPERTY(QStringList errors READ errors NOTIFY pluginsChanged)
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY pluginsChanged)
    Q_PROPERTY(QStringList searchRoots READ searchRoots NOTIFY pluginsChanged)
    Q_PROPERTY(QString writablePluginDirectory READ writablePluginDirectory CONSTANT)

public:
    explicit QmlPluginManager(QObject *uasManager, QObject *linkManager,
                              QObject *settings,
                              QObject *parent = nullptr);
    ~QmlPluginManager() override;

    QQmlEngine *engine() const;
    ApmQmlApi *api() const;

    int pluginCount() const;
    QStringList pluginIds() const;
    QStringList errors() const;
    QVariantList plugins() const;
    QStringList searchRoots() const;
    QString writablePluginDirectory() const;

    void setMainWindow(QObject *mainWindow);
    void setActiveVehicle(QObject *activeVehicle);

    /** Scan explicit roots, or the installed and user roots when empty. */
    void start(QMenu *toolsMenu,
               const QStringList &searchRoots = QStringList());
    Q_INVOKABLE void reload();
    Q_INVOKABLE bool openPlugin(const QString &pluginId);
    void shutdown();

signals:
    void pluginsChanged();
    void pluginLoadError(const QString &pluginId, const QString &message);

private:
    class Private;
    std::unique_ptr<Private> d;
};

#endif // QMLPLUGINMANAGER_H
