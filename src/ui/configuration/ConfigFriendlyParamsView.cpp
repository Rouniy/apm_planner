#include "ConfigFriendlyParamsView.h"

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaType>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {
class FriendlyWheelEventFilter final : public QObject
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

class FriendlyDoubleSpinBox final : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;

protected:
    QString textFromValue(double value) const override
    {
        QString text = locale().toString(value, 'f', decimals());
        const QString decimalPoint(locale().decimalPoint());
        const int decimalPosition = text.indexOf(decimalPoint);
        if (decimalPosition >= 0) {
            while (text.endsWith(QLatin1Char('0'))
                   && text.size() > decimalPosition + decimalPoint.size()) {
                text.chop(1);
            }
            if (text.endsWith(decimalPoint)) {
                text.chop(decimalPoint.size());
            }
        }
        return text;
    }
};

class FriendlyBitmaskMenu final : public QMenu
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

QVariant valueWithOriginalType(const QVariant &reference,
                               const QVariant &value)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int typeId = reference.typeId();
#else
    const int typeId = reference.userType();
#endif
    switch (typeId) {
    case QMetaType::Int:
        return value.toInt();
    case QMetaType::UInt:
        return value.toUInt();
    case QMetaType::LongLong:
        return value.toLongLong();
    case QMetaType::ULongLong:
        return value.toULongLong();
    case QMetaType::Float:
        return value.toFloat();
    case QMetaType::Double:
        return value.toDouble();
    default:
        return value;
    }
}

QString editorKindName(ParamField::EditorKind kind)
{
    switch (kind) {
    case ParamField::EditorKind::Combo:
        return QStringLiteral("combo");
    case ParamField::EditorKind::Bitmask:
        return QStringLiteral("bitmask");
    case ParamField::EditorKind::Numeric:
        return QStringLiteral("numeric");
    }
    return QStringLiteral("numeric");
}
}

struct ConfigFriendlyParamsView::Row
{
    ParamField field;
    QWidget *widget = nullptr;
    QToolButton *favoriteButton = nullptr;
    QLabel *label = nullptr;
    QFrame *editorFrame = nullptr;
    QComboBox *comboBox = nullptr;
    QDoubleSpinBox *numericEditor = nullptr;
    QToolButton *bitmaskButton = nullptr;
    QList<QAction *> bitActions;
    QLabel *unitsLabel = nullptr;
    QLabel *statusLabel = nullptr;
    QVariant pendingValue;
    QVariant latestRequestedValue;
    QList<QPair<quint64, QVariant>> supersededValues;
    quint64 pendingGeneration = 0;
    quint64 supersededGeneration = 0;
    bool pending = false;
    bool hasLatestRequestedValue = false;
    bool filteredVisible = true;
};

