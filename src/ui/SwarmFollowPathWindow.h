#ifndef SWARMFOLLOWPATHWINDOW_H
#define SWARMFOLLOWPATHWINDOW_H

#include "comm/SwarmCommandService.h"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>

class QCloseEvent;
class SwarmTelemetryRegistry;

/** Replaceable boundary around the application-owned exact swarm sender. */
class SwarmFollowPathCommandInterface
{
public:
    using SessionCancelledHandler =
        std::function<void(quint64, const QString &)>;

    virtual ~SwarmFollowPathCommandInterface() = default;

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

/** Mission Planner 10 Tools > Swarm Follow Path (Beta). */
class SwarmFollowPathWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies
    {
        std::function<bool(QWidget *, const QString &, const QString &,
                           const QString &)> confirmDangerous;
    };

    static constexpr int WindowWidth = 1180;
    static constexpr int WindowHeight = 680;
    static constexpr int MinimumWindowWidth = 920;
    static constexpr int MinimumWindowHeight = 560;

    explicit SwarmFollowPathWindow(QWidget *owner = nullptr);
    SwarmFollowPathWindow(SwarmTelemetryRegistry *registry,
                          SwarmFollowPathCommandInterface *commands,
                          Dependencies dependencies,
                          QWidget *owner = nullptr);
    ~SwarmFollowPathWindow() override;

    static SwarmFollowPathWindow *OpenWindow(QWidget *owner = nullptr);

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

    static QPointer<SwarmFollowPathWindow> s_current;
};

#endif // SWARMFOLLOWPATHWINDOW_H
