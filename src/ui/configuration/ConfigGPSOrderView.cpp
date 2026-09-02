#include "ConfigGPSOrderView.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cmath>

namespace {
class GPSOrderWheelFilter final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        Q_UNUSED(object)
        if (event->type() == QEvent::Wheel) {
            event->ignore();
            return true;
        }
        return false;
    }
};

class GPSOrderNumericEditor final : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;

protected:
    QString textFromValue(double value) const override
    {
        QString text = locale().toString(value, 'f', 6);
        const auto decimal = locale().decimalPoint();
        while (text.contains(decimal) && text.endsWith(QLatin1Char('0'))) {
            text.chop(1);
        }
        if (text.endsWith(decimal)) {
            text.chop(1);
        }
        return text;
    }
};

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

int optionIndex(const QList<ParamOption> &options, const QVariant &value)
{
    for (int index = 0; index < options.size(); ++index) {
        if (variantsEqual(options.at(index).value, value)) {
            return index;
        }
    }
    return -1;
}
} // namespace

struct ConfigGPSOrderView::FieldWidgets
{
    QWidget *row = nullptr;
    QLabel *label = nullptr;
    QComboBox *combo = nullptr;
    QDoubleSpinBox *numeric = nullptr;
    QLabel *units = nullptr;
    QLabel *status = nullptr;
};

ConfigGPSOrderView::ConfigGPSOrderView(
    const ParameterMetaDataCatalog &catalog, QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigGPSOrderViewModel(this)),
      m_wheelFilter(new GPSOrderWheelFilter(this))
{
    setObjectName(QStringLiteral("ConfigGPSOrderView"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(true);
    QPalette opaquePalette = palette();
    opaquePalette.setColor(QPalette::Window, QColor(QStringLiteral("#1A201D")));
    opaquePalette.setColor(QPalette::WindowText,
                           QColor(QStringLiteral("#E6EDE9")));
    setPalette(opaquePalette);
    setStyleSheet(QStringLiteral(
        "ConfigGPSOrderView, QWidget#gpsOrderFields {"
        " background: #1A201D; color: #E6EDE9; }"
        "QLabel#gpsOrderTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#gpsOrderIntro, QLabel[gpsOrderFieldLabel=\"true\"] {"
        " color: #C8C8C8; }"
        "QTableWidget#gpsOrderTable { background: #0D1210;"
        " alternate-background-color: #131916; color: #E6EDE9;"
        " gridline-color: #2A322D; }"
        "QTableWidget#gpsOrderTable QHeaderView::section {"
        " background: #202623; color: #E6EDE9;"
        " border: 1px solid #2A322D; padding: 5px; }"
        "QComboBox, QDoubleSpinBox, QPushButton { background: #161B18;"
        " color: #E6EDE9; border: 1px solid #2A322D; padding: 4px; }"
        "QComboBox:disabled, QDoubleSpinBox:disabled, QPushButton:disabled {"
        " color: #68736D; }"
        "QLabel#gpsOrderStatus, QLabel[gpsOrderFieldStatus=\"true\"] {"
        " color: #34D399; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);

    auto *title = new QLabel(m_viewModel->Title(), this);
    title->setObjectName(QStringLiteral("gpsOrderTitle"));
    root->addWidget(title);

    auto *intro = new QLabel(m_viewModel->Intro(), this);
    intro->setObjectName(QStringLiteral("gpsOrderIntro"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    m_refreshButton = new QPushButton(tr("Refresh Params"), this);
    m_refreshButton->setObjectName(QStringLiteral("gpsOrderRefreshButton"));
    m_refreshButton->setSizePolicy(QSizePolicy::Maximum,
                                   QSizePolicy::Preferred);
    root->addWidget(m_refreshButton, 0, Qt::AlignLeft);

    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("gpsOrderTable"));
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({
        tr("Order"), tr("NodeID"), tr("Name"),
        tr("GPS1"), tr("GPS2")
    });
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(true);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(
        4, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 80);
    m_table->setColumnWidth(1, 120);
    m_table->setColumnWidth(2, 200);
    m_table->setMinimumHeight(190);
    root->addWidget(m_table, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("gpsOrderStatus"));
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    m_fieldsWidget = new QWidget(this);
    m_fieldsWidget->setObjectName(QStringLiteral("gpsOrderFields"));
    m_fieldsWidget->setAttribute(Qt::WA_OpaquePaintEvent);
    m_fieldsWidget->setAutoFillBackground(true);
    m_fieldsWidget->setPalette(opaquePalette);
    m_fieldsLayout = new QVBoxLayout(m_fieldsWidget);
    m_fieldsLayout->setContentsMargins(0, 0, 0, 0);
    m_fieldsLayout->setSpacing(0);
    m_fieldsLayout->addStretch(1);
    root->addWidget(m_fieldsWidget);

    connect(m_viewModel, &ConfigGPSOrderViewModel::rowsChanged,
            this, &ConfigGPSOrderView::rebuildRows);
    connect(m_viewModel, &ConfigGPSOrderViewModel::structureChanged,
            this, &ConfigGPSOrderView::rebuildFields);
    connect(m_viewModel, &ConfigGPSOrderViewModel::fieldChanged,
            this, &ConfigGPSOrderView::syncField);
    connect(m_viewModel, &ConfigGPSOrderViewModel::statusChanged,
            this, &ConfigGPSOrderView::syncState);
    connect(m_viewModel, &ConfigGPSOrderViewModel::stateChanged,
            this, &ConfigGPSOrderView::syncState);
    connect(m_viewModel, &ConfigGPSOrderViewModel::writeRequested,
            this, &ConfigGPSOrderView::writeRequested);
    connect(m_viewModel, &ConfigGPSOrderViewModel::refreshRequested,
            this, &ConfigGPSOrderView::refreshRequested);
    connect(m_refreshButton, &QPushButton::clicked,
            this, &ConfigGPSOrderView::requestRefresh);

    m_viewModel->setCatalog(catalog);
    rebuildRows();
    rebuildFields();
    syncState();
}

ConfigGPSOrderView::~ConfigGPSOrderView()
{
    clearFields();
}

QSize ConfigGPSOrderView::sizeHint() const
{
    return QSize(800, 560);
}

void ConfigGPSOrderView::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_viewModel->setCatalog(catalog);
}

void ConfigGPSOrderView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigGPSOrderView::setConnected(bool connected)
{
    m_viewModel->setConnected(connected);
}

void ConfigGPSOrderView::setArmed(bool armed)
{
    m_armed = armed;
}

void ConfigGPSOrderView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigGPSOrderView::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    m_viewModel->parameterWriteFailed(componentId, name, reason);
}

void ConfigGPSOrderView::refreshFailed(const QString &reason)
{
    m_viewModel->refreshFailed(reason);
}

void ConfigGPSOrderView::refreshCanceled()
{
    m_viewModel->refreshCanceled();
}

QString ConfigGPSOrderView::ArmedRefreshWarningTitle()
{
    return tr("Refresh Params");
}

QString ConfigGPSOrderView::ArmedRefreshWarningText()
{
    return tr("Update Params\nDON'T DO THIS IF YOU ARE IN THE AIR\n");
}

void ConfigGPSOrderView::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), palette().brush(QPalette::Window));
}

