#include "TranslationEditorModel.h"

#include <QSet>

#include <algorithm>

TranslationEditorModel::TranslationEditorModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int TranslationEditorModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int TranslationEditorModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant TranslationEditorModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()
        || index.column() < 0 || index.column() >= ColumnCount) {
        return QVariant();
    }

    const Row &row = m_rows.at(index.row());
    const ResxTranslationEntry &entry = row.entry;
    const RowState current = state(row);
    switch (role) {
    case RelativePathRole:
        return entry.relativePath;
    case KeyRole:
        return entry.key;
    case SourceTextRole:
        return entry.sourceText;
    case TranslationRole:
        return entry.translation;
    case CommentRole:
        return entry.comment;
    case ExistingRole:
        return entry.hasExistingTranslation;
    case MissingRole:
        return current.missing;
    case WillExportRole:
        return current.willExport;
    case ModifiedRole:
        return current.modified;
    case Qt::ToolTipRole:
        return entry.comment.isEmpty() ? QVariant() : QVariant(entry.comment);
    case Qt::CheckStateRole:
        return index.column() == ExistingColumn
            ? QVariant(entry.hasExistingTranslation ? Qt::Checked
                                                    : Qt::Unchecked)
            : QVariant();
    case Qt::TextAlignmentRole:
        return index.column() == ExistingColumn
            ? QVariant(int(Qt::AlignCenter)) : QVariant();
    case Qt::DisplayRole:
        switch (index.column()) {
        case FileColumn: return entry.relativePath;
        case InternalKeyColumn: return entry.key;
        case SourceTextColumn: return entry.sourceText;
        case TranslationColumn: return entry.translation;
        case ExistingColumn: return QVariant();
        default: return QVariant();
        }
    case Qt::EditRole:
        return index.column() == TranslationColumn
            ? QVariant(entry.translation) : QVariant();
    default:
        return QVariant();
    }
}

QVariant TranslationEditorModel::headerData(
    int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QAbstractTableModel::headerData(section, orientation, role);
    switch (section) {
    case FileColumn: return tr("File");
    case InternalKeyColumn: return tr("Internal key");
    case SourceTextColumn: return tr("English / neutral");
    case TranslationColumn: return tr("Other language");
    case ExistingColumn: return tr("Existing");
    default: return QVariant();
    }
}

Qt::ItemFlags TranslationEditorModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == TranslationColumn)
        result |= Qt::ItemIsEditable;
    return result;
}

bool TranslationEditorModel::setData(
    const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()
        || index.column() != TranslationColumn || role != Qt::EditRole) {
        return false;
    }
    return setTranslation(index.row(), value.toString(), true);
}

QHash<int, QByteArray> TranslationEditorModel::roleNames() const
{
    QHash<int, QByteArray> result = QAbstractTableModel::roleNames();
    result.insert(RelativePathRole, QByteArrayLiteral("relativePath"));
    result.insert(KeyRole, QByteArrayLiteral("key"));
    result.insert(SourceTextRole, QByteArrayLiteral("sourceText"));
    result.insert(TranslationRole, QByteArrayLiteral("translation"));
    result.insert(CommentRole, QByteArrayLiteral("comment"));
    result.insert(ExistingRole, QByteArrayLiteral("hasExistingTranslation"));
    result.insert(MissingRole, QByteArrayLiteral("isMissing"));
    result.insert(WillExportRole, QByteArrayLiteral("willExport"));
    result.insert(ModifiedRole, QByteArrayLiteral("isModified"));
    return result;
}

void TranslationEditorModel::loadProject(
    const ResxTranslationProject &project)
{
    beginResetModel();
    m_rows.clear();
    m_rows.reserve(project.entries.size());
    for (const ResxTranslationEntry &entry : project.entries)
        m_rows.append({entry, entry.translation});
    m_sourceRoot = project.sourceRoot;
    m_culture = project.culture;
    m_warnings = project.warnings;
    m_resourceFileCount = project.resourceFiles;
    m_hasProject = true;
    recount();
    rebuildAliasIndex();
    endResetModel();
    emit projectChanged();
    emit countsChanged();
}

void TranslationEditorModel::clearProject()
{
    beginResetModel();
    m_rows.clear();
    m_aliasesByKey.clear();
    m_sourceRoot.clear();
    m_culture.clear();
    m_warnings.clear();
    m_resourceFileCount = 0;
    m_missingCount = 0;
    m_translatedCount = 0;
    m_modifiedCount = 0;
    m_hasProject = false;
    endResetModel();
    emit projectChanged();
    emit countsChanged();
}

ResxTranslationEntry TranslationEditorModel::entryAt(int row) const
{
    return row >= 0 && row < m_rows.size()
        ? m_rows.at(row).entry : ResxTranslationEntry();
}

QVector<ResxTranslationEntry> TranslationEditorModel::snapshot() const
{
    QVector<ResxTranslationEntry> result;
    result.reserve(m_rows.size());
    for (const Row &row : m_rows)
        result.append(row.entry);
    return result;
}

