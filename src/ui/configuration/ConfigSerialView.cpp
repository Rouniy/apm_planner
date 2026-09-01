#include "ConfigSerialView.h"

#include <QAction>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {
class SerialWheelEventFilter final : public QObject
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

class SerialBitmaskMenu final : public QMenu
{
public:
    using QMenu::QMenu;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QAction *action = actionAt(event->pos());
        if (action && action->isEnabled() && action->isCheckable()) {
            action->trigger();
            event->accept();
            return;
        }
        QMenu::mouseReleaseEvent(event);
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
}

struct ConfigSerialView::RowWidgets
{
    QFrame *frame = nullptr;
    QLabel *portLabel = nullptr;
    QComboBox *baudCombo = nullptr;
    QComboBox *protocolCombo = nullptr;
    QToolButton *optionsButton = nullptr;
    QLabel *optionsSummary = nullptr;
    QLabel *statusLabel = nullptr;
    QList<QAction *> bitActions;
};

ConfigSerialView::ConfigSerialView(
    const ParameterMetaDataCatalog &catalog, QWidget *parent)
    : QWidget(parent),
      m_viewModel(new ConfigSerialViewModel(this))
{
    m_wheelFilter = new SerialWheelEventFilter(this);
    setObjectName(QStringLiteral("ConfigSerialView"));
    setStyleSheet(QStringLiteral(
        "ConfigSerialView { background: #1A201D; color: #E6EDE9; }"
        "ConfigSerialView QComboBox, ConfigSerialView QToolButton {"
        " background: #161B18; color: #E6EDE9; border: 1px solid #2A322D;"
        " padding: 4px; }"
        "QLabel#serialPortsTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#serialPortsWarning { color: #E0B040; font-weight: bold; }"
        "QLabel#serialPortStatus { color: #34D399; font-size: 11px; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(8);

    auto *title = new QLabel(tr("Serial Ports"), this);
    title->setObjectName(QStringLiteral("serialPortsTitle"));
    root->addWidget(title);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("serialPortsHeader"));
    auto *headerLayout = new QGridLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setHorizontalSpacing(0);
    headerLayout->setColumnMinimumWidth(0, 160);
    headerLayout->setColumnMinimumWidth(1, 180);
    headerLayout->setColumnMinimumWidth(2, 180);
    headerLayout->setColumnStretch(3, 1);
    const QStringList headers = {
        tr("Port Name"), tr("Speed"), tr("Protcol"), tr("Options")
    };
    for (int column = 0; column < headers.size(); ++column) {
        auto *label = new QLabel(headers.at(column), header);
        label->setObjectName(
            QStringLiteral("serialPortsHeader%1").arg(column));
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        headerLayout->addWidget(label, 0, column);
    }
    root->addWidget(header);

    m_rowsWidget = new QWidget(this);
    m_rowsWidget->setObjectName(QStringLiteral("serialPortsRows"));
    m_rowsLayout = new QVBoxLayout(m_rowsWidget);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(0);
    root->addWidget(m_rowsWidget);

    m_warningLabel = new QLabel(this);
    m_warningLabel->setObjectName(QStringLiteral("serialPortsWarning"));
    m_warningLabel->setWordWrap(true);
    m_warningLabel->setContentsMargins(0, 8, 0, 0);
    root->addWidget(m_warningLabel);

    auto *note = new QLabel(m_viewModel->Note(), this);
    note->setObjectName(QStringLiteral("serialPortsNote"));
    note->setWordWrap(true);
    root->addWidget(note);
    root->addStretch(1);

    connect(m_viewModel, &ConfigSerialViewModel::structureChanged,
            this, &ConfigSerialView::rebuildRows);
    connect(m_viewModel, &ConfigSerialViewModel::rowChanged,
            this, &ConfigSerialView::syncRow);
    connect(m_viewModel, &ConfigSerialViewModel::warningChanged,
            this, &ConfigSerialView::syncWarning);
    connect(m_viewModel, &ConfigSerialViewModel::writeRequested,
            this, &ConfigSerialView::writeRequested);
    m_viewModel->setCatalog(catalog);
    syncWarning();
}

void ConfigSerialView::setCatalog(
    const ParameterMetaDataCatalog &catalog)
{
    m_viewModel->setCatalog(catalog);
}

void ConfigSerialView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    m_viewModel->setParameterSnapshot(parameters, preferredComponent);
}

void ConfigSerialView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    m_viewModel->parameterChanged(componentId, name, value);
}

void ConfigSerialView::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    m_viewModel->parameterWriteFailed(componentId, name, reason);
}