void ConfigGPSOrderView::rebuildRows()
{
    const QList<GpsCanRow> rows = m_viewModel->Rows();
    m_table->setRowCount(rows.size());
    for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const GpsCanRow row = rows.at(rowIndex);
        auto *order = new QTableWidgetItem(QString::number(row.Order));
        auto *node = new QTableWidgetItem(QString::number(row.NodeID));
        auto *name = new QTableWidgetItem(row.Name);
        order->setTextAlignment(Qt::AlignCenter);
        node->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(rowIndex, 0, order);
        m_table->setItem(rowIndex, 1, node);
        m_table->setItem(rowIndex, 2, name);

        auto *gps1 = new QPushButton(tr("GPS1"), m_table);
        gps1->setObjectName(
            QStringLiteral("gpsOrderOverride1_%1").arg(row.NodeID));
        gps1->setProperty("gpsOrderOverride", 1);
        gps1->setProperty("nodeId", row.NodeID);
        m_table->setCellWidget(rowIndex, 3, gps1);
        connect(gps1, &QPushButton::clicked, this, [this, row]() {
            if (!m_viewModel->Override1(row)) {
                syncState();
            }
        });

        auto *gps2 = new QPushButton(tr("GPS2"), m_table);
        gps2->setObjectName(
            QStringLiteral("gpsOrderOverride2_%1").arg(row.NodeID));
        gps2->setProperty("gpsOrderOverride", 2);
        gps2->setProperty("nodeId", row.NodeID);
        m_table->setCellWidget(rowIndex, 4, gps2);
        connect(gps2, &QPushButton::clicked, this, [this, row]() {
            if (!m_viewModel->Override2(row)) {
                syncState();
            }
        });
    }
    syncState();
}