ConfigFriendlyParamsView::ConfigFriendlyParamsView(
    bool advanced, const ParameterMetaDataCatalog &catalog, QWidget *parent,
    bool enforceMetadataRanges)
    : QWidget(parent),
      m_viewModel(new ConfigFriendlyParamsViewModel(
          advanced, this, enforceMetadataRanges)),
      m_searchDebounce(new QTimer(this))
{
    setObjectName(QStringLiteral("ConfigFriendlyParamsView"));
    setStyleSheet(QStringLiteral(
        "ConfigFriendlyParamsView { background: #1A201D; color: #E6EDE9; }"
        "ConfigFriendlyParamsView QLineEdit, ConfigFriendlyParamsView QComboBox,"
        "ConfigFriendlyParamsView QDoubleSpinBox, ConfigFriendlyParamsView QToolButton {"
        " background: #161B18; color: #E6EDE9; border: 1px solid #2A322D;"
        " padding: 4px; }"
        "ConfigFriendlyParamsView QPushButton { padding: 5px 12px; }"
        "QLabel#friendlyParamsTitle { color: #E8E8E8; font-size: 16px;"
        " font-weight: bold; }"
        "QLabel#statusLabel { color: #34D399; }"));

    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(16, 16, 16, 16);
    m_rootLayout->setSpacing(10);

    m_titleLabel = new QLabel(
        advanced ? tr("Advanced Params") : tr("Standard Params"), this);
    m_titleLabel->setObjectName(QStringLiteral("friendlyParamsTitle"));
    m_rootLayout->addWidget(m_titleLabel);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_refreshButton = new QPushButton(tr("Refresh Params"), this);
    m_refreshButton->setObjectName(QStringLiteral("refreshButton"));
    toolbar->addWidget(m_refreshButton);

    m_componentSelector = new QComboBox(this);
    m_componentSelector->setObjectName(QStringLiteral("componentSelector"));
    m_componentSelector->setToolTip(tr("MAVLink component"));
    m_componentSelector->setVisible(false);
    m_wheelFilter = new FriendlyWheelEventFilter(this);
    m_componentSelector->installEventFilter(m_wheelFilter);
    toolbar->addWidget(m_componentSelector);

    m_searchBox = new QLineEdit(this);
    m_searchBox->setObjectName(QStringLiteral("searchBox"));
    m_searchBox->setFixedWidth(240);
    m_searchBox->setPlaceholderText(tr("Search by name or label…"));
    toolbar->addWidget(m_searchBox);

    m_introLabel = new QLabel(
        tr("Human-readable parameters with descriptions. Connect, then Refresh. "
           "Star a row to pin it to the top; type to filter."), this);
    m_introLabel->setObjectName(QStringLiteral("friendlyParamsIntro"));
    m_introLabel->setWordWrap(true);
    toolbar->addWidget(m_introLabel, 1);
    m_rootLayout->addLayout(toolbar);

    m_fieldsScroll = new QScrollArea(this);
    m_fieldsScroll->setObjectName(QStringLiteral("fieldsScroll"));
    m_fieldsScroll->setFrameShape(QFrame::NoFrame);
    m_fieldsScroll->setWidgetResizable(true);
    m_fieldsContent = new QWidget(m_fieldsScroll);
    m_fieldsContent->setObjectName(QStringLiteral("fieldsContent"));
    m_fieldsLayout = new QVBoxLayout(m_fieldsContent);
    m_fieldsLayout->setContentsMargins(0, 0, 0, 0);
    m_fieldsLayout->setSpacing(0);
    m_emptyLabel = new QLabel(tr("No described parameters are available."),
                              m_fieldsContent);
    m_emptyLabel->setObjectName(QStringLiteral("emptyLabel"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_fieldsLayout->addWidget(m_emptyLabel);
    m_fieldsLayout->addStretch(1);
    m_fieldsScroll->setWidget(m_fieldsContent);
    m_rootLayout->addWidget(m_fieldsScroll, 1);

    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(250);

    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        emit refreshRequested(m_viewModel->selectedComponent());
    });
    connect(m_searchBox, &QLineEdit::textChanged, this, [this]() {
        m_searchDebounce->start();
    });
    connect(m_searchDebounce, &QTimer::timeout, this, [this]() {
        m_viewModel->setSearch(m_searchBox->text());
    });
    connect(m_componentSelector,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (index >= 0) {
            m_viewModel->setSelectedComponent(
                m_componentSelector->itemData(index).toInt());
        }
    });
    connect(m_viewModel, &ConfigFriendlyParamsViewModel::componentsChanged,
            this, &ConfigFriendlyParamsView::updateComponentSelector);
    connect(m_viewModel, &ConfigFriendlyParamsViewModel::structureChanged,
            this, &ConfigFriendlyParamsView::rebuildRows);
    connect(m_viewModel, &ConfigFriendlyParamsViewModel::layoutChanged,
            this, &ConfigFriendlyParamsView::applyLayout);
    connect(m_viewModel,
            &ConfigFriendlyParamsViewModel::parameterValueChanged,
            this, [this](int componentId, const QString &name,
                         const QVariant &value) {
        if (Row *row = m_rowsByKey.value(rowKey(componentId, name), nullptr)) {
            hydrateRow(row, value);
        }
    });

    m_viewModel->setCatalog(catalog, enforceMetadataRanges);
}

