#include "CotIdentityModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMetaType>

#include <cmath>

namespace {
QJsonValue jsonText(const QString &text)
{
    return text.isNull() ? QJsonValue(QJsonValue::Null) : QJsonValue(text);
}

QString primitiveText(const QJsonValue &value)
{
    if (value.isNull() || value.isUndefined()) {
        return QString();
    }
    if (value.isString()) {
        return value.toString();
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true")
                              : QStringLiteral("false");
    }
    if (value.isDouble()) {
        return value.toVariant().toString();
    }
    return QString();
}

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}
}

constexpr int CotIdentityModel::MaxRows;
constexpr int CotIdentityModel::MaxTextLength;
constexpr int CotIdentityModel::MaxJsonBytes;

bool CotIdentityRecord::operator==(const CotIdentityRecord &other) const
{
    return systemId == other.systemId
        && eventUid == other.eventUid
        && eventUid.isNull() == other.eventUid.isNull()
        && includeTakv == other.includeTakv
        && contactCallsign == other.contactCallsign
        && contactCallsign.isNull() == other.contactCallsign.isNull()
        && contactEndpoint == other.contactEndpoint
        && contactEndpoint.isNull() == other.contactEndpoint.isNull()
        && vmf == other.vmf
        && vmf.isNull() == other.vmf.isNull();
}

CotIdentityModel::CotIdentityModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int CotIdentityModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_identities.size();
}

int CotIdentityModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant CotIdentityModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0
        || index.row() >= m_identities.size()
        || index.column() < 0 || index.column() >= ColumnCount) {
        return QVariant();
    }

    const CotIdentityRecord &identity = m_identities.at(index.row());
    if (role == Qt::CheckStateRole && index.column() == TakvColumn) {
        return identity.includeTakv ? Qt::Checked : Qt::Unchecked;
    }
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
        case SystemIdColumn:
            return role == Qt::DisplayRole
                ? QVariant(CotIdentityModel::systemIdText(identity.systemId))
                : identity.systemId.toVariant();
        case EventUidColumn:
            return identity.eventUid;
        case TakvColumn:
            return identity.includeTakv;
        case ContactCallsignColumn:
            return identity.contactCallsign;
        case ContactEndpointColumn:
            return identity.contactEndpoint;
        case VmfColumn:
            return identity.vmf;
        default:
            return QVariant();
        }
    }

    switch (role) {
    case SystemIdRole:
        return identity.systemId.toVariant();
    case EventUidRole:
        return identity.eventUid;
    case TakvRole:
        return identity.includeTakv;
    case ContactCallsignRole:
        return identity.contactCallsign;
    case ContactEndpointRole:
        return identity.contactEndpoint;
    case VmfRole:
        return identity.vmf;
    default:
        return QVariant();
    }
}

QVariant CotIdentityModel::headerData(int section, Qt::Orientation orientation,
                                      int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
    case SystemIdColumn:
        return tr("System ID");
    case EventUidColumn:
        return tr("Event UID");
    case TakvColumn:
        return tr("TAKV");
    case ContactCallsignColumn:
        return tr("Contact callsign");
    case ContactEndpointColumn:
        return tr("Contact endpoint");
    case VmfColumn:
        return tr("VMF");
    default:
        return QVariant();
    }
}

Qt::ItemFlags CotIdentityModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable
        | Qt::ItemIsEditable;
    if (index.column() == TakvColumn) {
        result |= Qt::ItemIsUserCheckable;
    }
    return result;
}

bool CotIdentityModel::setData(const QModelIndex &index,
                               const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0
        || index.row() >= m_identities.size()
        || index.column() < 0 || index.column() >= ColumnCount) {
        return false;
    }

    CotIdentityRecord &identity = m_identities[index.row()];
    QVector<int> changedRoles{Qt::DisplayRole, Qt::EditRole};
    switch (index.column()) {
    case SystemIdColumn: {
        if (role != Qt::EditRole) {
            return false;
        }
        const QJsonValue systemId = value.isValid() && !value.isNull()
            ? QJsonValue::fromVariant(value) : QJsonValue(QJsonValue::Null);
        if (!isSupportedCell(systemId) || !isBoundedCell(systemId)) {
            return false;
        }
        if (identity.systemId == systemId) {
            return true;
        }
        identity.systemId = systemId;
        changedRoles.append(SystemIdRole);
        break;
    }
    case EventUidColumn:
    case ContactCallsignColumn:
    case ContactEndpointColumn:
    case VmfColumn: {
        if (role != Qt::EditRole) {
            return false;
        }
        const QString text = value.isNull() ? QString() : value.toString();
        if (text.size() > MaxTextLength) {
            return false;
        }
        QString *field = nullptr;
        int dataRole = EventUidRole;
        if (index.column() == EventUidColumn) {
            field = &identity.eventUid;
            dataRole = EventUidRole;
        } else if (index.column() == ContactCallsignColumn) {
            field = &identity.contactCallsign;
            dataRole = ContactCallsignRole;
        } else if (index.column() == ContactEndpointColumn) {
            field = &identity.contactEndpoint;
            dataRole = ContactEndpointRole;
        } else {
            field = &identity.vmf;
            dataRole = VmfRole;
        }
        if (sameText(*field, text)) {
            return true;
        }
        *field = text;
        changedRoles.append(dataRole);
        break;
    }
    case TakvColumn: {
        if (role != Qt::EditRole && role != Qt::CheckStateRole) {
            return false;
        }
        const bool checked = role == Qt::CheckStateRole
            ? value.toInt() == Qt::Checked : value.toBool();
        if (identity.includeTakv == checked) {
            return true;
        }
        identity.includeTakv = checked;
        changedRoles.append(Qt::CheckStateRole);
        changedRoles.append(TakvRole);
        break;
    }
    default:
        return false;
    }

    emit dataChanged(index, index, changedRoles);
    return true;
}

