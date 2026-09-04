#include "ConfigTradHeliView.h"

#include "HeliCollectivePlot.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMetaType>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QVariant>
#include <QVBoxLayout>

#include <cmath>

namespace {

bool variantsEqual(const QVariant &left, const QVariant &right)
{
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    if (leftOk && rightOk) {
        return leftValue == rightValue
            || std::abs(leftValue - rightValue) <= 1.0e-6;
    }
    return left == right;
}

int decimalsFor(const ParamField &field)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int typeId = field.value.typeId();
#else
    const int typeId = field.value.userType();
#endif
    if (typeId == QMetaType::Int || typeId == QMetaType::UInt
        || typeId == QMetaType::LongLong
        || typeId == QMetaType::ULongLong) {
        return 0;
    }
    double step = std::abs(field.increment);
    for (int decimals = 0; decimals < 6; ++decimals) {
        if (step >= 1.0 || std::abs(step - std::round(step)) < 1.0e-9) {
            return decimals;
        }
        step *= 10.0;
    }
    return 6;
}

QString valueText(const QVariant &value)
{
    bool ok = false;
    const double numeric = value.toDouble(&ok);
    return ok && std::isfinite(numeric)
        ? QString::number(numeric, 'g', 12) : value.toString();
}

} // namespace

ConfigTradHeliView::ConfigTradHeliView(QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigTradHeliViewModel(this))
{
    setObjectName(QStringLiteral("ConfigTradHeliView"));
    buildUi();

    connect(m_viewModel, &ConfigTradHeliViewModel::structureChanged,
            this, &ConfigTradHeliView::rebuildFields);
    connect(m_viewModel, &ConfigTradHeliViewModel::fieldChanged,
            this, &ConfigTradHeliView::syncField);
    connect(m_viewModel, &ConfigTradHeliViewModel::stateChanged,
            this, &ConfigTradHeliView::syncState);
    connect(m_viewModel, &ConfigTradHeliViewModel::visualizationChanged,
            this, &ConfigTradHeliView::syncVisualization);
    connect(m_viewModel, &ConfigTradHeliViewModel::writeRequested,
            this, &ConfigTradHeliView::writeRequested);
    connect(m_viewModel, &ConfigTradHeliViewModel::refreshRequested,
            this, &ConfigTradHeliView::refreshRequested);
    connect(m_viewModel, &ConfigTradHeliViewModel::manualSafetyWarning,
            this, [this](const QString &warning) {
        m_safetyWarning->setText(warning);
        m_safetyWarning->setProperty("activeWarning", true);
        m_safetyWarning->style()->unpolish(m_safetyWarning);
        m_safetyWarning->style()->polish(m_safetyWarning);
        emit safetyWarning(warning);
    });

    rebuildFields();
    syncState();
    syncVisualization();
}

ConfigTradHeliView::~ConfigTradHeliView() = default;

QSize ConfigTradHeliView::sizeHint() const
{
    return QSize(1040, 760);
}

void ConfigTradHeliView::setCatalog(
    const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges)
{
    m_viewModel->setCatalog(catalog, enforceMetadataRanges);
}

void ConfigTradHeliView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigTradHeliView::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

void ConfigTradHeliView::setArmed(bool armed)
{
    m_viewModel->setArmed(armed);
}

void ConfigTradHeliView::setActive(bool active)
{
    m_viewModel->setActive(active);
}

void ConfigTradHeliView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigTradHeliView::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    m_viewModel->parameterWriteSubmitted(requestId, batchId);
}

void ConfigTradHeliView::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    m_viewModel->parameterWriteSubmissionFailed(requestId, reason);
}

void ConfigTradHeliView::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    m_viewModel->parameterWriteFailed(
        batchId, componentId, name, reason);
}

void ConfigTradHeliView::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    m_viewModel->parameterBatchCompleted(batchId, succeeded, failed);
}

void ConfigTradHeliView::refreshFailed(const QString &reason)
{
    m_viewModel->refreshFailed(reason);
}

void ConfigTradHeliView::refreshCanceled()
{
    m_viewModel->refreshCanceled();
}

void ConfigTradHeliView::remoteControlChannelRawChanged(
    int zeroBasedChannel, float pwm)
{
    m_viewModel->setRcInput(zeroBasedChannel, pwm);
    m_viewModel->pumpVisualization();
}

