#include "PropagationSettingsWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace
{
QCheckBox *checkBox(const QString &text, const QString &objectName,
                    QWidget *parent)
{
    auto *check = new QCheckBox(text, parent);
    check->setObjectName(objectName);
    check->setContentsMargins(0, 3, 0, 3);
    return check;
}

QDoubleSpinBox *numberEditor(const QString &objectName, int decimals,
                             QWidget *parent)
{
    auto *number = new QDoubleSpinBox(parent);
    number->setObjectName(objectName);
    number->setRange(0.0, decimals == 2 ? 1.0 : 2000.0);
    number->setDecimals(decimals);
    number->setSingleStep(0.1);
    return number;
}

void addRow(QGridLayout *grid, int row, const QString &caption,
            const QString &captionName, QWidget *editor, QWidget *parent)
{
    auto *label = new QLabel(caption, parent);
    label->setObjectName(captionName);
    grid->addWidget(label, row, 0);
    grid->addWidget(editor, row, 1);
}
}

PropagationSettingsWindow::PropagationSettingsWindow(QWidget *owner)
    : PropagationSettingsWindow(*PropagationSettingsStore::instance(), owner)
{}

PropagationSettingsWindow::PropagationSettingsWindow(
    PropagationSettingsStore &store, QWidget *owner)
    : QWidget(owner, Qt::Window)
    , m_store(&store)
{
    buildUi(owner);
    syncFromStore();
    wireChanges();
    connect(&store, &PropagationSettingsStore::settingsChanged,
            this, [this](const PropagationSettings &settings) {
                syncFromStore();
                emit settingsChanged(settings);
            });
    connect(&store, &QObject::destroyed, this, [this]() {
        m_store = nullptr;
        setEnabled(false);
    });
    m_loading = false;
}

PropagationSettingsWindow::~PropagationSettingsWindow() = default;

