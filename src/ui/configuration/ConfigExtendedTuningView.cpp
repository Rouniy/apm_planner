#include "ConfigExtendedTuningView.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {
bool valuesEqual(const QVariant &left, const QVariant &right)
{
    bool leftOk = false;
    bool rightOk = false;
    const double leftValue = left.toDouble(&leftOk);
    const double rightValue = right.toDouble(&rightOk);
    return leftOk && rightOk
        ? std::abs(leftValue - rightValue) <= 1.0e-6
        : left == right;
}

quint64 bitMask(int bit)
{
    return bit >= 0 && bit < 64 ? (quint64(1) << bit) : 0;
}
} // namespace

ConfigExtendedTuningView::ConfigExtendedTuningView(QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigExtendedTuningViewModel(this))
{
    setObjectName(QStringLiteral("ConfigExtendedTuningView"));
    buildUi();

    connect(m_viewModel, &ConfigExtendedTuningViewModel::structureChanged,
            this, &ConfigExtendedTuningView::rebuildRows);
    connect(m_viewModel, &ConfigExtendedTuningViewModel::rowsChanged,
            this, &ConfigExtendedTuningView::syncRows);
    connect(m_viewModel, &ConfigExtendedTuningViewModel::stateChanged,
            this, &ConfigExtendedTuningView::syncState);
    connect(m_viewModel,
            &ConfigExtendedTuningViewModel::largeIncreaseConfirmationRequested,
            this, &ConfigExtendedTuningView::confirmLargeIncrease);
    connect(m_viewModel, &ConfigExtendedTuningViewModel::writeRequested,
            this, &ConfigExtendedTuningView::writeRequested);
    connect(m_viewModel, &ConfigExtendedTuningViewModel::refreshRequested,
            this, &ConfigExtendedTuningView::refreshRequested);

    rebuildRows();
    syncState();
}

ConfigExtendedTuningView::~ConfigExtendedTuningView() = default;

QSize ConfigExtendedTuningView::sizeHint() const
{
    return QSize(1060, 780);
}

void ConfigExtendedTuningView::setCatalog(
    const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges)
{
    m_viewModel->setCatalog(catalog, enforceMetadataRanges);
}

void ConfigExtendedTuningView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent, bool completeSnapshot, bool preserveStagedEdits)
{
    m_viewModel->setParameterSnapshot(
        parameters, preferredComponent, completeSnapshot,
        preserveStagedEdits);
}

void ConfigExtendedTuningView::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

void ConfigExtendedTuningView::setArmed(bool armed)
{
    m_viewModel->setArmed(armed);
}

void ConfigExtendedTuningView::setHeartbeatFresh(bool fresh)
{
    m_viewModel->setHeartbeatFresh(fresh);
}

void ConfigExtendedTuningView::setHeartbeat(bool fresh, bool armed)
{
    m_viewModel->setHeartbeat(fresh, armed);
}

void ConfigExtendedTuningView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigExtendedTuningView::parameterWriteSubmitted(
    quint64 requestId, qulonglong batchId)
{
    m_viewModel->parameterWriteSubmitted(requestId, batchId);
}

void ConfigExtendedTuningView::parameterWriteSubmissionFailed(
    quint64 requestId, const QString &reason)
{
    m_viewModel->parameterWriteSubmissionFailed(requestId, reason);
}

void ConfigExtendedTuningView::parameterWriteFailed(
    qulonglong batchId, int componentId, const QString &name,
    const QString &reason)
{
    m_viewModel->parameterWriteFailed(batchId, componentId, name, reason);
}

void ConfigExtendedTuningView::parameterWriteCancelled(
    qulonglong batchId, int componentId, const QString &name)
{
    m_viewModel->parameterWriteCancelled(batchId, componentId, name);
}

void ConfigExtendedTuningView::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    m_viewModel->parameterBatchCompleted(batchId, succeeded, failed);
}

void ConfigExtendedTuningView::refreshFailed(const QString &reason)
{
    m_viewModel->refreshFailed(reason);
}

void ConfigExtendedTuningView::refreshCanceled()
{
    m_viewModel->refreshCanceled();
}

