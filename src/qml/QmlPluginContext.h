#ifndef QMLPLUGINCONTEXT_H
#define QMLPLUGINCONTEXT_H

#include <QObject>
#include <QString>
#include <QVariant>

/** Per-plugin metadata and a convenience settings namespace. */
class QmlPluginContext final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString path READ path CONSTANT)

public:
    QmlPluginContext(const QString &id, const QString &name,
                     const QString &version, const QString &path,
                     QObject *parent = nullptr);

    QString id() const { return m_id; }
    QString name() const { return m_name; }
    QString version() const { return m_version; }
    QString path() const { return m_path; }

    Q_INVOKABLE QVariant setting(const QString &key,
                                 const QVariant &defaultValue = QVariant()) const;
    Q_INVOKABLE void setSetting(const QString &key, const QVariant &value);
    Q_INVOKABLE void removeSetting(const QString &key);

private:
    QString settingsKey(const QString &key) const;

    QString m_id;
    QString m_name;
    QString m_version;
    QString m_path;
};

#endif // QMLPLUGINCONTEXT_H
