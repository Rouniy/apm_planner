#include "ConfigCompassView.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
const QString kCalibrationUnavailable = QStringLiteral(
    "Not yet ported: exact-target compass calibration is unavailable.");

void clearLayout(QLayout *layout) {
  while (QLayoutItem *item = layout->takeAt(0)) {
    if (QWidget *widget = item->widget()) {
      // Replacing a focused spin box must not turn its editingFinished signal
      // into a parameter write while the model is rebuilding presentation.
      widget->blockSignals(true);
      delete widget;
    }
    if (QLayout *child = item->layout()) {
      clearLayout(child);
    }
    delete item;
  }
}

const ParamField *fieldNamed(const QList<ParamField> &fields,
                             const QString &name) {
  const auto found = std::find_if(
      fields.cbegin(), fields.cend(), [&name](const ParamField &field) {
        return field.name.compare(name, Qt::CaseInsensitive) == 0;
      });
  return found == fields.cend() ? nullptr : &*found;
}
} // namespace

ConfigCompassView::ConfigCompassView(QWidget *parent)
    : QWidget(parent), m_viewModel(new ConfigCompassViewModel(this)) {
  setObjectName(QStringLiteral("ConfigCompassView"));
  buildUi();

  connect(m_viewModel, &ConfigCompassViewModel::fieldsChanged, this,
          &ConfigCompassView::rebuildFields);
  connect(m_viewModel, &ConfigCompassViewModel::stateChanged, this, [this]() {
    syncFlags();
    syncFieldValues();
    syncState();
  });
  connect(m_viewModel, &ConfigCompassViewModel::rowsChanged, this,
          [this](int) { updateMoveButtons(); });
  connect(m_viewModel, &ConfigCompassViewModel::writeRequested, this,
          &ConfigCompassView::writeRequested);
  connect(m_viewModel, &ConfigCompassViewModel::refreshRequested, this,
          &ConfigCompassView::refreshRequested);
  connect(m_viewModel, &ConfigCompassViewModel::rebootRequested, this,
          &ConfigCompassView::rebootRequested);

  rebuildFields();
  syncFlags();
  syncState();
}

ConfigCompassView::~ConfigCompassView() = default;

QSize ConfigCompassView::sizeHint() const { return QSize(980, 760); }

void ConfigCompassView::setCatalog(const ParameterMetaDataCatalog &catalog,
                                   bool enforceRanges) {
  m_viewModel->setCatalog(catalog, enforceRanges);
}

void ConfigCompassView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent, bool completeSnapshot) {
  m_viewModel->setParameterSnapshot(parameters, preferredComponent,
                                    completeSnapshot);
}

void ConfigCompassView::setConnected(bool connected) {
  m_viewModel->setConnected(connected);
}

void ConfigCompassView::setArmed(bool armed) { m_viewModel->setArmed(armed); }