bool CotIdentityModel::insertRows(int row, int count,
                                  const QModelIndex &parent)
{
    if (parent.isValid() || row < 0 || row > m_identities.size()
        || count <= 0 || count > MaxRows - m_identities.size()) {
        return false;
    }

    QVector<int> available;
    available.reserve(count);
    for (int systemId = 0; systemId <= 255 && available.size() < count;
         ++systemId) {
        if (rowForSystemId(systemId) < 0) {
            available.append(systemId);
        }
    }
    if (available.size() != count) {
        return false;
    }

    beginInsertRows(QModelIndex(), row, row + count - 1);
    for (int offset = 0; offset < count; ++offset) {
        CotIdentityRecord identity;
        identity.systemId = QString::number(available.at(offset));
        m_identities.insert(row + offset, identity);
    }
    endInsertRows();
    return true;
}

bool CotIdentityModel::removeRows(int row, int count,
                                  const QModelIndex &parent)
{
    if (parent.isValid() || row < 0 || count <= 0
        || row > m_identities.size() - count) {
        return false;
    }
    beginRemoveRows(QModelIndex(), row, row + count - 1);
    m_identities.remove(row, count);
    endRemoveRows();
    return true;
}

QHash<int, QByteArray> CotIdentityModel::roleNames() const
{
    QHash<int, QByteArray> names = QAbstractTableModel::roleNames();
    names.insert(SystemIdRole, QByteArrayLiteral("systemId"));
    names.insert(EventUidRole, QByteArrayLiteral("eventUid"));
    names.insert(TakvRole, QByteArrayLiteral("includeTakv"));
    names.insert(ContactCallsignRole, QByteArrayLiteral("contactCallsign"));
    names.insert(ContactEndpointRole, QByteArrayLiteral("contactEndpoint"));
    names.insert(VmfRole, QByteArrayLiteral("vmf"));
    return names;
}

bool CotIdentityModel::appendIdentity(const CotIdentityRecord &identity)
{
    return insertIdentity(m_identities.size(), identity);
}

bool CotIdentityModel::insertIdentity(int row,
                                      const CotIdentityRecord &identity)
{
    if (row < 0 || row > m_identities.size()
        || m_identities.size() >= MaxRows
        || !isSupportedCell(identity.systemId)
        || !isBoundedCell(identity.systemId)
        || identity.eventUid.size() > MaxTextLength
        || identity.contactCallsign.size() > MaxTextLength
        || identity.contactEndpoint.size() > MaxTextLength
        || identity.vmf.size() > MaxTextLength) {
        return false;
    }
    beginInsertRows(QModelIndex(), row, row);
    m_identities.insert(row, boundedIdentity(identity));
    endInsertRows();
    return true;
}

int CotIdentityModel::rowForSystemId(int systemId) const
{
    if (!isValidSystemId(systemId)) {
        return -1;
    }
    for (int row = 0; row < m_identities.size(); ++row) {
        if (systemIdText(m_identities.at(row).systemId)
            == QString::number(systemId)) {
            return row;
        }
    }
    return -1;
}

bool CotIdentityModel::identityForSystemId(
    int systemId, CotIdentityRecord *identity) const
{
    const int row = rowForSystemId(systemId);
    if (row < 0 || !identity) {
        return false;
    }
    *identity = m_identities.at(row);
    return true;
}

CotIdentityRecord CotIdentityModel::identityAt(int row) const
{
    return row >= 0 && row < m_identities.size()
        ? m_identities.at(row) : CotIdentityRecord();
}

QVector<CotIdentityRecord> CotIdentityModel::identities() const
{
    return m_identities;
}

void CotIdentityModel::setIdentities(
    const QVector<CotIdentityRecord> &identities)
{
    QVector<CotIdentityRecord> bounded;
    bounded.reserve(qMin(identities.size(), MaxRows));
    for (const CotIdentityRecord &identity : identities) {
        if (bounded.size() >= MaxRows) {
            break;
        }
        if (!isSupportedCell(identity.systemId)
            || !isBoundedCell(identity.systemId)
            || identity.eventUid.size() > MaxTextLength
            || identity.contactCallsign.size() > MaxTextLength
            || identity.contactEndpoint.size() > MaxTextLength
            || identity.vmf.size() > MaxTextLength) {
            continue;
        }
        bounded.append(boundedIdentity(identity));
    }
    beginResetModel();
    m_identities = bounded;
    endResetModel();
}

