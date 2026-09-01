#include "FlightPlannerMissionModel.h"

#include <QMetaType>

#include <algorithm>
#include <array>
#include <cmath>

FlightPlannerMissionModel::FlightPlannerMissionModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int FlightPlannerMissionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(activeStore().size());
}

int FlightPlannerMissionModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant FlightPlannerMissionModel::data(const QModelIndex &index, int role) const
{
    WpRow *row = rowAt(index.row());
    if (!index.isValid() || !row
        || (role != Qt::DisplayRole && role != Qt::EditRole)) {
        return QVariant();
    }
    switch (index.column()) {
    case NumberColumn: return row->DisplayNumber();
    case CommandColumn:
        return role == Qt::EditRole
            ? QVariant::fromValue(row->Command()) : row->CommandName();
    case P1Column: return row->P1();
    case P2Column: return row->P2();
    case P3Column: return row->P3();
    case P4Column: return row->P4();
    case LatColumn: return row->Lat();
    case LngColumn: return row->Lng();
    case AltColumn: return row->AltDisplay();
    case FrameColumn:
        return role == Qt::EditRole
            ? QVariant::fromValue(row->Frame()) : row->FrameName();
    case GradColumn: return row->Grad();
    case AngleColumn: return row->Angle();
    case DistColumn: return row->Dist();
    case AzColumn: return row->Az();
    case ZoneColumn: return row->Zone();
    case EastingColumn: return row->Easting();
    case NorthingColumn: return row->Northing();
    case MgrsColumn: return row->Mgrs();
    default: return QVariant();
    }
}

bool FlightPlannerMissionModel::setData(const QModelIndex &index,
                                        const QVariant &value, int role)
{
    WpRow *row = rowAt(index.row());
    if (!index.isValid() || !row || role != Qt::EditRole) {
        return false;
    }
    const auto setDouble = [row, &value](void (WpRow::*setter)(double)) {
        bool ok = false;
        const double converted = value.toDouble(&ok);
        if (!ok || !std::isfinite(converted)) return false;
        (row->*setter)(converted);
        return true;
    };
    switch (index.column()) {
    case CommandColumn:
        if (value.userType() == QMetaType::QString) {
            quint16 command = 0;
            if (!WpRow::commandForName(value.toString(), &command)) return false;
            row->setCommand(command);
            return true;
        } else {
            bool ok = false;
            const qlonglong command = value.toLongLong(&ok);
            if (!ok || command < 0 || command > 65535) return false;
            row->setCommand(static_cast<quint16>(command));
            return true;
        }
    case P1Column: return setDouble(&WpRow::setP1);
    case P2Column: return setDouble(&WpRow::setP2);
    case P3Column: return setDouble(&WpRow::setP3);
    case P4Column: return setDouble(&WpRow::setP4);
    case LatColumn: {
        bool ok = false;
        const double latitude = value.toDouble(&ok);
        if (!ok || !std::isfinite(latitude)
            || latitude < -90.0 || latitude > 90.0) {
            return false;
        }
        row->setLat(latitude);
        return true;
    }
    case LngColumn: {
        bool ok = false;
        const double longitude = value.toDouble(&ok);
        if (!ok || !std::isfinite(longitude)
            || longitude < -180.0 || longitude > 180.0) {
            return false;
        }
        row->setLng(longitude);
        return true;
    }
    case AltColumn: return setDouble(&WpRow::setAltDisplay);
    case FrameColumn:
        if (value.userType() == QMetaType::QString) {
            const QString frame = value.toString().trimmed();
            const QStringList frameNames = WpRow::FrameList();
            const bool known = std::any_of(
                frameNames.cbegin(), frameNames.cend(),
                [&frame](const QString &candidate) {
                    return candidate.compare(frame, Qt::CaseInsensitive) == 0;
                });
            if (!known) return false;
            row->setFrameName(frame);
            return true;
        } else {
            bool ok = false;
            const qlonglong frame = value.toLongLong(&ok);
            if (!ok || frame < 0 || frame > 255) return false;
            row->setFrame(static_cast<quint8>(frame));
            return true;
        }
    case ZoneColumn: row->setZone(value.toString()); return true;
    case EastingColumn: row->setEasting(value.toString()); return true;
    case NorthingColumn: row->setNorthing(value.toString()); return true;
    case MgrsColumn: row->setMgrs(value.toString()); return true;
    default:
        return false;
    }
}

