#ifndef STATUSMESSAGESETTINGS_H
#define STATUSMESSAGESETTINGS_H

#include <QObject>
#include <QStringList>

#include <memory>

class QSettings;

/** Application-owned Mission Planner high-priority message threshold. */
class StatusMessageSettings final : public QObject
{
    Q_OBJECT

public:
    explicit StatusMessageSettings(QSettings *settings = nullptr,
                                   QObject *parent = nullptr);
    ~StatusMessageSettings() override;

    static StatusMessageSettings *instance();
    static QString settingsKey();
    static QStringList severityNames();

    int severity() const { return m_severity; }
    bool shouldPromote(const QString &text, int severity) const;

public slots:
    bool setSeverity(int severity);
    void reload();

signals:
    void severityChanged(int severity);

private:
    int readSeverity() const;

    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings = nullptr;
    int m_severity = 4;
};

#endif // STATUSMESSAGESETTINGS_H