void ConfigExtendedTuningView::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigExtendedTuningView { background: #1A201D; color: #E6EDE9; }"
        "QLabel#extendedTuningTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#extendedTuningIntro { color: #C8C8C8; }"
        "QLabel#extendedTuningStatus { color: #34D399; }"
        "QGroupBox[extendedTuningCard=\"true\"] { color: #E6EDE9;"
        " border: 1px solid #303A35; margin-top: 12px; padding-top: 8px;"
        " font-weight: bold; background: #202623; }"
        "QGroupBox[extendedTuningCard=\"true\"]::title {"
        " subcontrol-origin: margin; left: 9px; padding: 0 4px; }"
        "QWidget[extendedTuningRow=\"true\"] { background: #202623; }"
        "QPushButton, QComboBox, QDoubleSpinBox, QToolButton {"
        " background: #161B18; color: #E6EDE9; border: 1px solid #303A35;"
        " padding: 4px; }"
        "QPushButton:disabled, QComboBox:disabled, QDoubleSpinBox:disabled,"
        " QToolButton:disabled { color: #68736D; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("extendedTuningTitle"));
    root->addWidget(m_title);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_write = new QPushButton(tr("Write Params"), this);
    m_write->setObjectName(QStringLiteral("extendedTuningWrite"));
    toolbar->addWidget(m_write);
    m_refreshParams = new QPushButton(tr("Refresh Params"), this);
    m_refreshParams->setObjectName(
        QStringLiteral("extendedTuningRefreshParams"));
    toolbar->addWidget(m_refreshParams);
    m_refreshScreen = new QPushButton(tr("Refresh Screen"), this);
    m_refreshScreen->setObjectName(
        QStringLiteral("extendedTuningRefreshScreen"));
    toolbar->addWidget(m_refreshScreen);
    m_lockRollPitch = new QCheckBox(
        tr("Lock Pitch and Roll Values"), this);
    m_lockRollPitch->setObjectName(
        QStringLiteral("extendedTuningLockRollPitch"));
    toolbar->addWidget(m_lockRollPitch);
    m_intro = new QLabel(this);
    m_intro->setObjectName(QStringLiteral("extendedTuningIntro"));
    m_intro->setWordWrap(true);
    toolbar->addWidget(m_intro, 1);
    root->addLayout(toolbar);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("extendedTuningStatus"));
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("extendedTuningScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_cards = new QWidget(scroll);
    m_cards->setObjectName(QStringLiteral("extendedTuningCards"));
    m_cardsLayout = new QGridLayout(m_cards);
    m_cardsLayout->setContentsMargins(0, 0, 0, 0);
    m_cardsLayout->setHorizontalSpacing(10);
    m_cardsLayout->setVerticalSpacing(10);
    m_cardsLayout->setColumnStretch(0, 1);
    m_cardsLayout->setColumnStretch(1, 1);
    scroll->setWidget(m_cards);
    root->addWidget(scroll, 1);

    connect(m_write, &QPushButton::clicked,
            m_viewModel, [this]() { m_viewModel->Save(); });
    connect(m_refreshParams, &QPushButton::clicked,
            m_viewModel, &ConfigExtendedTuningViewModel::Refresh);
    connect(m_refreshScreen, &QPushButton::clicked,
            m_viewModel, &ConfigExtendedTuningViewModel::RefreshScreen);
    connect(m_lockRollPitch, &QCheckBox::toggled,
            m_viewModel, &ConfigExtendedTuningViewModel::setLockRollPitch);
}

void ConfigExtendedTuningView::rebuildRows()
{
    while (QLayoutItem *item = m_cardsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_rowEditors.clear();

    const QList<ExtendedTuningGroupDescriptor> groups = m_viewModel->Groups();
    const QList<ExtendedTuningRow> rows = m_viewModel->Rows();
    m_rowEditors.resize(rows.size());
    for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const ExtendedTuningGroupDescriptor &descriptor = groups.at(groupIndex);
        auto *group = new QGroupBox(descriptor.title, m_cards);
        group->setObjectName(
            QStringLiteral("extendedTuningSection%1")
                .arg(groupObjectSuffix(descriptor.title)));
        group->setProperty("extendedTuningCard", true);
        group->setProperty("sectionIndex", groupIndex);
        group->setMinimumWidth(350);
        auto *layout = new QVBoxLayout(group);
        layout->setContentsMargins(8, 12, 8, 8);
        layout->setSpacing(2);

        for (int offset = 0; offset < descriptor.rowCount; ++offset) {
            const int rowIndex = descriptor.firstRow + offset;
            if (rowIndex < 0 || rowIndex >= rows.size()) {
                continue;
            }
            RowEditors editors = makeEditors(rowIndex, rows.at(rowIndex), group);
            layout->addWidget(editors.row);
            m_rowEditors[rowIndex] = editors;
        }
        layout->addStretch(1);
        m_cardsLayout->addWidget(group, groupIndex / 2, groupIndex % 2,
                                 Qt::AlignTop);
    }
    m_cardsLayout->setRowStretch((groups.size() + 1) / 2, 1);
    syncRows();
}