QVariant FlightPlannerMissionModel::headerData(int section,
                                               Qt::Orientation orientation,
                                               int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    static const QStringList headers{
        QStringLiteral("#"), QStringLiteral("Command"),
        QStringLiteral("P1"), QStringLiteral("P2"),
        QStringLiteral("P3"), QStringLiteral("P4"),
        QStringLiteral("Lat"), QStringLiteral("Lon"),
        QStringLiteral("Alt"), QStringLiteral("Frame"),
        QStringLiteral("Grad %"), QStringLiteral("Angle"),
        QStringLiteral("Dist"), QStringLiteral("AZ"),
        QStringLiteral("Zone"), QStringLiteral("Easting"),
        QStringLiteral("Northing"), QStringLiteral("MGRS"),
    };
    return section >= 0 && section < headers.size()
        ? headers.at(section) : QVariant();
}

Qt::ItemFlags FlightPlannerMissionModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags result = QAbstractTableModel::flags(index);
    if (!index.isValid()) {
        return result;
    }
    switch (index.column()) {
    case NumberColumn:
    case GradColumn:
    case AngleColumn:
    case DistColumn:
    case AzColumn:
        break;
    default:
        result |= Qt::ItemIsEditable;
        break;
    }
    return result;
}

bool FlightPlannerMissionModel::removeRows(int row, int count,
                                           const QModelIndex &parent)
{
    Store &rows = activeStore();
    if (parent.isValid() || row < 0 || count <= 0
        || row + count > static_cast<int>(rows.size())) {
        return false;
    }
    beginRemoveRows(QModelIndex(), row, row + count - 1);
    rows.erase(rows.begin() + row, rows.begin() + row + count);
    renumber(rows);
    endRemoveRows();
    emit rowsChanged(m_activeStore);
    return true;
}

QString FlightPlannerMissionModel::MissionType() const
{
    return storeName(m_activeStore);
}

FlightPlannerMissionModel::MissionStore
FlightPlannerMissionModel::missionStore() const
{
    return m_activeStore;
}

WpRow *FlightPlannerMissionModel::rowAt(int row) const
{
    const Store &rows = activeStore();
    return row >= 0 && row < static_cast<int>(rows.size())
        ? rows.at(static_cast<size_t>(row)).get() : nullptr;
}

QVector<WpRowData> FlightPlannerMissionModel::rows(MissionStore type) const
{
    QVector<WpRowData> result;
    const Store &source = store(type);
    result.reserve(static_cast<int>(source.size()));
    for (const auto &row : source) {
        result.append(row->toData());
    }
    return result;
}

int FlightPlannerMissionModel::storeRowCount(MissionStore type) const
{
    return static_cast<int>(store(type).size());
}

QString FlightPlannerMissionModel::storeName(MissionStore type)
{
    switch (type) {
    case MissionStore::Fence: return QStringLiteral("Fence");
    case MissionStore::Rally: return QStringLiteral("Rally");
    case MissionStore::Mission:
    default: return QStringLiteral("Mission");
    }
}

bool FlightPlannerMissionModel::storeForName(const QString &name,
                                             MissionStore *type)
{
    if (!type) return false;
    if (name.compare(QStringLiteral("Mission"), Qt::CaseInsensitive) == 0) {
        *type = MissionStore::Mission;
        return true;
    }
    if (name.compare(QStringLiteral("Fence"), Qt::CaseInsensitive) == 0) {
        *type = MissionStore::Fence;
        return true;
    }
    if (name.compare(QStringLiteral("Rally"), Qt::CaseInsensitive) == 0) {
        *type = MissionStore::Rally;
        return true;
    }
    return false;
}

void FlightPlannerMissionModel::setMissionType(const QString &typeName)
{
    MissionStore type = MissionStore::Mission;
    if (storeForName(typeName, &type)) {
        setMissionStore(type);
    }
}

void FlightPlannerMissionModel::setMissionStore(MissionStore type)
{
    if (m_activeStore == type) return;
    beginResetModel();
    m_activeStore = type;
    endResetModel();
    emit missionTypeChanged(MissionType());
}

WpRow *FlightPlannerMissionModel::appendRow(const WpRowData &data)
{
    Store &rows = activeStore();
    const int index = static_cast<int>(rows.size());
    beginInsertRows(QModelIndex(), index, index);
    rows.push_back(makeRow(data));
    renumber(rows);
    WpRow *result = rows.back().get();
    endInsertRows();
    emit rowsChanged(m_activeStore);
    return result;
}

WpRow *FlightPlannerMissionModel::insertRowData(int row, const WpRowData &data)
{
    Store &rows = activeStore();
    row = std::clamp(row, 0, static_cast<int>(rows.size()));
    beginInsertRows(QModelIndex(), row, row);
    auto inserted = makeRow(data);
    WpRow *result = inserted.get();
    rows.insert(rows.begin() + row, std::move(inserted));
    renumber(rows);
    endInsertRows();
    emit rowsChanged(m_activeStore);
    return result;
}