QByteArray CotIdentityModel::toJson() const
{
    return encodeJson(m_identities);
}

bool CotIdentityModel::loadJson(const QByteArray &json, QString *error)
{
    QVector<CotIdentityRecord> decoded;
    if (!decodeJson(json, &decoded, error)) {
        return false;
    }
    setIdentities(decoded);
    return true;
}

bool CotIdentityModel::loadSetting(const QVariant &storedValue,
                                   QString *error)
{
    if (!storedValue.isValid() || storedValue.isNull()) {
        setError(error, QString());
        setIdentities(QVector<CotIdentityRecord>());
        return true;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int type = storedValue.typeId();
#else
    const int type = storedValue.userType();
#endif
    if (type == QMetaType::QByteArray) {
        return loadJson(storedValue.toByteArray(), error);
    }
    if (type == QMetaType::QString) {
        return loadJson(storedValue.toString().toUtf8(), error);
    }
    setError(error, tr("Unsupported CoT identity setting type."));
    return false;
}

QByteArray CotIdentityModel::encodeJson(
    const QVector<CotIdentityRecord> &identities)
{
    QJsonArray root;
    for (const CotIdentityRecord &source : identities) {
        if (root.size() >= MaxRows) {
            break;
        }
        if (!isSupportedCell(source.systemId)
            || !isBoundedCell(source.systemId)) {
            continue;
        }
        const CotIdentityRecord identity = boundedIdentity(source);
        QJsonArray row;
        row.append(identity.systemId);
        row.append(jsonText(identity.eventUid));
        row.append(identity.includeTakv);
        row.append(jsonText(identity.contactCallsign));
        row.append(jsonText(identity.contactEndpoint));
        row.append(jsonText(identity.vmf));
        root.append(row);
    }
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool CotIdentityModel::decodeJson(
    const QByteArray &json, QVector<CotIdentityRecord> *identities,
    QString *error)
{
    if (!identities) {
        setError(error, tr("Missing CoT identity output collection."));
        return false;
    }
    if (json.size() > MaxJsonBytes) {
        setError(error, tr("CoT identity data is too large."));
        return false;
    }
    if (json.trimmed().isEmpty()) {
        identities->clear();
        setError(error, QString());
        return true;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        setError(error, tr("Invalid CoT identity JSON."));
        return false;
    }

    QVector<CotIdentityRecord> decoded;
    decoded.reserve(qMin(document.array().size(), MaxRows));
    const QJsonArray root = document.array();
    if (root.size() > MaxRows) {
        setError(error, tr("CoT identity data contains too many rows."));
        return false;
    }
    for (const QJsonValue &entry : root) {
        if (!entry.isArray()) {
            setError(error, tr("Invalid CoT identity row."));
            return false;
        }
        const QJsonArray row = entry.toArray();
        if (row.size() != ColumnCount) {
            setError(error, tr("A CoT identity row must contain six cells."));
            return false;
        }
        if (!isSupportedCell(row.at(0)) || !isBoundedCell(row.at(0))) {
            setError(error, tr("Invalid CoT identity system-id cell."));
            return false;
        }
        const int textColumns[] = {1, 3, 4, 5};
        for (int column : textColumns) {
            if ((!row.at(column).isNull() && !row.at(column).isString())
                || !isBoundedCell(row.at(column))) {
                setError(error, tr("Invalid CoT identity text cell."));
                return false;
            }
        }
        if (!row.at(2).isBool()) {
            setError(error, tr("Invalid CoT TAKV cell."));
            return false;
        }

        CotIdentityRecord identity;
        identity.systemId = row.at(0);
        identity.eventUid = primitiveText(row.at(1));
        identity.includeTakv = row.at(2).toBool();
        identity.contactCallsign = primitiveText(row.at(3));
        identity.contactEndpoint = primitiveText(row.at(4));
        identity.vmf = primitiveText(row.at(5));
        decoded.append(boundedIdentity(identity));
    }

    *identities = decoded;
    setError(error, QString());
    return true;
}

bool CotIdentityModel::isValidSystemId(int systemId)
{
    return systemId >= 0 && systemId <= 255;
}

QString CotIdentityModel::systemIdText(const QJsonValue &value)
{
    return primitiveText(value);
}

CotIdentityRecord CotIdentityModel::boundedIdentity(
    const CotIdentityRecord &identity)
{
    return identity;
}

bool CotIdentityModel::sameText(const QString &left, const QString &right)
{
    return left == right && left.isNull() == right.isNull();
}

bool CotIdentityModel::isSupportedCell(const QJsonValue &value)
{
    return value.isNull() || value.isString() || value.isBool()
        || (value.isDouble() && std::isfinite(value.toDouble()));
}

bool CotIdentityModel::isBoundedCell(const QJsonValue &value)
{
    return !value.isString() || value.toString().size() <= MaxTextLength;
}
