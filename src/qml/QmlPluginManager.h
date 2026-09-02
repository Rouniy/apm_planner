#ifndef QMLPLUGINMANAGER_H
#define QMLPLUGINMANAGER_H

#include <QObject>
#include <QStringList>

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

    void setMainWindow(QObject *mainWindow);
    void setActiveVehicle(QObject *activeVehicle);

    /** Scan explicit roots, or the installed and user roots when empty. */
    Q_INVOKABLE void start(QMenu *toolsMenu,
                           const QStringList &searchRoots = QStringList());
    Q_INVOKABLE void shutdown();

signals:
    void pluginsChanged();
    void pluginLoadError(const QString &pluginId, const QString &message);

private:
    class Private;
    std::unique_ptr<Private> d;
};

#endif // QMLPLUGINMANAGER_H
