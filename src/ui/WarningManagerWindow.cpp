#include "WarningManagerWindow.h"

#include "services/WarningTelemetrySource.h"

#include <QComboBox>
#include <QCompleter>
#include <QDoubleValidator>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{

QStringList normalizedFields(QStringList fields)
{
    for (QString &field : fields) field = field.trimmed();
    fields.removeAll(QString());
    fields.removeDuplicates();
    std::sort(fields.begin(), fields.end(), [](const QString &left,
                                               const QString &right) {
        return QString::localeAwareCompare(left, right) < 0;
    });
    return fields;
}

CustomWarning newRule(const QStringList &fields)
{
    CustomWarning rule;
    if (!fields.isEmpty()) rule.name = fields.first();
    return rule;
}

CustomWarning *findRule(QVector<CustomWarning> *rules, quint64 id)
{
    if (!rules || id == 0) return nullptr;
    for (CustomWarning &rule : *rules) {
        if (rule.id == id) return &rule;
        if (CustomWarning *child = findRule(&rule.child, id)) return child;
    }
    return nullptr;
}

bool removeChild(CustomWarning *parent, quint64 id)
{
    if (!parent) return false;
    for (int index = 0; index < parent->child.size(); ++index) {
        CustomWarning &candidate = parent->child[index];
        if (candidate.id == id) {
            const QVector<CustomWarning> descendants = candidate.child;
            parent->child.removeAt(index);
            for (int childIndex = 0; childIndex < descendants.size();
                 ++childIndex) {
                parent->child.insert(index + childIndex,
                                     descendants.at(childIndex));
            }
            return true;
        }
        if (removeChild(&candidate, id)) return true;
    }
    return false;
}

bool removeRuleById(QVector<CustomWarning> *rules, quint64 id)
{
    if (!rules || id == 0) return false;
    for (int index = 0; index < rules->size(); ++index) {
        if (rules->at(index).id == id) {
            // Removing an IF rule removes its complete AND chain.
            rules->removeAt(index);
            return true;
        }
        if (removeChild(&(*rules)[index], id)) return true;
    }
    return false;
}

int conditionCount(const QVector<CustomWarning> &rules)
{
    int count = 0;
    for (const CustomWarning &rule : rules) {
        ++count;
        count += conditionCount(rule.child);
    }
    return count;
}

QVector<quint64> flattenedIds(const QVector<CustomWarning> &rules)
{
    QVector<quint64> ids;
    std::function<void(const QVector<CustomWarning> &)> append =
        [&](const QVector<CustomWarning> &items) {
            for (const CustomWarning &item : items) {
                ids.append(item.id);
                append(item.child);
            }
        };
    append(rules);
    return ids;
}

bool sameRule(const CustomWarning &left, const CustomWarning &right)
{
    if (left.id != right.id || left.name != right.name
        || left.condition != right.condition
        || left.threshold != right.threshold || left.type != right.type
        || left.color != right.color
        || left.repeatSeconds != right.repeatSeconds
        || left.text != right.text
        || left.child.size() != right.child.size()) {
        return false;
    }
    for (int index = 0; index < left.child.size(); ++index) {
        if (!sameRule(left.child.at(index), right.child.at(index))) return false;
    }
    return true;
}

bool sameRules(const QVector<CustomWarning> &left,
               const QVector<CustomWarning> &right)
{
    if (left.size() != right.size()) return false;
    for (int index = 0; index < left.size(); ++index) {
        if (!sameRule(left.at(index), right.at(index))) return false;
    }
    return true;
}

void clearLayout(QLayout *layout)
{
    if (!layout) return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) delete widget;
        if (QLayout *child = item->layout()) clearLayout(child);
        delete item;
    }
}

void configureColumns(QGridLayout *layout)
{
    if (!layout) return;
    const int widths[] = {44, 170, 80, 100, 150, 115, 78, 280, 34, 34};
    for (int column = 0; column < 10; ++column) {
        layout->setColumnMinimumWidth(column, widths[column]);
    }
    layout->setColumnStretch(1, 1);
    layout->setColumnStretch(7, 2);
    layout->setHorizontalSpacing(5);
}

QString rowObjectName(const char *prefix, quint64 id)
{
    return QString::fromLatin1(prefix) + QString::number(id);
}

} // namespace