ConfigExtendedTuningView::RowEditors
ConfigExtendedTuningView::makeEditors(
    int rowIndex, const ExtendedTuningRow &row, QWidget *parent)
{
    RowEditors result;
    const QString number = QStringLiteral("%1").arg(rowIndex + 1, 2, 10,
                                                     QLatin1Char('0'));
    result.row = new QWidget(parent);
    result.row->setObjectName(QStringLiteral("extendedTuningRow") + number);
    result.row->setProperty("extendedTuningRow", true);
    result.row->setProperty("rowIndex", rowIndex);
    result.row->setProperty("parameterName", row.resolvedName);
    auto *layout = new QHBoxLayout(result.row);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(5);

    result.label = new QLabel(row.label, result.row);
    result.label->setObjectName(QStringLiteral("extendedTuningLabel") + number);
    result.label->setMinimumWidth(108);
    result.label->setWordWrap(true);
    layout->addWidget(result.label, 1);

    if (row.field.editorKind == ParamField::EditorKind::Combo) {
        result.combo = new QComboBox(result.row);
        result.combo->setObjectName(
            QStringLiteral("extendedTuningEditor") + number);
        result.combo->setMinimumWidth(145);
        for (const ParamOption &option : row.field.options) {
            result.combo->addItem(option.text, option.value);
        }
        layout->addWidget(result.combo);
        connect(result.combo, QOverload<int>::of(&QComboBox::activated),
                this, [this, rowIndex, combo = result.combo](int index) {
            if (index < 0
                || !m_viewModel->stageValue(rowIndex, combo->itemData(index))) {
                syncRow(rowIndex);
            }
        });
    } else if (row.field.editorKind == ParamField::EditorKind::Bitmask) {
        result.bitmask = new QToolButton(result.row);
        result.bitmask->setObjectName(
            QStringLiteral("extendedTuningEditor") + number);
        result.bitmask->setMinimumWidth(145);
        result.bitmask->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(result.bitmask);
        for (const BitOption &option : row.field.bitOptions) {
            auto *action = menu->addAction(
                QStringLiteral("%1: %2").arg(option.bit).arg(option.label));
            action->setCheckable(true);
            action->setProperty("bit", option.bit);
            result.bitActions.append(action);
            connect(action, &QAction::toggled, this,
                    [this, rowIndex](bool) { stageBitmask(rowIndex); });
        }
        result.bitmask->setMenu(menu);
        layout->addWidget(result.bitmask);
    } else {
        result.numeric = new QDoubleSpinBox(result.row);
        result.numeric->setObjectName(
            QStringLiteral("extendedTuningEditor") + number);
        result.numeric->setMinimumWidth(145);
        result.numeric->setDecimals(decimalsFor(row.field));
        result.numeric->setSingleStep(
            row.field.increment > 0.0 ? row.field.increment : 0.01);
        result.numeric->setRange(
            row.field.hasRange && row.field.enforceRange
                ? row.field.minimum : -1.0e9,
            row.field.hasRange && row.field.enforceRange
                ? row.field.maximum : 1.0e9);
        layout->addWidget(result.numeric);
        connect(result.numeric,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, rowIndex](double value) {
            if (!m_viewModel->stageValue(rowIndex, value)) {
                syncRow(rowIndex);
            }
        });
    }

    result.units = new QLabel(row.field.units, result.row);
    result.units->setObjectName(
        QStringLiteral("extendedTuningUnits") + number);
    result.units->setMinimumWidth(36);
    layout->addWidget(result.units);
    result.status = new QLabel(result.row);
    result.status->setObjectName(
        QStringLiteral("extendedTuningRowStatus") + number);
    result.status->setMinimumWidth(28);
    result.status->setStyleSheet(QStringLiteral("color: #34D399;"));
    layout->addWidget(result.status);

    const QString toolTip = fieldToolTip(row);
    result.label->setToolTip(toolTip);
    if (result.combo) {
        result.combo->setToolTip(toolTip);
    } else if (result.numeric) {
        result.numeric->setToolTip(toolTip);
    } else if (result.bitmask) {
        result.bitmask->setToolTip(toolTip);
    }
    return result;
}

