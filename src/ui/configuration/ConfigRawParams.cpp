#include "ConfigRawParams.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace {
constexpr int NameRole = Qt::UserRole + 1;
constexpr int SearchRole = Qt::UserRole + 2;
constexpr int ModifiedRole = Qt::UserRole + 3;
constexpr int FavoriteRole = Qt::UserRole + 4;
constexpr int NumericRole = Qt::UserRole + 5;
constexpr int OptionKindRole = Qt::UserRole + 6;
constexpr int OptionEntriesRole = Qt::UserRole + 7;
constexpr int DisplayedValueRole = Qt::UserRole + 8;
constexpr int ParameterTypeRole = Qt::UserRole + 9;
constexpr int ReadOnlyRole = Qt::UserRole + 10;

enum OptionKind
{
    NoOptions,
    EnumOptions,
    BitmaskOptions
};

const QString kFavoritesKey = QStringLiteral(
    "ConfigRawParams/favorites/v1");
const QString kSplitterKey = QStringLiteral(
    "ConfigRawParams/splitterSizes/v1");
const QString kTreeCollapsedKey = QStringLiteral(
    "ConfigRawParams/treeCollapsed/v1");

struct SafeMessageResult
{
    QMessageBox::StandardButton button = QMessageBox::NoButton;
    bool ownerAlive = false;
};

SafeMessageResult showSafeMessage(
    QWidget *owner, QMessageBox::Icon icon, const QString &title,
    const QString &message,
    QMessageBox::StandardButtons buttons = QMessageBox::Ok,
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton)
{
    const QPointer<QWidget> guard(owner);
    QMessageBox box(icon, title, message, buttons, nullptr);
    if (defaultButton != QMessageBox::NoButton) {
        box.setDefaultButton(defaultButton);
    }
    if (owner) {
        QObject::connect(owner, &QObject::destroyed,
                         &box, &QDialog::reject);
    }
    box.exec();
    const auto button = box.clickedButton()
        ? box.standardButton(box.clickedButton())
        : QMessageBox::NoButton;
    return {button, !guard.isNull()};
}

struct SafeFileDialogResult
{
    QString path;
    bool ownerAlive = false;
};

SafeFileDialogResult showSafeFileDialog(
    QWidget *owner, const QString &title, const QString &filter,
    QFileDialog::AcceptMode acceptMode)
{
    const QPointer<QWidget> guard(owner);
    QFileDialog dialog(nullptr, title, QString(), filter);
    dialog.setAcceptMode(acceptMode);
    dialog.setFileMode(acceptMode == QFileDialog::AcceptOpen
                           ? QFileDialog::ExistingFile
                           : QFileDialog::AnyFile);
    if (owner) {
        QObject::connect(owner, &QObject::destroyed,
                         &dialog, &QDialog::reject);
    }
    const bool accepted = dialog.exec() == QDialog::Accepted;
    return {accepted ? dialog.selectedFiles().value(0) : QString(),
            !guard.isNull()};
}

int naturalCompare(const QString &left, const QString &right)
{
    int leftIndex = 0;
    int rightIndex = 0;
    while (leftIndex < left.size() && rightIndex < right.size()) {
        const QChar leftCharacter = left.at(leftIndex);
        const QChar rightCharacter = right.at(rightIndex);
        if (leftCharacter.isDigit() && rightCharacter.isDigit()) {
            int leftEnd = leftIndex;
            int rightEnd = rightIndex;
            while (leftEnd < left.size() && left.at(leftEnd).isDigit()) {
                ++leftEnd;
            }
            while (rightEnd < right.size() && right.at(rightEnd).isDigit()) {
                ++rightEnd;
            }
            int leftSignificant = leftIndex;
            int rightSignificant = rightIndex;
            while (leftSignificant < leftEnd
                   && left.at(leftSignificant) == QLatin1Char('0')) {
                ++leftSignificant;
            }
            while (rightSignificant < rightEnd
                   && right.at(rightSignificant) == QLatin1Char('0')) {
                ++rightSignificant;
            }
            const int leftDigits = leftEnd - leftSignificant;
            const int rightDigits = rightEnd - rightSignificant;
            if (leftDigits != rightDigits) {
                return leftDigits < rightDigits ? -1 : 1;
            }
            for (int offset = 0; offset < leftDigits; ++offset) {
                const QChar leftDigit = left.at(leftSignificant + offset);
                const QChar rightDigit = right.at(rightSignificant + offset);
                if (leftDigit != rightDigit) {
                    return leftDigit < rightDigit ? -1 : 1;
                }
            }
            leftIndex = leftEnd;
            rightIndex = rightEnd;
            continue;
        }
        const QChar foldedLeft = leftCharacter.toUpper();
        const QChar foldedRight = rightCharacter.toUpper();
        if (foldedLeft != foldedRight) {
            return foldedLeft < foldedRight ? -1 : 1;
        }
        ++leftIndex;
        ++rightIndex;
    }
    if (leftIndex == left.size() && rightIndex == right.size()) {
        return 0;
    }
    return leftIndex == left.size() ? -1 : 1;
}

class RawParameterProxyModel final : public QSortFilterProxyModel
{
public:
    explicit RawParameterProxyModel(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
        setDynamicSortFilter(true);
        setSortCaseSensitivity(Qt::CaseInsensitive);
    }

    void setSearchText(const QString &value)
    {
        const QString normalized = value.trimmed();
        if (m_search == normalized) {
            return;
        }
        m_search = normalized;
        invalidateFilter();
    }

    void setPrefix(const QString &value)
    {
        if (m_prefix == value) {
            return;
        }
        m_prefix = value;
        invalidateFilter();
    }

    void setModifiedOnly(bool value)
    {
        if (m_modifiedOnly == value) {
            return;
        }
        m_modifiedOnly = value;
        invalidateFilter();
    }

    void refreshRowState()
    {
        invalidateFilter();
        invalidate();
    }

protected:
    bool filterAcceptsRow(int sourceRow,
                          const QModelIndex &sourceParent) const override
    {
        const QModelIndex nameIndex = sourceModel()->index(
            sourceRow, 0, sourceParent);
        const QString name = sourceModel()->data(
            nameIndex, NameRole).toString();
        if (!m_prefix.isEmpty()
            && name.compare(m_prefix, Qt::CaseInsensitive) != 0
            && !name.startsWith(
                m_prefix + QLatin1Char('_'), Qt::CaseInsensitive)) {
            return false;
        }
        if (m_modifiedOnly
            && !sourceModel()->data(
                nameIndex, ModifiedRole).toBool()) {
            return false;
        }
        return m_search.isEmpty()
            || sourceModel()->data(nameIndex, SearchRole).toString()
                   .contains(m_search, Qt::CaseInsensitive);
    }

    bool lessThan(const QModelIndex &left,
                  const QModelIndex &right) const override
    {
        const QModelIndex leftName = sourceModel()->index(left.row(), 0);
        const QModelIndex rightName = sourceModel()->index(right.row(), 0);
        const bool leftFavorite = sourceModel()->data(
            leftName, FavoriteRole).toBool();
        const bool rightFavorite = sourceModel()->data(
            rightName, FavoriteRole).toBool();
        if (leftFavorite != rightFavorite) {
            // QSortFilterProxyModel reverses lessThan's arguments for a
            // descending sort. Adjust the relation so favorites remain first
            // in both directions, matching Mission Planner.
            return sortOrder() == Qt::AscendingOrder
                ? leftFavorite : !leftFavorite;
        }

        const QVariant leftNumber = sourceModel()->data(left, NumericRole);
        const QVariant rightNumber = sourceModel()->data(right, NumericRole);
        if (leftNumber.isValid() && rightNumber.isValid()) {
            const double first = leftNumber.toDouble();
            const double second = rightNumber.toDouble();
            if (first != second) {
                return first < second;
            }
        } else {
            const int comparison = naturalCompare(
                sourceModel()->data(left).toString(),
                sourceModel()->data(right).toString());
            if (comparison != 0) {
                return comparison < 0;
            }
        }
        return naturalCompare(
            sourceModel()->data(leftName, NameRole).toString(),
            sourceModel()->data(rightName, NameRole).toString()) < 0;
    }

private:
    QString m_search;
    QString m_prefix;
    bool m_modifiedOnly = false;
};

class ParameterOptionsDelegate final : public QStyledItemDelegate
{
public:
    using StageCallback = std::function<void(const QString &, double)>;

    explicit ParameterOptionsDelegate(StageCallback stage,
                                      QObject *parent = nullptr)
        : QStyledItemDelegate(parent), m_stage(std::move(stage))
    {
    }