void ConfigCompassView::parameterChanged(int componentId, const QString &name,
                                         const QVariant &value) {
  m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigCompassView::parameterWriteSubmitted(quint64 requestId,
                                                qulonglong batchId) {
  m_viewModel->parameterWriteSubmitted(requestId, batchId);
}

void ConfigCompassView::parameterWriteSubmissionFailed(quint64 requestId,
                                                       const QString &reason) {
  m_viewModel->parameterWriteSubmissionFailed(requestId, reason);
}

void ConfigCompassView::parameterWriteFailed(qulonglong batchId,
                                             int componentId,
                                             const QString &name,
                                             const QString &reason) {
  m_viewModel->parameterWriteFailed(batchId, componentId, name, reason);
}

void ConfigCompassView::parameterWriteCancelled(qulonglong batchId,
                                                int componentId,
                                                const QString &name) {
  m_viewModel->parameterWriteCancelled(batchId, componentId, name);
}

void ConfigCompassView::parameterBatchCompleted(qulonglong batchId,
                                                int succeeded, int failed) {
  m_viewModel->parameterBatchCompleted(batchId, succeeded, failed);
}

void ConfigCompassView::refreshFailed(const QString &reason) {
  m_viewModel->refreshFailed(reason);
}

void ConfigCompassView::refreshCanceled() { m_viewModel->refreshCanceled(); }

void ConfigCompassView::rebootSubmitted(quint64 requestId) {
  m_viewModel->rebootSubmitted(requestId);
}

void ConfigCompassView::rebootSubmissionFailed(quint64 requestId,
                                               const QString &reason) {
  m_viewModel->rebootSubmissionFailed(requestId, reason);
}

void ConfigCompassView::rebootAcknowledged(quint64 requestId, bool accepted,
                                           const QString &reason) {
  m_viewModel->rebootAcknowledged(requestId, accepted, reason);
}

void ConfigCompassView::rebootCancelled(quint64 requestId,
                                        const QString &reason) {
  m_viewModel->rebootCancelled(requestId, reason);
}

void ConfigCompassView::buildUi() {
  setStyleSheet(QStringLiteral(
      "ConfigCompassView { background: #1A201D; color: #E6EDE9; }"
      "QLabel#compassTitle { color: #E8E8E8; font-size: 16px;"
      " font-weight: bold; }"
      "QLabel#compassStatus, QLabel#compassCompassStatus { color: #E0A030; }"
      "QGroupBox { border: 1px solid #56615B; border-radius: 4px;"
      " margin-top: 9px; padding-top: 9px; font-weight: bold; }"
      "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 "
      "4px; }"
      "QPushButton, QComboBox, QDoubleSpinBox { background: #161B18;"
      " color: #E6EDE9; border: 1px solid #303A35; padding: 5px; }"
      "QPushButton:disabled, QComboBox:disabled, QDoubleSpinBox:disabled,"
      " QCheckBox:disabled { color: #68736D; }"));

  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  auto *scroll = new QScrollArea(this);
  scroll->setObjectName(QStringLiteral("compassScroll"));
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *content = new QWidget(scroll);
  content->setObjectName(QStringLiteral("compassContent"));
  auto *layout = new QVBoxLayout(content);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(8);

  auto *title = new QLabel(tr("Compass"), content);
  title->setObjectName(QStringLiteral("compassTitle"));
  layout->addWidget(title);

  auto *instruction = new QLabel(
      tr("Set the Compass Priority by reordering the compasses in the table "
         "below (Highest at the top)"),
      content);
  instruction->setObjectName(QStringLiteral("compassPriorityInstruction"));
  instruction->setWordWrap(true);
  layout->addWidget(instruction);

  m_table = new QTableView(content);
  m_table->setObjectName(QStringLiteral("compassTable"));
  m_table->setModel(m_viewModel);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSelectionMode(QAbstractItemView::SingleSelection);
  m_table->setShowGrid(true);
  m_table->verticalHeader()->setVisible(false);
  m_table->horizontalHeader()->setSectionResizeMode(
      QHeaderView::ResizeToContents);
  m_table->horizontalHeader()->setSectionResizeMode(
      ConfigCompassViewModel::DevTypeColumn, QHeaderView::Stretch);
  m_table->setMinimumHeight(150);
  m_table->setMaximumHeight(240);
  layout->addWidget(m_table);

  auto *moves = new QHBoxLayout;
  m_moveUp = new QPushButton(tr("Move selected up"), content);
  m_moveUp->setObjectName(QStringLiteral("compassMoveUp"));
  m_moveDown = new QPushButton(tr("Move selected down"), content);
  m_moveDown->setObjectName(QStringLiteral("compassMoveDown"));
  moves->addWidget(m_moveUp);
  moves->addWidget(m_moveDown);
  moves->addStretch(1);
  layout->addLayout(moves);

  auto *useLabel = new QLabel(
      tr("Do you want to disable any of the first 3 compasses?"), content);
  useLabel->setObjectName(QStringLiteral("compassUseInstruction"));
  layout->addWidget(useLabel);
  auto *flags = new QHBoxLayout;
  for (int slot = 1; slot <= 3; ++slot) {
    m_use[slot - 1] = new QCheckBox(tr("Use Compass %1").arg(slot), content);
    m_use[slot - 1]->setObjectName(QStringLiteral("compassUse%1").arg(slot));
    flags->addWidget(m_use[slot - 1]);
  }
  m_removeMissing = new QPushButton(tr("Remove Missing"), content);
  m_removeMissing->setObjectName(QStringLiteral("compassRemoveMissing"));
  flags->addWidget(m_removeMissing);
  m_learn = new QCheckBox(tr("Automatically learn offsets"), content);
  m_learn->setObjectName(QStringLiteral("compassLearn"));
  flags->addWidget(m_learn);
  flags->addStretch(1);
  layout->addLayout(flags);

  auto *rebootNote =
      new QLabel(tr("A reboot is required to adjust the ordering."), content);
  rebootNote->setObjectName(QStringLiteral("compassRebootNote"));
  layout->addWidget(rebootNote);
  m_reboot = new QPushButton(tr("Reboot"), content);
  m_reboot->setObjectName(QStringLiteral("compassReboot"));
  m_reboot->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  layout->addWidget(m_reboot, 0, Qt::AlignLeft);

  auto *calibrationNote = new QLabel(
      tr("A mag calibration is required to remap the above changes."), content);
  calibrationNote->setObjectName(QStringLiteral("compassCalibrationNote"));
  layout->addWidget(calibrationNote);
  m_compassStatus = new QLabel(content);
  m_compassStatus->setObjectName(QStringLiteral("compassCompassStatus"));
  m_compassStatus->setWordWrap(true);
  layout->addWidget(m_compassStatus);

  auto *calibration = new QGroupBox(tr("Onboard Mag Calibration"), content);
  calibration->setObjectName(QStringLiteral("compassCalibrationGroup"));
  auto *calibrationLayout = new QVBoxLayout(calibration);
  auto *unavailable = new QLabel(
      tr("Not yet ported: exact-target compass calibration is unavailable."),
      calibration);
  unavailable->setObjectName(QStringLiteral("compassCalibrationUnavailable"));
  unavailable->setWordWrap(true);
  calibrationLayout->addWidget(unavailable);
  auto *calibrationActions = new QHBoxLayout;
  const QList<QPair<QString, QString>> calibrationButtons{
      {QStringLiteral("compassCalStart"), tr("Start")},
      {QStringLiteral("compassCalAccept"), tr("Accept")},
      {QStringLiteral("compassCalCancel"), tr("Cancel")},
      {QStringLiteral("compassCalFromLog"), tr("Calibrate from Log…")}};
  for (const auto &definition : calibrationButtons) {
    auto *button = new QPushButton(definition.second, calibration);
    button->setObjectName(definition.first);
    button->setEnabled(false);
    button->setToolTip(kCalibrationUnavailable);
    calibrationActions->addWidget(button);
  }
  calibrationActions->addStretch(1);
  calibrationLayout->addLayout(calibrationActions);
  auto *progressGrid = new QGridLayout;
  for (int index = 0; index < 3; ++index) {
    progressGrid->addWidget(
        new QLabel(tr("Mag %1").arg(index + 1), calibration), index, 0);
    auto *progress = new QProgressBar(calibration);
    progress->setObjectName(
        QStringLiteral("compassCalProgress%1").arg(index + 1));
    progress->setRange(0, 100);
    progress->setValue(0);
    progress->setEnabled(false);
    progress->setToolTip(kCalibrationUnavailable);
    progressGrid->addWidget(progress, index, 1);
  }
  calibrationLayout->addLayout(progressGrid);
  layout->addWidget(calibration);

  auto *largeVehicle = new QPushButton(tr("Large Vehicle MagCal"), content);
  largeVehicle->setObjectName(QStringLiteral("compassLargeVehicleMagCal"));
  largeVehicle->setEnabled(false);
  largeVehicle->setToolTip(kCalibrationUnavailable);
  largeVehicle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  layout->addWidget(largeVehicle, 0, Qt::AlignLeft);

  auto *advanced = new QGroupBox(tr("Advanced compass settings"), content);
  advanced->setObjectName(QStringLiteral("compassAdvancedGroup"));
  auto *advancedLayout = new QVBoxLayout(advanced);
  m_fieldLayout = new QVBoxLayout;
  advancedLayout->addLayout(m_fieldLayout);

  auto *declinationRow = new QHBoxLayout;
  auto *declinationLabel = new QLabel(tr("Declination (deg)"), advanced);
  declinationLabel->setObjectName(QStringLiteral("compassDeclinationLabel"));
  declinationLabel->setMinimumWidth(230);
  declinationRow->addWidget(declinationLabel);
  m_declination = new QDoubleSpinBox(advanced);
  m_declination->setObjectName(QStringLiteral("compassDeclination"));
  m_declination->setDecimals(3);
  m_declination->setSingleStep(0.1);
  m_declination->setRange(-180.0, 180.0);
  m_declination->setMinimumWidth(180);
  declinationRow->addWidget(m_declination);
  m_writeDeclination = new QPushButton(tr("Write Declination"), advanced);
  m_writeDeclination->setObjectName(QStringLiteral("compassWriteDeclination"));
  declinationRow->addWidget(m_writeDeclination);
  declinationRow->addStretch(1);
  advancedLayout->addLayout(declinationRow);

  m_quickPixhawk = new QPushButton(tr("Pixhawk/Cube (onboard)"), advanced);
  m_quickPixhawk->setObjectName(QStringLiteral("compassQuickPixhawk"));
  m_quickPixhawk->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  advancedLayout->addWidget(m_quickPixhawk, 0, Qt::AlignLeft);
  layout->addWidget(advanced);

  m_refresh = new QPushButton(tr("Refresh Params"), content);
  m_refresh->setObjectName(QStringLiteral("compassRefresh"));
  m_refresh->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  layout->addWidget(m_refresh, 0, Qt::AlignLeft);
  m_status = new QLabel(content);
  m_status->setObjectName(QStringLiteral("compassStatus"));
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  layout->addStretch(1);

  scroll->setWidget(content);
  root->addWidget(scroll);

  connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged,
          this, [this]() { updateMoveButtons(); });
  connect(m_moveUp, &QPushButton::clicked, this, [this]() {
    const QModelIndex current = m_table->currentIndex();
    if (!current.isValid() || !m_viewModel->moveUp(current.row())) {
      updateMoveButtons();
    }
  });
  connect(m_moveDown, &QPushButton::clicked, this, [this]() {
    const QModelIndex current = m_table->currentIndex();
    if (!current.isValid() || !m_viewModel->moveDown(current.row())) {
      updateMoveButtons();
    }
  });
  for (int slot = 1; slot <= 3; ++slot) {
    connect(m_use[slot - 1], &QCheckBox::toggled, this,
            [this, slot](bool checked) {
              if (!m_viewModel->setUseCompass(slot, checked)) {
                syncFlags();
              }
            });
  }
  connect(m_removeMissing, &QPushButton::clicked, m_viewModel,
          &ConfigCompassViewModel::removeMissing);
  connect(m_learn, &QCheckBox::toggled, this, [this](bool checked) {
    if (!m_viewModel->setLearnOffsets(checked)) {
      syncFlags();
    }
  });
  connect(m_reboot, &QPushButton::clicked, this,
          &ConfigCompassView::confirmAndRequestReboot);
  connect(m_writeDeclination, &QPushButton::clicked, this, [this]() {
    if (!m_viewModel->writeDeclinationDegrees(m_declination->value())) {
      syncFieldValues();
      syncState();
    }
  });
  connect(m_quickPixhawk, &QPushButton::clicked, m_viewModel,
          &ConfigCompassViewModel::quickPixhawk);
  connect(m_refresh, &QPushButton::clicked, m_viewModel,
          &ConfigCompassViewModel::Refresh);
}

