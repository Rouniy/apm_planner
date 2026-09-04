#include "Terrain3DWindow.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace
{
constexpr unsigned long kWorkerShutdownWaitMs = 250;

bool sameSnapshot(const Terrain3DCore::Snapshot &left,
                  const Terrain3DCore::Snapshot &right)
{
    return left.vehicle.latitude == right.vehicle.latitude
        && left.vehicle.longitude == right.vehicle.longitude
        && left.vehicle.altitudeM == right.vehicle.altitudeM
        && left.relativeAltitudeM == right.relativeAltitudeM
        && left.rollDeg == right.rollDeg
        && left.pitchDeg == right.pitchDeg
        && left.yawDeg == right.yawDeg
        && left.velocityNorthMps == right.velocityNorthMps
        && left.velocityEastMps == right.velocityEastMps
        && left.velocityVerticalMps == right.velocityVerticalMps
        && left.linkId == right.linkId
        && left.targetGeneration == right.targetGeneration
        && left.capturedMonotonicMs == right.capturedMonotonicMs
        && left.mode == right.mode
        && left.armed == right.armed
        && left.systemId == right.systemId
        && left.componentId == right.componentId;
}

bool validTargetEpoch(const Terrain3DCore::Snapshot &snapshot)
{
    return snapshot.linkId >= 0 && snapshot.systemId > 0;
}

bool sameTargetEpoch(const Terrain3DCore::Snapshot &left,
                     const Terrain3DCore::Snapshot &right)
{
    return left.linkId == right.linkId
        && left.systemId == right.systemId
        && left.componentId == right.componentId
        && left.targetGeneration == right.targetGeneration;
}

bool focusIsEditor(QWidget *focus)
{
    for (QWidget *widget = focus; widget; widget = widget->parentWidget()) {
        if (qobject_cast<QAbstractSpinBox *>(widget)) {
            return true;
        }
    }
    return false;
}
}

struct Terrain3DWindow::WorkerResult
{
    bool rebuild = false;
    bool cancelled = false;
    QString error;
    Terrain3DCore::Snapshot snapshot;
    Terrain3DCore::Settings settings;
    Terrain3DCore::Mesh mesh;
    Terrain3DCore::Camera camera;
    Terrain3DCore::RenderResult rendered;
};