void ConfigTradHeliView::servoOutputChanged(
    int oneBasedChannel, int pwm)
{
    m_viewModel->setServoOutput(oneBasedChannel, pwm);
    m_viewModel->pumpVisualization();
}

void ConfigTradHeliView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_viewModel->setActive(true);
}

void ConfigTradHeliView::hideEvent(QHideEvent *event)
{
    setActive(false);
    QWidget::hideEvent(event);
}

void ConfigTradHeliView::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigTradHeliView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#tradHeliTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#tradHeliIntro { color: #C8C8C8; }"
        "QLabel#tradHeliStatus, QLabel#tradHeliServoStatus { color: #34D399; }"
        "QLabel#tradHeliSafetyWarning { color: #F5B942; }"
        "QLabel#tradHeliSafetyWarning[activeWarning=\"true\"] { color: #FF6B6B; }"
        "QFrame[heliPanel=\"true\"], QWidget[heliFieldRow=\"true\"] {"
        " background: #202623; border: 1px solid #303A35; }"
        "QPushButton { background: #161B18; color: #E6EDE9;"
        " border: 1px solid #303A35; padding: 5px 10px; }"
        "QPushButton:disabled { color: #68736D; }"
        "QComboBox, QDoubleSpinBox { background: #161B18; color: #E6EDE9;"
        " border: 1px solid #303A35; padding: 4px; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *title = new QLabel(m_viewModel->Title(), this);
    title->setObjectName(QStringLiteral("tradHeliTitle"));
    root->addWidget(title);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_refresh = new QPushButton(tr("Refresh Params"), this);
    m_refresh->setObjectName(QStringLiteral("tradHeliRefreshParams"));
    toolbar->addWidget(m_refresh);
    auto *intro = new QLabel(m_viewModel->Intro(), this);
    intro->setObjectName(QStringLiteral("tradHeliIntro"));
    intro->setWordWrap(true);
    toolbar->addWidget(intro, 1);
    root->addLayout(toolbar);

    auto *swashRow = new QWidget(this);
    swashRow->setObjectName(QStringLiteral("tradHeliSwashplateType"));
    auto *swashLayout = new QHBoxLayout(swashRow);
    swashLayout->setContentsMargins(0, 0, 0, 0);
    swashLayout->setSpacing(16);
    swashLayout->addWidget(new QLabel(tr("Swashplate Type"), swashRow));
    m_ccpm = new QRadioButton(tr("CCPM"), swashRow);
    m_ccpm->setObjectName(QStringLiteral("tradHeliSwashCcpm"));
    swashLayout->addWidget(m_ccpm);
    m_h1 = new QRadioButton(tr("H1 (Single Servo)"), swashRow);
    m_h1->setObjectName(QStringLiteral("tradHeliSwashH1"));
    swashLayout->addWidget(m_h1);
    swashLayout->addStretch(1);
    root->addWidget(swashRow);

    auto *manualRow = new QWidget(this);
    manualRow->setObjectName(QStringLiteral("tradHeliManualServo"));
    auto *manualLayout = new QHBoxLayout(manualRow);
    manualLayout->setContentsMargins(0, 0, 0, 0);
    manualLayout->setSpacing(6);
    manualLayout->addWidget(new QLabel(tr("Manual Servo:"), manualRow));
    const QList<QPair<QString, int>> modes = {
        {tr("Manual"), 1}, {tr("Max"), 2}, {tr("Center"), 3},
        {tr("Min"), 4}, {tr("Test"), 5}, {tr("Disable"), 0},
    };
    const QStringList names = {
        QStringLiteral("tradHeliManual"),
        QStringLiteral("tradHeliManualMax"),
        QStringLiteral("tradHeliManualCenter"),
        QStringLiteral("tradHeliManualMin"),
        QStringLiteral("tradHeliManualTest"),
        QStringLiteral("tradHeliManualDisable"),
    };
    for (int index = 0; index < modes.size(); ++index) {
        auto *button = new QPushButton(modes.at(index).first, manualRow);
        button->setObjectName(names.at(index));
        button->setProperty("manualServoButton", true);
        button->setProperty("mode", modes.at(index).second);
        button->setVisible(true);
        manualLayout->addWidget(button);
        m_manualButtons.append(button);
        connect(button, &QPushButton::clicked, this,
                [this, mode = modes.at(index).second]() {
            requestManualMode(mode);
        });
    }
    m_servoStatus = new QLabel(manualRow);
    m_servoStatus->setObjectName(QStringLiteral("tradHeliServoStatus"));
    m_servoStatus->setWordWrap(true);
    manualLayout->addWidget(m_servoStatus, 1);
    root->addWidget(manualRow);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("tradHeliStatus"));
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("tradHeliScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto *body = new QWidget(scroll);
    body->setObjectName(QStringLiteral("tradHeliBody"));
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(12);

    auto *livePanel = new QFrame(body);
    livePanel->setObjectName(QStringLiteral("tradHeliLivePanel"));
    livePanel->setProperty("heliPanel", true);
    auto *liveLayout = new QHBoxLayout(livePanel);
    liveLayout->setContentsMargins(10, 10, 10, 10);
    liveLayout->setSpacing(12);

    m_plot = new HeliCollectivePlot(livePanel);
    m_plot->setObjectName(QStringLiteral("tradHeliCollectivePlot"));
    m_plot->setMinimumWidth(430);
    liveLayout->addWidget(m_plot, 2);

    auto *liveControls = new QWidget(livePanel);
    auto *liveControlsLayout = new QVBoxLayout(liveControls);
    liveControlsLayout->setContentsMargins(0, 0, 0, 0);
    liveControlsLayout->setSpacing(6);
    auto *liveTitle = new QLabel(tr("Live servo setup"), liveControls);
    liveTitle->setObjectName(QStringLiteral("tradHeliLiveTitle"));
    QFont liveTitleFont = liveTitle->font();
    liveTitleFont.setBold(true);
    liveTitle->setFont(liveTitleFont);
    liveControlsLayout->addWidget(liveTitle);

    auto *collectiveAndServos = new QWidget(liveControls);
    auto *collectiveAndServosLayout = new QHBoxLayout(collectiveAndServos);
    collectiveAndServosLayout->setContentsMargins(0, 0, 0, 0);
    collectiveAndServosLayout->setSpacing(12);
    auto *collectiveColumn = new QWidget(collectiveAndServos);
    auto *collectiveLayout = new QVBoxLayout(collectiveColumn);
    collectiveLayout->setContentsMargins(0, 0, 0, 0);
    auto *collectiveCaption = new QLabel(tr("Collective"), collectiveColumn);
    collectiveCaption->setAlignment(Qt::AlignCenter);
    collectiveLayout->addWidget(collectiveCaption);
    m_collective = new QProgressBar(collectiveColumn);
    m_collective->setObjectName(QStringLiteral("tradHeliCollectiveInput"));
    m_collective->setOrientation(Qt::Vertical);
    m_collective->setRange(800, 2200);
    m_collective->setTextVisible(false);
    m_collective->setFixedSize(34, 170);
    collectiveLayout->addWidget(m_collective, 0, Qt::AlignHCenter);
    m_collectiveValue = new QLabel(collectiveColumn);
    m_collectiveValue->setObjectName(
        QStringLiteral("tradHeliCollectiveInputValue"));
    m_collectiveValue->setAlignment(Qt::AlignCenter);
    collectiveLayout->addWidget(m_collectiveValue);
    collectiveAndServosLayout->addWidget(collectiveColumn);

    auto *servoGauge = new QFrame(collectiveAndServos);
    servoGauge->setObjectName(QStringLiteral("tradHeliServoPositionGauge"));
    servoGauge->setProperty("heliPanel", true);
    servoGauge->setMinimumWidth(180);
    auto *servoLayout = new QVBoxLayout(servoGauge);
    auto *servoCaption = new QLabel(tr("Swash servo positions"), servoGauge);
    servoCaption->setAlignment(Qt::AlignCenter);
    servoCaption->setWordWrap(true);
    servoLayout->addWidget(servoCaption);
    m_servo1 = new QLabel(servoGauge);
    m_servo1->setObjectName(QStringLiteral("tradHeliServo1Position"));
    m_servo2 = new QLabel(servoGauge);
    m_servo2->setObjectName(QStringLiteral("tradHeliServo2Position"));
    m_servo3 = new QLabel(servoGauge);
    m_servo3->setObjectName(QStringLiteral("tradHeliServo3Position"));
    for (QLabel *label : {m_servo1, m_servo2, m_servo3}) {
        label->setAlignment(Qt::AlignCenter);
        QFont valueFont = label->font();
        valueFont.setPointSize(valueFont.pointSize() + 2);
        valueFont.setBold(true);
        label->setFont(valueFont);
        servoLayout->addWidget(label, 1);
    }
    collectiveAndServosLayout->addWidget(servoGauge, 1);
    liveControlsLayout->addWidget(collectiveAndServos, 1);

    liveControlsLayout->addWidget(new QLabel(tr("Rudder"), liveControls));
    m_rudder = new QProgressBar(liveControls);
    m_rudder->setObjectName(QStringLiteral("tradHeliRudderInput"));
    m_rudder->setRange(800, 2200);
    m_rudder->setTextVisible(false);
    m_rudder->setFixedHeight(22);
    liveControlsLayout->addWidget(m_rudder);
    m_rudderValue = new QLabel(liveControls);
    m_rudderValue->setObjectName(QStringLiteral("tradHeliRudderInputValue"));
    liveControlsLayout->addWidget(m_rudderValue);
    m_collectiveRange = new QLabel(liveControls);
    m_collectiveRange->setObjectName(QStringLiteral("tradHeliCollectiveRange"));
    m_collectiveRange->setWordWrap(true);
    liveControlsLayout->addWidget(m_collectiveRange);
    m_rudderRange = new QLabel(liveControls);
    m_rudderRange->setObjectName(QStringLiteral("tradHeliRudderRange"));
    m_rudderRange->setWordWrap(true);
    liveControlsLayout->addWidget(m_rudderRange);
    m_safetyWarning = new QLabel(
        tr("Range capture runs while H_SV_MAN is active. Remove blades "
           "before servo tests."), liveControls);
    m_safetyWarning->setObjectName(QStringLiteral("tradHeliSafetyWarning"));
    m_safetyWarning->setWordWrap(true);
    liveControlsLayout->addWidget(m_safetyWarning);
    liveLayout->addWidget(liveControls, 1);
    bodyLayout->addWidget(livePanel);

    m_fieldsContent = new QWidget(body);
    m_fieldsContent->setObjectName(QStringLiteral("tradHeliFields"));
    m_fieldsLayout = new QVBoxLayout(m_fieldsContent);
    m_fieldsLayout->setContentsMargins(0, 0, 0, 0);
    m_fieldsLayout->setSpacing(4);
    bodyLayout->addWidget(m_fieldsContent);
    bodyLayout->addStretch(1);
    scroll->setWidget(body);
    root->addWidget(scroll, 1);

    connect(m_refresh, &QPushButton::clicked,
            this, [this]() { m_viewModel->Refresh(); });
    connect(m_ccpm, &QRadioButton::clicked,
            this, [this]() {
        if (!m_viewModel->setSwashCcpm(true)) {
            syncState();
        }
    });
    connect(m_h1, &QRadioButton::clicked,
            this, [this]() {
        if (!m_viewModel->setSwashCcpm(false)) {
            syncState();
        }
    });
}