void ConfigCompassView::rebuildFields() {
  clearLayout(m_fieldLayout);
  m_fieldEditors.clear();
  m_fieldStatuses.clear();

  const QList<ParamField> fields = m_viewModel->Fields();
  const QStringList names = ConfigCompassViewModel::AdvancedFieldNames();
  for (const QString &name : names) {
    ParamField fallback;
    fallback.name = name;
    fallback.label = name;
    fallback.status = tr("n/a");
    fallback.readOnly = true;
    const ParamField *const present = fieldNamed(fields, name);
    const ParamField &field = present ? *present : fallback;

    auto *rowWidget = new QWidget(this);
    rowWidget->setObjectName(QStringLiteral("compassField_%1").arg(name));
    auto *row = new QGridLayout(rowWidget);
    row->setContentsMargins(0, 2, 0, 2);
    row->setColumnMinimumWidth(0, 230);
    auto *label =
        new QLabel(field.label.isEmpty() ? name : field.label, rowWidget);
    label->setObjectName(QStringLiteral("compassFieldLabel_%1").arg(name));
    label->setWordWrap(true);
    label->setToolTip(field.description);
    row->addWidget(label, 0, 0);

    QWidget *editor = nullptr;
    if (name == QLatin1String("COMPASS_AUTODEC")) {
      auto *check = new QCheckBox(rowWidget);
      editor = check;
      connect(check, &QCheckBox::toggled, this, [this, name](bool checked) {
        if (!m_viewModel->setFieldValue(name, checked ? 1 : 0)) {
          syncFieldValues();
        }
      });
    } else if (field.editorKind == ParamField::EditorKind::Combo &&
               !field.options.isEmpty()) {
      auto *combo = new QComboBox(rowWidget);
      for (const ParamOption &option : field.options) {
        combo->addItem(option.text, option.value);
      }
      editor = combo;
      connect(combo, QOverload<int>::of(&QComboBox::activated), this,
              [this, name, combo](int index) {
                if (index < 0 ||
                    !m_viewModel->setFieldValue(name, combo->itemData(index))) {
                  syncFieldValues();
                }
              });
    } else {
      auto *spin = new QDoubleSpinBox(rowWidget);
      spin->setDecimals(6);
      spin->setRange(
          field.hasRange && field.enforceRange ? field.minimum : -1.0e9,
          field.hasRange && field.enforceRange ? field.maximum : 1.0e9);
      spin->setSingleStep(field.increment > 0.0 ? field.increment : 0.01);
      editor = spin;
      connect(spin, &QDoubleSpinBox::editingFinished, this,
              [this, name, spin]() {
                if (!m_viewModel->setFieldValue(name, spin->value())) {
                  syncFieldValues();
                }
              });
    }
    editor->setObjectName(QStringLiteral("compassEditor_%1").arg(name));
    editor->setToolTip(field.description);
    editor->setMinimumWidth(180);
    row->addWidget(editor, 0, 1);

    auto *units = new QLabel(field.units, rowWidget);
    units->setObjectName(QStringLiteral("compassFieldUnits_%1").arg(name));
    row->addWidget(units, 0, 2);
    auto *fieldStatus = new QLabel(field.status, rowWidget);
    fieldStatus->setObjectName(
        QStringLiteral("compassFieldStatus_%1").arg(name));
    row->addWidget(fieldStatus, 0, 3);
    row->setColumnStretch(3, 1);

    m_fieldEditors.insert(name, editor);
    m_fieldStatuses.insert(name, fieldStatus);
    m_fieldLayout->addWidget(rowWidget);
  }
  syncFieldValues();
  syncState();
}