ConfigFriendlyParamsView::~ConfigFriendlyParamsView()
{
    clearRows();
}

void ConfigFriendlyParamsView::mutatePreservingTransientRows(
    const std::function<void()> &mutation)
{
    struct TransientRowState
    {
        QVariant pendingValue;
        QVariant latestRequestedValue;
        QList<QVariant> supersededValues;
        QString editorText;
        bool pending = false;
        bool hasLatestRequestedValue = false;
        bool editorModified = false;
        bool editorHadFocus = false;
    };
    QHash<QString, TransientRowState> transientRows;
    for (const Row *row : m_rows) {
        TransientRowState state;
        state.pending = row->pending;
        state.pendingValue = row->pendingValue;
        state.hasLatestRequestedValue = row->hasLatestRequestedValue;
        state.latestRequestedValue = row->latestRequestedValue;
        for (const auto &superseded : row->supersededValues) {
            state.supersededValues.append(superseded.second);
        }
        if (row->numericEditor) {
            if (auto *editor = row->numericEditor->findChild<QLineEdit *>()) {
                state.editorModified = editor->isModified();
                state.editorHadFocus = editor->hasFocus()
                    || row->numericEditor->hasFocus();
                if (state.editorModified || state.editorHadFocus) {
                    state.editorText = editor->text();
                }
            }
        }
        if (state.pending || !state.supersededValues.isEmpty()
            || state.editorModified || state.editorHadFocus) {
            transientRows.insert(
                rowKey(row->field.componentId, row->field.name), state);
        }
    }
    const int scrollPosition = m_fieldsScroll->verticalScrollBar()->value();

    mutation();

    for (auto iterator = transientRows.constBegin();
         iterator != transientRows.constEnd(); ++iterator) {
        Row *row = m_rowsByKey.value(iterator.key(), nullptr);
        if (!row) {
            continue;
        }
        const TransientRowState &state = iterator.value();
        row->hasLatestRequestedValue = state.hasLatestRequestedValue;
        row->latestRequestedValue = state.latestRequestedValue;
        const auto beginPending = [this, row](const QVariant &expectedValue,
                                              bool dispatch) {
            const QVariant authoritativeValue = row->field.value;
            hydrateRow(row, expectedValue);
            row->field.value = authoritativeValue;
            row->pending = true;
            row->pendingValue = expectedValue;
            const quint64 generation = ++row->pendingGeneration;
            row->statusLabel->setText(QStringLiteral("…"));
            if (dispatch) {
                queueWriteDispatch(row, expectedValue);
            }
            QTimer::singleShot(5000, row->widget, [row, generation]() {
                if (row->pending && row->pendingGeneration == generation) {
                    row->pending = false;
                    row->statusLabel->setText(QObject::tr("write failed"));
                }
            });
        };
        if (state.pending) {
            if (variantsEqual(row->field.value, state.pendingValue)) {
                row->statusLabel->setText(QStringLiteral("✓"));
            } else {
                beginPending(state.pendingValue, false);
            }
        }
        bool authoritativeValueIsSuperseded = false;
        for (const QVariant &value : state.supersededValues) {
            authoritativeValueIsSuperseded =
                authoritativeValueIsSuperseded
                || (variantsEqual(row->field.value, value)
                    && (!state.hasLatestRequestedValue
                        || !variantsEqual(
                            state.latestRequestedValue, value)));
            const quint64 generation = ++row->supersededGeneration;
            row->supersededValues.append(qMakePair(generation, value));
            QTimer::singleShot(5000, row->widget, [row, generation]() {
                for (int index = 0; index < row->supersededValues.size();
                     ++index) {
                    if (row->supersededValues.at(index).first == generation) {
                        row->supersededValues.removeAt(index);
                        break;
                    }
                }
            });
        }
        if (!state.pending && authoritativeValueIsSuperseded
            && state.hasLatestRequestedValue) {
            beginPending(state.latestRequestedValue, true);
        }
        if (row->numericEditor
            && (state.editorModified || state.editorHadFocus)) {
            if (auto *editor = row->numericEditor->findChild<QLineEdit *>()) {
                editor->setText(state.editorText);
                editor->setModified(state.editorModified);
                if (state.editorHadFocus) {
                    editor->setFocus();
                }
            }
        }
    }
    QTimer::singleShot(0, this, [this, scrollPosition]() {
        m_fieldsScroll->verticalScrollBar()->setValue(scrollPosition);
    });
}

