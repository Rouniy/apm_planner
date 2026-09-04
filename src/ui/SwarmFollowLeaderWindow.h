#ifndef SWARMFOLLOWLEADERWINDOW_H
#define SWARMFOLLOWLEADERWINDOW_H

#include "comm/SwarmCommandService.h"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>

class QCloseEvent;
class SwarmTelemetryRegistry;

/** Replaceable boundary around the application-owned exact swarm sender. */
class SwarmFollowLeaderCommandInterface
{
public:
    using SessionCancelledHandler =
        std::function<void(quint64, const QString &)>;

    virtual ~SwarmFollowLeaderCommandInterface() = default;

    virtual SwarmCommandService::Result reserve(
        QObject *owner, const QVector<SwarmCommandMember> &members,
        int maximumBatchHz, SwarmCommandSessionToken *token,
        QString *error) = 0;
    virtual SwarmCommandService::Result release(
        const SwarmCommandSessionToken &token) = 0;
    virtual bool routeIsEligible(
        const SwarmVehicleInstanceLease &lease, QString *error) = 0;
    virtual SwarmCommandService::BatchReport requestPositionStreams(
        const SwarmCommandSessionToken &token,
        const QVector<int> &slotIds, int rateHz) = 0;
    virtual SwarmCommandService::BatchReport sendPositionTargets(
        const SwarmCommandSessionToken &token,
        const QVector<SwarmPositionTarget> &targets) = 0;
    virtual void setSessionCancelledHandler(
        SessionCancelledHandler handler) = 0;
};

/** Mission Planner 10 Tools > Swarm Follow Leader (Beta). */
class SwarmFollowLeaderWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies
    {
        std::function<bool(QWidget *, const QString &, const QString &,
                           const QString &)> confirmDangerous;
    };

    static constexpr int WindowWidth = 1240;
    static constexpr int WindowHeight = 720;
    static constexpr int MinimumWindowWidth = 980;
    static constexpr int MinimumWindowHeight = 600;

    explicit SwarmFollowLeaderWindow(QWidget *owner = nullptr);
    SwarmFollowLeaderWindow(SwarmTelemetryRegistry *registry,
                            SwarmFollowLeaderCommandInterface *commands,
                            Dependencies dependencies,
                            QWidget *owner = nullptr);
    ~SwarmFollowLeaderWindow() override;

    static SwarmFollowLeaderWindow *OpenWindow(QWidget *owner = nullptr);

    QString statusText() const;
    bool isRunning() const noexcept;
    int vehicleCount() const noexcept;

public slots:
    void refreshVehicles();

signals:
    void runningChanged(bool running);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    class Implementation;
    std::unique_ptr<Implementation> m_impl;

    static QPointer<SwarmFollowLeaderWindow> s_current;
};

#endif // SWARMFOLLOWLEADERWINDOW_H