void ConfigGPSOrderView::rebuildFields()
{
    clearFields();
    const QList<ParamField> fields = m_viewModel->Fields();
    for (const ParamField &field : fields) {
        auto *widgets = new FieldWidgets;
        widgets->row = new QWidget(m_fieldsWidget);
        widgets->row->setObjectName(
            QStringLiteral("gpsOrderFieldRow_%1").arg(field.name));
        widgets->row->setProperty("parameterName", field.name);
        auto *grid = new QGridLayout(widgets->row);
        grid->setContentsMargins(0, 3, 0, 3);
        grid->setHorizontalSpacing(0);
        grid->setColumnMinimumWidth(0, 220);
        grid->setColumnMinimumWidth(1, 200);
        grid->setColumnStretch(3, 1);

        widgets->label = new QLabel(field.label, widgets->row);
        widgets->label->setObjectName(
            QStringLiteral("gpsOrderFieldLabel_%1").arg(field.name));
        widgets->label->setProperty("gpsOrderFieldLabel", true);
        widgets->label->setWordWrap(true);
        widgets->label->setToolTip(field.description);
        grid->addWidget(widgets->label, 0, 0);

        if (field.editorKind == ParamField::EditorKind::Combo) {
            widgets->combo = new QComboBox(widgets->row);
            widgets->combo->setObjectName(
                QStringLiteral("gpsOrderFieldEditor_%1").arg(field.name));
            widgets->combo->setMinimumWidth(180);
            widgets->combo->installEventFilter(m_wheelFilter);
            for (const ParamOption &option : field.options) {
                widgets->combo->addItem(option.text, option.value);
            }
            grid->addWidget(widgets->combo, 0, 1, Qt::AlignLeft);
            connect(widgets->combo,
                    QOverload<int>::of(&QComboBox::activated),
                    this, [this, fieldName = field.name,
                           combo = widgets->combo](int index) {
                if (index < 0
                    || !m_viewModel->setFieldValue(
                        fieldName, combo->itemData(index))) {
                    syncField(fieldName);
                }
            });
        } else {
            widgets->numeric = new GPSOrderNumericEditor(widgets->row);
            widgets->numeric->setObjectName(
                QStringLiteral("gpsOrderFieldEditor_%1").arg(field.name));
            widgets->numeric->setDecimals(6);
            widgets->numeric->setKeyboardTracking(false);
            widgets->numeric->setMinimumWidth(180);
            widgets->numeric->installEventFilter(m_wheelFilter);
            grid->addWidget(widgets->numeric, 0, 1, Qt::AlignLeft);
            connect(widgets->numeric, &QDoubleSpinBox::editingFinished,
                    this, [this, fieldName = field.name,
                           editor = widgets->numeric]() {
                if (!m_viewModel->setFieldValue(
                        fieldName, editor->value())) {
                    syncField(fieldName);
                }
            });
        }

        widgets->units = new QLabel(field.units, widgets->row);
        widgets->units->setObjectName(
            QStringLiteral("gpsOrderFieldUnits_%1").arg(field.name));
        widgets->units->setContentsMargins(8, 0, 8, 0);
        grid->addWidget(widgets->units, 0, 2);

        widgets->status = new QLabel(field.status, widgets->row);
        widgets->status->setObjectName(
            QStringLiteral("gpsOrderFieldStatus_%1").arg(field.name));
        widgets->status->setProperty("gpsOrderFieldStatus", true);
        grid->addWidget(widgets->status, 0, 3);

        const QString normalized = field.name.trimmed().toUpper();
        m_fields.insert(normalized, widgets);
        m_fieldsLayout->insertWidget(m_fieldsLayout->count() - 1,
                                     widgets->row);
        syncField(normalized);
    }
    syncState();
}

void ConfigGPSOrderView::syncField(const QString &name)
{
    const QString normalized = name.trimmed().toUpper();
    FieldWidgets *widgets = m_fields.value(normalized);
    if (!widgets) {
        return;
    }

    ParamField field;
    bool found = false;
    for (const ParamField &candidate : m_viewModel->Fields()) {
        if (candidate.name.trimmed().toUpper() == normalized) {
            field = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }

    const bool enabled = m_viewModel->CanEdit() && !field.readOnly;
    if (widgets->combo) {
        const QSignalBlocker blocker(widgets->combo);
        widgets->combo->setCurrentIndex(
            optionIndex(field.options, field.value));
        widgets->combo->setEnabled(enabled && !field.options.isEmpty());
    }
    if (widgets->numeric) {
        const QSignalBlocker blocker(widgets->numeric);
        if (field.hasRange) {
            widgets->numeric->setRange(field.minimum, field.maximum);
        } else {
            widgets->numeric->setRange(-1000000000.0, 1000000000.0);
        }
        widgets->numeric->setSingleStep(
            field.increment > 0.0 ? field.increment : 1.0);
        widgets->numeric->setValue(field.value.toDouble());
        widgets->numeric->setEnabled(enabled);
    }
    widgets->status->setText(field.status);
}

void ConfigGPSOrderView::syncState()
{
    m_refreshButton->setEnabled(
        m_viewModel->Connected() && !m_viewModel->Busy());
    m_statusLabel->setText(m_viewModel->Status());

    const bool canOverride = m_viewModel->CanEdit();
    const QList<QPushButton *> buttons =
        m_table->findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button->property("gpsOrderOverride").isValid()) {
            button->setEnabled(canOverride);
        }
    }
    const QStringList fieldNames = m_fields.keys();
    for (const QString &name : fieldNames) {
        syncField(name);
    }
}

void ConfigGPSOrderView::requestRefresh()
{
    if (m_armed
        && QMessageBox::warning(
               this, ArmedRefreshWarningTitle(), ArmedRefreshWarningText(),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    m_viewModel->Refresh();
}

void ConfigGPSOrderView::clearFields()
{
    for (FieldWidgets *widgets : m_fields) {
        delete widgets->row;
        delete widgets;
    }
    m_fields.clear();
}