void ConfigFriendlyParamsView::setCatalog(
    const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges)
{
    mutatePreservingTransientRows([this, &catalog, enforceMetadataRanges]() {
        m_viewModel->setCatalog(catalog, enforceMetadataRanges);
    });
    setUnavailableMessage(QString());
}

void ConfigFriendlyParamsView::setParameterSnapshot(
    const QList<ConfigFriendlyParameterValue> &parameters,
    int preferredComponent)
{
    mutatePreservingTransientRows([this, &parameters, preferredComponent]() {
        m_viewModel->setParameterSnapshot(parameters, preferredComponent);
    });
}

void ConfigFriendlyParamsView::setUnavailableMessage(const QString &message)
{
    m_emptyLabel->setText(message.isEmpty()
                              ? tr("No described parameters are available.")
                              : message);
}

void ConfigFriendlyParamsView::setCustomParameterNames(
    const QStringList &names)
{
    mutatePreservingTransientRows([this, &names]() {
        m_viewModel->setCustomParameterNames(names);
    });
}

void ConfigFriendlyParamsView::setEmbeddedMode(bool embedded)
{
    m_embeddedMode = embedded;
    m_rootLayout->setContentsMargins(
        embedded ? QMargins(0, 0, 0, 0) : QMargins(16, 16, 16, 16));
    m_titleLabel->setVisible(!embedded);
    m_refreshButton->setVisible(!embedded);
    m_searchBox->setVisible(!embedded);
    m_introLabel->setVisible(!embedded);
    updateComponentSelector();
    for (Row *row : m_rows) {
        row->favoriteButton->setVisible(!embedded);
    }
}

int ConfigFriendlyParamsView::visibleParameterCount() const
{
    int count = 0;
    for (const Row *row : m_rows) {
        count += row->filteredVisible ? 1 : 0;
    }
    return count;
}

int ConfigFriendlyParamsView::selectedComponent() const
{
    return m_viewModel->selectedComponent();
}

void ConfigFriendlyParamsView::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (Row *row = m_rowsByKey.value(rowKey(componentId, name), nullptr)) {
        if (!(row->pending && variantsEqual(row->pendingValue, value))) {
            for (const auto &superseded : row->supersededValues) {
                if (variantsEqual(superseded.second, value)) {
                    if (row->hasLatestRequestedValue
                        && variantsEqual(
                            row->latestRequestedValue, value)) {
                        break;
                    }
                    if (!row->pending && row->hasLatestRequestedValue) {
                        row->pending = true;
                        const quint64 generation = ++row->pendingGeneration;
                        row->pendingValue = row->latestRequestedValue;
                        row->statusLabel->setText(QStringLiteral("…"));
                        queueWriteDispatch(row, row->pendingValue);
                        QTimer::singleShot(
                            5000, row->widget, [row, generation]() {
                            if (row->pending
                                && row->pendingGeneration == generation) {
                                row->pending = false;
                                row->statusLabel->setText(
                                    QObject::tr("write failed"));
                            }
                        });
                    }
                    return;
                }
            }
        }
    }
    m_viewModel->updateParameter(componentId, name, value);
}

