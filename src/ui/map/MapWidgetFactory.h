#ifndef MAPWIDGETFACTORY_H
#define MAPWIDGETFACTORY_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class AbstractMapWidget;
enum class MapWidgetRole;
class QSettings;
class QWidget;

struct MapWidgetBackendInfo
{
    QString id;
    QString displayName;
};

class MapWidgetFactory final : public QObject
{
    Q_OBJECT

public:
    using Creator = std::function<AbstractMapWidget *(
        MapWidgetRole role, const QString &sharedCacheRoot,
        QWidget *widgetParent, QObject *owner)>;

    static constexpr const char *SettingsKey = "MapWidgetBackend";
    static constexpr const char *OPMapBackendId = "OPMapControl";
    static constexpr const char *QGroundControlBackendId =
        "QGroundControl";
    static constexpr const char *SharedCacheContract =
        "APMPlanner3.PureImageCache.v1";

    explicit MapWidgetFactory(QSettings *settings = nullptr,
                              QObject *parent = nullptr);
    ~MapWidgetFactory() override;

    static MapWidgetFactory *instance();

    bool RegisterBackend(const QString &id, const QString &displayName,
                         const QString &cacheContract,
                         const Creator &creator);
    QList<MapWidgetBackendInfo> AvailableBackends() const;
    QString RequestedBackend() const;
    QString CurrentBackend() const;
    QString LastStatus() const;
    QString SharedCacheRoot() const;
    AbstractMapWidget *CreateMapWidget(MapWidgetRole role,
                                      QWidget *widgetParent,
                                      QObject *owner = nullptr);

public slots:
    bool SetBackend(const QString &id);

signals:
    void BackendChanged(const QString &id);
    void AvailableBackendsChanged();
    void StatusMessage(const QString &message);

private:
    struct Registration
    {
        QString displayName;
        Creator creator;
    };

    QString normalizedBackend(const QString &requested) const;
    static QString canonicalBackend(const QString &requested);
    void saveRequestedBackend();

    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings = nullptr;
    QHash<QString, Registration> m_backends;
    QStringList m_registrationOrder;
    QString m_requestedBackend;
    QString m_effectiveBackend;
    QString m_lastStatus;
};

#endif // MAPWIDGETFACTORY_H
