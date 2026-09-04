#ifndef HUDDISPLAYSETTINGS_H
#define HUDDISPLAYSETTINGS_H

#include <QObject>

#include <memory>

class QSettings;

/**
 * Application-wide owner of the live Mission Planner HUD overlay setting.
 *
 * Production consumers share instance(). Tests can inject an INI-backed
 * QSettings; the injected object remains owned by its caller.
 */
class HudDisplaySettings final : public QObject
{
    Q_OBJECT

public:
    explicit HudDisplaySettings(QSettings *settings = nullptr,
                                QObject *parent = nullptr);
    ~HudDisplaySettings() override;

    static HudDisplaySettings *instance();

    bool overlayEnabled() const { return m_overlayEnabled; }

public slots:
    void setOverlayEnabled(bool enabled);
    void reload();

signals:
    void overlayEnabledChanged(bool enabled);

private:
    std::unique_ptr<QSettings> m_ownedSettings;
    QSettings *m_settings = nullptr;
    bool m_overlayEnabled = true;
};

#endif // HUDDISPLAYSETTINGS_H