Terrain3DWindow::Terrain3DWindow(Dependencies dependencies, QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_dependencies(std::move(dependencies))
{
    m_fallbackClock.start();
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(100);
    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(75);
    buildUi(owner);

    connect(m_refreshTimer, &QTimer::timeout,
            this, &Terrain3DWindow::refreshSnapshot);
    connect(m_resizeTimer, &QTimer::timeout,
            this, &Terrain3DWindow::requestRender);
    m_refreshTimer->start();
    QTimer::singleShot(0, this, &Terrain3DWindow::refreshSnapshot);
}

Terrain3DWindow::~Terrain3DWindow()
{
    stopWorker();
}

Terrain3DWindow *Terrain3DWindow::OpenWindow(
    Dependencies dependencies, QWidget *owner)
{
    auto *window = new Terrain3DWindow(std::move(dependencies), owner);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

void Terrain3DWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("Terrain3DWindow"));
    setWindowTitle(tr("3D Terrain View"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setFocusPolicy(Qt::StrongFocus);
    resize(WindowWidth, WindowHeight);
    setMinimumSize(MinimumWindowWidth, MinimumWindowHeight);
    setStyleSheet(QStringLiteral(
        "Terrain3DWindow { background: #151817; color: #e8ecea; }"
        "QWidget#TerrainToolbar, QWidget#TerrainFooter { background: #292d2b; }"
        "QLabel#TerrainPointerStatus { background: #222624; padding: 5px 10px; }"
        "QLabel#TerrainLimitations { color: #e2b86b; }"));
    if (owner) {
        move(owner->frameGeometry().center() - rect().center());
    }

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *toolbarWidget = new QWidget(this);
    toolbarWidget->setObjectName(QStringLiteral("TerrainToolbar"));
    auto *toolbar = new QHBoxLayout(toolbarWidget);
    toolbar->setContentsMargins(10, 7, 10, 7);
    toolbar->setSpacing(8);

    m_lockToVehicle = new QCheckBox(tr("Lock to MAV"), toolbarWidget);
    m_lockToVehicle->setObjectName(QStringLiteral("LockToVehicle"));
    m_lockToVehicle->setChecked(true);
    toolbar->addWidget(m_lockToVehicle);

    m_fogEnabled = new QCheckBox(tr("Fog"), toolbarWidget);
    m_fogEnabled->setObjectName(QStringLiteral("FogEnabled"));
    m_fogEnabled->setChecked(true);
    toolbar->addWidget(m_fogEnabled);

    m_imageryEnabled = new QCheckBox(tr("Imagery"), toolbarWidget);
    m_imageryEnabled->setObjectName(QStringLiteral("ImageryEnabled"));
    m_imageryEnabled->setChecked(false);
    m_imageryEnabled->setEnabled(false);
    m_imageryEnabled->setToolTip(tr(
        "Map imagery draping is not available yet; elevation shading is active."));
    toolbar->addWidget(m_imageryEnabled);

    toolbar->addWidget(new QLabel(tr("Range"), toolbarWidget));
    m_rangeM = new QSpinBox(toolbarWidget);
    m_rangeM->setObjectName(QStringLiteral("RangeM"));
    m_rangeM->setRange(250, 5000);
    m_rangeM->setSingleStep(250);
    m_rangeM->setSuffix(tr(" m"));
    m_rangeM->setValue(1500);
    m_rangeM->setFixedWidth(105);
    toolbar->addWidget(m_rangeM);

    toolbar->addWidget(new QLabel(tr("Grid"), toolbarWidget));
    m_gridSize = new QSpinBox(toolbarWidget);
    m_gridSize->setObjectName(QStringLiteral("GridSize"));
    m_gridSize->setRange(17, 65);
    m_gridSize->setSingleStep(8);
    m_gridSize->setValue(33);
    m_gridSize->setFixedWidth(65);
    toolbar->addWidget(m_gridSize);

    toolbar->addWidget(new QLabel(tr("Imagery min/max"), toolbarWidget));
    m_textureMinZoom = new QSpinBox(toolbarWidget);
    m_textureMinZoom->setObjectName(QStringLiteral("TextureMinZoom"));
    m_textureMinZoom->setRange(1, 20);
    m_textureMinZoom->setValue(12);
    m_textureMinZoom->setFixedWidth(55);
    m_textureMinZoom->setEnabled(false);
    m_textureMinZoom->setToolTip(m_imageryEnabled->toolTip());
    toolbar->addWidget(m_textureMinZoom);
    m_textureMaxZoom = new QSpinBox(toolbarWidget);
    m_textureMaxZoom->setObjectName(QStringLiteral("TextureMaxZoom"));
    m_textureMaxZoom->setRange(1, 20);
    m_textureMaxZoom->setValue(20);
    m_textureMaxZoom->setFixedWidth(55);
    m_textureMaxZoom->setEnabled(false);
    m_textureMaxZoom->setToolTip(m_imageryEnabled->toolTip());
    toolbar->addWidget(m_textureMaxZoom);

    toolbar->addWidget(new QLabel(tr("Vertical ×"), toolbarWidget));
    m_verticalExaggeration = new QDoubleSpinBox(toolbarWidget);
    m_verticalExaggeration->setObjectName(
        QStringLiteral("VerticalExaggeration"));
    m_verticalExaggeration->setRange(0.25, 8.0);
    m_verticalExaggeration->setSingleStep(0.25);
    m_verticalExaggeration->setDecimals(2);
    m_verticalExaggeration->setValue(1.0);
    m_verticalExaggeration->setFixedWidth(70);
    toolbar->addWidget(m_verticalExaggeration);

    m_reloadButton = new QPushButton(tr("Reload terrain"), toolbarWidget);
    m_reloadButton->setObjectName(QStringLiteral("ReloadTerrain"));
    toolbar->addWidget(m_reloadButton);
    toolbar->addStretch(1);
    root->addWidget(toolbarWidget);

    m_image = new QLabel(this);
    m_image->setObjectName(QStringLiteral("TerrainImage"));
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setScaledContents(true);
    m_image->setMouseTracking(true);
    m_image->setFocusPolicy(Qt::NoFocus);
    m_image->setMinimumSize(320, 240);
    m_image->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_image->setStyleSheet(QStringLiteral("background: black;"));
    m_image->installEventFilter(this);
    root->addWidget(m_image, 1);

    m_pointerStatus = new QLabel(
        tr("Move over the terrain to inspect a point."), this);
    m_pointerStatus->setObjectName(QStringLiteral("TerrainPointerStatus"));
    m_pointerStatus->setTextFormat(Qt::PlainText);
    m_pointerStatus->setTextInteractionFlags(Qt::NoTextInteraction);
    root->addWidget(m_pointerStatus);

    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("TerrainFooter"));
    auto *footerLayout = new QGridLayout(footer);
    footerLayout->setContentsMargins(10, 6, 10, 6);
    footerLayout->setHorizontalSpacing(16);
    footerLayout->setVerticalSpacing(2);
    m_status = new QLabel(
        tr("Waiting for a valid vehicle GPS position."), footer);
    m_status->setObjectName(QStringLiteral("TerrainStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    footerLayout->addWidget(m_status, 0, 0);
    auto *help = new QLabel(
        tr("Free camera: W/S/A/D · Q/E yaw · R/F altitude · arrows pitch/yaw"),
        footer);
    help->setObjectName(QStringLiteral("TerrainCameraHelp"));
    help->setTextFormat(Qt::PlainText);
    footerLayout->addWidget(help, 0, 1, Qt::AlignRight);
    m_details = new QLabel(
        tr("SRTM terrain has not been loaded."), footer);
    m_details->setObjectName(QStringLiteral("TerrainDetails"));
    m_details->setTextFormat(Qt::PlainText);
    m_details->setWordWrap(true);
    footerLayout->addWidget(m_details, 1, 0, 1, 2);
    m_limitations = new QLabel(
        tr("Elevation shading only. Terrain clicks inspect coordinates; "
           "guided commands are disabled."), footer);
    m_limitations->setObjectName(QStringLiteral("TerrainLimitations"));
    m_limitations->setTextFormat(Qt::PlainText);
    m_limitations->setWordWrap(true);
    footerLayout->addWidget(m_limitations, 2, 0, 1, 2);
    footerLayout->setColumnStretch(0, 1);
    root->addWidget(footer);

    connect(m_reloadButton, &QPushButton::clicked,
            this, &Terrain3DWindow::ReloadTerrain);
    connect(m_rangeM, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this]() { markReloadRequired(); });
    connect(m_gridSize, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this]() { markReloadRequired(); });
    connect(m_fogEnabled, &QCheckBox::toggled,
            this, [this]() { requestRender(); });
    connect(m_verticalExaggeration,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this]() { requestRender(); });
    connect(m_lockToVehicle, &QCheckBox::toggled,
            this, [this](bool locked) {
        if (!locked && m_mesh.isValid()) {
            m_freeCamera = m_lastCamera;
            m_haveFreeCamera = true;
        }
        requestRender();
    });
    setBusy(false);
}