void ConfigCompassView::syncFieldValues() {
  const QList<ParamField> fields = m_viewModel->Fields();
  for (const QString &name : ConfigCompassViewModel::AdvancedFieldNames()) {
    const ParamField *const field = fieldNamed(fields, name);
    QWidget *const editor = m_fieldEditors.value(name);
    QLabel *const status = m_fieldStatuses.value(name);
    if (!editor) {
      continue;
    }
    if (status) {
      status->setText(field ? field->status : tr("n/a"));
    }
    if (!field) {
      continue;
    }
    const QSignalBlocker blocker(editor);
    if (auto *check = qobject_cast<QCheckBox *>(editor)) {
      check->setChecked(field->value.toDouble() != 0.0);
    } else if (auto *combo = qobject_cast<QComboBox *>(editor)) {
      int selected = -1;
      for (int index = 0; index < combo->count(); ++index) {
        if (valuesEqual(combo->itemData(index), field->value)) {
          selected = index;
          break;
        }
      }
      combo->setCurrentIndex(selected);
    } else if (auto *spin = qobject_cast<QDoubleSpinBox *>(editor)) {
      spin->setValue(field->value.toDouble());
    }
  }

  {
    const QSignalBlocker blocker(m_declination);
    m_declination->setValue(m_viewModel->DeclinationDegrees());
  }
}

