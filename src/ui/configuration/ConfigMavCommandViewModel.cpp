#include "ConfigMavCommandViewModel.h"

#include <QMetaType>

#include <algorithm>
#include <cmath>

ConfigMavCommandViewModel::ConfigMavCommandViewModel(QObject *parent)
    : ConfigMavCommandViewModel(MissionCommandCatalog::instance(), parent)
{
}

ConfigMavCommandViewModel::ConfigMavCommandViewModel(
    MissionCommandCatalog *catalog, QObject *parent)
    : QAbstractTableModel(parent)
    , m_catalog(catalog)
{
    Q_ASSERT(m_catalog);
    ReloadCommand();
}

int ConfigMavCommandViewModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_commands.size();
}

int ConfigMavCommandViewModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ConfigMavCommandViewModel::data(const QModelIndex &index,
                                         int role) const
{
    if (!index.isValid() || index.row() < 0
        || index.row() >= m_commands.size()
        || (role != Qt::DisplayRole && role != Qt::EditRole)) {
        return {};
    }
    const MissionCommandDefinition &row = m_commands.at(index.row());
    if (index.column() == IdColumn) return row.Id;
    if (index.column() == NameColumn) return row.Name;
    const int label = index.column() - P1Column;
    return label >= 0 && label < 7
        ? MissionCommandCatalog::NormalizeLabels(row.ParameterLabels).at(label)
        : QVariant();
}

bool ConfigMavCommandViewModel::setData(const QModelIndex &index,
                                        const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0
        || index.row() >= m_commands.size() || role != Qt::EditRole) {
        return false;
    }
    MissionCommandDefinition &row = m_commands[index.row()];
    if (index.column() == IdColumn) {
        bool ok = false;
        const double numericId = value.toDouble(&ok);
        if (!ok || !std::isfinite(numericId) || numericId < 0.0
            || numericId > 65535.0
            || numericId != std::floor(numericId)) return false;
        const qlonglong id = static_cast<qlonglong>(numericId);
        if (row.Id == id) return true;
        row.Id = static_cast<quint16>(id);
    } else if (index.column() == NameColumn) {
        const QString name = value.toString();
        if (row.Name == name) return true;
        row.Name = name;
    } else {
        const int label = index.column() - P1Column;
        if (label < 0 || label >= 7) return false;
        row.ParameterLabels = MissionCommandCatalog::NormalizeLabels(
            row.ParameterLabels);
        const QString text = value.toString();
        if (row.ParameterLabels.at(label) == text) return true;
        row.ParameterLabels[label] = text;
    }
    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    return true;
}

QVariant ConfigMavCommandViewModel::headerData(
    int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    static const QStringList headers{
        QStringLiteral("ID"), QStringLiteral("Name"),
        QStringLiteral("P1"), QStringLiteral("P2"),
        QStringLiteral("P3"), QStringLiteral("P4"),
        QStringLiteral("P5 / X"), QStringLiteral("P6 / Y"),
        QStringLiteral("P7 / Z")};
    return section >= 0 && section < headers.size()
        ? headers.at(section) : QVariant();
}

Qt::ItemFlags ConfigMavCommandViewModel::flags(
    const QModelIndex &index) const
{
    return index.isValid()
        ? QAbstractTableModel::flags(index) | Qt::ItemIsEditable
        : QAbstractTableModel::flags(index);
}

QString ConfigMavCommandViewModel::Status() const
{
    return m_status;
}

QVector<MissionCommandDefinition>
ConfigMavCommandViewModel::Commands() const
{
    return m_commands;
}

int ConfigMavCommandViewModel::AddCommand(int id, const QString &name)
{
    if (id < 0 || id > 65535) {
        setStatus(tr("The command ID must be an integer from 0 through 65535."));
        return -1;
    }
    const auto duplicateId = std::find_if(
        m_commands.cbegin(), m_commands.cend(), [id](const auto &row) {
            return row.Id == id;
        });
    if (duplicateId != m_commands.cend()) {
        setStatus(tr("Command ID %1 is already in the custom list.").arg(id));
        return -1;
    }
    QString commandName = MissionCommandCatalog::CanonicalName(
        static_cast<quint16>(id));
    if (commandName.isEmpty()) {
        commandName = name.trimmed().toUpper();
        if (commandName.isEmpty()) return -1;
    }
    const auto duplicateName = std::find_if(
        m_commands.cbegin(), m_commands.cend(),
        [&commandName](const auto &row) {
            return row.Name.compare(commandName, Qt::CaseInsensitive) == 0;
        });
    if (duplicateName != m_commands.cend()) {
        setStatus(tr("Command name '%1' is already in the custom list.")
                      .arg(commandName));
        return -1;
    }

    const int inserted = m_commands.size();
    beginInsertRows(QModelIndex(), inserted, inserted);
    m_commands.append({static_cast<quint16>(id), commandName,
                       {QString(), QString(), QString(), QString(),
                        QStringLiteral("Lat"), QStringLiteral("Lon"),
                        QStringLiteral("Alt")}});
    endInsertRows();
    setStatus(tr("Added %1; edit its labels and press Save.")
                  .arg(commandName));
    return inserted;
}

bool ConfigMavCommandViewModel::RemoveCommand(int row)
{
    if (row < 0 || row >= m_commands.size()) return false;
    beginRemoveRows(QModelIndex(), row, row);
    m_commands.removeAt(row);
    endRemoveRows();
    setStatus(tr("Removed from the editor; press Save to persist the change."));
    return true;
}

bool ConfigMavCommandViewModel::SaveCommand()
{
    QVector<MissionCommandDefinition> definitions = m_commands;
    for (MissionCommandDefinition &definition : definitions) {
        definition.Name = definition.Name.trimmed();
        definition.ParameterLabels = MissionCommandCatalog::NormalizeLabels(
            definition.ParameterLabels);
    }
    QString error;
    if (!m_catalog->Save(definitions, &error)) {
        setStatus(tr("Cannot save: %1").arg(error));
        return false;
    }
    setStatus(tr("Saved %1 custom command definitions. The Flight Planner list is updated.")
                  .arg(m_commands.size()));
    return true;
}

void ConfigMavCommandViewModel::ReloadCommand()
{
    beginResetModel();
    m_commands = m_catalog->LoadDefinitions();
    endResetModel();
    setStatus(tr("Loaded %1 custom command definitions.")
                  .arg(m_commands.size()));
}

void ConfigMavCommandViewModel::setStatus(const QString &status)
{
    if (m_status == status) return;
    m_status = status;
    emit statusChanged(m_status);
}