WarningManagerWindow::WarningManagerWindow(
    WarningEngine *engine, const QStringList &fields, QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_engine(engine)
    , m_fields(normalizedFields(fields))
{
    setObjectName(QStringLiteral("WarningManagerWindow"));
    setWindowTitle(tr("Warning Manager"));
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose, true);
    resize(1250, 620);
    setMinimumWidth(1000);
    if (parent) move(parent->frameGeometry().center() - rect().center());

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(8);

    auto *title = new QLabel(tr("Warning Manager"), this);
    title->setObjectName(QStringLiteral("WarningManagerTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_addButton = new QPushButton(tr("Add Warning"), this);
    m_addButton->setObjectName(QStringLiteral("AddWarningButton"));
    m_saveButton = new QPushButton(tr("Save"), this);
    m_saveButton->setObjectName(QStringLiteral("SaveWarningsButton"));
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("WarningStatus"));
    m_status->setWordWrap(true);
    toolbar->addWidget(m_addButton);
    toolbar->addWidget(m_saveButton);
    toolbar->addWidget(m_status, 1);
    root->addLayout(toolbar);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("WarningRulesScroll"));
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setFrameShape(QFrame::NoFrame);
    m_rulesWidget = new QWidget(scroll);
    m_rulesWidget->setObjectName(QStringLiteral("WarningRules"));
    m_rulesWidget->setMinimumWidth(1125);
    m_rulesLayout = new QVBoxLayout(m_rulesWidget);
    m_rulesLayout->setContentsMargins(0, 0, 0, 0);
    m_rulesLayout->setSpacing(0);
    scroll->setWidget(m_rulesWidget);
    root->addWidget(scroll, 1);

    auto *footer = new QLabel(
        tr("Columns: source, comparison, threshold, action type, color, "
           "repeat seconds, message. Message tokens: {name}, {value}, "
           "{warning}. Thresholds use the labeled canonical/raw units. "
           "Unknown or stale fields are preserved but do not trigger. "
           "Imported Mission Planner warnings.xml files contain no unit "
           "metadata and may use display units such as feet or knots; verify "
           "every imported threshold before use."),
        this);
    footer->setObjectName(QStringLiteral("WarningManagerHelp"));
    footer->setWordWrap(true);
    footer->setStyleSheet(QStringLiteral("color: #999999;"));
    root->addWidget(footer);

    connect(m_addButton, &QPushButton::clicked,
            this, &WarningManagerWindow::addRootRule);
    connect(m_saveButton, &QPushButton::clicked,
            this, &WarningManagerWindow::saveRules);
    if (m_engine) {
        connect(m_engine, &WarningEngine::rulesChanged, this, [this]() {
            if (!m_localMutation) {
                m_operationStatus.clear();
                scheduleRebuild();
            }
            updateStatus();
        });
        connect(m_engine, &QObject::destroyed, this, [this]() {
            m_engine = nullptr;
            m_operationStatus = tr("Warning service is unavailable.");
            scheduleRebuild();
            updateStatus();
        });
    }
    rebuildRows();
    updateStatus();
}

WarningManagerWindow::~WarningManagerWindow()
{
    if (m_engine) disconnect(m_engine, nullptr, this, nullptr);
    m_engine = nullptr;
}

void WarningManagerWindow::setExternalStatus(const QString &status)
{
    m_externalStatus = status.trimmed();
    updateStatus();
}

void WarningManagerWindow::setTelemetryValues(
    const QHash<QString, double> &values)
{
    if (m_telemetryValues == values) return;
    m_telemetryValues = values;
    refreshTelemetryRows();
}

void WarningManagerWindow::scheduleRebuild()
{
    if (m_rebuildPending) return;
    m_rebuildPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_rebuildPending = false;
        rebuildRows();
        updateStatus();
    });
}