void ConfigCompassView::syncFlags() {
  for (int slot = 1; slot <= 3; ++slot) {
    const QSignalBlocker blocker(m_use[slot - 1]);
    m_use[slot - 1]->setChecked(m_viewModel->UseCompass(slot));
  }
  const QSignalBlocker blocker(m_learn);
  m_learn->setChecked(m_viewModel->LearnOffsets());
}

void ConfigCompassView::syncState() {
  const bool ready =
      m_viewModel->Connected() && m_viewModel->SnapshotComplete() &&
      m_viewModel->SnapshotReady() && !m_viewModel->ReconciliationRequired() &&
      !m_viewModel->Busy();
  const bool editable = ready && !m_viewModel->Armed();

  m_table->setEnabled(editable);
  for (int slot = 1; slot <= 3; ++slot) {
    m_use[slot - 1]->setEnabled(editable && m_viewModel->HasUseCompass(slot));
  }
  m_learn->setEnabled(editable && m_viewModel->HasLearn());
  const QVector<CompassPriorityRow> rows = m_viewModel->Rows();
  m_removeMissing->setEnabled(
      editable &&
      std::any_of(rows.cbegin(), rows.cend(),
                  [](const CompassPriorityRow &row) { return row.missing; }));

  const QList<ParamField> fields = m_viewModel->Fields();
  for (const QString &name : ConfigCompassViewModel::AdvancedFieldNames()) {
    const ParamField *const field = fieldNamed(fields, name);
    if (QWidget *editor = m_fieldEditors.value(name)) {
      editor->setEnabled(editable && field && !field->readOnly);
    }
  }
  m_declination->setEnabled(editable && m_viewModel->HasDeclination());
  m_writeDeclination->setEnabled(editable && m_viewModel->HasDeclination());
  const ParamField *const quickExternal =
      fieldNamed(fields, QStringLiteral("COMPASS_EXTERNAL"));
  const ParamField *const quickOrientation =
      fieldNamed(fields, QStringLiteral("COMPASS_ORIENT"));
  const bool quickAvailable = quickExternal && !quickExternal->readOnly &&
                              quickOrientation && !quickOrientation->readOnly;
  m_quickPixhawk->setEnabled(editable && quickAvailable);
  m_quickPixhawk->setToolTip(
      quickAvailable
          ? QString()
          : tr("COMPASS_EXTERNAL and COMPASS_ORIENT are unavailable on this "
               "vehicle."));
  m_reboot->setEnabled(ready && !m_viewModel->Armed() &&
                       !m_viewModel->RebootOutcomeUncertain());
  m_refresh->setEnabled(m_viewModel->Connected() && !m_viewModel->Busy());
  m_compassStatus->setText(m_viewModel->CompassStatus());
  m_compassStatus->setVisible(!m_viewModel->CompassStatus().isEmpty());
  m_status->setText(m_viewModel->Status());
  updateMoveButtons();
}