QString Terrain3DWindow::statusText() const
{
    return m_status ? m_status->text() : QString();
}

QString Terrain3DWindow::detailsText() const
{
    return m_details ? m_details->text() : QString();
}

QString Terrain3DWindow::pointerText() const
{
    return m_pointerStatus ? m_pointerStatus->text() : QString();
}

qint64 Terrain3DWindow::nowMs() const
{
    try {
        return m_dependencies.monotonicClock
            ? m_dependencies.monotonicClock() : m_fallbackClock.elapsed();
    } catch (...) {
        return m_fallbackClock.elapsed();
    }
}

Terrain3DCore::Settings Terrain3DWindow::currentSettings() const
{
    Terrain3DCore::Settings settings;
    settings.rangeM = m_rangeM->value();
    settings.gridSize = m_gridSize->value();
    settings.textureMinZoom = m_textureMinZoom->value();
    settings.textureMaxZoom = m_textureMaxZoom->value();
    settings.verticalExaggeration = m_verticalExaggeration->value();
    settings.fogEnabled = m_fogEnabled->isChecked();
    // This slice is deliberately truthful: texture UI remains disabled until
    // the shared-cache imagery atlas is wired by a later vertical slice.
    settings.imageryEnabled = false;
    return settings.normalized();
}

void Terrain3DWindow::refreshSnapshot()
{
    if (m_busy) {
        return;
    }
    if (!m_dependencies.snapshot) {
        m_status->setText(tr("Vehicle telemetry is unavailable."));
        return;
    }

    Terrain3DCore::Snapshot snapshot;
    try {
        snapshot = m_dependencies.snapshot();
    } catch (const std::exception &exception) {
        m_status->setText(tr("Could not read vehicle state: %1")
                              .arg(QString::fromUtf8(exception.what())));
        return;
    } catch (...) {
        m_status->setText(tr("Could not read vehicle state."));
        return;
    }

    if (!snapshot.hasPosition() || !validTargetEpoch(snapshot)) {
        m_haveSnapshot = false;
        m_mesh = {};
        m_frame = {};
        m_image->clear();
        m_haveFreeCamera = false;
        m_status->setText(tr("Waiting for a valid vehicle GPS position."));
        return;
    }

    const bool targetChanged = m_haveSnapshot
        && !sameTargetEpoch(snapshot, m_latestSnapshot);
    const bool changed = !m_haveSnapshot
        || !sameSnapshot(snapshot, m_latestSnapshot);
    m_latestSnapshot = snapshot;
    m_haveSnapshot = true;
    if (targetChanged) {
        m_mesh = {};
        m_frame = {};
        m_image->clear();
        m_haveFreeCamera = false;
        m_status->setText(tr("Vehicle target changed; refreshing terrain."));
        beginReload(snapshot);
        return;
    }
    if (!m_mesh.isValid()) {
        beginReload(snapshot);
        return;
    }

    const Terrain3DCore::LocalPoint local =
        m_mesh.center.toLocal(snapshot.vehicle);
    if (std::isfinite(local.eastM) && std::isfinite(local.northM)
        && std::max(std::abs(local.eastM), std::abs(local.northM))
            > m_mesh.rangeM * 0.6) {
        beginReload(snapshot);
        return;
    }
    if (changed) {
        beginRender(snapshot);
    }
}