    QWidget *createEditor(QWidget *parent,
                          const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override
    {
        Q_UNUSED(option)
        if (index.data(ReadOnlyRole).toBool()
            || index.data(OptionKindRole).toInt() != EnumOptions) {
            return nullptr;
        }
        auto *combo = new QComboBox(parent);
        combo->setObjectName(QStringLiteral("ConfigRawParamsEnumEditor"));
        const QVariantList entries = index.data(OptionEntriesRole).toList();
        for (const QVariant &entry : entries) {
            const QVariantMap map = entry.toMap();
            combo->addItem(map.value(QStringLiteral("label")).toString(),
                           map.value(QStringLiteral("value")));
        }
        auto *self = const_cast<ParameterOptionsDelegate *>(this);
        connect(combo, QOverload<int>::of(&QComboBox::activated), self,
                [self, combo](int) {
            emit self->commitData(combo);
            emit self->closeEditor(combo);
        });
        return combo;
    }

    void setEditorData(QWidget *editor,
                       const QModelIndex &index) const override
    {
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo) {
            return;
        }
        const QVariant displayed = index.data(DisplayedValueRole);
        const auto type = static_cast<ParameterType>(
            index.data(ParameterTypeRole).toInt());
        int selected = -1;
        for (int item = 0; item < combo->count(); ++item) {
            if (ParameterCodec::valuesEqual(
                    displayed, combo->itemData(item), type)) {
                selected = item;
                break;
            }
        }
        combo->setCurrentIndex(selected);
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override
    {
        Q_UNUSED(model)
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo || combo->currentIndex() < 0 || !m_stage) {
            return;
        }
        bool ok = false;
        const double value = combo->currentData().toDouble(&ok);
        if (ok && std::isfinite(value)) {
            m_stage(index.data(NameRole).toString(), value);
        }
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyle *const itemStyle = option.widget
            ? option.widget->style() : QApplication::style();
        const int kind = index.data(OptionKindRole).toInt();
        if (kind == NoOptions) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        if (kind == BitmaskOptions) {
            QStyleOptionButton button;
            button.rect = option.rect.adjusted(2, 3, -2, -3);
            button.state = index.data(ReadOnlyRole).toBool()
                ? QStyle::State_None : QStyle::State_Enabled;
            if (option.state.testFlag(QStyle::State_MouseOver)) {
                button.state |= QStyle::State_MouseOver;
            }
            button.text = QObject::tr("Set Bitmask");
            itemStyle->drawControl(
                QStyle::CE_PushButton, &button, painter,
                qobject_cast<QWidget *>(parent()));
            return;
        }

        QStyleOptionComboBox combo;
        combo.rect = option.rect.adjusted(1, 2, -1, -2);
        combo.state = index.data(ReadOnlyRole).toBool()
            ? QStyle::State_None : QStyle::State_Enabled;
        combo.frame = true;
        const QVariant displayed = index.data(DisplayedValueRole);
        const auto type = static_cast<ParameterType>(
            index.data(ParameterTypeRole).toInt());
        for (const QVariant &entry : index.data(OptionEntriesRole).toList()) {
            const QVariantMap map = entry.toMap();
            if (ParameterCodec::valuesEqual(
                    displayed, map.value(QStringLiteral("value")), type)) {
                combo.currentText = map.value(
                    QStringLiteral("label")).toString();
                break;
            }
        }
        itemStyle->drawComplexControl(
            QStyle::CC_ComboBox, &combo, painter,
            qobject_cast<QWidget *>(parent()));
        itemStyle->drawControl(
            QStyle::CE_ComboBoxLabel, &combo, painter,
            qobject_cast<QWidget *>(parent()));
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        Q_UNUSED(model)
        if (index.data(ReadOnlyRole).toBool()
            || index.data(OptionKindRole).toInt() != BitmaskOptions) {
            return QStyledItemDelegate::editorEvent(
                event, model, option, index);
        }
        bool activate = false;
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            activate = mouse->button() == Qt::LeftButton
                && option.rect.contains(mouse->pos());
        } else if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            activate = key->key() == Qt::Key_Space
                || key->key() == Qt::Key_Return
                || key->key() == Qt::Key_Enter;
        }
        if (!activate) {
            return false;
        }
        showBitmaskEditor(index);
        return true;
    }

private:
    static int bitWidth(ParameterType type)
    {
        switch (type) {
        case ParameterType::UInt8:
        case ParameterType::Int8:
            return 8;
        case ParameterType::UInt16:
        case ParameterType::Int16:
            return 16;
        default:
            return 32;
        }
    }

    static bool isSigned(ParameterType type)
    {
        return type == ParameterType::Int8
            || type == ParameterType::Int16
            || type == ParameterType::Int32;
    }

    static quint32 rawBits(const QVariant &value, ParameterType type)
    {
        if (isSigned(type)) {
            return static_cast<quint32>(value.toLongLong());
        }
        return static_cast<quint32>(value.toULongLong());
    }

    static double numericValue(quint32 bits, ParameterType type)
    {
        const int width = bitWidth(type);
        const quint32 widthMask = width == 32
            ? std::numeric_limits<quint32>::max()
            : (quint32(1) << width) - 1U;
        bits &= widthMask;
        if (isSigned(type)
            && (bits & (quint32(1) << (width - 1))) != 0U) {
            return static_cast<double>(
                static_cast<qint64>(bits) - (qint64(1) << width));
        }
        return static_cast<double>(bits);
    }

    void showBitmaskEditor(const QModelIndex &index)
    {
        if (!m_stage) {
            return;
        }
        const QString name = index.data(NameRole).toString();
        const auto type = static_cast<ParameterType>(
            index.data(ParameterTypeRole).toInt());
        const int width = bitWidth(type);
        const quint32 widthMask = width == 32
            ? std::numeric_limits<quint32>::max()
            : (quint32(1) << width) - 1U;
        quint32 currentBits = rawBits(
            index.data(DisplayedValueRole), type) & widthMask;
        quint32 knownMask = 0;

        // Non-blocking heap ownership lets a target/page reset destroy the
        // editor normally, without resuming an event handler on deleted view
        // and delegate objects.
        auto *dialog = new QDialog(
            qobject_cast<QWidget *>(parent()));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        const QPointer<ParameterOptionsDelegate> guard(this);
        dialog->setObjectName(QStringLiteral("ConfigRawParamsBitmaskEditor"));
        dialog->setWindowTitle(QObject::tr("%1 Bitmask").arg(name));
        dialog->setWindowFlag(Qt::WindowStaysOnTopHint, true);
        auto *layout = new QVBoxLayout(dialog);
        QList<QCheckBox *> boxes;
        for (const QVariant &entry : index.data(OptionEntriesRole).toList()) {
            const QVariantMap map = entry.toMap();
            const int bit = map.value(QStringLiteral("bit")).toInt();
            if (bit < 0 || bit >= width) {
                continue;
            }
            const quint32 mask = quint32(1) << bit;
            knownMask |= mask;
            auto *box = new QCheckBox(
                map.value(QStringLiteral("label")).toString(), dialog);
            box->setProperty("bitPosition", bit);
            box->setChecked((currentBits & mask) != 0U);
            boxes.append(box);
            layout->addWidget(box);
        }
        auto apply = [guard, name, type, currentBits, knownMask, boxes]() {
            if (!guard || !guard->m_stage) {
                return;
            }
            quint32 updated = currentBits & ~knownMask;
            for (QCheckBox *box : boxes) {
                if (box->isChecked()) {
                    updated |= quint32(1)
                        << box->property("bitPosition").toInt();
                }
            }
            guard->m_stage(name, numericValue(updated, type));
        };
        for (QCheckBox *box : boxes) {
            connect(box, &QCheckBox::toggled, dialog,
                    [apply](bool) { apply(); });
        }
        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Close, dialog);
        connect(buttons, &QDialogButtonBox::rejected,
                dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog->open();
    }

    StageCallback m_stage;
};

class ExpressionParser
{
public:
    explicit ExpressionParser(QString expression)
        : m_expression(std::move(expression))
    {
        m_expression.replace(QLatin1Char(','), QLatin1Char('.'));
    }

    bool evaluate(double *result, QString *error)
    {
        m_position = 0;
        m_error.clear();
        const double value = parseExpression();
        skipSpace();
        if (m_error.isEmpty() && m_position != m_expression.size()) {
            m_error = QObject::tr("Unexpected character at position %1.")
                          .arg(m_position + 1);
        }
        if (m_error.isEmpty() && !std::isfinite(value)) {
            m_error = QObject::tr("The result must be a finite number.");
        }
        if (!m_error.isEmpty()) {
            if (error) {
                *error = m_error;
            }
            return false;
        }
        if (result) {
            *result = value;
        }
        return true;
    }

private:
    void skipSpace()
    {
        while (m_position < m_expression.size()
               && m_expression.at(m_position).isSpace()) {
            ++m_position;
        }
    }

    bool consume(QChar character)
    {
        skipSpace();
        if (m_position < m_expression.size()
            && m_expression.at(m_position) == character) {
            ++m_position;
            return true;
        }
        return false;
    }

    double parseExpression()
    {
        double value = parseTerm();
        while (m_error.isEmpty()) {
            if (consume(QLatin1Char('+'))) {
                value += parseTerm();
            } else if (consume(QLatin1Char('-'))) {
                value -= parseTerm();
            } else {
                break;
            }
        }
        return value;
    }

