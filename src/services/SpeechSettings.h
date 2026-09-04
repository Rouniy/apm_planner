#ifndef SPEECHSETTINGS_H
#define SPEECHSETTINGS_H

#include <QObject>
#include <QString>

#include <memory>

class QSettings;

/**
 * Application-owned Mission Planner speech master setting.
 *
 * Production callers share instance(). Tests and isolated consumers may
 * inject their own QSettings object, which remains owned by its caller.
 */
class SpeechSettings final : public QObject
{
    Q_OBJECT

public:
    explicit SpeechSettings(QSettings *settings = nullptr,
                            QObject *parent = nullptr);
    ~SpeechSettings() override;

    static SpeechSettings *instance();
    static QString settingsKey();

    bool isEnabled() const { return m_enabled; }

public slots:
    void setEnabled(bool enabled);
    void reload();

signals:
    void enabledChanged(bool enabled);

private:
    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings = nullptr;
    bool m_enabled = false;
};

#endif // SPEECHSETTINGS_H