void Terrain3DWindow::ReloadTerrain()
{
    if (m_busy) {
        m_pendingReload = true;
        return;
    }
    if (!m_dependencies.snapshot) {
        m_status->setText(tr("Vehicle telemetry is unavailable."));
        return;
    }
    Terrain3DCore::Snapshot snapshot;
    try {
        snapshot = m_dependencies.snapshot();
    } catch (...) {
        m_status->setText(tr("Could not read vehicle state."));
        return;
    }
    if (!snapshot.hasPosition() || !validTargetEpoch(snapshot)) {
        m_status->setText(tr("Waiting for a valid vehicle GPS position."));
        return;
    }
    m_latestSnapshot = snapshot;
    m_haveSnapshot = true;
    beginReload(snapshot);
}

void Terrain3DWindow::beginReload(
    const Terrain3DCore::Snapshot &snapshot)
{
    if (!m_dependencies.elevation) {
        m_status->setText(tr("Terrain elevation provider is unavailable."));
        return;
    }
    m_status->setText(tr("Loading %1×%1 SRTM terrain…")
                          .arg(currentSettings().gridSize));
    startWorker(true, snapshot);
}

void Terrain3DWindow::beginRender(
    const Terrain3DCore::Snapshot &snapshot)
{
    if (!m_mesh.isValid()) {
        beginReload(snapshot);
        return;
    }
    startWorker(false, snapshot);
}