void ConfigTradHeliView::rebuildFields()
{
    while (QLayoutItem *item = m_fieldsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            delete widget;
        }
        delete item;
    }
    m_fieldEditors.clear();

    const QList<ParamField> fields = m_viewModel->Fields();
    for (const ParamField &field : fields) {
        FieldEditors editors;
        editors.row = new QWidget(m_fieldsContent);
        const QString suffix = objectSuffix(field.name);
        editors.row->setObjectName(
            QStringLiteral("tradHeliField_%1").arg(suffix));
        editors.row->setProperty("heliFieldRow", true);
        editors.row->setProperty("parameterName", field.name);
        auto *layout = new QGridLayout(editors.row);
        layout->setContentsMargins(8, 4, 8, 4);
        layout->setHorizontalSpacing(8);

        editors.label = new QLabel(field.label, editors.row);
        editors.label->setObjectName(
            QStringLiteral("tradHeliFieldLabel_%1").arg(suffix));
        editors.label->setMinimumWidth(220);
        editors.label->setWordWrap(true);
        editors.label->setToolTip(field.description);
        layout->addWidget(editors.label, 0, 0);

        if (field.editorKind == ParamField::EditorKind::Combo) {
            editors.combo = new QComboBox(editors.row);
            editors.combo->setObjectName(
                QStringLiteral("tradHeliFieldCombo_%1").arg(suffix));
            editors.combo->setMinimumWidth(180);
            editors.combo->setToolTip(field.description);
            for (const ParamOption &option : field.options) {
                editors.combo->addItem(option.text, option.value);
            }
            if (field.options.isEmpty() && field.value.isValid()) {
                editors.combo->addItem(valueText(field.value), field.value);
            }
            layout->addWidget(editors.combo, 0, 1);
            connect(editors.combo,
                    QOverload<int>::of(&QComboBox::activated),
                    this, [this, name = field.name,
                           combo = editors.combo](int index) {
                if (index < 0
                    || !m_viewModel->setFieldValue(
                        name, combo->itemData(index))) {
                    syncField(name);
                }
            });
        } else {
            editors.numeric = new QDoubleSpinBox(editors.row);
            editors.numeric->setObjectName(
                QStringLiteral("tradHeliFieldNumeric_%1").arg(suffix));
            editors.numeric->setMinimumWidth(180);
            editors.numeric->setDecimals(decimalsFor(field));
            editors.numeric->setSingleStep(
                field.increment > 0.0 ? field.increment : 0.01);
            if (field.hasRange && std::isfinite(field.minimum)
                && std::isfinite(field.maximum)
                && field.minimum <= field.maximum) {
                editors.numeric->setRange(field.minimum, field.maximum);
            } else {
                editors.numeric->setRange(-1.0e18, 1.0e18);
            }
            editors.numeric->setToolTip(field.description);
            layout->addWidget(editors.numeric, 0, 1);
            connect(editors.numeric, &QDoubleSpinBox::editingFinished,
                    this, [this, name = field.name, editor = editors.numeric]() {
                if (!m_viewModel->setFieldValue(name, editor->value())) {
                    syncField(name);
                }
            });
        }

        editors.units = new QLabel(field.units, editors.row);
        editors.units->setObjectName(
            QStringLiteral("tradHeliFieldUnits_%1").arg(suffix));
        layout->addWidget(editors.units, 0, 2);
        editors.status = new QLabel(field.status, editors.row);
        editors.status->setObjectName(
            QStringLiteral("tradHeliFieldStatus_%1").arg(suffix));
        editors.status->setMinimumWidth(90);
        layout->addWidget(editors.status, 0, 3);
        layout->setColumnStretch(3, 1);

        m_fieldsLayout->addWidget(editors.row);
        m_fieldEditors.insert(field.name, editors);
    }
    m_fieldsLayout->addStretch(1);
    syncState();
}