    double parseTerm()
    {
        double value = parsePower();
        while (m_error.isEmpty()) {
            if (consume(QLatin1Char('*'))) {
                value *= parsePower();
            } else if (consume(QLatin1Char('/'))) {
                value /= parsePower();
            } else {
                break;
            }
        }
        return value;
    }

    double parsePower()
    {
        const double base = parseUnary();
        if (m_error.isEmpty() && consume(QLatin1Char('^'))) {
            return std::pow(base, parsePower());
        }
        return base;
    }

    double parseUnary()
    {
        if (consume(QLatin1Char('+'))) {
            return parseUnary();
        }
        if (consume(QLatin1Char('-'))) {
            return -parseUnary();
        }
        return parsePrimary();
    }

    double parsePrimary()
    {
        skipSpace();
        if (consume(QLatin1Char('('))) {
            const double value = parseExpression();
            if (!consume(QLatin1Char(')')) && m_error.isEmpty()) {
                m_error = QObject::tr("A closing parenthesis is missing.");
            }
            return value;
        }
        skipSpace();
        const int start = m_position;
        bool digits = false;
        while (m_position < m_expression.size()
               && m_expression.at(m_position).isDigit()) {
            digits = true;
            ++m_position;
        }
        if (m_position < m_expression.size()
            && m_expression.at(m_position) == QLatin1Char('.')) {
            ++m_position;
            while (m_position < m_expression.size()
                   && m_expression.at(m_position).isDigit()) {
                digits = true;
                ++m_position;
            }
        }
        if (!digits) {
            if (m_error.isEmpty()) {
                m_error = QObject::tr("A number is expected at position %1.")
                              .arg(start + 1);
            }
            return 0.0;
        }
        if (m_position < m_expression.size()
            && (m_expression.at(m_position) == QLatin1Char('e')
                || m_expression.at(m_position) == QLatin1Char('E'))) {
            const int exponentStart = m_position++;
            if (m_position < m_expression.size()
                && (m_expression.at(m_position) == QLatin1Char('+')
                    || m_expression.at(m_position) == QLatin1Char('-'))) {
                ++m_position;
            }
            const int exponentDigits = m_position;
            while (m_position < m_expression.size()
                   && m_expression.at(m_position).isDigit()) {
                ++m_position;
            }
            if (m_position == exponentDigits) {
                m_position = exponentStart;
            }
        }
        bool ok = false;
        const double value = QLocale::c().toDouble(
            m_expression.mid(start, m_position - start), &ok);
        if (!ok && m_error.isEmpty()) {
            m_error = QObject::tr("The numeric literal is invalid.");
        }
        return value;
    }

    QString m_expression;
    QString m_error;
    int m_position = 0;
};

QString metadataField(const ParameterMetaData &metadata,
                      const QString &name)
{
    for (auto iterator = metadata.fields.constBegin();
         iterator != metadata.fields.constEnd(); ++iterator) {
        if (iterator.key().compare(name, Qt::CaseInsensitive) == 0) {
            return iterator.value();
        }
    }
    return QString();
}

QString parameterOptionsText(const ParameterMetaData &metadata)
{
    QStringList lines;
    if (!metadata.rangeText.trimmed().isEmpty()) {
        lines.append(metadata.rangeText.trimmed());
    }
    for (const ParameterMetaDataOption &option : metadata.values) {
        lines.append(option.rawCode + QStringLiteral(": ") + option.label);
    }
    if (metadata.values.isEmpty()) {
        for (const auto &bit : metadata.bitmaskValues) {
            lines.append(QStringLiteral("%1: %2")
                             .arg(bit.first).arg(bit.second));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

QString parameterSearchText(const QString &name, const QString &displayed,
                            const QString &defaultText,
                            const ParameterMetaData &metadata)
{
    return QStringList{
        name,
        displayed,
        defaultText,
        metadata.units,
        parameterOptionsText(metadata),
        metadata.title,
        metadata.description
    }.join(QLatin1Char(' '));
}

} // namespace

ConfigRawParams::ConfigRawParams(
    const ParameterMetaDataCatalog &catalog, QWidget *parent,
    bool enforceMetadataRanges)
    : QWidget(parent),
      m_catalog(catalog),
      m_enforceMetadataRanges(enforceMetadataRanges)
{
    setObjectName(QStringLiteral("ConfigRawParams"));
    setMinimumSize(700, 420);
    resize(990, 528);
    buildUi();
    loadFavorites();
    rebuildRows();
    updateActionState();
}

void ConfigRawParams::buildUi()
{
    setStyleSheet(QStringLiteral(
        "ConfigRawParams { background:#262728; color:#ffffff; }"
        "ConfigRawParams QTreeView, ConfigRawParams QTableView {"
        " background:#262728; color:#ffffff; border:0; gridline-color:#262728; }"
        "ConfigRawParams QTableView::item { background:#434445; }"
        "ConfigRawParams QHeaderView::section { background:#262728;"
        " color:#ffffff; border:0; padding:3px; }"
        "ConfigRawParams QLineEdit { background:#434445; color:#ffffff;"
        " border:0; padding:2px; }"
        "ConfigRawParams QCheckBox, ConfigRawParams QLabel { color:#ffffff; }"
        "ConfigRawParams QPushButton { color:#405704; border:1px solid #799429;"
        " border-radius:2px; padding:2px;"
        " background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        " stop:0 #94C11F,stop:1 #CDE296); }"
        "ConfigRawParams QPushButton:hover { background:#CDE296; }"
        "ConfigRawParams QPushButton:pressed { background:#94C11F; }"
        "ConfigRawParams QPushButton:disabled { color:#6b764e;"
        " background:#4d5537; border-color:#596343; }"
        "ConfigRawParams QToolButton { color:#ffffff; background:#262728;"
        " border:0; }"));

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("splitContainer1"));
    root->addWidget(m_splitter);

    m_treePanel = new QWidget(m_splitter);
    auto *treeLayout = new QVBoxLayout(m_treePanel);
    treeLayout->setContentsMargins(0, 0, 0, 0);
    treeLayout->setSpacing(0);
    m_tree = new QTreeWidget(m_treePanel);
    m_tree->setObjectName(QStringLiteral("treeView1"));
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    treeLayout->addWidget(m_tree);
    m_splitter->addWidget(m_treePanel);

    auto *content = new QWidget(m_splitter);
    auto *contentLayout = new QHBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    m_collapseButton = new QToolButton(content);
    m_collapseButton->setObjectName(QStringLiteral("but_collapse"));
    m_collapseButton->setText(QStringLiteral("<"));
    m_collapseButton->setFixedSize(18, 18);
    contentLayout->addWidget(m_collapseButton, 0, Qt::AlignTop);

    m_model = new QStandardItemModel(0, ColumnCount, this);
    m_model->setHorizontalHeaderLabels({
        tr("Name"), tr("Value"), tr("Default"), tr("Units"),
        tr("Options"), tr("Desc"), tr("Fav")
    });
    m_proxy = new RawParameterProxyModel(this);
    m_proxy->setSourceModel(m_model);

    m_table = new QTableView(content);
    m_table->setObjectName(QStringLiteral("Params"));
    m_table->setModel(m_proxy);
    m_table->setAlternatingRowColors(false);
    m_table->setWordWrap(true);
    m_table->setSortingEnabled(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(
        QAbstractItemView::DoubleClicked
        | QAbstractItemView::SelectedClicked
        | QAbstractItemView::EditKeyPressed);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(36);
    m_table->horizontalHeader()->setSectionsMovable(true);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->setColumnWidth(Command, 130);
    m_table->setColumnWidth(Value, 70);
    m_table->setColumnWidth(DefaultValue, 70);
    m_table->setColumnWidth(Units, 60);
    m_table->setColumnWidth(Options, 150);
    m_table->setColumnWidth(Favorite, 30);
    m_table->horizontalHeader()->setSectionResizeMode(
        Description, QHeaderView::Stretch);
    m_table->setColumnHidden(DefaultValue, true);
    m_table->setItemDelegateForColumn(
        Options,
        new ParameterOptionsDelegate(
            [this](const QString &name, double value) {
                QString error;
                if (!stageNumericValue(name, value, false, &error)) {
                    if (error.startsWith(QStringLiteral("out-of-range:"))) {
                        const QString message = error.mid(
                            QStringLiteral("out-of-range:").size());
                        const qulonglong revision = m_stateRevision;
                        const QPointer<ConfigRawParams> guard(this);
                        const SafeMessageResult answer = showSafeMessage(
                            this, QMessageBox::Question,
                            tr("Parameter Range"),
                            message + QLatin1Char('\n')
                                + tr("Use it anyway?"),
                            QMessageBox::Yes | QMessageBox::No,
                            QMessageBox::No);
                        if (!answer.ownerAlive || !guard
                            || revision != m_stateRevision) {
                            return;
                        }
                        if (answer.button == QMessageBox::Yes
                            && stageNumericValue(
                                name, value, true, &error)) {
                            setStatus(tr("%1 staged outside its documented range.")
                                          .arg(name));
                            return;
                        }
                    }
                    if (!error.isEmpty()) {
                        setStatus(error.startsWith(
                                      QStringLiteral("out-of-range:"))
                                      ? error.mid(QStringLiteral(
                                            "out-of-range:").size())
                                      : error,
                                  true);
                    }
                    return;
                }
                setStatus(m_stagedValues.contains(name)
                              ? tr("%1 staged. Write Params is required.")
                                    .arg(name)
                              : tr("%1 matches the live value.").arg(name));
            },
            m_table));
    m_table->sortByColumn(Command, Qt::AscendingOrder);
    contentLayout->addWidget(m_table, 1);

    auto *rail = new QWidget(content);
    rail->setObjectName(QStringLiteral("tableLayoutPanel1"));
    rail->setFixedWidth(122);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(6, 6, 6, 3);
    railLayout->setSpacing(6);

    const auto addButton = [rail, railLayout](const QString &name,
                                               const QString &text) {
        auto *button = new QPushButton(text, rail);
        button->setObjectName(name);
        button->setFixedWidth(110);
        button->setMinimumHeight(20);
        railLayout->addWidget(button);
        return button;
    };
    m_loadButton = addButton(QStringLiteral("BUT_load"), tr("Load from file"));
    m_saveButton = addButton(QStringLiteral("BUT_save"), tr("Save to file"));
    railLayout->addSpacing(4);
    m_writeButton = addButton(
        QStringLiteral("BUT_writePIDS"), tr("Write Params"));
    m_refreshButton = addButton(
        QStringLiteral("BUT_rerequestparams"), tr("Refresh Params"));
    m_compareButton = addButton(
        QStringLiteral("BUT_compare"), tr("Compare Params"));

    auto *rawUnits = new QLabel(
        tr("All Units are in raw\nformat with no scaling"), rail);
    rawUnits->setObjectName(QStringLiteral("label1"));
    rawUnits->setAlignment(Qt::AlignCenter);
    rawUnits->setWordWrap(true);
    railLayout->addWidget(rawUnits);

    auto *searchLabel = new QLabel(tr("Search"), rail);
    searchLabel->setObjectName(QStringLiteral("label2"));
    railLayout->addWidget(searchLabel);
    m_search = new QLineEdit(rail);
    m_search->setObjectName(QStringLiteral("txt_search"));
    m_search->setFixedSize(110, 20);
    railLayout->addWidget(m_search);
    m_modified = new QCheckBox(tr("Modified"), rail);
    m_modified->setObjectName(QStringLiteral("chk_modified"));
    railLayout->addWidget(m_modified);
    auto *noneDefault = new QCheckBox(tr("None Default"), rail);
    noneDefault->setObjectName(QStringLiteral("chk_none_default"));
    noneDefault->setVisible(false);
    railLayout->addWidget(noneDefault);
    railLayout->addStretch(1);
    m_status = new QLabel(rail);
    m_status->setObjectName(QStringLiteral("rawParamsStatus"));
    m_status->setWordWrap(true);
    m_status->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
    railLayout->addWidget(m_status);
    contentLayout->addWidget(rail);
    m_splitter->addWidget(content);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);

    const QVariantList savedSizes = QSettings().value(kSplitterKey).toList();
    if (savedSizes.size() == 2) {
        m_splitter->setSizes({savedSizes.at(0).toInt(),
                              savedSizes.at(1).toInt()});
    } else {
        m_splitter->setSizes({180, 806});
    }
    m_treeCollapsed = QSettings().value(kTreeCollapsedKey, false).toBool();
    m_treePanel->setVisible(!m_treeCollapsed);
    m_collapseButton->setText(m_treeCollapsed
                                  ? QStringLiteral(">")
                                  : QStringLiteral("<"));

    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(500);
    connect(m_model, &QStandardItemModel::itemChanged,
            this, &ConfigRawParams::sourceItemChanged);
    connect(m_search, &QLineEdit::textChanged,
            m_searchDebounce, QOverload<>::of(&QTimer::start));
    connect(m_searchDebounce, &QTimer::timeout,
            this, &ConfigRawParams::applySearchFilter);
    connect(m_modified, &QCheckBox::toggled, this, [this](bool value) {
        static_cast<RawParameterProxyModel *>(m_proxy)
            ->setModifiedOnly(value);
    });
    connect(m_tree, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem *current) {
        static_cast<RawParameterProxyModel *>(m_proxy)->setPrefix(
            current ? current->data(0, Qt::UserRole).toString()
                    : QString());
    });
    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
        QVariantList values;
        for (int size : m_splitter->sizes()) {
            values.append(size);
        }
        QSettings().setValue(kSplitterKey, values);
    });
    connect(m_collapseButton, &QToolButton::clicked,
            this, &ConfigRawParams::toggleTree);
    connect(m_loadButton, &QPushButton::clicked,
            this, &ConfigRawParams::loadFromFile);
    connect(m_saveButton, &QPushButton::clicked,
            this, &ConfigRawParams::saveToFile);
    connect(m_compareButton, &QPushButton::clicked,
            this, &ConfigRawParams::compareWithFile);
    connect(m_writeButton, &QPushButton::clicked,
            this, &ConfigRawParams::writeStagedParameters);
    connect(m_refreshButton, &QPushButton::clicked, this, [this]() {
        emit refreshRequested(m_componentId);
    });
    m_saveShortcut = new QShortcut(QKeySequence::Save, this);
    m_saveShortcut->setObjectName(QStringLiteral("writeParamsShortcut"));
    connect(m_saveShortcut, &QShortcut::activated,
            this, &ConfigRawParams::writeStagedParameters);
}