void WarningManagerWindow::rebuildRows()
{
    m_valueLabels.clear();
    m_sourceEditors.clear();
    m_thresholdEditors.clear();
    clearLayout(m_rulesLayout);
    m_displayedIds.clear();

    auto *header = new QFrame(m_rulesWidget);
    auto *headerLayout = new QGridLayout(header);
    headerLayout->setContentsMargins(6, 4, 6, 5);
    configureColumns(headerLayout);
    const QStringList headings = {
        tr("IF / AND"), tr("Source"), tr("Comparison"), tr("Threshold"),
        tr("Type"), tr("Color"), tr("Repeat (s)"), tr("Message"),
        QString(), QString()};
    for (int column = 0; column < headings.size(); ++column) {
        auto *label = new QLabel(headings.at(column), header);
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        headerLayout->addWidget(label, 0, column);
    }
    m_rulesLayout->addWidget(header);

    const QVector<CustomWarning> rules = m_engine
        ? m_engine->rules() : QVector<CustomWarning>();
    if (rules.isEmpty()) {
        auto *empty = new QFrame(m_rulesWidget);
        auto *emptyLayout = new QVBoxLayout(empty);
        auto *instructions = new QLabel(
            m_engine
                ? tr("No warning conditions are configured. Add a warning, "
                     "choose a telemetry source, and save it to persist.")
                : tr("The warning service is unavailable."),
            empty);
        instructions->setObjectName(QStringLiteral("WarningEmptyInstructions"));
        instructions->setWordWrap(true);
        emptyLayout->addWidget(instructions, 0, Qt::AlignCenter);
        auto *add = new QPushButton(tr("Add Warning"), empty);
        add->setObjectName(QStringLiteral("EmptyAddWarningButton"));
        add->setEnabled(bool(m_engine));
        connect(add, &QPushButton::clicked,
                this, &WarningManagerWindow::addRootRule);
        emptyLayout->addWidget(add, 0, Qt::AlignCenter);
        m_rulesLayout->addWidget(empty);
        m_rulesLayout->addStretch(1);
        return;
    }

    const QStringList conditionNames = WarningEngine::conditionNames();
    const QStringList typeNames = WarningEngine::typeNames();
    const QStringList colorNames = WarningEngine::colorNames();
    std::function<void(const QVector<CustomWarning> &, int)> appendRows =
        [&](const QVector<CustomWarning> &items, int depth) {
            for (const CustomWarning &rule : items) {
                m_displayedIds.append(rule.id);
                auto *row = new QFrame(m_rulesWidget);
                row->setObjectName(rowObjectName("WarningRule_", rule.id));
                row->setFrameShape(QFrame::NoFrame);
                row->setStyleSheet(QStringLiteral(
                    "QFrame { border-bottom: 1px solid palette(mid); }"));
                auto *grid = new QGridLayout(row);
                grid->setContentsMargins(6 + depth * 20, 6, 6, 6);
                configureColumns(grid);

                auto *depthLabel = new QLabel(
                    depth == 0 ? tr("IF") : tr("AND"), row);
                QFont depthFont = depthLabel->font();
                depthFont.setBold(true);
                depthLabel->setFont(depthFont);
                grid->addWidget(depthLabel, 0, 0);

                auto *source = new QComboBox(row);
                source->setObjectName(rowObjectName("WarningSource_", rule.id));
                source->setEditable(true);
                source->setInsertPolicy(QComboBox::NoInsert);
                source->setMaxVisibleItems(20);
                source->addItems(m_fields);
                if (!rule.name.isEmpty()
                    && source->findText(rule.name, Qt::MatchExactly) < 0) {
                    source->addItem(rule.name);
                }
                source->setEditText(rule.name);
                if (source->completer()) {
                    source->completer()->setCaseSensitivity(Qt::CaseInsensitive);
                    source->completer()->setCompletionMode(
                        QCompleter::PopupCompletion);
                }
                grid->addWidget(source, 0, 1);
                m_sourceEditors.insert(rule.id, source);

                auto *currentValue = new QLabel(row);
                currentValue->setObjectName(
                    rowObjectName("WarningTelemetry_", rule.id));
                currentValue->setStyleSheet(QStringLiteral("color: #999999;"));
                currentValue->setTextInteractionFlags(
                    Qt::TextSelectableByMouse);
                grid->addWidget(currentValue, 1, 1);
                m_valueLabels.insert(rule.id, currentValue);

                auto *condition = new QComboBox(row);
                condition->setObjectName(
                    rowObjectName("WarningCondition_", rule.id));
                for (int index = 0; index < conditionNames.size(); ++index) {
                    condition->addItem(conditionNames.at(index), index);
                }
                condition->setCurrentIndex(condition->findData(
                    static_cast<int>(rule.condition)));
                grid->addWidget(condition, 0, 2);

                auto *threshold = new QLineEdit(row);
                threshold->setObjectName(
                    rowObjectName("WarningThreshold_", rule.id));
                threshold->setText(QString::number(rule.threshold, 'g', 17));
                auto *thresholdValidator = new QDoubleValidator(threshold);
                thresholdValidator->setNotation(
                    QDoubleValidator::ScientificNotation);
                QLocale thresholdLocale = QLocale::c();
                thresholdLocale.setNumberOptions(QLocale::RejectGroupSeparator);
                thresholdValidator->setLocale(thresholdLocale);
                threshold->setValidator(thresholdValidator);
                threshold->setToolTip(
                    tr("Finite threshold in the labeled canonical/raw unit. "
                       "Mission Planner's editor normally limits newly entered "
                       "values to -999999..999999; imported finite values outside "
                       "that range are preserved exactly."));
                grid->addWidget(threshold, 0, 3);
                m_thresholdEditors.insert(rule.id, threshold);

                auto *type = new QComboBox(row);
                type->setObjectName(rowObjectName("WarningType_", rule.id));
                for (int index = 0; index < typeNames.size(); ++index) {
                    type->addItem(typeNames.at(index), index);
                }
                type->setCurrentIndex(type->findData(
                    static_cast<int>(rule.type)));
                grid->addWidget(type, 0, 4);

                auto *color = new QComboBox(row);
                color->setObjectName(rowObjectName("WarningColor_", rule.id));
                color->addItems(colorNames);
                if (!rule.color.isEmpty()
                    && color->findText(rule.color, Qt::MatchExactly) < 0) {
                    color->addItem(rule.color);
                }
                color->setCurrentText(rule.color.isEmpty()
                                          ? QStringLiteral("NoColor")
                                          : rule.color);
                grid->addWidget(color, 0, 5);

                auto *repeat = new QSpinBox(row);
                repeat->setObjectName(rowObjectName("WarningRepeat_", rule.id));
                repeat->setRange(0, 86400);
                repeat->setKeyboardTracking(false);
                repeat->setValue(qBound(0, rule.repeatSeconds, 86400));
                grid->addWidget(repeat, 0, 6);

                auto *text = new QLineEdit(rule.text, row);
                text->setObjectName(rowObjectName("WarningText_", rule.id));
                text->setPlaceholderText(
                    tr("WARNING: {name} is {value}"));
                grid->addWidget(text, 0, 7);

                auto *addChild = new QPushButton(QStringLiteral("+"), row);
                addChild->setObjectName(
                    rowObjectName("WarningAddChild_", rule.id));
                addChild->setToolTip(tr("Add AND condition"));
                addChild->setEnabled(rule.child.isEmpty());
                grid->addWidget(addChild, 0, 8);

                auto *remove = new QPushButton(QString::fromUtf8("−"), row);
                remove->setObjectName(
                    rowObjectName("WarningRemove_", rule.id));
                remove->setToolTip(tr("Remove condition"));
                grid->addWidget(remove, 0, 9);

                const quint64 id = rule.id;
                connect(source, &QComboBox::editTextChanged,
                        this, [this, id](const QString &value) {
                    if (value.trimmed().isEmpty()) return;
                    applyRuleChange(id, [value](CustomWarning &item) {
                        item.name = value.trimmed();
                    });
                });
                connect(condition, QOverload<int>::of(
                            &QComboBox::currentIndexChanged),
                        this, [this, id, condition](int) {
                    const int value = condition->currentData().toInt();
                    applyRuleChange(id, [value](CustomWarning &item) {
                        item.condition = static_cast<CustomWarning::Conditional>(value);
                    });
                });
                connect(threshold, &QLineEdit::editingFinished,
                        this, [this, id, threshold]() {
                    bool parsed = false;
                    const double value = QLocale::c().toDouble(
                        threshold->text().trimmed(), &parsed);
                    if (!parsed || !std::isfinite(value)) {
                        m_operationStatus = tr(
                            "Threshold must be a finite number using '.' as the decimal separator.");
                        scheduleRebuild();
                        updateStatus();
                        return;
                    }
                    applyRuleChange(id, [value](CustomWarning &item) {
                        item.threshold = value;
                    });
                });
                connect(type, QOverload<int>::of(
                            &QComboBox::currentIndexChanged),
                        this, [this, id, type](int) {
                    const int value = type->currentData().toInt();
                    applyRuleChange(id, [value](CustomWarning &item) {
                        item.type = static_cast<CustomWarning::WarningType>(value);
                    });
                });
                connect(color, &QComboBox::currentTextChanged,
                        this, [this, id](const QString &value) {
                    applyRuleChange(id, [value](CustomWarning &item) {
                        item.color = value;
                    });
                });
                connect(repeat, QOverload<int>::of(&QSpinBox::valueChanged),
                        this, [this, id](int value) {
                    applyRuleChange(id, [value](CustomWarning &item) {
                        item.repeatSeconds = value;
                    });
                });
                connect(text, &QLineEdit::textChanged,
                        this, [this, id](const QString &value) {
                    applyRuleChange(id, [value](CustomWarning &item) {
                        item.text = value;
                    });
                });
                connect(addChild, &QPushButton::clicked,
                        this, [this, id]() { addChildRule(id); });
                connect(remove, &QPushButton::clicked,
                        this, [this, id]() { removeRule(id); });

                m_rulesLayout->addWidget(row);
                appendRows(rule.child, depth + 1);
            }
        };
    appendRows(rules, 0);
    m_rulesLayout->addStretch(1);
    refreshTelemetryRows();
}