void ConfigFriendlyParamsView::parameterWriteFailed(
    int componentId, const QString &name, const QString &reason)
{
    if (Row *row = m_rowsByKey.value(rowKey(componentId, name), nullptr)) {
        row->pending = false;
        row->statusLabel->setText(reason.isEmpty() ? tr("write failed")
                                                   : reason);
    }
}

void ConfigFriendlyParamsView::parameterWriteFailed(
    int componentId, const QString &name, const QVariant &attemptedValue,
    const QString &reason)
{
    if (Row *row = m_rowsByKey.value(rowKey(componentId, name), nullptr)) {
        if (row->pending
            && !variantsEqual(row->pendingValue, attemptedValue)) {
            return;
        }
        row->pending = false;
        row->statusLabel->setText(reason.isEmpty() ? tr("write failed")
                                                   : reason);
    }
}

void ConfigFriendlyParamsView::queueWriteDispatch(
    Row *row, const QVariant &expectedValue)
{
    if (!row) {
        return;
    }
    const QString key = rowKey(row->field.componentId, row->field.name);
    QTimer::singleShot(0, this, [this, key, expectedValue]() {
        Row *current = m_rowsByKey.value(key, nullptr);
        if (!current || !current->pending
            || !variantsEqual(current->pendingValue, expectedValue)) {
            return;
        }
        emit writeRequested(current->field.componentId, current->field.name,
                            expectedValue);
    });
}