void ConfigRawParams::setCatalog(
    const ParameterMetaDataCatalog &catalog, bool enforceMetadataRanges)
{
    ++m_stateRevision;
    invalidateOpenBitmaskEditors();
    // An explicit exception is valid only for the exact metadata range the
    // user reviewed. A new catalog must ask again against its own schema.
    m_rangeOverrides.clear();
    m_catalog = catalog;
    m_enforceMetadataRanges = enforceMetadataRanges;
    const QStringList rejected = revalidateStagedValues();
    rebuildRows();
    if (!rejected.isEmpty()) {
        setStatus(tr("Metadata changed; rejected staged values: %1")
                      .arg(rejected.join(QStringLiteral(", "))),
                  true);
    }
}

void ConfigRawParams::setParameterSnapshot(
    const QList<ParameterRecord> &records, int preferredComponent)
{
    ++m_stateRevision;
    invalidateOpenBitmaskEditors();
    QList<int> components;
    for (const ParameterRecord &record : records) {
        if (!components.contains(record.key.componentId)) {
            components.append(record.key.componentId);
        }
    }
    std::sort(components.begin(), components.end());
    const int selectedComponent = components.contains(preferredComponent)
        ? preferredComponent : components.value(0, preferredComponent);
    if (selectedComponent != m_componentId) {
        m_stagedValues.clear();
        m_rangeOverrides.clear();
        ++m_stagedRevision;
    }
    m_componentId = selectedComponent;

    QHash<QString, ParameterRecord> replacement;
    for (const ParameterRecord &record : records) {
        if (record.key.componentId == m_componentId) {
            ParameterRecord normalized = record;
            normalized.key.name = normalizedName(record.key.name);
            replacement.insert(normalized.key.name, normalized);
        }
    }
    m_liveRecords = replacement;
    const QStringList rejected = revalidateStagedValues();
    loadFavorites();
    rebuildRows();
    if (!rejected.isEmpty()) {
        setStatus(tr("The refreshed schema rejected staged values: %1")
                      .arg(rejected.join(QStringLiteral(", "))),
                  true);
    } else {
        setStatus(m_liveRecords.isEmpty()
                      ? tr("No committed parameters are available.")
                      : tr("%1 parameters loaded.").arg(m_liveRecords.size()));
    }
}

void ConfigRawParams::setConnected(bool connected)
{
    if (m_connected == connected) {
        return;
    }
    ++m_stateRevision;
    invalidateOpenBitmaskEditors();
    m_connected = connected;
    if (!m_connected && (m_activeBatchId != 0 || m_submissionPending)) {
        m_activeBatchId = 0;
        m_submissionPending = false;
        setStatus(tr("Parameter writing stopped because the target disconnected."),
                  true);
    }
    updateActionState();
}

int ConfigRawParams::visibleParameterCount() const
{
    return m_proxy ? m_proxy->rowCount() : 0;
}

QVariant ConfigRawParams::stagedValue(const QString &name) const
{
    return m_stagedValues.value(normalizedName(name));
}

QString ConfigRawParams::lastStatusText() const
{
    return m_status ? m_status->text() : QString();
}

QString ConfigRawParams::normalizedName(const QString &name)
{
    return name.trimmed().toUpper();
}

QString ConfigRawParams::formattedValue(const QVariant &value)
{
    bool ok = false;
    const double numeric = value.toDouble(&ok);
    return ok && std::isfinite(numeric)
        ? QLocale::c().toString(numeric, 'g', 15)
        : value.toString();
}