void ConfigExtendedTuningView::syncRows()
{
    const int count = qMin(m_rowEditors.size(), m_viewModel->Rows().size());
    for (int row = 0; row < count; ++row) {
        syncRow(row);
    }
    syncState();
}

void ConfigExtendedTuningView::syncRow(int rowIndex)
{
    const QList<ExtendedTuningRow> rows = m_viewModel->Rows();
    if (rowIndex < 0 || rowIndex >= rows.size()
        || rowIndex >= m_rowEditors.size()) {
        return;
    }
    const ExtendedTuningRow &row = rows.at(rowIndex);
    RowEditors &editors = m_rowEditors[rowIndex];
    if (!editors.row) {
        return;
    }
    editors.row->setProperty("parameterName", row.resolvedName);
    editors.row->setProperty("available", row.exists);
    editors.row->setProperty("dirty", row.dirty());
    const QString toolTip = fieldToolTip(row);
    editors.label->setToolTip(toolTip);
    editors.units->setText(row.field.units);
    editors.status->setText(row.exists ? row.field.status : tr("n/a"));

    if (editors.combo) {
        QSignalBlocker blocker(editors.combo);
        int index = optionIndex(editors.combo, row.field.value);
        if (row.exists && index < 0 && row.field.value.isValid()) {
            editors.combo->addItem(
                tr("Unknown (%1)").arg(row.field.value.toString()),
                row.field.value);
            index = editors.combo->count() - 1;
        }
        editors.combo->setCurrentIndex(index);
    } else if (editors.numeric) {
        QSignalBlocker blocker(editors.numeric);
        bool ok = false;
        const double value = row.field.value.toDouble(&ok);
        if (ok) {
            editors.numeric->setValue(value);
        }
    } else if (editors.bitmask) {
        bool ok = false;
        const quint64 value = row.field.value.toULongLong(&ok);
        for (QAction *action : editors.bitActions) {
            const QSignalBlocker blocker(action);
            const quint64 mask = bitMask(action->property("bit").toInt());
            action->setChecked(ok && mask != 0 && (value & mask) != 0);
        }
        editors.bitmask->setText(bitmaskSummary(row));
    }
}

void ConfigExtendedTuningView::syncState()
{
    m_title->setText(m_viewModel->Title());
    m_intro->setText(m_viewModel->Intro());
    m_status->setText(m_viewModel->Status());
    m_write->setEnabled(m_viewModel->CanWrite());
    m_refreshParams->setEnabled(
        m_viewModel->Connected() && !m_viewModel->Busy());
    m_refreshScreen->setEnabled(
        m_viewModel->SnapshotReady() && !m_viewModel->Busy());
    {
        const QSignalBlocker blocker(m_lockRollPitch);
        m_lockRollPitch->setChecked(m_viewModel->LockRollPitch());
    }
    m_lockRollPitch->setEnabled(
        m_viewModel->SnapshotReady() && !m_viewModel->Busy());

    const QList<ExtendedTuningRow> rows = m_viewModel->Rows();
    const int count = qMin(rows.size(), m_rowEditors.size());
    for (int rowIndex = 0; rowIndex < count; ++rowIndex) {
        const ExtendedTuningRow &row = rows.at(rowIndex);
        RowEditors &editors = m_rowEditors[rowIndex];
        const bool enabled = m_viewModel->CanEditRow(rowIndex);
        if (editors.combo) {
            editors.combo->setEnabled(enabled && editors.combo->count() > 0);
        } else if (editors.numeric) {
            editors.numeric->setEnabled(enabled);
        } else if (editors.bitmask) {
            editors.bitmask->setEnabled(enabled
                                        && !editors.bitActions.isEmpty());
        }
    }
}