bool FlightPlannerMissionModel::moveWaypointUp(int row)
{
    if (row <= 0 || row >= rowCount()) return false;
    Store &rows = activeStore();
    beginMoveRows(QModelIndex(), row, row, QModelIndex(), row - 1);
    std::swap(rows.at(static_cast<size_t>(row)),
              rows.at(static_cast<size_t>(row - 1)));
    renumber(rows);
    endMoveRows();
    emit rowsChanged(m_activeStore);
    return true;
}

bool FlightPlannerMissionModel::moveWaypointDown(int row)
{
    if (row < 0 || row + 1 >= rowCount()) return false;
    Store &rows = activeStore();
    beginMoveRows(QModelIndex(), row, row, QModelIndex(), row + 2);
    std::swap(rows.at(static_cast<size_t>(row)),
              rows.at(static_cast<size_t>(row + 1)));
    renumber(rows);
    endMoveRows();
    emit rowsChanged(m_activeStore);
    return true;
}

void FlightPlannerMissionModel::clearActiveStore()
{
    if (activeStore().empty()) return;
    beginResetModel();
    activeStore().clear();
    endResetModel();
    emit rowsChanged(m_activeStore);
}

void FlightPlannerMissionModel::replaceStore(MissionStore type,
                                             const QVector<WpRowData> &newRows)
{
    Store replacement;
    replacement.reserve(static_cast<size_t>(newRows.size()));
    for (const WpRowData &row : newRows) {
        replacement.push_back(makeRow(row));
    }
    renumber(replacement);

    const bool active = type == m_activeStore;
    if (active) beginResetModel();
    store(type).swap(replacement);
    if (active) endResetModel();
    emit rowsChanged(type);
}

void FlightPlannerMissionModel::appendStore(MissionStore type,
                                            const QVector<WpRowData> &newRows)
{
    if (newRows.isEmpty()) return;
    Store &target = store(type);
    const bool active = type == m_activeStore;
    const int first = static_cast<int>(target.size());
    if (active) beginInsertRows(QModelIndex(), first,
                                first + newRows.size() - 1);
    target.reserve(target.size() + static_cast<size_t>(newRows.size()));
    for (const WpRowData &row : newRows) {
        target.push_back(makeRow(row));
    }
    renumber(target);
    if (active) endInsertRows();
    emit rowsChanged(type);
}

FlightPlannerMissionModel::Store &
FlightPlannerMissionModel::store(MissionStore type)
{
    switch (type) {
    case MissionStore::Fence: return m_fenceRows;
    case MissionStore::Rally: return m_rallyRows;
    case MissionStore::Mission:
    default: return m_missionRows;
    }
}

const FlightPlannerMissionModel::Store &
FlightPlannerMissionModel::store(MissionStore type) const
{
    switch (type) {
    case MissionStore::Fence: return m_fenceRows;
    case MissionStore::Rally: return m_rallyRows;
    case MissionStore::Mission:
    default: return m_missionRows;
    }
}

FlightPlannerMissionModel::Store &FlightPlannerMissionModel::activeStore()
{
    return store(m_activeStore);
}

const FlightPlannerMissionModel::Store &
FlightPlannerMissionModel::activeStore() const
{
    return store(m_activeStore);
}

std::unique_ptr<WpRow>
FlightPlannerMissionModel::makeRow(const WpRowData &data)
{
    auto row = std::make_unique<WpRow>(data);
    WpRow *rowPointer = row.get();
    connect(rowPointer, &WpRow::changed, this,
            [this, rowPointer]() { publishRowChange(rowPointer); });
    return row;
}

void FlightPlannerMissionModel::renumber(Store &rows)
{
    const bool wasMutating = m_mutating;
    m_mutating = true;
    for (size_t index = 0; index < rows.size(); ++index) {
        rows.at(index)->setSeq(static_cast<int>(index));
    }
    m_mutating = wasMutating;
}

void FlightPlannerMissionModel::publishRowChange(WpRow *row)
{
    if (m_mutating || !row) return;
    const std::array<MissionStore, 3> stores{
        MissionStore::Mission, MissionStore::Fence, MissionStore::Rally};
    for (MissionStore type : stores) {
        const Store &rows = store(type);
        const auto it = std::find_if(rows.begin(), rows.end(),
            [row](const std::unique_ptr<WpRow> &candidate) {
                return candidate.get() == row;
            });
        if (it == rows.end()) continue;
        if (type == m_activeStore) {
            const int rowIndex = static_cast<int>(
                std::distance(rows.begin(), it));
            emit dataChanged(index(rowIndex, 0),
                             index(rowIndex, ColumnCount - 1));
        }
        emit rowsChanged(type);
        return;
    }
}