bool ConfigRawParams::equivalent(
    const QVariant &left, const QVariant &right, ParameterType type)
{
    return ParameterCodec::valuesEqual(left, right, type);
}

QVariant ConfigRawParams::typedValue(
    double value, ParameterType type, bool *ok)
{
    bool valid = std::isfinite(value);
    QVariant result;
    const bool integer = ParameterCodec::isInteger(type);
    if (valid && integer && std::trunc(value) != value) {
        valid = false;
    }
    if (valid) {
        switch (type) {
        case ParameterType::UInt8:
            valid = value >= 0.0 && value <= 255.0;
            if (valid) result = QVariant::fromValue(quint32(value));
            break;
        case ParameterType::Int8:
            valid = value >= -128.0 && value <= 127.0;
            if (valid) result = QVariant::fromValue(qint32(value));
            break;
        case ParameterType::UInt16:
            valid = value >= 0.0 && value <= 65535.0;
            if (valid) result = QVariant::fromValue(quint32(value));
            break;
        case ParameterType::Int16:
            valid = value >= -32768.0 && value <= 32767.0;
            if (valid) result = QVariant::fromValue(qint32(value));
            break;
        case ParameterType::UInt32:
            valid = value >= 0.0 && value <= 4294967295.0;
            if (valid) result = QVariant::fromValue(quint32(value));
            break;
        case ParameterType::Int32:
            valid = value >= -2147483648.0 && value <= 2147483647.0;
            if (valid) result = QVariant::fromValue(qint32(value));
            break;
        case ParameterType::Real32:
            valid = std::isfinite(static_cast<float>(value));
            if (valid) result = QVariant::fromValue(static_cast<float>(value));
            break;
        case ParameterType::UInt64:
        case ParameterType::Int64:
        case ParameterType::Real64:
        case ParameterType::Unknown:
            valid = false;
            break;
        }
    }
    if (ok) {
        *ok = valid;
    }
    return valid ? result : QVariant();
}

bool ConfigRawParams::stageParameter(
    const QString &name, const QString &expression, bool allowOutOfRange,
    QString *error)
{
    double value = 0.0;
    QString parserError;
    ExpressionParser parser(expression);
    if (!parser.evaluate(&value, &parserError)) {
        if (error) {
            *error = parserError;
        }
        return false;
    }
    static const QRegularExpression reverseParameter(
        QStringLiteral("^(?:RC|HS)\\d+_REV$"),
        QRegularExpression::CaseInsensitiveOption);
    const QString normalized = normalizedName(name);
    if (value == 0.0 && reverseParameter.match(normalized).hasMatch()) {
        value = -1.0;
    }
    return stageNumericValue(normalized, value, allowOutOfRange, error);
}

bool ConfigRawParams::stageNumericValue(
    const QString &name, double value, bool allowOutOfRange, QString *error)
{
    const QString normalized = normalizedName(name);
    const auto record = m_liveRecords.constFind(normalized);
    if (record == m_liveRecords.constEnd()) {
        if (error) {
            *error = tr("Parameter %1 is not present on this vehicle.")
                         .arg(normalized);
        }
        return false;
    }
    const ParameterMetaData metadata = m_catalog.value(normalized);
    if (m_catalog.contains(normalized) && metadata.readOnly) {
        if (error) {
            *error = tr("Parameter %1 is read-only.").arg(normalized);
        }
        return false;
    }
    if (m_enforceMetadataRanges && metadata.hasRange
        && (value < metadata.minimum || value > metadata.maximum)
        && !allowOutOfRange) {
        if (error) {
            *error = QStringLiteral("out-of-range:")
                + tr("%1 is outside the documented range %2.")
                      .arg(formattedValue(value), metadata.rangeText);
        }
        return false;
    }
    bool typed = false;
    const QVariant converted = typedValue(value, record->type, &typed);
    if (!typed) {
        if (error) {
            *error = tr("%1 cannot be represented by this parameter's MAVLink type.")
                         .arg(formattedValue(value));
        }
        return false;
    }
    if (equivalent(record->value, converted, record->type)) {
        m_stagedValues.remove(normalized);
        m_rangeOverrides.remove(normalized);
    } else {
        m_stagedValues.insert(normalized, converted);
        const bool outsideRange = m_enforceMetadataRanges
            && metadata.hasRange
            && (value < metadata.minimum || value > metadata.maximum);
        if (outsideRange && allowOutOfRange) {
            m_rangeOverrides.insert(normalized);
        } else {
            m_rangeOverrides.remove(normalized);
        }
    }
    ++m_stagedRevision;
    updateRow(normalized);
    updateActionState();
    if (error) {
        error->clear();
    }
    return true;
}

void ConfigRawParams::clearStagedChanges()
{
    if (m_stagedValues.isEmpty()) {
        return;
    }
    const QStringList names = m_stagedValues.keys();
    m_stagedValues.clear();
    m_rangeOverrides.clear();
    ++m_stagedRevision;
    for (const QString &name : names) {
        updateRow(name);
    }
    updateActionState();
}

void ConfigRawParams::invalidateOpenBitmaskEditors()
{
    const QList<QDialog *> editors = findChildren<QDialog *>(
        QStringLiteral("ConfigRawParamsBitmaskEditor"));
    for (QDialog *editor : editors) {
        editor->reject();
    }
}

QStringList ConfigRawParams::revalidateStagedValues()
{
    QStringList rejected;
    for (auto iterator = m_stagedValues.begin();
         iterator != m_stagedValues.end();) {
        const QString name = iterator.key();
        const auto live = m_liveRecords.constFind(name);
        QString reason;
        QVariant converted;
        if (live == m_liveRecords.constEnd()) {
            reason = tr("not present");
        } else {
            const ParameterMetaData metadata = m_catalog.value(name);
            if (m_catalog.contains(name) && metadata.readOnly) {
                reason = tr("read-only");
            } else {
                bool numericOk = false;
                const double numeric = iterator.value().toDouble(&numericOk);
                bool typedOk = false;
                if (numericOk && std::isfinite(numeric)) {
                    converted = typedValue(numeric, live->type, &typedOk);
                }
                if (!typedOk) {
                    reason = tr("incompatible type");
                } else if (m_enforceMetadataRanges && metadata.hasRange
                           && !m_rangeOverrides.contains(name)
                           && (numeric < metadata.minimum
                               || numeric > metadata.maximum)) {
                    reason = tr("outside range %1").arg(metadata.rangeText);
                }
            }
        }

        if (!reason.isEmpty()) {
            rejected.append(QStringLiteral("%1 (%2)").arg(name, reason));
            m_rangeOverrides.remove(name);
            iterator = m_stagedValues.erase(iterator);
        } else if (equivalent(live->value, converted, live->type)) {
            m_rangeOverrides.remove(name);
            iterator = m_stagedValues.erase(iterator);
        } else {
            iterator.value() = converted;
            ++iterator;
        }
    }
    return rejected;
}