void ConfigTradHeliView::syncField(const QString &name)
{
    const QList<ParamField> fields = m_viewModel->Fields();
    const ParamField *field = findField(name, fields);
    const auto found = m_fieldEditors.find(name.trimmed().toUpper());
    if (!field || found == m_fieldEditors.end()) {
        return;
    }
    FieldEditors &editors = found.value();
    editors.label->setText(field->label);
    editors.units->setText(field->units);
    editors.status->setText(field->status);
    if (editors.combo) {
        const QSignalBlocker blocker(editors.combo);
        int selected = -1;
        for (int index = 0; index < editors.combo->count(); ++index) {
            if (variantsEqual(editors.combo->itemData(index), field->value)) {
                selected = index;
                break;
            }
        }
        editors.combo->setCurrentIndex(selected);
    } else if (editors.numeric) {
        const QSignalBlocker blocker(editors.numeric);
        bool ok = false;
        const double value = field->value.toDouble(&ok);
        if (ok && std::isfinite(value)) {
            editors.numeric->setValue(value);
        }
    }
    syncState();
}

void ConfigTradHeliView::syncState()
{
    const bool connected = m_viewModel->Connected();
    const bool ready = m_viewModel->SnapshotReady();
    const bool armed = m_viewModel->Armed();
    const bool pending = m_viewModel->HasPendingWrites();
    m_refresh->setEnabled(connected && ready && !pending);
    m_status->setText(m_viewModel->Status());
    m_servoStatus->setText(m_viewModel->ServoStatus());

    const bool swashEditable = connected && ready && !armed
        && m_viewModel->HasLegacySwash() && !pending;
    m_ccpm->setEnabled(swashEditable);
    m_h1->setEnabled(swashEditable);
    {
        const QSignalBlocker ccpmBlocker(m_ccpm);
        const QSignalBlocker h1Blocker(m_h1);
        const bool hasSwash = m_viewModel->HasLegacySwash();
        m_ccpm->setChecked(hasSwash && m_viewModel->SwashIsCcpm());
        m_h1->setChecked(hasSwash && !m_viewModel->SwashIsCcpm());
    }

    for (QPushButton *button : m_manualButtons) {
        const int mode = button->property("mode").toInt();
        const bool safeForArmedState = mode == 0 || !armed;
        const bool safeForPendingState = mode == 0 || !pending;
        button->setEnabled(connected && ready && safeForArmedState
                           && safeForPendingState
                           && m_viewModel->ManualModeSupported(mode));
        button->setVisible(true);
    }

    const QList<ParamField> fields = m_viewModel->Fields();
    for (const ParamField &field : fields) {
        const auto found = m_fieldEditors.constFind(field.name);
        if (found == m_fieldEditors.constEnd()) {
            continue;
        }
        const bool editable = connected && ready && !armed && !pending
            && !field.readOnly;
        if (found->combo) {
            found->combo->setEnabled(editable && !field.options.isEmpty());
        }
        if (found->numeric) {
            found->numeric->setEnabled(editable);
        }
        found->status->setText(field.status);
    }
}

