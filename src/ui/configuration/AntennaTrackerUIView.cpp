#include "AntennaTrackerUIView.h"

#include "AntennaTrackerAxisPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

using Axis = AntennaTrackerUIViewModel::Axis;
using Field = AntennaTrackerUIViewModel::Field;

AntennaTrackerUIView::AntennaTrackerUIView(AntennaTrackerUIViewModel *viewModel, QWidget *parent)
    : AntennaTrackerUIView(viewModel, PageLayout::Live, parent)
{
}

AntennaTrackerUIView::AntennaTrackerUIView(AntennaTrackerUIViewModel *viewModel, PageLayout layout,
                                           QWidget *parent)
    : QWidget(parent)
    , m_viewModel(viewModel)
    , m_layout(layout)
{
    buildUi();
    bindViewModel();
    syncAll();
}

AntennaTrackerUIView::~AntennaTrackerUIView() = default;

QString AntennaTrackerUIView::prefix() const
{
    return m_layout == PageLayout::Live ? QStringLiteral("trkLive") : QStringLiteral("trkSerial");
}

void AntennaTrackerUIView::buildUi()
{
    const QString p = prefix();
    const bool live = m_layout == PageLayout::Live;
    setObjectName(p + QStringLiteral("Page"));
    setStyleSheet(QStringLiteral(
        "AntennaTrackerUIView { background: #1A201D; color: #E6EDE9; }"
        "AntennaTrackerUIView QLabel { color: #E6EDE9; }"
        "AntennaTrackerUIView QLabel[pageTitle=\"true\"] { color: #E8E8E8; font-size: 16px; font-weight: bold; }"
        "AntennaTrackerUIView QLabel[fieldLabel=\"true\"] { color: #BBC5BF; }"
        "AntennaTrackerUIView QLabel[axisTitle=\"true\"] { color: #BBC5BF; font-size: 15px; }"
        "AntennaTrackerUIView QLabel[warning=\"true\"] { color: #E2A33A; font-weight: bold; }"
        "AntennaTrackerUIView QLabel[bigValue=\"true\"] { font-size: 18px; font-weight: bold; }"
        "AntennaTrackerUIView QLabel[accent=\"true\"] { color: #7FB2E5; }"
        "AntennaTrackerUIView QFrame[inputBox=\"true\"] { background: #161B18; border: 1px solid #2A322D; border-radius: 4px; }"
        "AntennaTrackerUIView QLineEdit, AntennaTrackerUIView QComboBox {"
        " background: #161B18; color: #E6EDE9; border: 1px solid #2A322D; padding: 4px; }"
        "AntennaTrackerUIView QPushButton { padding: 5px 12px; }"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(p + QStringLiteral("Scroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    auto *content = new QWidget(m_scroll);
    content->setObjectName(p + QStringLiteral("Content"));
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(16, 16, 16, 16); // StackPanel Margin="16"
    root->setSpacing(10);                     // Spacing="10"

    m_title = new QLabel(AntennaTrackerUIViewModel::Title(), content);
    m_title->setObjectName(p + QStringLiteral("Title"));
    m_title->setProperty("pageTitle", true);
    root->addWidget(m_title);

    // Interface / port / baud / Connect / Find Trim Pan.
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);
    m_interface = new QComboBox(content);
    m_interface->setObjectName(p + QStringLiteral("Interface"));
    m_interface->setMinimumWidth(live ? 130 : 120);
    header->addWidget(m_interface);
    m_port = new QComboBox(content);
    m_port->setObjectName(p + QStringLiteral("Port"));
    m_port->setMinimumWidth(120);
    header->addWidget(m_port);
    m_baud = new QComboBox(content);
    m_baud->setObjectName(p + QStringLiteral("Baud"));
    m_baud->setMinimumWidth(100);
    header->addWidget(m_baud);
    m_connect = new QPushButton(AntennaTrackerUIViewModel::ConnectText(), content);
    m_connect->setObjectName(p + QStringLiteral("ConnectButton"));
    header->addWidget(m_connect);
    m_findTrim = new QPushButton(live ? AntennaTrackerUIViewModel::FindTrimPanLiveText()
                                      : AntennaTrackerUIViewModel::FindTrimPanSerialText(),
                                 content);
    m_findTrim->setObjectName(p + QStringLiteral("FindTrimButton"));
    header->addWidget(m_findTrim);
    header->addStretch(1);
    root->addLayout(header);

    m_warning = new QLabel(AntennaTrackerUIViewModel::ServoWarningText(), content);
    m_warning->setObjectName(p + QStringLiteral("Warning"));
    m_warning->setProperty("warning", true);
    m_warning->setWordWrap(true);
    root->addWidget(m_warning);

    if (live) {
        m_telemetryBox = new QFrame(content);
        m_telemetryBox->setObjectName(p + QStringLiteral("TelemetryBox"));
        m_telemetryBox->setProperty("inputBox", true);
        auto *grid = new QGridLayout(m_telemetryBox);
        grid->setContentsMargins(12, 12, 12, 12);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(4);
        const struct
        {
            const char *label;
            const char *name;
            QLabel **target;
            bool accent;
        } cells[] = {
            {"Vehicle Az", "VehicleAzimuth", &m_vehicleAzimuth, true},
            {"Vehicle El", "VehicleElevation", &m_vehicleElevation, true},
            {"Commanded Az", "CommandedAzimuth", &m_commandedAzimuth, false},
            {"Commanded El", "CommandedElevation", &m_commandedElevation, false},
        };
        int column = 0;
        for (const auto &cell : cells) {
            auto *caption = new QLabel(tr(cell.label), m_telemetryBox);
            caption->setObjectName(p + QLatin1String(cell.name) + QStringLiteral("Label"));
            caption->setProperty("fieldLabel", true);
            auto *value = new QLabel(AntennaTrackerUIViewModel::PlaceholderText(), m_telemetryBox);
            value->setObjectName(p + QLatin1String(cell.name));
            value->setProperty("bigValue", true);
            if (cell.accent) {
                value->setProperty("accent", true);
            }
            *cell.target = value;
            grid->addWidget(caption, 0, column);
            grid->addWidget(value, 1, column);
            grid->setColumnStretch(column, 1);
            ++column;
        }
        root->addWidget(m_telemetryBox);

        m_manualBox = new QFrame(content);
        m_manualBox->setObjectName(p + QStringLiteral("ManualBox"));
        m_manualBox->setProperty("inputBox", true);
        auto *manual = new QVBoxLayout(m_manualBox);
        manual->setContentsMargins(12, 12, 12, 12);
        manual->setSpacing(8);
        auto *manualRow = new QHBoxLayout;
        manualRow->setSpacing(12);
        m_manualMode = new QCheckBox(AntennaTrackerUIViewModel::ManualSlewText(), m_manualBox);
        m_manualMode->setObjectName(p + QStringLiteral("ManualMode"));
        manualRow->addWidget(m_manualMode);
        m_homeCenter = new QPushButton(AntennaTrackerUIViewModel::HomeCenterText(), m_manualBox);
        m_homeCenter->setObjectName(p + QStringLiteral("HomeCenterButton"));
        manualRow->addWidget(m_homeCenter);
        manualRow->addStretch(1);
        manual->addLayout(manualRow);
        auto *sliders = new QGridLayout;
        sliders->setHorizontalSpacing(8);
        sliders->setVerticalSpacing(6);
        sliders->setColumnMinimumWidth(0, 90); // ColumnDefinitions="90,*,70"
        sliders->setColumnMinimumWidth(2, 70);
        sliders->setColumnStretch(1, 1);
        auto *azLabel = new QLabel(tr("Azimuth"), m_manualBox);
        azLabel->setObjectName(p + QStringLiteral("ManualAzimuthCaption"));
        azLabel->setProperty("fieldLabel", true);
        m_manualAzimuth = new QSlider(Qt::Horizontal, m_manualBox);
        m_manualAzimuth->setObjectName(p + QStringLiteral("ManualAzimuth"));
        m_manualAzimuth->setRange(-180, 180);
        m_manualAzimuthLabel = new QLabel(QStringLiteral("0°"), m_manualBox);
        m_manualAzimuthLabel->setObjectName(p + QStringLiteral("ManualAzimuthLabel"));
        m_manualAzimuthLabel->setProperty("fieldLabel", true);
        sliders->addWidget(azLabel, 0, 0);
        sliders->addWidget(m_manualAzimuth, 0, 1);
        sliders->addWidget(m_manualAzimuthLabel, 0, 2);
        auto *elLabel = new QLabel(tr("Elevation"), m_manualBox);
        elLabel->setObjectName(p + QStringLiteral("ManualElevationCaption"));
        elLabel->setProperty("fieldLabel", true);
        m_manualElevation = new QSlider(Qt::Horizontal, m_manualBox);
        m_manualElevation->setObjectName(p + QStringLiteral("ManualElevation"));
        m_manualElevation->setRange(-90, 90);
        m_manualElevationLabel = new QLabel(QStringLiteral("0°"), m_manualBox);
        m_manualElevationLabel->setObjectName(p + QStringLiteral("ManualElevationLabel"));
        m_manualElevationLabel->setProperty("fieldLabel", true);
        sliders->addWidget(elLabel, 1, 0);
        sliders->addWidget(m_manualElevation, 1, 1);
        sliders->addWidget(m_manualElevationLabel, 1, 2);
        manual->addLayout(sliders);
        root->addWidget(m_manualBox);
    }

    // Pan | Tilt (Grid ColumnDefinitions="*,12,*").
    auto *axes = new QHBoxLayout;
    axes->setContentsMargins(0, 0, 0, 0);
    axes->setSpacing(12);
    AntennaTrackerAxisPanel::Options panOptions;
    panOptions.title = tr("Pan");
    panOptions.reverseText = live ? tr("Reverse") : tr("Rev");
    panOptions.objectPrefix = p + QStringLiteral("Pan");
    panOptions.showCenterLabel = !live;
    panOptions.trimMin = -180.0;
    panOptions.trimMax = 180.0;
    m_pan = new AntennaTrackerAxisPanel(Axis::Pan, panOptions, content);
    m_pan->setProperty("inputBox", true);
    axes->addWidget(m_pan, 1);
    AntennaTrackerAxisPanel::Options tiltOptions = panOptions;
    tiltOptions.title = tr("Tilt");
    tiltOptions.objectPrefix = p + QStringLiteral("Tilt");
    tiltOptions.trimMin = -45.0;
    tiltOptions.trimMax = 45.0;
    m_tilt = new AntennaTrackerAxisPanel(Axis::Tilt, tiltOptions, content);
    m_tilt->setProperty("inputBox", true);
    axes->addWidget(m_tilt, 1);
    root->addLayout(axes);

    m_status = new QLabel(content);
    m_status->setObjectName(p + QStringLiteral("Status"));
    m_status->setProperty("accent", true);
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    root->addWidget(m_status);
    root->addStretch(1);

    m_scroll->setWidget(content);
    outer->addWidget(m_scroll);
}

void AntennaTrackerUIView::bindViewModel()
{
    AntennaTrackerUIViewModel *vm = m_viewModel;
    if (!vm) {
        return;
    }

    // View -> view model.
    connect(m_interface, &QComboBox::currentTextChanged, vm,
            &AntennaTrackerUIViewModel::setSelectedInterface);
    connect(m_port, &QComboBox::currentTextChanged, vm, &AntennaTrackerUIViewModel::setSelectedPort);
    connect(m_baud, &QComboBox::currentTextChanged, vm, &AntennaTrackerUIViewModel::setSelectedBaud);
    connect(m_connect, &QPushButton::clicked, vm, &AntennaTrackerUIViewModel::connectOrDisconnect);
    connect(m_findTrim, &QPushButton::clicked, vm, &AntennaTrackerUIViewModel::findTrimPan);
    for (AntennaTrackerAxisPanel *panel : {m_pan, m_tilt}) {
        connect(panel, &AntennaTrackerAxisPanel::fieldEdited, vm, &AntennaTrackerUIViewModel::setField);
        connect(panel, &AntennaTrackerAxisPanel::trimEdited, vm, &AntennaTrackerUIViewModel::setTrim);
        connect(panel, &AntennaTrackerAxisPanel::reverseToggled, vm,
                &AntennaTrackerUIViewModel::setReverse);
    }
    if (m_manualMode) {
        connect(m_manualMode, &QCheckBox::toggled, vm, &AntennaTrackerUIViewModel::setManualMode);
        connect(m_homeCenter, &QPushButton::clicked, vm, &AntennaTrackerUIViewModel::homeCenter);
        connect(m_manualAzimuth, &QSlider::valueChanged, vm,
                [vm](int value) { vm->setManualAzimuth(value); });
        connect(m_manualElevation, &QSlider::valueChanged, vm,
                [vm](int value) { vm->setManualElevation(value); });
    }

    // View model -> view.
    connect(vm, &AntennaTrackerUIViewModel::selectedInterfaceChanged, this,
            [this](const QString &value) { selectComboText(m_interface, value); });
    connect(vm, &AntennaTrackerUIViewModel::selectedPortChanged, this,
            [this](const QString &value) { selectComboText(m_port, value); });
    connect(vm, &AntennaTrackerUIViewModel::selectedBaudChanged, this,
            [this](const QString &value) { selectComboText(m_baud, value); });
    connect(vm, &AntennaTrackerUIViewModel::portsChanged, this, [this]() { syncPorts(); });
    connect(vm, &AntennaTrackerUIViewModel::fieldChanged, this,
            [this](Axis axis, Field field, const QString &value) {
        (axis == Axis::Pan ? m_pan : m_tilt)->setFieldText(field, value);
    });
    connect(vm, &AntennaTrackerUIViewModel::trimChanged, this, [this](Axis axis, double value) {
        (axis == Axis::Pan ? m_pan : m_tilt)->setTrim(value);
    });
    connect(vm, &AntennaTrackerUIViewModel::trimRangeChanged, this,
            [this](Axis axis, double minimum, double maximum) {
        (axis == Axis::Pan ? m_pan : m_tilt)->setTrimRange(minimum, maximum);
    });
    connect(vm, &AntennaTrackerUIViewModel::reverseChanged, this, [this](Axis axis, bool value) {
        (axis == Axis::Pan ? m_pan : m_tilt)->setReverse(value);
    });
    connect(vm, &AntennaTrackerUIViewModel::connectTextChanged, m_connect, &QPushButton::setText);
    connect(vm, &AntennaTrackerUIViewModel::statusChanged, m_status, &QLabel::setText);
    connect(vm, &AntennaTrackerUIViewModel::controlsEnabledChanged, this,
            [this]() { syncEnabledStates(); });
    connect(vm, &AntennaTrackerUIViewModel::speedAccelEnabledChanged, this,
            [this]() { syncEnabledStates(); });
    connect(vm, &AntennaTrackerUIViewModel::telemetryChanged, this, [this]() { syncTelemetry(); });
    connect(vm, &AntennaTrackerUIViewModel::manualModeChanged, this, [this]() { syncManual(); });
    connect(vm, &AntennaTrackerUIViewModel::manualAzimuthChanged, this, [this]() { syncManual(); });
    connect(vm, &AntennaTrackerUIViewModel::manualElevationChanged, this, [this]() { syncManual(); });
}

void AntennaTrackerUIView::selectComboText(QComboBox *combo, const QString &text)
{
    if (combo->currentText() == text) {
        return;
    }
    const QSignalBlocker blocker(combo);
    const int index = combo->findText(text);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    } else if (!text.isEmpty()) {
        combo->addItem(text);
        combo->setCurrentIndex(combo->count() - 1);
    } else {
        combo->setCurrentIndex(-1);
    }
}

void AntennaTrackerUIView::syncPorts()
{
    if (!m_viewModel) {
        return;
    }
    const QSignalBlocker blocker(m_port);
    m_port->clear();
    m_port->addItems(m_viewModel->ports());
    const int index = m_port->findText(m_viewModel->selectedPort());
    m_port->setCurrentIndex(index);
}

void AntennaTrackerUIView::syncEnabledStates()
{
    if (!m_viewModel) {
        return;
    }
    const bool controls = m_viewModel->controlsEnabled();
    const bool speedAccel = m_viewModel->speedAccelEnabled();
    m_interface->setEnabled(controls);
    m_port->setEnabled(controls);
    m_baud->setEnabled(controls);
    for (AntennaTrackerAxisPanel *panel : {m_pan, m_tilt}) {
        panel->setControlsEnabled(controls);
        panel->setSpeedAccelEnabled(speedAccel);
    }
}

void AntennaTrackerUIView::syncTelemetry()
{
    if (!m_viewModel || !m_vehicleAzimuth) {
        return;
    }
    m_vehicleAzimuth->setText(m_viewModel->vehicleAzimuth());
    m_vehicleElevation->setText(m_viewModel->vehicleElevation());
    m_commandedAzimuth->setText(m_viewModel->commandedAzimuth());
    m_commandedElevation->setText(m_viewModel->commandedElevation());
}

void AntennaTrackerUIView::syncManual()
{
    if (!m_viewModel || !m_manualMode) {
        return;
    }
    const bool manual = m_viewModel->manualMode();
    {
        const QSignalBlocker blocker(m_manualMode);
        m_manualMode->setChecked(manual);
    }
    {
        const QSignalBlocker blocker(m_manualAzimuth);
        m_manualAzimuth->setValue(qRound(m_viewModel->manualAzimuth()));
    }
    {
        const QSignalBlocker blocker(m_manualElevation);
        m_manualElevation->setValue(qRound(m_viewModel->manualElevation()));
    }
    m_manualAzimuth->setEnabled(manual);
    m_manualElevation->setEnabled(manual);
    // MP10 StringFormat='{}{0:0}°'
    m_manualAzimuthLabel->setText(QStringLiteral("%1°").arg(m_manualAzimuth->value()));
    m_manualElevationLabel->setText(QStringLiteral("%1°").arg(m_manualElevation->value()));
}

void AntennaTrackerUIView::syncAll()
{
    if (!m_viewModel) {
        return;
    }
    {
        const QSignalBlocker blocker(m_interface);
        m_interface->clear();
        m_interface->addItems(m_viewModel->interfaces());
    }
    selectComboText(m_interface, m_viewModel->selectedInterface());
    {
        const QSignalBlocker blocker(m_baud);
        m_baud->clear();
        m_baud->addItems(m_viewModel->bauds());
    }
    selectComboText(m_baud, m_viewModel->selectedBaud());
    syncPorts();
    for (AntennaTrackerAxisPanel *panel : {m_pan, m_tilt}) {
        const Axis axis = panel->axis();
        for (Field field : {Field::Range, Field::PwmRange, Field::Center, Field::Speed, Field::Accel}) {
            panel->setFieldText(field, m_viewModel->field(axis, field));
        }
        panel->setTrimRange(m_viewModel->trimMin(axis), m_viewModel->trimMax(axis));
        panel->setTrim(m_viewModel->trim(axis));
        panel->setReverse(m_viewModel->reverse(axis));
    }
    m_connect->setText(m_viewModel->connectText());
    m_status->setText(m_viewModel->status());
    syncEnabledStates();
    syncTelemetry();
    syncManual();
}

void AntennaTrackerUIView::activate()
{
    if (m_viewModel) {
        m_viewModel->activate();
    }
}

void AntennaTrackerUIView::deactivate()
{
    if (m_viewModel) {
        m_viewModel->deactivate();
    }
}