void WarningManagerWindow::refreshTelemetryRows()
{
    if (!m_engine) return;
    const QVector<CustomWarning> rules = m_engine->rules();
    std::function<void(const QVector<CustomWarning> &)> refresh =
        [&](const QVector<CustomWarning> &items) {
            for (const CustomWarning &rule : items) {
                QLabel *const label = m_valueLabels.value(rule.id);
                QComboBox *const source = m_sourceEditors.value(rule.id);
                QLineEdit *const threshold =
                    m_thresholdEditors.value(rule.id);
                const QString unit =
                    WarningTelemetrySource::fieldUnits(rule.name);
                const QString unitSuffix = unit.isEmpty()
                    ? QString() : QStringLiteral(" ") + unit;
                QString state;
                if (rule.condition == CustomWarning::NONE) {
                    state = tr("Inactive: comparison is NONE");
                } else if (!m_fields.contains(rule.name)) {
                    state = tr("Unknown source: unavailable");
                } else {
                    const auto value = m_telemetryValues.constFind(rule.name);
                    if (value == m_telemetryValues.constEnd()
                        || !std::isfinite(value.value())) {
                        state = tr("Unavailable or stale");
                    } else {
                        state = tr("Current: %1%2")
                            .arg(QString::number(value.value(), 'g', 12),
                                 unitSuffix);
                    }
                }
                if (!unit.isEmpty() && !state.contains(unitSuffix)) {
                    state += tr(" · Unit: %1").arg(unit);
                }
                if (label) {
                    label->setText(state);
                    label->setToolTip(state);
                }
                if (source) source->setToolTip(state);
                if (threshold) {
                    threshold->setToolTip(unit.isEmpty()
                        ? tr("Finite threshold in this field's labeled raw/unitless value. "
                             "The MP editor's normal entry range is -999999..999999.")
                        : tr("Finite threshold in %1. The MP editor's normal entry "
                             "range is -999999..999999; imported values outside it "
                             "are preserved.").arg(unit));
                }
                refresh(rule.child);
            }
        };
    refresh(rules);
}

