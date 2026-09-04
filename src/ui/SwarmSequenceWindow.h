#ifndef SWARMSEQUENCEWINDOW_H
#define SWARMSEQUENCEWINDOW_H

#include "comm/SwarmTelemetryRegistry.h"
#include "services/SwarmSequenceExecutor.h"
#include "tools/SwarmSequenceCore.h"

#include <QMap>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>

class QComboBox;
class QCloseEvent;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTableWidget;
class SequenceLayoutControl;

/** Fakeable GUI-thread boundary around the application-owned executor. */
class SwarmSequenceWindowInterface : public QObject
{
public:
    using ChangedHandler = std::function<void()>;

    explicit SwarmSequenceWindowInterface(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~SwarmSequenceWindowInterface() override = default;

    virtual bool executorReady(QString *error) const = 0;
    virtual bool prepareRunStep(
        const SwarmSequenceRunStepRequest &request,
        SwarmSequencePreparedRunStep *prepared, QString *error) const = 0;
    virtual bool runStep(const SwarmSequencePreparedRunStep &prepared,
                         QString *error) = 0;
    virtual bool prepareTakeoff(
        const QVector<SwarmSequenceTakeoffAssignment> &assignments,
        SwarmSequencePreparedTakeoff *prepared, QString *error) const = 0;
    virtual bool startTakeoff(const SwarmSequencePreparedTakeoff &prepared,
                              QString *error) = 0;
    virtual void cancelActiveOperation(const QString &reason) = 0;
    virtual bool isActive() const noexcept = 0;
    virtual SwarmSequenceExecutor::State state() const noexcept = 0;
    virtual QString statusText() const = 0;
    virtual quint64 operationGeneration() const noexcept = 0;
    virtual SwarmSequenceOperationReport lastReport() const = 0;
    virtual void setChangedHandler(ChangedHandler handler) = 0;
};

/** Mission Planner 10 Tools > Swarm Sequence Layout Editor (Beta). */
class SwarmSequenceWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies
    {
        std::function<QString(QWidget *)> chooseLoadPath;
        std::function<QString(QWidget *, const QString &)> chooseSavePath;
        std::function<QString(QWidget *)> chooseBackgroundPath;
        std::function<QString(QWidget *, const QString &)> requestLayoutName;
        std::function<bool(QWidget *, const QString &, const QString &,
                           const QString &)> confirmDangerous;
    };

    static constexpr int WindowWidth = 1380;
    static constexpr int WindowHeight = 820;
    static constexpr int MinimumWindowWidth = 1080;
    static constexpr int MinimumWindowHeight = 680;
    enum ItemDataRole {
        LinkSessionEpochRole = Qt::UserRole + 1,
        InstanceEpochRole = Qt::UserRole + 2
    };

    explicit SwarmSequenceWindow(QWidget *owner = nullptr);
    SwarmSequenceWindow(SwarmTelemetryRegistry *registry,
                        Dependencies dependencies,
                        QWidget *owner = nullptr);
    SwarmSequenceWindow(SwarmTelemetryRegistry *registry,
                        SwarmSequenceWindowInterface *interface,
                        Dependencies dependencies,
                        QWidget *owner = nullptr);
    ~SwarmSequenceWindow() override;

    /** Activates the single MP10-compatible modeless editor instance. */
    static SwarmSequenceWindow *OpenWindow(QWidget *owner = nullptr);

    const SwarmSequenceDocument &document() const noexcept;
    QString statusText() const;
    QString currentFilePath() const { return m_currentFilePath; }

    /** Deterministic seams used by tests and non-dialog integrations. */
    SwarmSequenceIssue setDocument(const SwarmSequenceDocument &document);
    SwarmSequenceIssue loadPath(const QString &path);
    SwarmSequenceIssue savePath(const QString &path);

public slots:
    void refreshVehicles();

signals:
    void documentChanged();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    static Dependencies DefaultDependencies();
    static bool isArduCopter(const SwarmTelemetrySnapshot &snapshot);
    static bool sameVehicle(const SwarmVehicleInstanceLease &left,
                            const SwarmVehicleInstanceLease &right);
    static QString vehicleLabel(const SwarmVehicleInstanceLease &vehicle);

    void buildUi(QWidget *owner);
    void connectUi();
    void refreshVehicles(bool explicitlyRequested);
    QVector<SwarmVehicleInstanceLease> discoverVehicles() const;
    void rebuildVehicleControls();
    void rebuildAssignments();
    void syncAll(bool selectFirst);
    void syncLayouts(int selectedIndex = -1);
    void syncCurrentLayout();
    void syncOffsets();
    void syncSteps(int selectedRow = -1);
    void syncStepDisplay();
    void resetOfflineState(const QString &status);
    void setStatus(const QString &status);
    QString currentLayoutId() const;
    const SwarmSequenceLayout *currentLayout() const;
    int nextLayoutNumber() const;
    SwarmVehicleInstanceLease vehicleForCombo(const QComboBox *combo) const;
    int optionIndex(const SwarmVehicleInstanceLease &vehicle) const;

    void chooseAndLoad();
    void chooseAndSave();
    void createLayout();
    void addStep();
    void resizeSlots(int count);
    void offsetCellChanged(int row, int column);
    void offsetDragged(int systemId, double x, double y);
    void stepSelected(int row);
    void moveSelectedStep(int delta);
    void removeSelectedStep();
    void assignmentChanged(int systemId, QComboBox *combo);
    void chooseBackground();
    void runCurrentStep();
    void takeoffAssigned();
    void executorChanged();
    void updateCommandActions();
    void clearOrigin(const QString &status);
    void clearTargets();
    bool assignmentsMatch(
        const QVector<SwarmSequenceExactAssignment> &assignments) const;
    bool takeoffAssignmentsMatch(
        const QVector<SwarmSequenceTakeoffAssignment> &assignments) const;

    static QPointer<SwarmSequenceWindow> s_current;

    QPointer<SwarmTelemetryRegistry> m_registry;
    QPointer<SwarmSequenceWindowInterface> m_interface;
    Dependencies m_dependencies;
    SwarmSequenceEditor m_editor;
    QVector<SwarmVehicleInstanceLease> m_vehicleOptions;
    QMap<int, SwarmVehicleInstanceLease> m_assignments;
    SwarmVehicleInstanceLease m_anchor;
    QString m_currentFilePath;
    bool m_loading = false;
    bool m_refreshingVehicles = false;
    int m_stepIndex = 0;
    quint64 m_revision = 0;
    quint64 m_ownedOperationGeneration = 0;
    SwarmSequenceOrigin m_origin;
    bool m_closing = false;

    QComboBox *m_layouts = nullptr;
    QSpinBox *m_vehicleCount = nullptr;
    SequenceLayoutControl *m_canvas = nullptr;
    QTableWidget *m_offsets = nullptr;
    QListWidget *m_steps = nullptr;
    QComboBox *m_anchorCombo = nullptr;
    QTableWidget *m_assignmentTable = nullptr;
    QLabel *m_stepDisplay = nullptr;
    QLabel *m_originDisplay = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_runStep = nullptr;
    QPushButton *m_takeoff = nullptr;
    QLabel *m_commandHint = nullptr;
};

#endif // SWARMSEQUENCEWINDOW_H