void ConfigFriendlyParamsView::rebuildRows()
{
    clearRows();
    const QList<ParamField> fields = m_viewModel->fields();
    for (const ParamField &field : fields) {
        auto *row = new Row;
        row->field = field;
        row->widget = new QWidget(m_fieldsContent);
        row->widget->setObjectName(QStringLiteral("paramField_") + field.name);
        row->widget->setProperty("parameterName", field.name);
        row->widget->setProperty("componentId", field.componentId);
        row->widget->setProperty("editorKind",
                                 editorKindName(field.editorKind));

        auto *layout = new QHBoxLayout(row->widget);
        layout->setContentsMargins(0, 3, 0, 3);
        layout->setSpacing(0);

        row->favoriteButton = new QToolButton(row->widget);
        row->favoriteButton->setObjectName(QStringLiteral("favoriteButton"));
        row->favoriteButton->setCheckable(true);
        row->favoriteButton->setChecked(field.favorite);
        row->favoriteButton->setFixedWidth(32);
        row->favoriteButton->setVisible(!m_embeddedMode);
        updateFavoriteButton(row);
        layout->addWidget(row->favoriteButton);

        row->label = new QLabel(field.label, row->widget);
        row->label->setObjectName(QStringLiteral("fieldLabel"));
        row->label->setFixedWidth(220);
        row->label->setWordWrap(true);
        row->label->setToolTip(field.description);
        row->label->setContentsMargins(0, 0, 8, 0);
        layout->addWidget(row->label);

        row->editorFrame = new QFrame(row->widget);
        row->editorFrame->setFixedWidth(200);
        auto *editorLayout = new QHBoxLayout(row->editorFrame);
        editorLayout->setContentsMargins(0, 0, 20, 0);
        if (field.editorKind == ParamField::EditorKind::Combo) {
            row->comboBox = new QComboBox(row->editorFrame);
            row->comboBox->setObjectName(QStringLiteral("valueComboBox"));
            row->comboBox->setMinimumWidth(180);
            row->comboBox->installEventFilter(m_wheelFilter);
            for (const ParamOption &option : field.options) {
                row->comboBox->addItem(option.text, option.value);
            }
            editorLayout->addWidget(row->comboBox);
            connect(row->comboBox,
                    QOverload<int>::of(&QComboBox::activated),
                    this, [this, row](int index) {
                submitValue(row, row->comboBox->itemData(index));
            });
        } else if (field.editorKind == ParamField::EditorKind::Bitmask) {
            row->bitmaskButton = new QToolButton(row->editorFrame);
            row->bitmaskButton->setObjectName(QStringLiteral("bitmaskButton"));
            row->bitmaskButton->setMinimumWidth(180);
            row->bitmaskButton->setPopupMode(QToolButton::InstantPopup);
            auto *menu = new FriendlyBitmaskMenu(row->bitmaskButton);
            menu->setMaximumHeight(320);
            for (const BitOption &option : field.bitOptions) {
                if (option.bit < 0 || option.bit >= 63) {
                    continue;
                }
                QAction *action = menu->addAction(
                    QStringLiteral("%1: %2").arg(option.bit).arg(option.label));
                action->setCheckable(true);
                action->setData(option.bit);
                row->bitActions.append(action);
                connect(action, &QAction::toggled, this, [this, row]() {
                    qulonglong knownMask = 0;
                    qulonglong selectedBits = 0;
                    for (QAction *bitAction : row->bitActions) {
                        const qulonglong mask = qulonglong(1)
                            << bitAction->data().toInt();
                        knownMask |= mask;
                        if (bitAction->isChecked()) {
                            selectedBits |= mask;
                        }
                    }
                    const qulonglong current = row->pending
                        ? row->pendingValue.toULongLong()
                        : row->field.value.toULongLong();
                    const qulonglong value = (current & ~knownMask)
                        | selectedBits;
                    updateBitmaskSummary(row);
                    submitValue(row, QVariant::fromValue(value));
                });
            }
            row->bitmaskButton->setMenu(menu);
            editorLayout->addWidget(row->bitmaskButton);
        } else {
            row->numericEditor = new FriendlyDoubleSpinBox(row->editorFrame);
            row->numericEditor->setObjectName(QStringLiteral("numericEditor"));
            row->numericEditor->setMinimumWidth(180);
            row->numericEditor->installEventFilter(m_wheelFilter);
            row->numericEditor->setDecimals(6);
            row->numericEditor->setRange(-1.0e18, 1.0e18);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const int valueType = field.value.typeId();
#else
            const int valueType = field.value.userType();
#endif
            const bool integerValue = valueType == QMetaType::Int
                || valueType == QMetaType::UInt
                || valueType == QMetaType::LongLong
                || valueType == QMetaType::ULongLong;
            if (integerValue) {
                row->numericEditor->setDecimals(0);
            }
            row->numericEditor->setSingleStep(integerValue
                ? qMax(1.0, field.increment)
                : (field.increment > 0.0 ? field.increment : 0.01));
            editorLayout->addWidget(row->numericEditor);
            connect(row->numericEditor, &QDoubleSpinBox::editingFinished,
                    this, [this, row]() {
                submitValue(row, row->numericEditor->value());
            });
        }
        layout->addWidget(row->editorFrame);

        row->unitsLabel = new QLabel(field.units, row->widget);
        row->unitsLabel->setObjectName(QStringLiteral("unitsLabel"));
        row->unitsLabel->setContentsMargins(8, 0, 8, 0);
        layout->addWidget(row->unitsLabel);

        row->statusLabel = new QLabel(row->widget);
        row->statusLabel->setObjectName(QStringLiteral("statusLabel"));
        layout->addWidget(row->statusLabel, 1);

        QWidget *editor = row->comboBox
            ? static_cast<QWidget *>(row->comboBox)
            : row->bitmaskButton
                ? static_cast<QWidget *>(row->bitmaskButton)
                : static_cast<QWidget *>(row->numericEditor);
        editor->setEnabled(!field.readOnly);
        if (field.readOnly) {
            row->statusLabel->setText(tr("read only"));
        }
        connect(row->favoriteButton, &QToolButton::toggled,
                this, [this, row](bool checked) {
            row->field.favorite = checked;
            updateFavoriteButton(row);
            m_viewModel->setFavorite(row->field.componentId,
                                     row->field.name, checked);
        });

        m_rows.append(row);
        m_rowsByKey.insert(rowKey(field.componentId, field.name), row);
        hydrateRow(row, field.value);
    }
    applyLayout();
}