void Terrain3DWindow::startWorker(
    bool rebuild, const Terrain3DCore::Snapshot &snapshot)
{
    if (m_busy) {
        m_pendingRender = true;
        return;
    }
    const Terrain3DCore::Settings settings = currentSettings();
    const Terrain3DCore::Mesh mesh = m_mesh;
    const Terrain3DCore::ElevationProvider elevation =
        m_dependencies.elevation;
    const qint64 clockNow = nowMs();
    const bool locked = m_lockToVehicle->isChecked();
    const Terrain3DCore::Camera freeCamera = m_haveFreeCamera
        ? m_freeCamera : m_lastCamera;
    const QSize viewport = m_image->size().expandedTo(QSize(320, 240));
    const int width = std::min(viewport.width(),
                               Terrain3DCore::MaximumRenderWidth);
    const int height = std::min(viewport.height(),
                                Terrain3DCore::MaximumRenderHeight);
    m_cancelFlag = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancel = m_cancelFlag;
    const auto result = std::make_shared<WorkerResult>();
    result->rebuild = rebuild;
    result->snapshot = snapshot;
    result->settings = settings;

    setBusy(true);
    QThread *thread = QThread::create(
        [result, rebuild, snapshot, settings, mesh, elevation,
         clockNow, locked, freeCamera, width, height, cancel]() {
            try {
                Terrain3DCore::Mesh workingMesh = mesh;
                if (rebuild) {
                    Terrain3DCore::MeshBuildResult built =
                        Terrain3DCore::buildMesh(
                            snapshot, settings, elevation,
                            [cancel]() { return cancel->load(); });
                    if (built.error == Terrain3DCore::ErrorCode::Cancelled
                        || cancel->load()) {
                        result->cancelled = true;
                        return;
                    }
                    if (!built.ok()) {
                        result->error = built.message;
                        return;
                    }
                    workingMesh = std::move(built.mesh);
                }
                if (cancel->load()) {
                    result->cancelled = true;
                    return;
                }
                result->mesh = workingMesh;
                result->camera = rebuild || locked
                    ? Terrain3DCore::Camera::locked(
                          workingMesh, snapshot, clockNow)
                    : freeCamera;
                result->rendered = Terrain3DCore::render(
                    workingMesh, snapshot, settings, result->camera,
                    width, height);
                if (cancel->load()) {
                    result->cancelled = true;
                    return;
                }
                if (!result->rendered.ok()) {
                    result->error = result->rendered.message;
                }
            } catch (const std::exception &exception) {
                result->error = QString::fromUtf8(exception.what());
            } catch (...) {
                result->error = QStringLiteral(
                    "Unknown terrain worker exception.");
            }
        });
    m_thread = thread;
    connect(thread, &QThread::finished, this,
            [this, result, thread]() { finishWorker(*result, thread); });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void Terrain3DWindow::finishWorker(
    const WorkerResult &result, QThread *thread)
{
    if (m_thread == thread) {
        m_thread = nullptr;
    }
    setBusy(false);
    m_cancelFlag.reset();

    // Recheck only the physical-target epoch before publishing. Telemetry may
    // legitimately advance while rendering, but a target/link change must
    // never inherit the previous vehicle's terrain frame.
    Terrain3DCore::Snapshot current;
    bool currentValid = false;
    try {
        if (m_dependencies.snapshot) {
            current = m_dependencies.snapshot();
            currentValid = current.hasPosition() && validTargetEpoch(current);
        }
    } catch (...) {
        currentValid = false;
    }
    if (!currentValid || !sameTargetEpoch(result.snapshot, current)) {
        m_mesh = {};
        m_frame = {};
        m_image->clear();
        m_haveFreeCamera = false;
        m_pendingReload = false;
        m_pendingRender = false;
        m_haveSnapshot = currentValid;
        if (currentValid) {
            m_latestSnapshot = current;
            m_status->setText(
                tr("Vehicle target changed; refreshing terrain."));
        } else {
            m_status->setText(
                tr("Waiting for a valid vehicle GPS position."));
        }
        QTimer::singleShot(0, this, &Terrain3DWindow::refreshSnapshot);
        return;
    }
    if (result.cancelled) {
        m_status->setText(tr("Terrain operation cancelled."));
        continuePendingWork();
        return;
    }
    if (!result.error.isEmpty()) {
        m_status->setText(tr("Terrain load failed: %1").arg(result.error));
        continuePendingWork();
        return;
    }
    if (!result.mesh.isValid() || result.rendered.image.isNull()) {
        m_status->setText(tr("3D render failed: the terrain frame is empty."));
        continuePendingWork();
        return;
    }

    m_mesh = result.mesh;
    m_latestSnapshot = result.snapshot;
    m_haveSnapshot = true;
    m_lastCamera = result.camera;
    if (result.rebuild) {
        m_reloadRequired = false;
        m_haveFreeCamera = false;
        if (!m_lockToVehicle->isChecked()) {
            m_freeCamera = result.camera;
            m_haveFreeCamera = true;
        }
    } else if (!m_lockToVehicle->isChecked()) {
        m_freeCamera = result.camera;
        m_haveFreeCamera = true;
    }
    presentFrame(result.rendered, result.snapshot, result.camera);
    continuePendingWork();
}

void Terrain3DWindow::presentFrame(
    const Terrain3DCore::RenderResult &rendered,
    const Terrain3DCore::Snapshot &snapshot,
    const Terrain3DCore::Camera &camera)
{
    Q_UNUSED(snapshot)
    Q_UNUSED(camera)
    m_frame = rendered.image;
    m_image->setPixmap(QPixmap::fromImage(m_frame));
    m_details->setText(
        rendered.details
        + (m_lockToVehicle->isChecked()
               ? tr(" · camera locked to MAV")
               : tr(" · free camera W/S/A/D Q/E R/F; arrows change pitch/yaw")));
    m_status->setText(m_reloadRequired
        ? tr("Terrain settings changed; select Reload terrain to apply them.")
        : tr("Terrain ready. Move over the view to inspect coordinates."));
}

void Terrain3DWindow::markReloadRequired()
{
    if (!m_mesh.isValid()) {
        return;
    }
    m_reloadRequired = true;
    m_status->setText(
        tr("Terrain settings changed; select Reload terrain to apply them."));
}

void Terrain3DWindow::requestRender()
{
    if (!m_mesh.isValid() || !m_haveSnapshot) {
        return;
    }
    if (m_busy) {
        m_pendingRender = true;
        return;
    }
    beginRender(m_latestSnapshot);
}

void Terrain3DWindow::inspectPoint(const QPointF &position, bool clicked)
{
    if (!m_mesh.isValid() || m_frame.isNull()
        || m_image->width() <= 0 || m_image->height() <= 0) {
        m_pointerStatus->setText(tr("No terrain intersection."));
        if (clicked) {
            m_status->setText(tr(
                "Guided target not sent: terrain inspection is read-only."));
        }
        return;
    }
    const double x = position.x() * m_frame.width() / m_image->width();
    const double y = position.y() * m_frame.height() / m_image->height();
    Terrain3DCore::Vector3 ray;
    Terrain3DCore::GeoPoint point;
    if (!Terrain3DCore::screenRay(m_lastCamera, x, y,
                                  m_frame.width(), m_frame.height(), &ray)
        || !Terrain3DCore::intersectTerrain(
            m_mesh, m_lastCamera, ray, &point)) {
        m_pointerStatus->setText(tr("No terrain intersection."));
    } else {
        m_pointerStatus->setText(
            tr("%1, %2 · terrain %3 m AMSL")
                .arg(point.latitude, 0, 'f', 6)
                .arg(point.longitude, 0, 'f', 6)
                .arg(point.altitudeM, 0, 'f', 1));
    }
    if (clicked) {
        m_status->setText(tr(
            "Read-only terrain inspection: guided target commands are disabled "
            "until exact-target acknowledgement support is available."));
    }
}

bool Terrain3DWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_image) {
        if (event->type() == QEvent::MouseMove) {
            auto *mouse = static_cast<QMouseEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            inspectPoint(mouse->position(), false);
#else
            inspectPoint(mouse->localPos(), false);
#endif
        } else if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                inspectPoint(mouse->position(), true);
#else
                inspectPoint(mouse->localPos(), true);
#endif
                setFocus(Qt::MouseFocusReason);
                return true;
            }
        } else if (event->type() == QEvent::Resize && m_resizeTimer
                   && m_mesh.isValid()) {
            m_resizeTimer->start();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void Terrain3DWindow::keyPressEvent(QKeyEvent *event)
{
    if (m_busy || !m_mesh.isValid() || focusIsEditor(focusWidget())) {
        QWidget::keyPressEvent(event);
        return;
    }
    Terrain3DCore::CameraMotion motion;
    double amount = 10.0;
    switch (event->key()) {
    case Qt::Key_W: motion = Terrain3DCore::CameraMotion::Forward; break;
    case Qt::Key_S: motion = Terrain3DCore::CameraMotion::Backward; break;
    case Qt::Key_A: motion = Terrain3DCore::CameraMotion::Left; break;
    case Qt::Key_D: motion = Terrain3DCore::CameraMotion::Right; break;
    case Qt::Key_Q:
    case Qt::Key_Left:
        motion = Terrain3DCore::CameraMotion::YawLeft;
        amount = 3.0;
        break;
    case Qt::Key_E:
    case Qt::Key_Right:
        motion = Terrain3DCore::CameraMotion::YawRight;
        amount = 3.0;
        break;
    case Qt::Key_R:
        motion = Terrain3DCore::CameraMotion::Up;
        amount = 5.0;
        break;
    case Qt::Key_F:
        motion = Terrain3DCore::CameraMotion::Down;
        amount = 5.0;
        break;
    case Qt::Key_Up:
        motion = Terrain3DCore::CameraMotion::PitchUp;
        amount = 2.0;
        break;
    case Qt::Key_Down:
        motion = Terrain3DCore::CameraMotion::PitchDown;
        amount = 2.0;
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }

    Terrain3DCore::Camera camera = m_haveFreeCamera
        ? m_freeCamera : m_lastCamera;
    {
        const QSignalBlocker blocker(m_lockToVehicle);
        m_lockToVehicle->setChecked(false);
    }
    m_freeCamera = camera.moved(motion, amount);
    m_lastCamera = m_freeCamera;
    m_haveFreeCamera = true;
    requestRender();
    event->accept();
}

void Terrain3DWindow::setBusy(bool busy)
{
    m_busy = busy;
    m_lockToVehicle->setEnabled(!busy);
    m_fogEnabled->setEnabled(!busy);
    m_rangeM->setEnabled(!busy);
    m_gridSize->setEnabled(!busy);
    m_verticalExaggeration->setEnabled(!busy);
    m_reloadButton->setEnabled(!busy);
    // The imagery controls remain disabled independently of worker state.
    m_imageryEnabled->setEnabled(false);
    m_textureMinZoom->setEnabled(false);
    m_textureMaxZoom->setEnabled(false);
}

void Terrain3DWindow::continuePendingWork()
{
    if (m_pendingReload) {
        m_pendingReload = false;
        m_pendingRender = false;
        QTimer::singleShot(0, this, &Terrain3DWindow::ReloadTerrain);
    } else if (m_pendingRender) {
        m_pendingRender = false;
        QTimer::singleShot(0, this, &Terrain3DWindow::requestRender);
    }
}

void Terrain3DWindow::stopWorker()
{
    if (m_refreshTimer) {
        m_refreshTimer->stop();
    }
    if (m_resizeTimer) {
        m_resizeTimer->stop();
    }
    if (m_cancelFlag) {
        m_cancelFlag->store(true);
    }
    QThread *thread = m_thread.data();
    m_thread = nullptr;
    if (!thread) {
        return;
    }
    disconnect(thread, nullptr, this, nullptr);
    thread->requestInterruption();
    if (thread->isRunning() && !thread->wait(kWorkerShutdownWaitMs)) {
        // The worker captures values and dependency callbacks only. Its
        // finished->deleteLater connection can safely outlive this window.
        return;
    }
    disconnect(thread, &QThread::finished, thread, &QObject::deleteLater);
    delete thread;
}

void Terrain3DWindow::closeEvent(QCloseEvent *event)
{
    stopWorker();
    QWidget::closeEvent(event);
}