bool TranslationEditorModel::acceptChanges()
{
    if (m_modifiedCount == 0)
        return false;
    for (Row &row : m_rows)
        row.acceptedTranslation = row.entry.translation;
    m_modifiedCount = 0;
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0, 0), index(m_rows.size() - 1, ColumnCount - 1),
                         {ModifiedRole});
    }
    emit countsChanged();
    return true;
}

bool TranslationEditorModel::revertAll()
{
    if (m_modifiedCount == 0)
        return false;
    for (Row &row : m_rows)
        row.entry.translation = row.acceptedTranslation;
    recount();
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0, 0), index(m_rows.size() - 1, ColumnCount - 1),
                         {Qt::DisplayRole, Qt::EditRole, TranslationRole,
                          MissingRole, WillExportRole, ModifiedRole});
    }
    emit countsChanged();
    return true;
}

int TranslationEditorModel::importTranslations(
    const ResxTranslationService::ImportResult &result)
{
    return result.success
        ? importTranslations(result.values, result.order) : 0;
}

int TranslationEditorModel::importTranslations(
    const QHash<TranslationIdentity, QString> &values,
    const QVector<TranslationIdentity> &order)
{
    QVector<TranslationIdentity> identities;
    identities.reserve(values.size());
    QSet<TranslationIdentity> seen;
    for (const TranslationIdentity &identity : order) {
        if (values.contains(identity) && !seen.contains(identity)) {
            identities.append(identity);
            seen.insert(identity);
        }
    }
    QVector<TranslationIdentity> remainder;
    remainder.reserve(values.size() - identities.size());
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!seen.contains(it.key()))
            remainder.append(it.key());
    }
    std::sort(remainder.begin(), remainder.end(),
              [](const TranslationIdentity &left,
                 const TranslationIdentity &right) {
        const int path = QString::compare(left.relativePath,
                                          right.relativePath,
                                          Qt::CaseInsensitive);
        if (path != 0)
            return path < 0;
        const int exactPath = QString::compare(left.relativePath,
                                               right.relativePath,
                                               Qt::CaseSensitive);
        return exactPath != 0 ? exactPath < 0 : left.key < right.key;
    });
    identities += remainder;

    QSet<int> assigned;
    int changed = 0;
    int firstChanged = m_rows.size();
    int lastChanged = -1;
    for (const TranslationIdentity &identity : identities) {
        const auto value = values.constFind(identity);
        if (value == values.cend())
            continue;
        const int row = uniqueImportRow(identity);
        if (row < 0 || assigned.contains(row))
            continue;
        // MP10 assigns each grid row at most once even when the first imported
        // value happens to equal the current translation.
        assigned.insert(row);
        if (setTranslation(row, value.value(), false)) {
            ++changed;
            firstChanged = qMin(firstChanged, row);
            lastChanged = qMax(lastChanged, row);
        }
    }
    if (lastChanged >= firstChanged) {
        emit dataChanged(index(firstChanged, 0),
                         index(lastChanged, ColumnCount - 1),
                         {Qt::DisplayRole, Qt::EditRole, TranslationRole,
                          MissingRole, WillExportRole, ModifiedRole});
        emit countsChanged();
    }
    return changed;
}

TranslationEditorModel::RowState TranslationEditorModel::state(
    const Row &row)
{
    RowState result;
    result.missing = !row.entry.hasExistingTranslation
        && row.entry.translation == row.entry.sourceText;
    result.willExport = row.entry.translation != row.entry.sourceText;
    result.modified = row.entry.translation != row.acceptedTranslation;
    return result;
}

QString TranslationEditorModel::normalizedPath(const QString &path)
{
    QString result = path;
    result.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return result;
}

QString TranslationEditorModel::foldedPath(const QString &path)
{
    return normalizedPath(path).toCaseFolded();
}

QString TranslationEditorModel::baseName(const QString &path)
{
    const QString normalized = normalizedPath(path);
    const int slash = normalized.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? normalized : normalized.mid(slash + 1);
}

QString TranslationEditorModel::resourceName(
    const QString &sourceRelativePath)
{
    QString normalized = normalizedPath(sourceRelativePath);
    if (!normalized.endsWith(QStringLiteral(".resx"), Qt::CaseInsensitive))
        return QString();
    normalized.chop(5);
    normalized.replace(QLatin1Char('/'), QLatin1Char('.'));
    return normalized + QStringLiteral(".resources");
}

void TranslationEditorModel::rebuildAliasIndex()
{
    m_aliasesByKey.clear();
    for (int row = 0; row < m_rows.size(); ++row) {
        const ResxTranslationEntry &entry = m_rows.at(row).entry;
        addAlias(entry.key, foldedPath(entry.relativePath), row);
        addAlias(entry.key, foldedPath(baseName(entry.relativePath)), row);
        addAlias(entry.key, foldedPath(resourceName(entry.relativePath)), row);
    }
}