void WarningManagerWindow::updateStatus()
{
    if (!m_status) return;
    QString status;
    if (!m_engine) {
        status = tr("Warning service is unavailable.");
    } else if (!m_operationStatus.isEmpty()) {
        status = m_operationStatus;
    } else {
        const int count = conditionCount(m_engine->rules());
        status = m_engine->dirty()
            ? tr("%1 warning condition(s). Changes take effect immediately; "
                 "save to persist.").arg(count)
            : tr("%1 warning condition(s). Saved configuration is active.")
                  .arg(count);
    }
    if (!m_externalStatus.isEmpty()) {
        if (!status.isEmpty()) status.append(QLatin1Char(' '));
        status.append(m_externalStatus);
    }
    m_status->setText(status);
    m_addButton->setEnabled(bool(m_engine));
    m_saveButton->setEnabled(bool(m_engine));
}

void WarningManagerWindow::applyRuleChange(
    quint64 id, const std::function<void(CustomWarning &)> &change)
{
    QPointer<WarningManagerWindow> self(this);
    QPointer<WarningEngine> service(m_engine);
    if (!service) {
        m_operationStatus = tr("Warning service is unavailable.");
        updateStatus();
        return;
    }
    QVector<CustomWarning> rules = service->rules();
    CustomWarning *const rule = findRule(&rules, id);
    if (!rule) {
        m_operationStatus = tr("The warning condition no longer exists.");
        scheduleRebuild();
        updateStatus();
        return;
    }
    change(*rule);
    QString error;
    m_localMutation = true;
    const bool accepted = service->setRules(rules, &error);
    if (!self) return;
    self->m_localMutation = false;
    if (!service) {
        self->m_operationStatus = self->tr("Warning service is unavailable.");
        self->scheduleRebuild();
        self->updateStatus();
        return;
    }
    if (!accepted) {
        self->m_operationStatus = error.isEmpty()
            ? self->tr("The warning change was rejected.") : error;
        self->scheduleRebuild();
    } else {
        self->m_operationStatus.clear();
        const QVector<CustomWarning> activeRules = service->rules();
        if (flattenedIds(activeRules) != self->m_displayedIds
            || !sameRules(activeRules, rules)) {
            self->scheduleRebuild();
        }
    }
    self->refreshTelemetryRows();
    self->updateStatus();
}