void ConfigTradHeliView::syncVisualization()
{
    m_plot->setStabilizeCurve(m_viewModel->StabilizeCurve());
    m_plot->setAcroCurve(m_viewModel->AcroCurve());
    m_plot->setCursorPercent(m_viewModel->CollectiveCursorPercent());

    const int collective = qBound(800,
        qRound(m_viewModel->CollectiveInput()), 2200);
    const int rudder = qBound(800, qRound(m_viewModel->RudderInput()), 2200);
    m_collective->setValue(collective);
    m_collectiveValue->setText(tr("%1 µs").arg(collective));
    m_rudder->setValue(rudder);
    m_rudderValue->setText(tr("Current: %1 µs").arg(rudder));
    m_servo1->setText(tr("S1  %1°").arg(
        m_viewModel->Servo1Position(), 0, 'f', 0));
    m_servo2->setText(tr("S2  %1°").arg(
        m_viewModel->Servo2Position(), 0, 'f', 0));
    m_servo3->setText(tr("S3  %1°").arg(
        m_viewModel->Servo3Position(), 0, 'f', 0));
    m_collectiveRange->setText(m_viewModel->CollectiveRangeText());
    m_rudderRange->setText(m_viewModel->RudderRangeText());
}

void ConfigTradHeliView::requestManualMode(int mode)
{
    if (mode != 0) {
        QMessageBox warning(QMessageBox::Warning, tr("Remove blades"),
            tr("Confirm that all main and tail rotor blades have been removed "
               "before enabling manual servo movement."),
            QMessageBox::Ok | QMessageBox::Cancel, this);
        warning.setObjectName(QStringLiteral("tradHeliBladesRemovedWarning"));
        warning.setDefaultButton(QMessageBox::Cancel);
        warning.setEscapeButton(QMessageBox::Cancel);
        if (warning.exec() != QMessageBox::Ok) {
            return;
        }
    }
    if (!m_viewModel->setManualServoMode(mode)) {
        syncState();
    }
}

const ParamField *ConfigTradHeliView::findField(
    const QString &name, const QList<ParamField> &fields) const
{
    const QString normalized = name.trimmed().toUpper();
    for (const ParamField &field : fields) {
        if (field.name == normalized) {
            return &field;
        }
    }
    return nullptr;
}

QString ConfigTradHeliView::objectSuffix(const QString &name)
{
    QString suffix = name.trimmed().toUpper();
    for (int index = 0; index < suffix.size(); ++index) {
        const QChar character = suffix.at(index);
        if (!character.isLetterOrNumber() && character != QLatin1Char('_')) {
            suffix[index] = QLatin1Char('_');
        }
    }
    return suffix;
}