void ConfigExtendedTuningView::stageBitmask(int rowIndex)
{
    const QList<ExtendedTuningRow> rows = m_viewModel->Rows();
    if (rowIndex < 0 || rowIndex >= rows.size()
        || rowIndex >= m_rowEditors.size()) {
        return;
    }
    const ExtendedTuningRow &row = rows.at(rowIndex);
    RowEditors &editors = m_rowEditors[rowIndex];
    bool ok = false;
    quint64 value = row.field.value.toULongLong(&ok);
    if (!ok) {
        value = 0;
    }
    quint64 knownMask = 0;
    quint64 selected = 0;
    for (QAction *action : editors.bitActions) {
        const quint64 mask = bitMask(action->property("bit").toInt());
        knownMask |= mask;
        if (action->isChecked()) {
            selected |= mask;
        }
    }
    value = (value & ~knownMask) | selected;
    if (!m_viewModel->stageValue(
            rowIndex, QVariant::fromValue<qulonglong>(value))) {
        syncRow(rowIndex);
    }
}

void ConfigExtendedTuningView::confirmLargeIncrease(
    const QStringList &parameterNames)
{
    if (parameterNames.isEmpty()) {
        return;
    }
    QMessageBox warning(QMessageBox::Warning, tr("Large Value"),
                        tr("The following parameters are more than double their "
                           "accepted values:\n\n%1\n\nWrite them anyway?")
                            .arg(parameterNames.join(QStringLiteral(", "))),
                        QMessageBox::Save | QMessageBox::Cancel, this);
    warning.setObjectName(QStringLiteral("extendedTuningLargeValueWarning"));
    warning.setDefaultButton(QMessageBox::Cancel);
    warning.setEscapeButton(QMessageBox::Cancel);
    if (warning.exec() == QMessageBox::Save) {
        m_viewModel->Save(true);
    }
}

int ConfigExtendedTuningView::optionIndex(
    const QComboBox *combo, const QVariant &value)
{
    if (!combo) {
        return -1;
    }
    for (int index = 0; index < combo->count(); ++index) {
        if (valuesEqual(combo->itemData(index), value)) {
            return index;
        }
    }
    return -1;
}

int ConfigExtendedTuningView::decimalsFor(const ParamField &field)
{
    double step = std::abs(field.increment);
    if (!(step > 0.0) || !std::isfinite(step)) {
        return 6;
    }
    for (int decimals = 0; decimals < 6; ++decimals) {
        if (step >= 1.0
            || std::abs(step - std::round(step)) < 1.0e-9) {
            return decimals;
        }
        step *= 10.0;
    }
    return 6;
}

QString ConfigExtendedTuningView::bitmaskSummary(
    const ExtendedTuningRow &row)
{
    bool ok = false;
    const quint64 value = row.field.value.toULongLong(&ok);
    if (!ok || value == 0) {
        return tr("(none)");
    }
    int count = 0;
    QStringList labels;
    quint64 knownMask = 0;
    for (const BitOption &option : row.field.bitOptions) {
        const quint64 mask = bitMask(option.bit);
        knownMask |= mask;
        if (mask != 0 && (value & mask) != 0) {
            ++count;
            labels.append(QStringLiteral("%1: %2")
                              .arg(option.bit).arg(option.label));
        }
    }
    const quint64 unknown = value & ~knownMask;
    if (unknown != 0) {
        ++count;
        labels.append(tr("Unknown bits: 0x%1")
                          .arg(QString::number(unknown, 16).toUpper()));
    }
    return count <= 2 ? labels.join(QStringLiteral(", "))
                      : tr("%1 bits set").arg(count);
}

QString ConfigExtendedTuningView::fieldToolTip(
    const ExtendedTuningRow &row)
{
    const QString name = row.resolvedName.isEmpty()
        ? row.candidates.join(QStringLiteral(" / ")) : row.resolvedName;
    if (row.field.description.trimmed().isEmpty()) {
        return name;
    }
    return name + QStringLiteral(":\n") + row.field.description.trimmed();
}

QString ConfigExtendedTuningView::groupObjectSuffix(const QString &title)
{
    QString result;
    bool upper = true;
    for (const QChar character : title) {
        if (!character.isLetterOrNumber()) {
            upper = true;
            continue;
        }
        result.append(upper ? character.toUpper() : character);
        upper = false;
    }
    return result;
}
