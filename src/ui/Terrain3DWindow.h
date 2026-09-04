#ifndef TERRAIN3DWINDOW_H
#define TERRAIN3DWINDOW_H

#include "terrain/Terrain3DCore.h"

#include <QElapsedTimer>
#include <QImage>
#include <QPointer>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>

class QCheckBox;
class QCloseEvent;
class QDoubleSpinBox;
class QEvent;
class QKeyEvent;
class QLabel;
class QPointF;
class QPushButton;
class QSpinBox;
class QThread;
class QTimer;

/** Mission Planner 10 TOOLS > 3D Terrain View modeless window. */
class Terrain3DWindow final : public QWidget
{
    Q_OBJECT

public:
    using SnapshotProvider =
        std::function<Terrain3DCore::Snapshot()>;
    using MonotonicClock = std::function<qint64()>;

    struct Dependencies
    {
        /** Must return a value-only snapshot; no live UAS object reaches workers. */
        SnapshotProvider snapshot;
        /** Uses the same millisecond epoch as Snapshot::capturedMonotonicMs. */
        MonotonicClock monotonicClock;
        /** Called on the terrain worker thread and therefore must be thread-safe. */
        Terrain3DCore::ElevationProvider elevation;
    };

    static constexpr int WindowWidth = 1100;
    static constexpr int WindowHeight = 760;
    static constexpr int MinimumWindowWidth = 720;
    static constexpr int MinimumWindowHeight = 520;

    explicit Terrain3DWindow(Dependencies dependencies,
                             QWidget *owner = nullptr);
    ~Terrain3DWindow() override;

    /** Creates a fresh independent modeless top-level window. */
    static Terrain3DWindow *OpenWindow(Dependencies dependencies,
                                       QWidget *owner = nullptr);
    /** Production integration backed by the current exact MAVLink target. */
    static Terrain3DWindow *OpenWindow(QWidget *owner = nullptr);

    QString statusText() const;
    QString detailsText() const;
    QString pointerText() const;
    bool isBusy() const { return m_busy; }
    const QImage &frame() const { return m_frame; }
    Terrain3DCore::Camera currentCamera() const { return m_lastCamera; }

public slots:
    void ReloadTerrain();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    struct WorkerResult;

    void buildUi(QWidget *owner);
    void refreshSnapshot();
    void beginReload(const Terrain3DCore::Snapshot &snapshot);
    void beginRender(const Terrain3DCore::Snapshot &snapshot);
    void startWorker(bool rebuild,
                     const Terrain3DCore::Snapshot &snapshot);
    void finishWorker(const WorkerResult &result, QThread *thread);
    void presentFrame(const Terrain3DCore::RenderResult &rendered,
                      const Terrain3DCore::Snapshot &snapshot,
                      const Terrain3DCore::Camera &camera);
    void markReloadRequired();
    void requestRender();
    void inspectPoint(const QPointF &position, bool clicked);
    void setBusy(bool busy);
    void continuePendingWork();
    void stopWorker();
    qint64 nowMs() const;
    Terrain3DCore::Settings currentSettings() const;

    Dependencies m_dependencies;
    QElapsedTimer m_fallbackClock;
    QTimer *m_refreshTimer = nullptr;
    QTimer *m_resizeTimer = nullptr;
    bool m_busy = false;
    bool m_reloadRequired = false;
    bool m_pendingReload = false;
    bool m_pendingRender = false;
    bool m_haveSnapshot = false;
    bool m_haveFreeCamera = false;
    Terrain3DCore::Snapshot m_latestSnapshot;
    Terrain3DCore::Mesh m_mesh;
    Terrain3DCore::Camera m_lastCamera;
    Terrain3DCore::Camera m_freeCamera;
    QImage m_frame;
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
    QPointer<QThread> m_thread;

    QCheckBox *m_lockToVehicle = nullptr;
    QCheckBox *m_fogEnabled = nullptr;
    QCheckBox *m_imageryEnabled = nullptr;
    QSpinBox *m_rangeM = nullptr;
    QSpinBox *m_gridSize = nullptr;
    QSpinBox *m_textureMinZoom = nullptr;
    QSpinBox *m_textureMaxZoom = nullptr;
    QDoubleSpinBox *m_verticalExaggeration = nullptr;
    QPushButton *m_reloadButton = nullptr;
    QLabel *m_image = nullptr;
    QLabel *m_pointerStatus = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_details = nullptr;
    QLabel *m_limitations = nullptr;
};

#endif // TERRAIN3DWINDOW_H