void TranslationEditorModel::addAlias(
    const QString &key, const QString &alias, int row)
{
    if (alias.isEmpty())
        return;
    QVector<int> &rows = m_aliasesByKey[key][alias];
    if (!rows.contains(row) && rows.size() < 2)
        rows.append(row);
}

int TranslationEditorModel::uniqueImportRow(
    const TranslationIdentity &identity) const
{
    const auto keys = m_aliasesByKey.constFind(identity.key);
    if (keys == m_aliasesByKey.cend())
        return -1;

    QSet<int> candidates;
    const auto appendAlias = [&keys, &candidates](const QString &alias) {
        const auto found = keys.value().constFind(alias);
        if (found == keys.value().cend())
            return;
        for (int row : found.value())
            candidates.insert(row);
    };
    const QString imported = foldedPath(identity.relativePath);
    appendAlias(imported);
    appendAlias(foldedPath(baseName(identity.relativePath)));
    if (imported.endsWith(QStringLiteral(".resources"))) {
        int start = 0;
        while (start < imported.size()) {
            appendAlias(imported.mid(start));
            const int dot = imported.indexOf(QLatin1Char('.'), start);
            if (dot < 0)
                break;
            start = dot + 1;
        }
    }

    int match = -1;
    for (int row : candidates) {
        if (row < 0 || row >= m_rows.size()
            || !ResxTranslationService::resumeFileMatches(
                identity.relativePath, m_rows.at(row).entry.relativePath)) {
            continue;
        }
        if (match >= 0 && match != row)
            return -1;
        match = row;
    }
    return match;
}

bool TranslationEditorModel::setTranslation(
    int rowIndex, const QString &translation, bool emitChange)
{
    if (rowIndex < 0 || rowIndex >= m_rows.size())
        return false;
    Row &row = m_rows[rowIndex];
    if (row.entry.translation == translation)
        return false;
    const RowState before = state(row);
    row.entry.translation = translation;
    const RowState after = state(row);
    m_missingCount += int(after.missing) - int(before.missing);
    m_translatedCount += int(after.willExport) - int(before.willExport);
    m_modifiedCount += int(after.modified) - int(before.modified);
    if (emitChange) {
        emit dataChanged(index(rowIndex, 0),
                         index(rowIndex, ColumnCount - 1),
                         {Qt::DisplayRole, Qt::EditRole, TranslationRole,
                          MissingRole, WillExportRole, ModifiedRole});
        emit countsChanged();
    }
    return true;
}

void TranslationEditorModel::recount()
{
    m_missingCount = 0;
    m_translatedCount = 0;
    m_modifiedCount = 0;
    for (const Row &row : m_rows) {
        const RowState current = state(row);
        m_missingCount += current.missing ? 1 : 0;
        m_translatedCount += current.willExport ? 1 : 0;
        m_modifiedCount += current.modified ? 1 : 0;
    }
}

TranslationEditorFilterModel::TranslationEditorFilterModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    connect(this, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &, int, int) {
        emit visibleCountChanged(rowCount());
    });
    connect(this, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex &, int, int) {
        emit visibleCountChanged(rowCount());
    });
    connect(this, &QAbstractItemModel::modelReset, this,
            [this]() { emit visibleCountChanged(rowCount()); });
}

void TranslationEditorFilterModel::setSearchText(const QString &text)
{
    if (m_searchText == text)
        return;
    m_searchText = text;
    refreshFilter();
}

void TranslationEditorFilterModel::setMissingOnly(bool enabled)
{
    if (m_missingOnly == enabled)
        return;
    m_missingOnly = enabled;
    refreshFilter();
}

void TranslationEditorFilterModel::setExportOnly(bool enabled)
{
    if (m_exportOnly == enabled)
        return;
    m_exportOnly = enabled;
    refreshFilter();
}

bool TranslationEditorFilterModel::filterAcceptsRow(
    int sourceRow, const QModelIndex &sourceParent) const
{
    if (!sourceModel())
        return false;
    const QModelIndex row = sourceModel()->index(
        sourceRow, TranslationEditorModel::FileColumn, sourceParent);
    if (!row.isValid())
        return false;
    if (m_missingOnly
        && !row.data(TranslationEditorModel::MissingRole).toBool()) {
        return false;
    }
    if (m_exportOnly
        && !row.data(TranslationEditorModel::WillExportRole).toBool()) {
        return false;
    }
    const QString search = m_searchText.trimmed();
    if (search.isEmpty())
        return true;
    return row.data(TranslationEditorModel::RelativePathRole).toString()
               .contains(search, Qt::CaseInsensitive)
        || row.data(TranslationEditorModel::KeyRole).toString()
               .contains(search, Qt::CaseInsensitive)
        || row.data(TranslationEditorModel::SourceTextRole).toString()
               .contains(search, Qt::CaseInsensitive)
        || row.data(TranslationEditorModel::TranslationRole).toString()
               .contains(search, Qt::CaseInsensitive);
}

void TranslationEditorFilterModel::refreshFilter()
{
    invalidateFilter();
    emit filterChanged();
}