void ConfigRawParams::rebuildRows()
{
    m_updatingModel = true;
    m_model->removeRows(0, m_model->rowCount());
    m_sourceRows.clear();
    QStringList names = m_liveRecords.keys();
    std::sort(names.begin(), names.end(), [](const QString &left,
                                             const QString &right) {
        return naturalCompare(left, right) < 0;
    });
    for (const QString &name : names) {
        const ParameterRecord record = m_liveRecords.value(name);
        const ParameterMetaData metadata = m_catalog.value(name);
        const QVariant displayed = m_stagedValues.contains(name)
            ? m_stagedValues.value(name) : record.value;
        const QString options = parameterOptionsText(metadata);
        const int optionKind = metadata.isBitmask()
            ? BitmaskOptions
            : (metadata.isEnum() ? EnumOptions : NoOptions);
        QVariantList optionEntries;
        if (optionKind == BitmaskOptions) {
            for (const auto &bit : metadata.bitmaskValues) {
                optionEntries.append(QVariantMap{
                    {QStringLiteral("bit"), bit.first},
                    {QStringLiteral("label"), bit.second}
                });
            }
        } else if (optionKind == EnumOptions) {
            for (const ParameterMetaDataOption &option : metadata.values) {
                bool numeric = false;
                option.value.toDouble(&numeric);
                if (numeric) {
                    optionEntries.append(QVariantMap{
                        {QStringLiteral("value"), option.value},
                        {QStringLiteral("label"), option.label}
                    });
                }
            }
        }
        const QString defaultText = metadataField(
            metadata, QStringLiteral("Default"));

        QList<QStandardItem *> row;
        auto *command = new QStandardItem(name);
        command->setEditable(false);
        auto *value = new QStandardItem(formattedValue(displayed));
        value->setEditable(!(m_catalog.contains(name) && metadata.readOnly));
        auto *defaultItem = new QStandardItem(defaultText);
        defaultItem->setEditable(false);
        auto *units = new QStandardItem(metadata.units);
        units->setEditable(false);
        auto *optionsItem = new QStandardItem(options);
        optionsItem->setEditable(
            optionKind != NoOptions && !metadata.readOnly);
        optionsItem->setData(optionKind, OptionKindRole);
        optionsItem->setData(optionEntries, OptionEntriesRole);
        optionsItem->setData(displayed, DisplayedValueRole);
        optionsItem->setData(static_cast<int>(record.type),
                             ParameterTypeRole);
        optionsItem->setData(metadata.readOnly, ReadOnlyRole);
        auto *description = new QStandardItem(metadata.description);
        description->setEditable(false);
        auto *favorite = new QStandardItem;
        favorite->setEditable(false);
        favorite->setCheckable(true);
        favorite->setCheckState(m_favorites.contains(name)
                                    ? Qt::Checked : Qt::Unchecked);
        row << command << value << defaultItem << units << optionsItem
            << description << favorite;
        const bool staged = m_stagedValues.contains(name);
        const QString searchText = parameterSearchText(
            name, formattedValue(displayed), defaultText, metadata);
        for (QStandardItem *item : row) {
            item->setData(name, NameRole);
            item->setBackground(staged ? QColor(QStringLiteral("#008000"))
                                       : QColor(QStringLiteral("#434445")));
            item->setForeground(QColor(Qt::white));
        }
        command->setData(searchText, SearchRole);
        command->setData(staged, ModifiedRole);
        command->setData(m_favorites.contains(name), FavoriteRole);
        value->setData(displayed.toDouble(), NumericRole);
        if (!defaultText.isEmpty()) {
            bool defaultOk = false;
            const double numericDefault = QLocale::c().toDouble(
                defaultText, &defaultOk);
            if (defaultOk) {
                defaultItem->setData(numericDefault, NumericRole);
            }
        }
        const QString tooltip = metadata.readOnly
            ? tr("Read-only parameter")
            : metadata.description;
        value->setToolTip(tooltip);
        command->setToolTip(metadata.title.isEmpty()
                                ? metadata.description
                                : metadata.title + QStringLiteral("\n")
                                      + metadata.description);
        const int rowIndex = m_model->rowCount();
        m_model->appendRow(row);
        m_sourceRows.insert(name, rowIndex);
    }
    m_updatingModel = false;
    rebuildPrefixTree();
    static_cast<RawParameterProxyModel *>(m_proxy)->refreshRowState();
    updateActionState();
}

void ConfigRawParams::updateRow(const QString &name)
{
    const int row = m_sourceRows.value(name, -1);
    const auto record = m_liveRecords.constFind(name);
    if (row < 0 || record == m_liveRecords.constEnd()) {
        return;
    }
    const bool staged = m_stagedValues.contains(name);
    const QVariant displayed = staged ? m_stagedValues.value(name)
                                       : record->value;
    m_updatingModel = true;
    if (QStandardItem *value = m_model->item(row, Value)) {
        value->setText(formattedValue(displayed));
        value->setData(displayed.toDouble(), NumericRole);
    }
    if (QStandardItem *options = m_model->item(row, Options)) {
        options->setData(displayed, DisplayedValueRole);
        options->setData(static_cast<int>(record->type), ParameterTypeRole);
    }
    const ParameterMetaData metadata = m_catalog.value(name);
    const QString searchText = parameterSearchText(
        name, formattedValue(displayed),
        metadataField(metadata, QStringLiteral("Default")), metadata);
    QStandardItem *command = m_model->item(row, Command);
    command->setData(searchText, SearchRole);
    command->setData(staged, ModifiedRole);
    command->setData(m_favorites.contains(name), FavoriteRole);
    for (int column = 0; column < ColumnCount; ++column) {
        if (QStandardItem *item = m_model->item(row, column)) {
            item->setBackground(staged
                                    ? QColor(QStringLiteral("#008000"))
                                    : QColor(QStringLiteral("#434445")));
        }
    }
    m_updatingModel = false;
    static_cast<RawParameterProxyModel *>(m_proxy)->refreshRowState();
}

void ConfigRawParams::rebuildPrefixTree()
{
    const QString selectedPrefix = m_tree->currentItem()
        ? m_tree->currentItem()->data(0, Qt::UserRole).toString()
        : QString();
    m_tree->clear();
    auto *all = new QTreeWidgetItem(m_tree, QStringList{tr("All")});
    all->setData(0, Qt::UserRole, QString());
    QHash<QString, QTreeWidgetItem *> items;
    items.insert(QString(), all);
    QStringList names = m_liveRecords.keys();
    std::sort(names.begin(), names.end(), [](const QString &left,
                                             const QString &right) {
        return naturalCompare(left, right) < 0;
    });
    QTreeWidgetItem *selection = all;
    for (const QString &name : names) {
        const QStringList parts = name.split(QLatin1Char('_'));
        QString prefix;
        QTreeWidgetItem *parent = all;
        for (const QString &part : parts) {
            prefix = prefix.isEmpty() ? part
                                      : prefix + QLatin1Char('_') + part;
            QTreeWidgetItem *item = items.value(prefix, nullptr);
            if (!item) {
                item = new QTreeWidgetItem(parent, QStringList{part});
                item->setData(0, Qt::UserRole, prefix);
                items.insert(prefix, item);
            }
            parent = item;
            if (prefix == selectedPrefix) {
                selection = item;
            }
        }
    }
    m_tree->setCurrentItem(selection);
    all->setExpanded(true);
}

void ConfigRawParams::sourceItemChanged(QStandardItem *item)
{
    if (m_updatingModel || !item) {
        return;
    }
    const QString name = item->data(NameRole).toString();
    if (item->column() == Favorite) {
        if (item->checkState() == Qt::Checked) {
            m_favorites.insert(name);
        } else {
            m_favorites.remove(name);
        }
        saveFavorites();
        if (QStandardItem *command = m_model->item(item->row(), Command)) {
            command->setData(m_favorites.contains(name), FavoriteRole);
        }
        static_cast<RawParameterProxyModel *>(m_proxy)->refreshRowState();
        return;
    }
    if (item->column() != Value) {
        return;
    }
    const QString enteredText = item->text();
    const qulonglong stateRevision = m_stateRevision;
    QString error;
    if (!stageParameter(name, enteredText, false, &error)) {
        bool retryOutsideRange = false;
        if (error.startsWith(QStringLiteral("out-of-range:"))) {
            const QString message = error.mid(
                QStringLiteral("out-of-range:").size());
            const QPointer<ConfigRawParams> guard(this);
            const SafeMessageResult answer = showSafeMessage(
                this, QMessageBox::Question, tr("Parameter Range"),
                message + QLatin1Char('\n') + tr("Use it anyway?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            retryOutsideRange = answer.button == QMessageBox::Yes;
            if (!answer.ownerAlive || !guard
                || stateRevision != m_stateRevision
                || !m_liveRecords.contains(name)) {
                return;
            }
            if (retryOutsideRange
                && stageParameter(name, enteredText, true, &error)) {
                setStatus(tr("%1 staged outside its documented range.")
                              .arg(name));
                return;
            }
        }
        Q_UNUSED(retryOutsideRange)
        setStatus(error.startsWith(QStringLiteral("out-of-range:"))
                      ? error.mid(QStringLiteral("out-of-range:").size())
                      : error,
                  true);
        updateRow(name);
        return;
    }
    setStatus(m_stagedValues.contains(name)
                  ? tr("%1 staged. Write Params is required.").arg(name)
                  : tr("%1 matches the live value.").arg(name));
}

void ConfigRawParams::applySearchFilter()
{
    static_cast<RawParameterProxyModel *>(m_proxy)->setSearchText(
        m_search->text());
}

void ConfigRawParams::updateActionState()
{
    const bool hasParameters = !m_liveRecords.isEmpty();
    const bool canWrite = m_connected && !m_stagedValues.isEmpty()
        && m_activeBatchId == 0 && !m_submissionPending;
    m_loadButton->setEnabled(hasParameters);
    m_saveButton->setEnabled(hasParameters);
    m_compareButton->setEnabled(hasParameters);
    m_refreshButton->setEnabled(m_connected);
    m_writeButton->setEnabled(canWrite);
    if (m_saveShortcut) {
        m_saveShortcut->setEnabled(canWrite);
    }
    m_table->setEnabled(
        hasParameters && m_activeBatchId == 0 && !m_submissionPending);
}

void ConfigRawParams::setStatus(const QString &message, bool error)
{
    m_status->setStyleSheet(error
        ? QStringLiteral("color:#ff6b6b;")
        : QStringLiteral("color:#ffffff;"));
    m_status->setText(message);
}

QMap<QString, QVariant> ConfigRawParams::displayedValues() const
{
    QMap<QString, QVariant> result;
    for (auto iterator = m_liveRecords.constBegin();
         iterator != m_liveRecords.constEnd(); ++iterator) {
        result.insert(iterator.key(), m_stagedValues.contains(iterator.key())
                                      ? m_stagedValues.value(iterator.key())
                                      : iterator.value().value);
    }
    return result;
}

bool ConfigRawParams::readParameterFile(
    const QString &path, QMap<QString, double> *values)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        showSafeMessage(
            this, QMessageBox::Critical, tr("Parameter File"),
            tr("Unable to open %1: %2").arg(path, file.errorString()));
        return false;
    }
    if (file.size() > 16 * 1024 * 1024) {
        showSafeMessage(this, QMessageBox::Critical,
                        tr("Parameter File"),
                        tr("The parameter file is larger than 16 MiB."));
        return false;
    }
    QString error;
    int errorLine = 0;
    if (!ConfigRawParamsFileCodec::load(
            &file, values, &error, &errorLine)) {
        Q_UNUSED(errorLine)
        showSafeMessage(this, QMessageBox::Critical,
                        tr("Parameter File"), error);
        return false;
    }
    return true;
}