void ConfigSerialView::rebuildRows()
{
    clearRows();
    for (const SerialPortRow &row : m_viewModel->Ports()) {
        auto *widgets = new RowWidgets;
        widgets->frame = new QFrame(m_rowsWidget);
        widgets->frame->setObjectName(
            QStringLiteral("serialPortRow_%1").arg(row.portName));
        widgets->frame->setProperty("portName", row.portName);
        widgets->frame->setFrameShape(QFrame::StyledPanel);
        widgets->frame->setFrameShadow(QFrame::Plain);

        auto *grid = new QGridLayout(widgets->frame);
        grid->setContentsMargins(0, 6, 0, 6);
        grid->setHorizontalSpacing(0);
        grid->setVerticalSpacing(4);
        grid->setColumnMinimumWidth(0, 160);
        grid->setColumnMinimumWidth(1, 180);
        grid->setColumnMinimumWidth(2, 180);
        grid->setColumnStretch(3, 1);

        widgets->portLabel = new QLabel(widgets->frame);
        widgets->portLabel->setObjectName(QStringLiteral("portLabel"));
        widgets->portLabel->setWordWrap(true);
        QFont labelFont = widgets->portLabel->font();
        labelFont.setBold(true);
        widgets->portLabel->setFont(labelFont);
        grid->addWidget(widgets->portLabel, 0, 0);

        widgets->baudCombo = new QComboBox(widgets->frame);
        widgets->baudCombo->setObjectName(QStringLiteral("baudCombo"));
        widgets->baudCombo->setMinimumWidth(160);
        widgets->baudCombo->installEventFilter(m_wheelFilter);
        for (const ParamOption &option : row.baudOptions) {
            widgets->baudCombo->addItem(option.text, option.value);
        }
        grid->addWidget(widgets->baudCombo, 0, 1);

        widgets->protocolCombo = new QComboBox(widgets->frame);
        widgets->protocolCombo->setObjectName(
            QStringLiteral("protocolCombo"));
        widgets->protocolCombo->setMinimumWidth(160);
        widgets->protocolCombo->installEventFilter(m_wheelFilter);
        for (const ParamOption &option : row.protocolOptions) {
            widgets->protocolCombo->addItem(option.text, option.value);
        }
        grid->addWidget(widgets->protocolCombo, 0, 2);

        auto *optionsLayout = new QHBoxLayout;
        optionsLayout->setContentsMargins(0, 0, 0, 0);
        optionsLayout->setSpacing(8);
        widgets->optionsButton = new QToolButton(widgets->frame);
        widgets->optionsButton->setObjectName(
            QStringLiteral("optionsButton"));
        widgets->optionsButton->setText(tr("Set Bitmask"));
        widgets->optionsButton->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new SerialBitmaskMenu(widgets->optionsButton);
        menu->setObjectName(QStringLiteral("optionsMenu"));
        menu->setMaximumHeight(320);
        widgets->optionsButton->setMenu(menu);
        for (const SerialBitOption &option : row.optionBits) {
            QAction *action = menu->addAction(
                QStringLiteral("%1: %2").arg(option.bit).arg(option.label));
            action->setCheckable(true);
            action->setProperty("bit", option.bit);
            widgets->bitActions.append(action);
            connect(action, &QAction::toggled, this,
                    [this, portName = row.portName, bit = option.bit](
                        bool enabled) {
                if (!m_viewModel->setOptionBit(portName, bit, enabled)) {
                    syncRow(portName);
                }
            });
        }
        widgets->optionsSummary = new QLabel(widgets->frame);
        widgets->optionsSummary->setObjectName(
            QStringLiteral("optionsSummary"));
        widgets->optionsSummary->setWordWrap(true);
        optionsLayout->addWidget(widgets->optionsButton);
        optionsLayout->addWidget(widgets->optionsSummary, 1);
        grid->addLayout(optionsLayout, 0, 3);

        widgets->statusLabel = new QLabel(widgets->frame);
        widgets->statusLabel->setObjectName(
            QStringLiteral("serialPortStatus"));
        grid->addWidget(widgets->statusLabel, 1, 1, 1, 3);

        connect(widgets->baudCombo,
                QOverload<int>::of(&QComboBox::activated),
                this, [this, portName = row.portName,
                       combo = widgets->baudCombo](int index) {
            if (index >= 0) {
                m_viewModel->selectBaud(portName, combo->itemData(index));
            }
        });
        connect(widgets->protocolCombo,
                QOverload<int>::of(&QComboBox::activated),
                this, [this, portName = row.portName,
                       combo = widgets->protocolCombo](int index) {
            if (index >= 0) {
                m_viewModel->selectProtocol(
                    portName, combo->itemData(index));
            }
        });

        m_rows.insert(row.portName, widgets);
        m_rowsLayout->addWidget(widgets->frame);
        syncRow(row.portName);
    }
}

void ConfigSerialView::syncRow(const QString &portName)
{
    RowWidgets *widgets = m_rows.value(portName);
    if (!widgets) {
        return;
    }
    SerialPortRow row;
    bool found = false;
    for (const SerialPortRow &candidate : m_viewModel->Ports()) {
        if (candidate.portName == portName) {
            row = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }

    widgets->portLabel->setText(row.label);
    {
        const QSignalBlocker blocker(widgets->baudCombo);
        widgets->baudCombo->setCurrentIndex(
            optionIndex(row.baudOptions, row.selectedBaud));
    }
    {
        const QSignalBlocker blocker(widgets->protocolCombo);
        widgets->protocolCombo->setCurrentIndex(
            optionIndex(row.protocolOptions, row.selectedProtocol));
    }
    widgets->protocolCombo->setEnabled(row.hasProtocol);
    widgets->optionsButton->setVisible(row.hasBits);
    widgets->optionsSummary->setVisible(row.hasOptions);
    widgets->optionsSummary->setText(row.optionsText);
    widgets->statusLabel->setText(row.status);
    for (QAction *action : widgets->bitActions) {
        const int bit = action->property("bit").toInt();
        bool isSet = false;
        for (const SerialBitOption &option : row.optionBits) {
            if (option.bit == bit) {
                isSet = option.isSet;
                break;
            }
        }
        const QSignalBlocker blocker(action);
        action->setChecked(isSet);
    }
}

void ConfigSerialView::syncWarning()
{
    m_warningLabel->setText(m_viewModel->Warning());
    m_warningLabel->setVisible(m_viewModel->HasWarning());
}

void ConfigSerialView::clearRows()
{
    qDeleteAll(m_rows);
    m_rows.clear();
    while (QLayoutItem *item = m_rowsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            delete widget;
        }
        delete item;
    }
}
