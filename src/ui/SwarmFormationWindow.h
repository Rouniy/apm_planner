#ifndef SWARMFORMATIONWINDOW_H
#define SWARMFORMATIONWINDOW_H

#include "comm/SwarmCommandService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "tools/SwarmFormationCore.h"

#include <QPointer>
#include <QElapsedTimer>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;
class FormationGridControl;

/** Small replaceable boundary around the application-owned swarm sender. */
class SwarmFormationCommandInterface
{
public:
    using SessionCancelledHandler =
        std::function<void(quint64, const QString &)>;

    virtual ~SwarmFormationCommandInterface() = default;

    virtual SwarmCommandService::Result reserve(
        QObject *owner, const QVector<SwarmCommandMember> &members,
        int maximumBatchHz, SwarmCommandSessionToken *token,
        QString *error) = 0;
    virtual SwarmCommandService::Result release(
        const SwarmCommandSessionToken &token) = 0;
    virtual SwarmCommandService::BatchReport requestStreams(
        const SwarmCommandSessionToken &token,
        const QVector<int> &slotIds, int rateHz) = 0;
    virtual SwarmCommandService::BatchReport sendPositionTargets(
        const SwarmCommandSessionToken &token,
        const QVector<SwarmPositionTarget> &targets) = 0;
    virtual void setSessionCancelledHandler(
        SessionCancelledHandler handler) = 0;
};

/** Mission Planner 10 Tools > Swarm Formation (Beta). */
class SwarmFormationWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies
    {
        std::function<bool(QWidget *, const QString &, const QString &,
                           const QString &)> confirmDangerous;
    };

    static constexpr int WindowWidth = 1180;
    static constexpr int WindowHeight = 720;
    static constexpr int MinimumWindowWidth = 900;
    static constexpr int MinimumWindowHeight = 600;

    explicit SwarmFormationWindow(QWidget *owner = nullptr);
    SwarmFormationWindow(SwarmTelemetryRegistry *registry,
                         SwarmFormationCommandInterface *commands,
                         Dependencies dependencies,
                         QWidget *owner = nullptr);
    ~SwarmFormationWindow() override;

    static SwarmFormationWindow *OpenWindow(QWidget *owner = nullptr);

    QString statusText() const;
    bool isRunning() const noexcept { return m_running; }
    int vehicleCount() const noexcept { return m_rows.size(); }

public slots:
    void refreshVehicles();

signals:
    void runningChanged(bool running);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct VehicleRow
    {
        SwarmVehicleInstanceLease lease;
        SwarmTelemetrySnapshot snapshot;
        SwarmFormationOffset offset;
        bool included = false;
    };

    static Dependencies DefaultDependencies();
    static bool sameVehicle(const SwarmVehicleInstanceLease &left,
                            const SwarmVehicleInstanceLease &right);
    static QString vehicleKey(const SwarmVehicleInstanceLease &vehicle);
    static QString vehicleLabel(const SwarmVehicleInstanceLease &vehicle);
    static QString familyLabel(const SwarmTelemetrySnapshot &snapshot);

    void initialize(QWidget *owner);
    void buildUi(QWidget *owner);
    void connectUi();
    void refreshVehicles(bool explicitlyRequested, bool stopRunning);
    QVector<SwarmTelemetrySnapshot> discoverVehicles() const;
    int rowForLease(const SwarmVehicleInstanceLease &lease) const;
    int rowForKey(const QString &key) const;
    bool isLeader(const VehicleRow &row) const;
    void rebuildVehicleWidgets();
    void rebuildLeaderCombo();
    void rebuildTable();
    void syncCanvas();
    void updateLiveStatus();
    QString liveStatus(const VehicleRow &row) const;

    void leaderChanged(int index);
    void tableCellChanged(int row, int column);
    void canvasItemDragged(const QString &key, double x, double y);
    void captureOffsets();
    void toggleRun();
    void runTick();
    void stopFormation(const QString &reason);
    void handleSessionCancelled(quint64 sessionId, const QString &reason);

    bool tryBuildPlan(SwarmFormationPlan *plan,
                      QList<SwarmTelemetrySnapshot> *snapshots,
                      QVector<SwarmCommandMember> *members,
                      QString *error) const;
    qint64 registryTimestamp() const;
    QVector<SwarmPositionTarget> positionTargets(
        const SwarmFormationTick &tick) const;
    QString confirmationTargets(const SwarmFormationPlan &plan) const;
    void setRunning(bool running);
    void setStatus(const QString &status);
    void setUnavailable(QPushButton *button, const QString &reason);

    static QPointer<SwarmFormationWindow> s_current;

    QPointer<SwarmTelemetryRegistry> m_registry;
    SwarmFormationCommandInterface *m_commands = nullptr;
    std::unique_ptr<SwarmFormationCommandInterface> m_ownedCommands;
    Dependencies m_dependencies;
    QVector<VehicleRow> m_rows;
    SwarmVehicleInstanceLease m_leader;
    SwarmFormationPlan m_activePlan;
    SwarmCommandSessionToken m_session;
    bool m_loading = false;
    bool m_refreshing = false;
    bool m_running = false;
    bool m_bootstrapping = false;
    QElapsedTimer m_bootstrapTimer;

    QComboBox *m_leaderCombo = nullptr;
    QCheckBox *m_alignYaw = nullptr;
    QCheckBox *m_aimGimbals = nullptr;
    QCheckBox *m_planeAttitude = nullptr;
    FormationGridControl *m_grid = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_runButton = nullptr;
    QDoubleSpinBox *m_takeoffAltitude = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_statusTimer = nullptr;
    QTimer *m_runTimer = nullptr;
};

#endif // SWARMFORMATIONWINDOW_H
