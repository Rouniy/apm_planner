#ifndef GUIDEDNAVIGATIONCONTROLLER_H
#define GUIDEDNAVIGATIONCONTROLLER_H

#include "services/GuidedAltitudeStore.h"
#include "services/GuidedNavigationService.h"
#include "terrain/Terrain3DCore.h"

#include <QObject>
#include <QPointer>
#include <functional>

class AbstractMapWidget;
class QDialog;
class QProgressDialog;
class QSettings;
class QWidget;

/** UI-owned dialogs for the application-owned single-vehicle guided lane. */
class GuidedNavigationController final : public QObject
{
    Q_OBJECT
public:
    using Context = GuidedAltitudeStore::Context;
    struct Dependencies {
        GuidedAltitudeStore *altitudes = nullptr;
        GuidedNavigationService *navigation = nullptr;
        SwarmTelemetryRegistry *telemetry = nullptr;
        // Optional for tests; production constructs ordinary application settings.
        QSettings *settings = nullptr;
        std::function<bool(const Context &)> isGuided;
    };
    explicit GuidedNavigationController(Dependencies dependencies, QWidget *owner);
    ~GuidedNavigationController() override;
    void attachMap(AbstractMapWidget *map);
    void terrainClick(const Terrain3DCore::GeoPoint &point,
                      const Terrain3DCore::Snapshot &rendered);
    bool busy() const;
    QString statusText() const { return m_status; }

public slots:
    void editAltitude();
    void flyToHere(double latitude, double longitude);
    void flyToCoordinates();
    void cancel();

signals:
    void statusChanged(const QString &text);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    using AltitudeContinuation = std::function<void(const Context &)>;
    bool begin(Context *context);
    void openAltitude(const Context &context, AltitudeContinuation continuation = {});
    void confirm(const Context &context, double latitude, double longitude,
                 double altitudeM, MAV_FRAME frame, bool changeMode,
                 GuidedNavigationService::Purpose purpose);
    void finish(const QString &text);
    void setStatus(const QString &text);
    void dismissPrompt();
    void dismissProgress();
    double multiplier() const;
    MAV_FRAME savedFrame() const;
    QString describe(const Context &context) const;

    QPointer<QWidget> m_owner;
    QPointer<GuidedAltitudeStore> m_altitudes;
    QPointer<GuidedNavigationService> m_navigation;
    QPointer<SwarmTelemetryRegistry> m_telemetry;
    QPointer<QSettings> m_settings;
    QPointer<QDialog> m_prompt;
    QPointer<QProgressDialog> m_progress;
    std::function<bool(const Context &)> m_isGuided;
    GuidedNavigationService::Plan m_plan;
    quint64 m_flow = 0;
    quint64 m_operation = 0;
    bool m_active = false;
    bool m_destroying = false;
    QString m_status;
};

#endif
