#ifndef MAVLINKLOGWINDOW_H
#define MAVLINKLOGWINDOW_H

#include "comm/TlogExportService.h"

#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>

class QLabel;
class QPushButton;
class QThread;

/** Mission Planner 10 TOOLS > Tlog Convert / Extract window. */
class MavlinkLogWindow final : public QWidget
{
    Q_OBJECT

public:
    struct Dependencies
    {
        std::function<bool(const QString &label)> confirmExport;
        std::function<QString(const QString &suggested,
                              const QString &label,
                              const QString &extension)> chooseOutput;
        std::function<TlogExportResult(
            TlogExportFormat format, const QString &input,
            const QString &selectedOutput,
            const TlogExportService::CancelRequested &cancel)> exportLog;
    };

    static constexpr int WindowWidth = 460;
    static constexpr int WindowHeight = 340;

    explicit MavlinkLogWindow(QWidget *owner = nullptr);
    MavlinkLogWindow(Dependencies dependencies, QWidget *owner = nullptr);
    ~MavlinkLogWindow() override;

    /** Creates a new independent modeless top-level window on every call. */
    static MavlinkLogWindow *OpenWindow(QWidget *owner = nullptr);

    /** Selects a local .tlog. Empty or missing paths restore the initial state. */
    void setTlogPath(const QString &path);
    QString tlogPath() const { return m_tlogPath; }
    QString statusText() const;
    bool isBusy() const { return m_busy; }

private:
    enum class Operation
    {
        Kml,
        Gpx,
        Csv,
        Text,
        Parameters,
        Missions
    };

    void buildUi(QWidget *owner);
    void pickTlog();
    void beginExport(Operation operation);
    void finishExport(const TlogExportResult &result, const QString &label);
    void setBusy(bool busy);
    void stopWorker();
    bool confirmSensitiveExport(const QString &label);
    QString chooseOutput(Operation operation);
    QString operationLabel(Operation operation) const;
    QString operationExtension(Operation operation) const;

    QString m_tlogPath;
    Dependencies m_dependencies;
    bool m_busy = false;
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
    QPointer<QThread> m_thread;
    QLabel *m_tlogName = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_pick = nullptr;
    QList<QPushButton *> m_exportButtons;
};

#endif // MAVLINKLOGWINDOW_H