void WarningManagerWindow::applyStructuralChange(
    const std::function<bool(QVector<CustomWarning> *, QString *)> &change)
{
    QPointer<WarningManagerWindow> self(this);
    QPointer<WarningEngine> service(m_engine);
    if (!service) {
        m_operationStatus = tr("Warning service is unavailable.");
        updateStatus();
        return;
    }
    QVector<CustomWarning> rules = service->rules();
    QString error;
    if (!change(&rules, &error)) {
        m_operationStatus = error.isEmpty()
            ? tr("The warning structure could not be changed.") : error;
        updateStatus();
        return;
    }
    m_localMutation = true;
    const bool accepted = service->setRules(std::move(rules), &error);
    if (!self) return;
    self->m_localMutation = false;
    if (!service) {
        self->m_operationStatus = self->tr("Warning service is unavailable.");
    } else if (!accepted) {
        self->m_operationStatus = error.isEmpty()
            ? self->tr("The warning change was rejected.") : error;
    } else {
        self->m_operationStatus.clear();
    }
    self->scheduleRebuild();
    self->updateStatus();
}

void WarningManagerWindow::addRootRule()
{
    applyStructuralChange([this](QVector<CustomWarning> *rules, QString *) {
        if (!rules) return false;
        rules->append(newRule(m_fields));
        return true;
    });
}

void WarningManagerWindow::addChildRule(quint64 id)
{
    applyStructuralChange(
        [this, id](QVector<CustomWarning> *rules, QString *error) {
            CustomWarning *const rule = findRule(rules, id);
            if (!rule) {
                if (error) *error = tr("The warning condition no longer exists.");
                return false;
            }
            if (!rule->child.isEmpty()) {
                if (error) *error = tr("Add an AND condition only to the last condition in a chain.");
                return false;
            }
            rule->child.append(newRule(m_fields));
            return true;
        });
}

void WarningManagerWindow::removeRule(quint64 id)
{
    applyStructuralChange([this, id](QVector<CustomWarning> *rules,
                                     QString *error) {
        if (removeRuleById(rules, id)) return true;
        if (error) *error = tr("The warning condition no longer exists.");
        return false;
    });
}

void WarningManagerWindow::saveRules()
{
    QPointer<WarningManagerWindow> self(this);
    QPointer<WarningEngine> service(m_engine);
    if (!service) {
        m_operationStatus = tr("Warning service is unavailable.");
        updateStatus();
        return;
    }
    QString error;
    const bool saved = service->save(&error);
    if (!self) return;
    if (!service) {
        self->m_operationStatus = self->tr("Warning service is unavailable.");
    } else if (!saved) {
        self->m_operationStatus = error.isEmpty()
            ? self->tr("Warning configuration could not be saved.") : error;
    } else {
        self->m_operationStatus = self->tr("Saved %1 warning condition(s).")
            .arg(conditionCount(service->rules()));
    }
    self->updateStatus();
}