void ConfigCompassView::updateMoveButtons() {
  const QModelIndex current = m_table->currentIndex();
  const bool editable = m_table->isEnabled() && current.isValid();
  m_moveUp->setEnabled(editable && current.row() > 0);
  m_moveDown->setEnabled(editable &&
                         current.row() + 1 < m_viewModel->rowCount());
}

void ConfigCompassView::confirmAndRequestReboot() {
  QMessageBox confirm(QMessageBox::Question, tr("Reboot"),
                      tr("Reboot the selected vehicle now?"),
                      QMessageBox::Yes | QMessageBox::Cancel, this);
  confirm.setObjectName(QStringLiteral("compassRebootConfirmation"));
  confirm.setDefaultButton(QMessageBox::Cancel);
  confirm.setEscapeButton(QMessageBox::Cancel);
  if (confirm.exec() == QMessageBox::Yes) {
    m_viewModel->Reboot();
  }
}

bool ConfigCompassView::valuesEqual(const QVariant &left,
                                    const QVariant &right) {
  if (!left.isValid() || !right.isValid()) {
    return !left.isValid() && !right.isValid();
  }
  bool leftOk = false;
  bool rightOk = false;
  const double leftValue = left.toDouble(&leftOk);
  const double rightValue = right.toDouble(&rightOk);
  return leftOk && rightOk ? std::abs(leftValue - rightValue) <= 1.0e-6
                           : left == right;
}