PropagationSettingsWindow *PropagationSettingsWindow::OpenWindow(
    QWidget *owner)
{
    auto *window = new PropagationSettingsWindow(owner);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

void PropagationSettingsWindow::buildUi(QWidget *owner)
{
    setObjectName(QStringLiteral("PropagationSettingsWindow"));
    setWindowTitle(tr("RF Propagation Settings"));
    setWindowFlag(Qt::Window, true);
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(WindowWidth, WindowHeight);
    setMinimumSize(MinimumWindowWidth, MinimumWindowHeight);
    setStyleSheet(QStringLiteral(
        "QWidget#PropagationSettingsWindow { background: #434445; color: #dddddd; }"
        "QLabel#propagationDescription, QLabel#propagationHelp { color: #bbbbbb; }"));
    if (owner) {
        move(owner->frameGeometry().center()
             - QPoint(WindowWidth / 2, WindowHeight / 2));
    }

    auto *windowLayout = new QVBoxLayout(this);
    windowLayout->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("propagationSettingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    windowLayout->addWidget(scroll);

    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("propagationSettingsContent"));
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(14);

    auto *description = new QLabel(tr(
        "Propagation overlays are displayed on Flight Data and Flight "
        "Planner maps."), content);
    description->setObjectName(QStringLiteral("propagationDescription"));
    description->setWordWrap(true);
    description->setTextFormat(Qt::PlainText);
    root->addWidget(description);

    m_elevation = checkBox(tr("Elevation"),
        QStringLiteral("propagationElevation"), content);
    m_terrain = checkBox(tr("Terrain"),
        QStringLiteral("propagationTerrain"), content);
    m_rf = checkBox(tr("RF Map"),
        QStringLiteral("propagationRfMap"), content);
    m_droneDistance = checkBox(tr("Drone Dist Left"),
        QStringLiteral("propagationDroneDistance"), content);
    m_homeDistance = checkBox(tr("Home Dist Left"),
        QStringLiteral("propagationHomeDistance"), content);
    m_showScale = checkBox(tr("Show Scale"),
        QStringLiteral("propagationShowScale"), content);
    m_manualAltitude = checkBox(tr("Altitude Filter"),
        QStringLiteral("propagationAltitudeFilter"), content);

    auto *modes = new QGridLayout;
    modes->setHorizontalSpacing(8);
    modes->setVerticalSpacing(0);
    modes->addWidget(m_elevation, 0, 0);
    modes->addWidget(m_terrain, 0, 1);
    modes->addWidget(m_rf, 1, 0);
    modes->addWidget(m_droneDistance, 1, 1);
    modes->addWidget(m_homeDistance, 2, 0);
    modes->addWidget(m_showScale, 2, 1);
    modes->addWidget(m_manualAltitude, 3, 0);
    modes->setColumnMinimumWidth(0, 185);
    modes->setColumnMinimumWidth(1, 185);
    modes->setColumnStretch(0, 1);
    modes->setColumnStretch(1, 1);
    root->addLayout(modes);

    auto *separator = new QFrame(content);
    separator->setObjectName(QStringLiteral("propagationSeparator"));
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    root->addWidget(separator);

    m_clearance = numberEditor(
        QStringLiteral("propagationClearance"), 1, content);
    m_resolution = new QComboBox(content);
    m_resolution->setObjectName(QStringLiteral("propagationResolution"));
    for (int value : {2, 4, 6, 8, 10}) {
        m_resolution->addItem(QString::number(value), value);
    }
    m_azimuthStep = new QComboBox(content);
    m_azimuthStep->setObjectName(QStringLiteral("propagationAzimuthStep"));
    for (double value : {0.5, 1.0, 2.0, 5.0, 10.0}) {
        m_azimuthStep->addItem(QString::number(value, 'g', 3), value);
    }
    m_convergence = new QComboBox(content);
    m_convergence->setObjectName(QStringLiteral("propagationConvergence"));
    for (double value : {0.0, 1.0, 5.0, 10.0, 15.0}) {
        m_convergence->addItem(QString::number(value, 'g', 3), value);
    }
    m_range = numberEditor(
        QStringLiteral("propagationRange"), 1, content);
    m_baseHeight = numberEditor(
        QStringLiteral("propagationBaseHeight"), 1, content);
    m_tolerance = numberEditor(
        QStringLiteral("propagationTolerance"), 2, content);
    m_minimumAltitude = numberEditor(
        QStringLiteral("propagationMinimumAltitude"), 1, content);
    m_maximumAltitude = numberEditor(
        QStringLiteral("propagationMaximumAltitude"), 1, content);

    auto *parameters = new QGridLayout;
    parameters->setHorizontalSpacing(12);
    parameters->setVerticalSpacing(8);
    parameters->setColumnStretch(0, 1);
    parameters->setColumnMinimumWidth(1, 150);
    addRow(parameters, 0, tr("Clearance [m]"),
           QStringLiteral("propagationClearanceLabel"), m_clearance, content);
    addRow(parameters, 1, tr("Resolution"),
           QStringLiteral("propagationResolutionLabel"), m_resolution, content);
    addRow(parameters, 2, tr("Azimuth Step"),
           QStringLiteral("propagationAzimuthStepLabel"), m_azimuthStep, content);
    addRow(parameters, 3, tr("Convergance"),
           QStringLiteral("propagationConvergenceLabel"), m_convergence, content);
    addRow(parameters, 4, tr("Range [Km]"),
           QStringLiteral("propagationRangeLabel"), m_range, content);
    addRow(parameters, 5, tr("Base Height [m]"),
           QStringLiteral("propagationBaseHeightLabel"), m_baseHeight, content);
    addRow(parameters, 6, tr("Tolerance 0..1"),
           QStringLiteral("propagationToleranceLabel"), m_tolerance, content);
    addRow(parameters, 7, tr("Min Alt [m]"),
           QStringLiteral("propagationMinimumAltitudeLabel"),
           m_minimumAltitude, content);
    addRow(parameters, 8, tr("Max Alt [m]"),
           QStringLiteral("propagationMaximumAltitudeLabel"),
           m_maximumAltitude, content);
    root->addLayout(parameters);

    auto *help = new QLabel(tr(
        "Elevation takes precedence when both Elevation and Terrain are "
        "enabled. Missing SRTM areas are left transparent and retried in "
        "the background."), content);
    help->setObjectName(QStringLiteral("propagationHelp"));
    help->setWordWrap(true);
    help->setTextFormat(Qt::PlainText);
    root->addWidget(help);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_close = new QPushButton(tr("Close"), content);
    m_close->setObjectName(QStringLiteral("propagationClose"));
    m_close->setMinimumWidth(90);
    buttons->addWidget(m_close);
    root->addLayout(buttons);
    root->addStretch(1);
    scroll->setWidget(content);

    auto *closeShortcut = new QShortcut(
        QKeySequence(Qt::CTRL | Qt::Key_W), this);
    closeShortcut->setObjectName(
        QStringLiteral("propagationCloseShortcut"));
    connect(closeShortcut, &QShortcut::activated,
            this, &PropagationSettingsWindow::close);
    connect(m_close, &QPushButton::clicked,
            this, &PropagationSettingsWindow::close);
}

void PropagationSettingsWindow::wireChanges()
{
    for (QCheckBox *check : {m_elevation, m_terrain, m_rf,
                             m_droneDistance, m_homeDistance, m_showScale,
                             m_manualAltitude}) {
        connect(check, &QCheckBox::toggled,
                this, &PropagationSettingsWindow::save);
    }
    for (QDoubleSpinBox *number : {m_clearance, m_range, m_baseHeight,
                                   m_tolerance, m_minimumAltitude,
                                   m_maximumAltitude}) {
        connect(number, qOverload<double>(&QDoubleSpinBox::valueChanged),
                this, &PropagationSettingsWindow::save);
    }
    for (QComboBox *choice : {m_resolution, m_azimuthStep, m_convergence}) {
        connect(choice, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &PropagationSettingsWindow::save);
    }
}

void PropagationSettingsWindow::syncFromStore()
{
    if (!m_store) {
        return;
    }
    const bool wasLoading = m_loading;
    m_loading = true;
    const PropagationSettings settings = m_store->settings();
    const QSignalBlocker elevationBlocker(m_elevation);
    const QSignalBlocker terrainBlocker(m_terrain);
    const QSignalBlocker rfBlocker(m_rf);
    const QSignalBlocker droneBlocker(m_droneDistance);
    const QSignalBlocker homeBlocker(m_homeDistance);
    const QSignalBlocker scaleBlocker(m_showScale);
    const QSignalBlocker altitudeBlocker(m_manualAltitude);
    const QSignalBlocker clearanceBlocker(m_clearance);
    const QSignalBlocker resolutionBlocker(m_resolution);
    const QSignalBlocker azimuthBlocker(m_azimuthStep);
    const QSignalBlocker convergenceBlocker(m_convergence);
    const QSignalBlocker rangeBlocker(m_range);
    const QSignalBlocker heightBlocker(m_baseHeight);
    const QSignalBlocker toleranceBlocker(m_tolerance);
    const QSignalBlocker minimumBlocker(m_minimumAltitude);
    const QSignalBlocker maximumBlocker(m_maximumAltitude);

    m_elevation->setChecked(settings.elevationMap);
    m_terrain->setChecked(settings.terrainMap);
    m_rf->setChecked(settings.rfMap);
    m_droneDistance->setChecked(settings.droneDistance);
    m_homeDistance->setChecked(settings.homeDistance);
    m_showScale->setChecked(settings.showScale);
    m_manualAltitude->setChecked(settings.manualAltitudeRange);
    m_clearance->setValue(settings.clearanceMeters);
    selectChoice(m_resolution, settings.resolutionPixels);
    selectChoice(m_azimuthStep, settings.azimuthStepDegrees);
    selectChoice(m_convergence, settings.convergenceDegrees);
    m_range->setValue(settings.rangeKilometers);
    m_baseHeight->setValue(settings.baseHeightMeters);
    m_tolerance->setValue(settings.tolerance);
    m_minimumAltitude->setValue(settings.minimumAltitude);
    m_maximumAltitude->setValue(settings.maximumAltitude);
    m_loading = wasLoading;
}

void PropagationSettingsWindow::save()
{
    if (m_loading || !m_store) {
        return;
    }
    if (!m_store->setSettings(currentSettings())) {
        syncFromStore();
    }
}

PropagationSettings PropagationSettingsWindow::currentSettings() const
{
    PropagationSettings result = m_store
        ? m_store->settings() : PropagationSettingsStore::defaults();
    result.clearanceMeters = m_clearance->value();
    result.resolutionPixels = m_resolution->currentData().toInt();
    result.azimuthStepDegrees = m_azimuthStep->currentData().toDouble();
    result.convergenceDegrees = m_convergence->currentData().toDouble();
    result.rangeKilometers = m_range->value();
    result.baseHeightMeters = m_baseHeight->value();
    result.tolerance = m_tolerance->value();
    result.minimumAltitude = m_minimumAltitude->value();
    result.maximumAltitude = m_maximumAltitude->value();
    result.elevationMap = m_elevation->isChecked();
    result.terrainMap = m_terrain->isChecked();
    result.rfMap = m_rf->isChecked();
    result.homeDistance = m_homeDistance->isChecked();
    result.droneDistance = m_droneDistance->isChecked();
    result.manualAltitudeRange = m_manualAltitude->isChecked();
    result.showScale = m_showScale->isChecked();
    return result;
}

void PropagationSettingsWindow::selectChoice(QComboBox *combo, double value)
{
    int selected = -1;
    for (int index = 0; index < combo->count(); ++index) {
        if (qAbs(combo->itemData(index).toDouble() - value) < 1.0e-9) {
            selected = index;
            break;
        }
    }
    combo->setCurrentIndex(selected >= 0 ? selected : 0);
}

void PropagationSettingsWindow::selectChoice(QComboBox *combo, int value)
{
    const int selected = combo->findData(value);
    combo->setCurrentIndex(selected >= 0 ? selected : 0);
}