void ConfigFriendlyParamsView::applyLayout()
{
    while (QLayoutItem *item = m_fieldsLayout->takeAt(0)) {
        delete item;
    }

    const QList<ParamField> visible = m_viewModel->visibleFields();
    QHash<QString, bool> visibleFavorites;
    for (const ParamField &field : visible) {
        visibleFavorites.insert(rowKey(field.componentId, field.name),
                                field.favorite);
    }
    for (Row *row : m_rows) {
        const QString key = rowKey(row->field.componentId, row->field.name);
        row->filteredVisible = visibleFavorites.contains(key);
        row->widget->setVisible(false);
        if (row->filteredVisible) {
            row->field.favorite = visibleFavorites.value(key);
        }
        const QSignalBlocker blocker(row->favoriteButton);
        row->favoriteButton->setChecked(row->field.favorite);
        updateFavoriteButton(row);
    }
    for (const ParamField &field : visible) {
        if (Row *row = m_rowsByKey.value(
                rowKey(field.componentId, field.name), nullptr)) {
            m_fieldsLayout->addWidget(row->widget);
            row->widget->setVisible(true);
        }
    }
    m_emptyLabel->setVisible(visible.isEmpty());
    m_fieldsLayout->addWidget(m_emptyLabel);
    m_fieldsLayout->addStretch(1);
}

void ConfigFriendlyParamsView::updateComponentSelector()
{
    const QSignalBlocker blocker(m_componentSelector);
    m_componentSelector->clear();
    const QList<int> components = m_viewModel->availableComponents();
    for (int component : components) {
        m_componentSelector->addItem(tr("Component %1").arg(component),
                                     component);
    }
    const int selected = m_componentSelector->findData(
        m_viewModel->selectedComponent());
    m_componentSelector->setCurrentIndex(selected);
    m_componentSelector->setVisible(
        !m_embeddedMode && components.size() > 1);
}

void ConfigFriendlyParamsView::hydrateRow(Row *row, const QVariant &value)
{
    row->field.value = value;
    if (row->comboBox) {
        const QSignalBlocker blocker(row->comboBox);
        int selected = -1;
        for (int index = 0; index < row->comboBox->count(); ++index) {
            if (variantsEqual(row->comboBox->itemData(index), value)) {
                selected = index;
                break;
            }
        }
        row->comboBox->setCurrentIndex(selected);
    } else if (row->numericEditor) {
        const QSignalBlocker blocker(row->numericEditor);
        row->numericEditor->setValue(value.toDouble());
        const bool outOfRange = row->field.hasRange
            && (value.toDouble() < row->field.minimum
                || value.toDouble() > row->field.maximum);
        setOutOfRange(row, outOfRange);
    } else if (row->bitmaskButton) {
        const qulonglong numeric = value.toULongLong();
        for (QAction *action : row->bitActions) {
            const QSignalBlocker blocker(action);
            action->setChecked((numeric & (qulonglong(1)
                               << action->data().toInt())) != 0);
        }
        updateBitmaskSummary(row);
    }

    if (row->pending && variantsEqual(row->pendingValue, value)) {
        row->pending = false;
        row->statusLabel->setText(QStringLiteral("✓"));
    } else if (row->pending) {
        row->pending = false;
        row->statusLabel->setText(tr("write mismatch"));
    } else if (!row->pending && !row->field.readOnly) {
        row->statusLabel->clear();
    }
}