void ConfigRawParams::loadFromFile()
{
    const qulonglong stateRevision = m_stateRevision;
    const SafeFileDialogResult selection = showSafeFileDialog(
        this, tr("Load Parameters"),
        tr("Parameter File (*.param *.parm);;All Files (*)"),
        QFileDialog::AcceptOpen);
    if (!selection.ownerAlive || stateRevision != m_stateRevision
        || selection.path.isEmpty()) {
        return;
    }
    const QString path = selection.path;
    QMap<QString, double> values;
    if (!readParameterFile(path, &values)) {
        return;
    }
    QStringList unknown;
    QStringList rejected;
    int staged = 0;
    for (auto iterator = values.constBegin(); iterator != values.constEnd();
         ++iterator) {
        if (!m_liveRecords.contains(iterator.key())) {
            unknown.append(iterator.key());
            continue;
        }
        QString error;
        bool accepted = stageNumericValue(
            iterator.key(), iterator.value(), false, &error);
        if (!accepted
            && error.startsWith(QStringLiteral("out-of-range:"))) {
            const QString warning = error.mid(
                QStringLiteral("out-of-range:").size());
            const SafeMessageResult answer = showSafeMessage(
                this, QMessageBox::Question, tr("Parameter Range"),
                tr("%1: %2\nUse it anyway?")
                    .arg(iterator.key(), warning),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (!answer.ownerAlive
                || stateRevision != m_stateRevision) {
                return;
            }
            if (answer.button == QMessageBox::Yes) {
                accepted = stageNumericValue(
                    iterator.key(), iterator.value(), true, &error);
            }
        }
        if (!accepted) {
            rejected.append(QStringLiteral("%1 (%2)")
                                .arg(iterator.key(),
                                     error.startsWith(
                                         QStringLiteral("out-of-range:"))
                                         ? error.mid(QStringLiteral(
                                               "out-of-range:").size())
                                         : error));
        } else if (m_stagedValues.contains(iterator.key())) {
            ++staged;
        }
    }
    if (!reportUnknownParameters(
            unknown, tr("Not present on this vehicle"))) {
        return;
    }
    if (!rejected.isEmpty()) {
        if (!reportUnknownParameters(rejected, tr("Rejected values"))) {
            return;
        }
    }
    setStatus(rejected.isEmpty()
                  ? tr("%1 values staged from file. Click Write Params to transmit.")
                        .arg(staged)
                  : tr("%1 values staged; %2 rejected.")
                        .arg(staged).arg(rejected.size()),
              !rejected.isEmpty());
}

void ConfigRawParams::saveToFile()
{
    const qulonglong stateRevision = m_stateRevision;
    const SafeFileDialogResult selection = showSafeFileDialog(
        this, tr("Save Parameters"),
        tr("Parameter File (*.param *.parm);;All Files (*)"),
        QFileDialog::AcceptSave);
    if (!selection.ownerAlive || stateRevision != m_stateRevision
        || selection.path.isEmpty()) {
        return;
    }
    QString path = selection.path;
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".param");
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        showSafeMessage(
            this, QMessageBox::Critical, tr("Parameter File"),
            tr("Unable to open %1: %2").arg(path, file.errorString()));
        return;
    }
    QString error;
    if (!ConfigRawParamsFileCodec::save(
            &file, displayedValues(), &error)
        || !file.commit()) {
        if (error.isEmpty()) {
            error = file.errorString();
        }
        showSafeMessage(this, QMessageBox::Critical,
                        tr("Parameter File"), error);
        return;
    }
    setStatus(tr("Saved %1 parameters to %2.")
                  .arg(m_liveRecords.size()).arg(path));
}

void ConfigRawParams::compareWithFile()
{
    const qulonglong stateRevision = m_stateRevision;
    const SafeFileDialogResult selection = showSafeFileDialog(
        this, tr("Compare Parameters"),
        tr("Parameter File (*.param *.parm);;All Files (*)"),
        QFileDialog::AcceptOpen);
    if (!selection.ownerAlive || stateRevision != m_stateRevision
        || selection.path.isEmpty()) {
        return;
    }
    reviewAndStageParameterFile(selection.path);
}

