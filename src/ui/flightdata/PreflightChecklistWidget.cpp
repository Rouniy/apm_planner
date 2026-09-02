#include "PreflightChecklistWidget.h"

#include "PreflightChecklistModel.h"

#include <QCheckBox>
#include <QColor>
#include <QGridLayout>
#include <QLabel>
#include <QPalette>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

PreflightChecklistWidget::PreflightChecklistWidget(
    PreflightChecklistModel *model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
{
    setObjectName(QStringLiteral("PreFlight"));
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setObjectName(QStringLiteral("PreFlightScrollArea"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    rootLayout->addWidget(scrollArea);

    auto *contents = new QWidget(scrollArea);
    contents->setObjectName(QStringLiteral("PreflightChecks"));
    auto *contentsLayout = new QVBoxLayout(contents);
    contentsLayout->setContentsMargins(6, 6, 6, 6);
    contentsLayout->setSpacing(4);
    scrollArea->setWidget(contents);

    if (m_model) {
        buildRows(contents);
        for (RowWidgets &row : m_rows) {
            contentsLayout->addWidget(row.container);
        }
        contentsLayout->addStretch(1);
        connect(m_model, &QAbstractItemModel::dataChanged,
                this, &PreflightChecklistWidget::syncRows);
        if (!m_rows.isEmpty()) {
            syncRows(m_model->index(0, 0),
                     m_model->index(m_rows.size() - 1, 0));
        }
    }
}

PreflightChecklistModel *PreflightChecklistWidget::model() const
{
    return m_model;
}

void PreflightChecklistWidget::buildRows(QWidget *rowParent)
{
    m_rows.reserve(m_model->rowCount());
    for (int row = 0; row < m_model->rowCount(); ++row) {
        RowWidgets widgets;
        widgets.container = new QWidget(rowParent);
        widgets.container->setObjectName(
            QStringLiteral("PreflightCheckRow_%1")
                .arg(m_model->data(m_model->index(row, 0),
                                   PreflightChecklistModel::IdRole)
                         .toString()));
        widgets.container->setAutoFillBackground(true);
        auto *layout = new QGridLayout(widgets.container);
        layout->setContentsMargins(6, 3, 6, 3);
        layout->setHorizontalSpacing(8);
        layout->setColumnStretch(0, 1);

        widgets.description = new QLabel(widgets.container);
        widgets.description->setObjectName(QStringLiteral("Description"));
        widgets.value = new QLabel(widgets.container);
        widgets.value->setObjectName(QStringLiteral("Value"));
        widgets.value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        widgets.checkBox = new QCheckBox(widgets.container);
        widgets.checkBox->setObjectName(QStringLiteral("Ok"));
        widgets.checkBox->setAccessibleName(tr("Preflight check completed"));

        layout->addWidget(widgets.description, 0, 0);
        layout->addWidget(widgets.value, 0, 1);
        layout->addWidget(widgets.checkBox, 0, 2);
        connect(widgets.checkBox, &QCheckBox::toggled,
                this, [this, row](bool checked) {
                    if (m_model
                        && !m_model->setManualState(row, checked)) {
                        syncRow(row);
                    }
                });
        m_rows.append(widgets);
    }
}

void PreflightChecklistWidget::syncRows(const QModelIndex &topLeft,
                                        const QModelIndex &bottomRight)
{
    if (!m_model || !topLeft.isValid() || !bottomRight.isValid()) {
        return;
    }
    const int first = qMax(0, topLeft.row());
    const int last = qMin(m_rows.size() - 1, bottomRight.row());
    for (int row = first; row <= last; ++row) {
        syncRow(row);
    }
}

void PreflightChecklistWidget::syncRow(int row)
{
    if (!m_model || row < 0 || row >= m_rows.size()) {
        return;
    }
    const QModelIndex index = m_model->index(row, 0);
    RowWidgets &widgets = m_rows[row];
    widgets.description->setText(
        m_model->data(index, PreflightChecklistModel::DescriptionRole)
            .toString());
    widgets.value->setText(
        m_model->data(index, PreflightChecklistModel::ValueRole).toString());

    const bool manual =
        m_model->data(index, PreflightChecklistModel::ManualRole).toBool();
    const bool satisfied =
        m_model->data(index, PreflightChecklistModel::SatisfiedRole).toBool();
    {
        const QSignalBlocker blocker(widgets.checkBox);
        widgets.checkBox->setChecked(satisfied);
    }
    widgets.checkBox->setAttribute(Qt::WA_TransparentForMouseEvents, !manual);
    widgets.checkBox->setFocusPolicy(manual ? Qt::StrongFocus : Qt::NoFocus);
    widgets.checkBox->setToolTip(
        manual ? tr("Mark this manual preflight check complete.")
               : tr("This check follows live vehicle telemetry."));

    const QColor foreground(
        m_model->data(index, PreflightChecklistModel::ForegroundRole)
            .toString());
    for (QLabel *label : {widgets.description, widgets.value}) {
        QPalette palette = label->palette();
        palette.setColor(QPalette::WindowText, foreground);
        label->setPalette(palette);
    }
}