void ConfigFriendlyParamsView::submitValue(Row *row, const QVariant &value)
{
    if (!row || row->field.readOnly) {
        return;
    }
    const QVariant typedValue = valueWithOriginalType(row->field.value, value);
    const bool outOfRange =
        row->field.editorKind == ParamField::EditorKind::Numeric
        && row->field.hasRange
        && (typedValue.toDouble() < row->field.minimum
            || typedValue.toDouble() > row->field.maximum);
    if (outOfRange && row->field.enforceRange) {
        setOutOfRange(row, true);
        row->statusLabel->setText(tr("out of range"));
        return;
    }
    setOutOfRange(row, outOfRange);
    for (int index = row->supersededValues.size() - 1; index >= 0; --index) {
        if (variantsEqual(row->supersededValues.at(index).second,
                          typedValue)) {
            row->supersededValues.removeAt(index);
        }
    }
    if (row->pending && variantsEqual(row->pendingValue, typedValue)) {
        return;
    }
    if (row->pending) {
        const quint64 supersededGeneration = ++row->supersededGeneration;
        row->supersededValues.append(
            qMakePair(supersededGeneration, row->pendingValue));
        QTimer::singleShot(5000, row->widget,
                           [row, supersededGeneration]() {
            for (int index = 0; index < row->supersededValues.size();
                 ++index) {
                if (row->supersededValues.at(index).first
                    == supersededGeneration) {
                    row->supersededValues.removeAt(index);
                    break;
                }
            }
        });
    } else if (variantsEqual(row->field.value, typedValue)) {
        row->statusLabel->setText(QStringLiteral("✓"));
        return;
    }
    row->pending = true;
    const quint64 generation = ++row->pendingGeneration;
    row->pendingValue = typedValue;
    row->latestRequestedValue = typedValue;
    row->hasLatestRequestedValue = true;
    row->statusLabel->setText(QStringLiteral("…"));
    emit writeRequested(row->field.componentId, row->field.name, typedValue);
    QTimer::singleShot(5000, row->widget, [row, generation]() {
        if (row->pending && row->pendingGeneration == generation) {
            row->pending = false;
            row->statusLabel->setText(QObject::tr("write failed"));
        }
    });
}

void ConfigFriendlyParamsView::setOutOfRange(Row *row, bool outOfRange)
{
    if (!row || !row->editorFrame || !row->numericEditor) {
        return;
    }
    row->editorFrame->setProperty("outOfRange", outOfRange);
    row->editorFrame->setStyleSheet(outOfRange
        ? QStringLiteral("QFrame { border: 2px solid #E64A4A; }")
        : QString());
}

void ConfigFriendlyParamsView::updateFavoriteButton(Row *row)
{
    row->favoriteButton->setText(row->favoriteButton->isChecked()
                                     ? QStringLiteral("★")
                                     : QStringLiteral("☆"));
    row->favoriteButton->setStyleSheet(row->favoriteButton->isChecked()
        ? QStringLiteral("color: #F5C518; background: transparent; border: none;"
                         " font-size: 18px;")
        : QStringLiteral("color: #888888; background: transparent; border: none;"
                         " font-size: 18px;"));
}

void ConfigFriendlyParamsView::updateBitmaskSummary(Row *row)
{
    QStringList enabled;
    for (QAction *action : row->bitActions) {
        if (action->isChecked()) {
            enabled.append(action->text());
        }
    }
    if (enabled.isEmpty()) {
        row->bitmaskButton->setText(tr("(none)"));
    } else if (enabled.size() <= 3) {
        row->bitmaskButton->setText(enabled.join(QStringLiteral(", ")));
    } else {
        row->bitmaskButton->setText(tr("%1 bits set").arg(enabled.size()));
    }
}

void ConfigFriendlyParamsView::clearRows()
{
    while (QLayoutItem *item = m_fieldsLayout->takeAt(0)) {
        delete item;
    }
    for (Row *row : m_rows) {
        delete row->widget;
        delete row;
    }
    m_rows.clear();
    m_rowsByKey.clear();
}

QString ConfigFriendlyParamsView::rowKey(
    int componentId, const QString &name) const
{
    return QString::number(componentId) + QLatin1Char(':')
        + name.trimmed().toUpper();
}