void ConfigRawParams::reviewAndStageParameterFile(const QString &path)
{
    const qulonglong stateRevision = m_stateRevision;
    QMap<QString, double> values;
    if (!readParameterFile(path, &values)) {
        return;
    }
    QStringList unknown;
    QStringList conversionRejected;
    struct Difference { QString name; double fileValue = 0.0; };
    QList<Difference> differences;
    for (auto iterator = values.constBegin(); iterator != values.constEnd();
         ++iterator) {
        const auto live = m_liveRecords.constFind(iterator.key());
        if (live == m_liveRecords.constEnd()) {
            unknown.append(iterator.key());
            continue;
        }
        bool typed = false;
        const QVariant imported = typedValue(
            iterator.value(), live->type, &typed);
        if (!typed) {
            conversionRejected.append(
                QStringLiteral("%1 (%2)")
                    .arg(iterator.key(),
                         tr("value cannot be represented by the parameter's MAVLink type")));
        } else if (!equivalent(live->value, imported, live->type)) {
            differences.append({iterator.key(), iterator.value()});
        }
    }
    if (differences.isEmpty()) {
        if (!reportUnknownParameters(
                unknown, tr("Not present on this vehicle"))) {
            return;
        }
        if (!reportUnknownParameters(
                conversionRejected, tr("Rejected values"))) {
            return;
        }
        if (!conversionRejected.isEmpty()) {
            setStatus(tr("No values staged; %1 rejected.")
                          .arg(conversionRejected.size()),
                      true);
            return;
        }
        showSafeMessage(
            this, QMessageBox::Information, tr("Compare Params"),
            tr("No shared parameters differ from the live vehicle."));
        return;
    }

    QDialog dialog(nullptr);
    const QPointer<ConfigRawParams> guard(this);
    connect(this, &QObject::destroyed, &dialog, &QDialog::reject);
    dialog.setWindowTitle(tr("Compare Params"));
    dialog.resize(620, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(differences.size(), 4, &dialog);
    table->setHorizontalHeaderLabels({
        tr("Parameter"), tr("Current Value"), tr("File Value"), tr("Use")
    });
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->verticalHeader()->hide();
    for (int row = 0; row < differences.size(); ++row) {
        const Difference difference = differences.at(row);
        table->setItem(row, 0, new QTableWidgetItem(difference.name));
        table->setItem(row, 1, new QTableWidgetItem(formattedValue(
            m_liveRecords.value(difference.name).value)));
        table->setItem(row, 2, new QTableWidgetItem(
            formattedValue(difference.fileValue)));
        auto *use = new QTableWidgetItem;
        use->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        use->setCheckState(Qt::Checked);
        table->setItem(row, 3, use);
    }
    layout->addWidget(table);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Continue"));
    connect(buttons, &QDialogButtonBox::accepted,
            &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const int dialogResult = dialog.exec();
    if (!guard || stateRevision != m_stateRevision
        || dialogResult != QDialog::Accepted) {
        return;
    }
    int staged = 0;
    QStringList rejected = conversionRejected;
    for (int row = 0; row < differences.size(); ++row) {
        if (table->item(row, 3)->checkState() != Qt::Checked) {
            continue;
        }
        const Difference difference = differences.at(row);
        QString error;
        bool accepted = stageNumericValue(
            difference.name, difference.fileValue, false, &error);
        if (!accepted
            && error.startsWith(QStringLiteral("out-of-range:"))) {
            const QString warning = error.mid(
                QStringLiteral("out-of-range:").size());
            const SafeMessageResult answer = showSafeMessage(
                this, QMessageBox::Question, tr("Parameter Range"),
                tr("%1: %2\nUse it anyway?")
                    .arg(difference.name, warning),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (!answer.ownerAlive || !guard
                || stateRevision != m_stateRevision) {
                return;
            }
            if (answer.button == QMessageBox::Yes) {
                accepted = stageNumericValue(
                    difference.name, difference.fileValue, true, &error);
            }
        }
        if (accepted) {
            ++staged;
        } else {
            const QString reason = error.startsWith(
                QStringLiteral("out-of-range:"))
                ? error.mid(QStringLiteral("out-of-range:").size())
                : (error.isEmpty() ? tr("rejected") : error);
            rejected.append(QStringLiteral("%1 (%2)")
                                .arg(difference.name, reason));
        }
    }
    if (!reportUnknownParameters(
            unknown, tr("Not present on this vehicle"))) {
        return;
    }
    if (!reportUnknownParameters(rejected, tr("Rejected values"))) {
        return;
    }
    setStatus(rejected.isEmpty()
                  ? tr("%1 compared values staged. Click Write Params to transmit.")
                        .arg(staged)
                  : tr("%1 compared values staged; %2 rejected.")
                        .arg(staged).arg(rejected.size()),
              !rejected.isEmpty());
}

bool ConfigRawParams::reportUnknownParameters(
    const QStringList &names, const QString &prefix)
{
    if (names.isEmpty()) {
        return true;
    }
    const QString shown = names.mid(0, 20).join(QStringLiteral(", "));
    const QString suffix = names.size() > 20
        ? tr(" and %1 more").arg(names.size() - 20) : QString();
    const qulonglong stateRevision = m_stateRevision;
    const SafeMessageResult result = showSafeMessage(
        this, QMessageBox::Warning, tr("Parameter File"),
        QStringLiteral("%1: %2%3").arg(prefix, shown, suffix));
    return result.ownerAlive && stateRevision == m_stateRevision;
}

void ConfigRawParams::writeStagedParameters()
{
    if (!m_connected) {
        setStatus(tr("Connect to the selected target before writing parameters."),
                  true);
        return;
    }
    if (m_activeBatchId != 0 || m_submissionPending) {
        setStatus(tr("A parameter batch is already in progress."), true);
        return;
    }
    const QStringList rejected = revalidateStagedValues();
    rebuildRows();
    if (!rejected.isEmpty()) {
        setStatus(tr("Review rejected staged values before writing: %1")
                      .arg(rejected.join(QStringLiteral(", "))),
                  true);
        return;
    }
    if (m_stagedValues.isEmpty()) {
        showSafeMessage(this, QMessageBox::Information,
                        tr("Write Params"),
                        tr("No parameters were changed."));
        return;
    }
    QString question;
    if (m_stagedValues.size() <= 20) {
        QStringList details;
        for (auto iterator = m_stagedValues.constBegin();
             iterator != m_stagedValues.constEnd(); ++iterator) {
            details.append(QStringLiteral("%1: %2 → %3")
                .arg(iterator.key(),
                     formattedValue(m_liveRecords.value(
                         iterator.key()).value),
                     formattedValue(iterator.value())));
        }
        question = tr("Write these parameters?\n\n%1")
                       .arg(details.join(QLatin1Char('\n')));
    } else {
        question = tr("Write %1 changed parameters?")
                       .arg(m_stagedValues.size());
    }
    const qulonglong stateRevision = m_stateRevision;
    const qulonglong stagedRevision = m_stagedRevision;
    const QPointer<ConfigRawParams> guard(this);
    const SafeMessageResult answer = showSafeMessage(
        this, QMessageBox::Question, tr("Write Params"), question,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (!answer.ownerAlive || !guard
        || stateRevision != m_stateRevision
        || stagedRevision != m_stagedRevision
        || answer.button != QMessageBox::Yes) {
        return;
    }
    QVariantList changes;
    for (auto iterator = m_stagedValues.constBegin();
         iterator != m_stagedValues.constEnd(); ++iterator) {
        changes.append(QVariantMap{
            {QStringLiteral("name"), iterator.key()},
            {QStringLiteral("value"), iterator.value()}
        });
    }
    setStatus(tr("Submitting %1 parameter writes…").arg(changes.size()));
    m_submissionPending = true;
    updateActionState();
    emit writeRequested(m_componentId, changes);
}

void ConfigRawParams::toggleTree()
{
    m_treeCollapsed = !m_treeCollapsed;
    m_treePanel->setVisible(!m_treeCollapsed);
    m_collapseButton->setText(m_treeCollapsed
                                  ? QStringLiteral(">")
                                  : QStringLiteral("<"));
    QSettings().setValue(kTreeCollapsedKey, m_treeCollapsed);
}

void ConfigRawParams::parameterChanged(
    int componentId, const QString &name, const QVariant &value)
{
    if (componentId != m_componentId) {
        return;
    }
    const QString normalized = normalizedName(name);
    auto record = m_liveRecords.find(normalized);
    if (record == m_liveRecords.end()) {
        return;
    }
    ++m_stateRevision;
    invalidateOpenBitmaskEditors();
    record->value = value;
    if (m_stagedValues.contains(normalized)
        && equivalent(value, m_stagedValues.value(normalized),
                      record->type)) {
        m_stagedValues.remove(normalized);
        m_rangeOverrides.remove(normalized);
        ++m_stagedRevision;
    }
    updateRow(normalized);
    updateActionState();
}

void ConfigRawParams::parameterWriteAcknowledged(
    int componentId, const QString &name, const QVariant &value, int type)
{
    Q_UNUSED(type)
    if (componentId != m_componentId) {
        return;
    }
    parameterChanged(componentId, name, value);
    setStatus(tr("%1 acknowledged.").arg(normalizedName(name)));
}

void ConfigRawParams::parameterWriteFailed(
    qulonglong transactionId, qulonglong batchId, int componentId,
    const QString &name, int reason, const QString &message)
{
    Q_UNUSED(transactionId)
    Q_UNUSED(reason)
    if (componentId != m_componentId || m_activeBatchId == 0
        || batchId != m_activeBatchId) {
        return;
    }
    setStatus(tr("%1 failed: %2").arg(normalizedName(name), message), true);
}

void ConfigRawParams::parameterWriteCancelled(
    qulonglong transactionId, qulonglong batchId, int componentId,
    const QString &name)
{
    Q_UNUSED(transactionId)
    if (componentId != m_componentId || m_activeBatchId == 0
        || batchId != m_activeBatchId) {
        return;
    }
    setStatus(tr("%1 write was cancelled.").arg(normalizedName(name)), true);
}

void ConfigRawParams::parameterBatchSubmitted(
    qulonglong batchId, int total)
{
    if (!m_submissionPending || batchId == 0) {
        return;
    }
    m_submissionPending = false;
    m_activeBatchId = batchId;
    setStatus(tr("Writing 0 of %1 parameters…").arg(total));
    updateActionState();
}

void ConfigRawParams::parameterBatchProgress(
    qulonglong batchId, int completed, int total,
    int succeeded, int failed)
{
    if (m_activeBatchId == 0 || batchId != m_activeBatchId) {
        return;
    }
    setStatus(tr("Writing %1 of %2 parameters (%3 succeeded, %4 failed)…")
                  .arg(completed).arg(total).arg(succeeded).arg(failed),
              failed > 0);
}

void ConfigRawParams::parameterBatchCompleted(
    qulonglong batchId, int succeeded, int failed)
{
    if (m_activeBatchId == 0 || batchId != m_activeBatchId) {
        return;
    }
    m_activeBatchId = 0;
    setStatus(tr("Parameter write complete: %1 succeeded, %2 failed.")
                  .arg(succeeded).arg(failed),
              failed > 0);
    updateActionState();
}

void ConfigRawParams::parameterWriteSubmissionFailed(const QString &reason)
{
    m_submissionPending = false;
    m_activeBatchId = 0;
    setStatus(reason.isEmpty() ? tr("The parameter batch was rejected.")
                               : reason,
              true);
    updateActionState();
}

void ConfigRawParams::parameterTargetChanged()
{
    ++m_stateRevision;
    invalidateOpenBitmaskEditors();
    m_submissionPending = false;
    m_activeBatchId = 0;
    m_liveRecords.clear();
    m_stagedValues.clear();
    m_rangeOverrides.clear();
    ++m_stagedRevision;
    m_sourceRows.clear();
    rebuildRows();
    setStatus(tr("The selected parameter target changed; waiting for its snapshot."));
}

void ConfigRawParams::loadFavorites()
{
    m_favorites.clear();
    const QString prefix = QString::number(m_componentId)
        + QLatin1Char(':');
    const QStringList stored = QSettings().value(kFavoritesKey).toStringList();
    for (const QString &entry : stored) {
        if (entry.startsWith(prefix)) {
            m_favorites.insert(normalizedName(entry.mid(prefix.size())));
        }
    }
}

void ConfigRawParams::saveFavorites() const
{
    const QString prefix = QString::number(m_componentId)
        + QLatin1Char(':');
    QStringList stored = QSettings().value(kFavoritesKey).toStringList();
    for (auto iterator = stored.begin(); iterator != stored.end();) {
        if (iterator->startsWith(prefix)) {
            iterator = stored.erase(iterator);
        } else {
            ++iterator;
        }
    }
    QStringList favorites = m_favorites.values();
    std::sort(favorites.begin(), favorites.end());
    for (const QString &name : favorites) {
        stored.append(prefix + name);
    }
    QSettings().setValue(kFavoritesKey, stored);
}
