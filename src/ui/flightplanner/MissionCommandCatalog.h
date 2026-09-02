#ifndef MISSIONCOMMANDCATALOG_H
#define MISSIONCOMMANDCATALOG_H

#include <QObject>
#include <QHash>
#include <QStringList>
#include <QVariantList>
#include <QVector>

class QSettings;

struct MissionCommandDefinition
{
    quint16 Id = 0;
    QString Name;
    QStringList ParameterLabels;
};

Q_DECLARE_METATYPE(MissionCommandDefinition)

class MissionCommandCatalog final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList commandNames READ Names NOTIFY catalogChanged)
    Q_PROPERTY(QVariantList definitions READ DefinitionMaps
               NOTIFY catalogChanged)
    Q_PROPERTY(qulonglong revision READ Revision NOTIFY revisionChanged)

public:
    explicit MissionCommandCatalog(QObject *parent = nullptr);
    MissionCommandCatalog(QSettings *settings, QObject *parent = nullptr);
    ~MissionCommandCatalog() override;

    static MissionCommandCatalog *instance();

    QVector<MissionCommandDefinition> LoadDefinitions() const;
    QStringList Names() const;
    bool TryGetId(const QString &name, quint16 *id) const;
    Q_INVOKABLE QString GetName(int id) const;
    Q_INVOKABLE int GetId(const QString &name) const;
    Q_INVOKABLE QStringList GetLabels(int id) const;
    Q_INVOKABLE QStringList EffectiveLabels(int id) const;
    Q_INVOKABLE QVariantList DefinitionMaps() const;
    qulonglong Revision() const { return m_revision; }

    bool Save(const QVector<MissionCommandDefinition> &definitions,
              QString *error = nullptr);
    Q_INVOKABLE bool SaveDefinitionMaps(const QVariantList &definitions);
    Q_INVOKABLE void Reload();

    static QStringList NormalizeLabels(const QStringList &labels);
    static bool Validate(
        const QVector<MissionCommandDefinition> &definitions,
        QString *error = nullptr);
    static QString CanonicalName(quint16 id);
    static bool IsKnownName(const QString &name, quint16 *id = nullptr);

signals:
    void catalogChanged();
    void revisionChanged(qulonglong revision);
    void saveFailed(const QString &error);

private:
    void ensureFresh() const;
    void loadFromSettings();
    static QString normalizedName(const QString &name);

    QSettings *m_settings = nullptr;
    bool m_ownsSettings = false;
    QVector<MissionCommandDefinition> m_definitions;
    QHash<QString, QStringList> m_labels;
    QHash<QString, quint16> m_extraIds;
    QString m_loadedLabelsJson;
    QString m_loadedIdsJson;
    qulonglong m_revision = 0;
};

#endif // MISSIONCOMMANDCATALOG_H
